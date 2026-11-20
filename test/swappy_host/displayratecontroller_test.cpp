/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "DisplayRateController.h"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using namespace std::chrono_literals;
using swappy::DisplayRateController;
using swappy::insertRefreshPeriod;
using swappy::periodToMillihertz;
using swappy::RefreshPeriodMap;

TEST(DisplayRateControllerTest, ConvertsPeriodsToRoundedIntegerMillihertzSafely) {
    EXPECT_EQ(periodToMillihertz(0ns), 0);
    EXPECT_EQ(periodToMillihertz(-10ms), 0);
    EXPECT_EQ(periodToMillihertz(4'166'667ns), 240'000);
    EXPECT_EQ(periodToMillihertz(8'333'333ns), 120'000);
    EXPECT_EQ(periodToMillihertz(12'500'000ns), 80'000);
    EXPECT_EQ(periodToMillihertz(16'666'666ns), 60'000);
    EXPECT_EQ(periodToMillihertz(16'666'667ns), 60'000);
    EXPECT_EQ(periodToMillihertz(20'833'333ns), 48'000);
    EXPECT_EQ(periodToMillihertz(24'999'995ns), 40'000);
    EXPECT_EQ(periodToMillihertz(25'000'000ns), 40'000);
    EXPECT_EQ(periodToMillihertz(29'166'667ns), 34'286);
    EXPECT_EQ(periodToMillihertz(33'333'333ns), 30'000);
    EXPECT_EQ(periodToMillihertz(41'666'667ns), 24'000);
}

TEST(DisplayRateControllerTest, VoteFrameRateGuardsZeroAndVotesArrRates) {
    DisplayRateController controller;
    EXPECT_FALSE(controller.hasVoted());
    EXPECT_EQ(controller.lastVotedMillihertz(), 0);

    // Zero or negative effective periods never divide by zero or emit a vote.
    EXPECT_FALSE(controller.voteFrameRateHz(0ns, 0ns).has_value());
    EXPECT_FALSE(controller.voteFrameRateHz(-10ms, 0ns).has_value());
    EXPECT_FALSE(controller.hasVoted());

    // Initial 60Hz vote with zero minSwapDuration (resetFramePacing path) succeeds cleanly.
    const auto initial60 = controller.voteFrameRateHz(16'666'667ns, 0ns);
    ASSERT_TRUE(initial60.has_value());
    EXPECT_FLOAT_EQ(*initial60, 60.0f);
    EXPECT_TRUE(controller.hasVoted());
    EXPECT_EQ(controller.lastVotedMillihertz(), 60'000);

    // 1ns rounding noise stays within the 0.1 Hz deadband and is suppressed.
    EXPECT_FALSE(controller.voteFrameRateHz(16'666'666ns, 0ns).has_value());
    EXPECT_EQ(controller.lastVotedMillihertz(), 60'000);

    // Sub-20 Hz ARR steps with <= 1.0 Hz spacing still emit votes outside 0.1 Hz.
    const auto vote16 = controller.voteFrameRateHz(62'500'000ns, 0ns);
    ASSERT_TRUE(vote16.has_value());
    EXPECT_FLOAT_EQ(*vote16, 16.0f);

    const auto vote15 = controller.voteFrameRateHz(66'666'667ns, 0ns);
    ASSERT_TRUE(vote15.has_value());
    EXPECT_FLOAT_EQ(*vote15, 15.0f);

    const auto vote11 = controller.voteFrameRateHz(90'909'091ns, 0ns);
    ASSERT_TRUE(vote11.has_value());
    EXPECT_FLOAT_EQ(*vote11, 11.0f);

    const auto vote10 = controller.voteFrameRateHz(100'000'000ns, 0ns);
    ASSERT_TRUE(vote10.has_value());
    EXPECT_FLOAT_EQ(*vote10, 10.0f);

    // minSwapDuration clamps faster target periods to the app-requested cap (30Hz).
    const auto clamped30 = controller.voteFrameRateHz(12'500'000ns, 33'333'333ns);
    ASSERT_TRUE(clamped30.has_value());
    EXPECT_FLOAT_EQ(*clamped30, 30.0f);

    // Window replacement forces the next vote to re-issue even at the same rate.
    controller.onWindowChanged();
    EXPECT_EQ(controller.lastVotedMillihertz(), 0);
    const auto revoted30 = controller.voteFrameRateHz(33'333'333ns, 33'333'333ns);
    ASSERT_TRUE(revoted30.has_value());
    EXPECT_FLOAT_EQ(*revoted30, 30.0f);
}

TEST(DisplayRateControllerTest, SuppressesVotesWithinPointOneHzDeadbandAndVotesOutsideIt) {
    DisplayRateController controller;
    ASSERT_TRUE(controller.voteFrameRateHz(62'500'000ns, 0ns).has_value()); // 16,000 mHz

    // 100 mHz delta (15,900 mHz -> 62,893,082 ns) stays within kVoteMarginMillihertz (100 mHz).
    EXPECT_FALSE(controller.voteFrameRateHz(62'893'082ns, 0ns).has_value());
    EXPECT_EQ(controller.lastVotedMillihertz(), 16'000);

    // 101 mHz delta (15,899 mHz -> 62,897,038 ns) crosses kVoteMarginMillihertz and emits a vote.
    const auto vote = controller.voteFrameRateHz(62'897'038ns, 0ns);
    ASSERT_TRUE(vote.has_value());
    EXPECT_FLOAT_EQ(*vote, 15.899f);
    EXPECT_EQ(controller.lastVotedMillihertz(), 15'899);
}

TEST(DisplayRateControllerTest, LegacyModeSelectionSkipsArrAndNonPositiveEntries) {
    RefreshPeriodMap supported;
    supported.emplace(0ns, 99);
    supported.emplace(-1ms, 98);
    insertRefreshPeriod(supported, 8'333'333ns, 1);   // 120Hz physical mode
    insertRefreshPeriod(supported, 11'111'111ns, 2);  // 90Hz physical mode
    insertRefreshPeriod(supported, 12'500'000ns, -1); // 80Hz ARR mode-less entry
    insertRefreshPeriod(supported, 16'666'667ns, 3);  // 60Hz physical mode
    insertRefreshPeriod(supported, 25'000'000ns, -1); // 40Hz ARR mode-less entry

    DisplayRateController controller;

    // For a 33.33ms (30fps) target, 120Hz*4, 90Hz*3, and 60Hz*2 all achieve 33.33ms;
    // 60Hz (mode 3) wins as the longest physical refresh period.
    const auto first = controller.selectDisplayMode(&supported, 33'333'333ns, 16'666'667ns);
    EXPECT_TRUE(first.changed);
    EXPECT_EQ(first.modeId, 3);
    EXPECT_EQ(first.refreshPeriod, 16'666'667ns);

    // Repeating the same selection does not re-issue a redundant mode switch.
    const auto repeat = controller.selectDisplayMode(&supported, 33'333'333ns, 16'666'667ns);
    EXPECT_FALSE(repeat.changed);
    EXPECT_EQ(repeat.modeId, 3);

    // A 25ms (40fps) target cannot select the -1 ARR entry; 120Hz*3 = 25ms (mode 1) wins.
    const auto fortyFps = controller.selectDisplayMode(&supported, 25'000'000ns, 8'333'333ns);
    EXPECT_TRUE(fortyFps.changed);
    EXPECT_EQ(fortyFps.modeId, 1);
    EXPECT_EQ(fortyFps.refreshPeriod, 8'333'333ns);
}

} // namespace

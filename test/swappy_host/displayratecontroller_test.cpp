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

    // 1ns rounding noise stays within the vote margin and is suppressed.
    EXPECT_FALSE(controller.voteFrameRateHz(16'666'666ns, 0ns).has_value());
    EXPECT_EQ(controller.lastVotedMillihertz(), 60'000);

    // Sub-20 Hz ARR steps with 1.0 Hz spacing still emit votes outside the 2% margin.
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

TEST(DisplayRateControllerTest, SuppressesVotesWithinTwoPercentMarginAndVotesOutsideIt) {
    EXPECT_EQ(DisplayRateController::voteMarginMillihertz(50'000), 1'000);
    EXPECT_EQ(DisplayRateController::voteMarginMillihertz(16'000), 320);
    // Below 5 Hz the 0.1 Hz floor applies.
    EXPECT_EQ(DisplayRateController::voteMarginMillihertz(4'000), 100);

    DisplayRateController controller;
    ASSERT_TRUE(controller.voteFrameRateHz(20'000'000ns, 0ns).has_value()); // 50,000 mHz

    // 1,000 mHz below (49,000 mHz -> 20,408,163 ns) and above (51,000 mHz -> 19,607,843 ns)
    // stay within the 2% margin.
    EXPECT_FALSE(controller.voteFrameRateHz(20'408'163ns, 0ns).has_value());
    EXPECT_FALSE(controller.voteFrameRateHz(19'607'843ns, 0ns).has_value());
    EXPECT_EQ(controller.lastVotedMillihertz(), 50'000);

    // 1,001 mHz below (48,999 mHz -> 20,408,580 ns) crosses the margin and emits a vote.
    const auto vote = controller.voteFrameRateHz(20'408'580ns, 0ns);
    ASSERT_TRUE(vote.has_value());
    EXPECT_FLOAT_EQ(*vote, 48.999f);
    EXPECT_EQ(controller.lastVotedMillihertz(), 48'999);
}

TEST(DisplayRateControllerTest, JitteringSteadyTargetVotesOnce) {
    DisplayRateController controller;
    // A rolling-mean frame time wandering +/-0.2 ms around 22 ms (45.0-45.9 Hz)
    // re-voted on most calls with a 0.1 Hz deadband.
    int votes = 0;
    for (int i = 0; i < 600; ++i) {
        const auto jitter = std::chrono::microseconds((i * 37) % 401 - 200);
        if (controller.voteFrameRateHz(22ms + jitter, 16'666'667ns).has_value()) {
            ++votes;
        }
    }
    EXPECT_EQ(votes, 1);

    // A real rate change still votes: 22 ms -> 25 ms (40 Hz).
    const auto vote40 = controller.voteFrameRateHz(25ms, 16'666'667ns);
    ASSERT_TRUE(vote40.has_value());
    EXPECT_FLOAT_EQ(*vote40, 40.0f);
}

TEST(DisplayRateControllerTest, VotesAppFloorExactlyWithinTwoPercentOfLastVote) {
    DisplayRateController controller;
    // A recovering frame time approaches the 60 Hz app floor: 59.0 Hz votes.
    const auto vote59 = controller.voteFrameRateHz(16'949'153ns, 16'666'667ns);
    ASSERT_TRUE(vote59.has_value());
    EXPECT_FLOAT_EQ(*vote59, 59.0f);

    // 59.5 Hz is still a measured frame time and stays within the 2% margin.
    EXPECT_FALSE(controller.voteFrameRateHz(16'806'723ns, 16'666'667ns).has_value());

    // Once the frame time is clamped to the floor, the vote reaches 60 Hz exactly even though
    // it is within 2% of 59.0 Hz; a TE divisor panel would otherwise stay at 48 Hz.
    const auto vote60 = controller.voteFrameRateHz(12ms, 16'666'667ns);
    ASSERT_TRUE(vote60.has_value());
    EXPECT_FLOAT_EQ(*vote60, 60.0f);

    // Further frames under the floor repeat the same 60 Hz target and stay suppressed, and a
    // measured 59.5 Hz frame time stays within the 2% margin of 60 Hz.
    EXPECT_FALSE(controller.voteFrameRateHz(10ms, 16'666'667ns).has_value());
    EXPECT_FALSE(controller.voteFrameRateHz(16'806'723ns, 16'666'667ns).has_value());
}

TEST(DisplayRateControllerTest, RevotesSlowTargetOnceHeldVoteNoLongerCoversFrame) {
    DisplayRateController controller;
    // 66 ms (15,152 mHz) is reachable when the app raises the auto-swap threshold.
    ASSERT_TRUE(controller.voteFrameRateHz(66ms, 16'666'667ns).has_value());

    // 66.6 ms (15,015 mHz) is within the 2% margin (303 mHz), and its frame time without
    // FRAME_MARGIN (65.6 ms) still fits the held 66 ms period.
    EXPECT_FALSE(controller.voteFrameRateHz(66'600'000ns, 16'666'667ns).has_value());

    // 67.3 ms (14,859 mHz) is also within the 2% margin, but its frame time without
    // FRAME_MARGIN (66.3 ms) no longer fits the held 66 ms period.
    const auto vote = controller.voteFrameRateHz(67'300'000ns, 16'666'667ns);
    ASSERT_TRUE(vote.has_value());
    EXPECT_FLOAT_EQ(*vote, 14.859f);
    EXPECT_EQ(controller.lastVotedMillihertz(), 14'859);
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

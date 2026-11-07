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

#include "RefreshPeriodSet.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>

namespace {

using namespace std::chrono_literals;
using swappy::copyRefreshPeriods;
using swappy::insertRefreshPeriod;
using swappy::RefreshPeriodMap;

TEST(RefreshPeriodSetTest, DeduplicatesWithinRoundingMarginAndPreservesModeIds) {
    RefreshPeriodMap periods;

    // ARR entry arrives first with modeId -1, followed by a physical mode 1ns apart.
    insertRefreshPeriod(periods, 16'666'666ns, -1);
    insertRefreshPeriod(periods, 16'666'667ns, 2);

    // Physical mode arrives first, followed by a duplicate ARR entry within 1us.
    insertRefreshPeriod(periods, 8'333'333ns, 1);
    insertRefreshPeriod(periods, 8'333'400ns, -1);

    // Mode-less ARR intermediate refresh periods remain distinct.
    insertRefreshPeriod(periods, 12'500'000ns, -1); // 80Hz
    insertRefreshPeriod(periods, 20'833'333ns, -1); // 48Hz
    insertRefreshPeriod(periods, 25'000'000ns, -1); // 40Hz

    EXPECT_EQ(periods.size(), 5u);
    EXPECT_EQ(periods.at(8'333'333ns), 1);
    EXPECT_EQ(periods.at(12'500'000ns), -1);
    EXPECT_EQ(periods.at(16'666'666ns), 2);
    EXPECT_EQ(periods.at(20'833'333ns), -1);
    EXPECT_EQ(periods.at(25'000'000ns), -1);
}

TEST(RefreshPeriodSetTest, IgnoresZeroAndNegativePeriods) {
    RefreshPeriodMap periods;

    insertRefreshPeriod(periods, 0ns, 0);
    insertRefreshPeriod(periods, -16'666'667ns, 1);
    insertRefreshPeriod(periods, 16'666'667ns, 2);

    EXPECT_EQ(periods.size(), 1u);
    EXPECT_EQ(periods.at(16'666'667ns), 2);
}

TEST(RefreshPeriodSetTest, CopyRefreshPeriodsRespectsNullAndAllocatedBounds) {
    RefreshPeriodMap periods;
    insertRefreshPeriod(periods, 8'333'333ns, 1);   // 120Hz
    insertRefreshPeriod(periods, 12'500'000ns, -1); // 80Hz
    insertRefreshPeriod(periods, 16'666'667ns, 2);  // 60Hz
    insertRefreshPeriod(periods, 20'833'333ns, -1); // 48Hz
    insertRefreshPeriod(periods, 25'000'000ns, -1); // 40Hz
    insertRefreshPeriod(periods, 33'333'333ns, -1); // 30Hz
    insertRefreshPeriod(periods, 41'666'667ns, -1); // 24Hz

    EXPECT_EQ(copyRefreshPeriods(nullptr, nullptr, 0), 0);
    EXPECT_EQ(copyRefreshPeriods(&periods, nullptr, 0), 7);

    std::array<uint64_t, 6> out = {0, 0, 0, 0, 0, 0xDEADBEEFULL};
    EXPECT_EQ(copyRefreshPeriods(&periods, out.data(), 0), 0);
    EXPECT_EQ(copyRefreshPeriods(&periods, out.data(), -1), 0);
    EXPECT_EQ(out[0], 0u);

    // Caller passes a 5-entry buffer (matching swappy_basic_test): writes 5 without overrun.
    EXPECT_EQ(copyRefreshPeriods(&periods, out.data(), 5), 5);
    EXPECT_EQ(out[0], 8'333'333u);
    EXPECT_EQ(out[1], 12'500'000u);
    EXPECT_EQ(out[2], 16'666'667u);
    EXPECT_EQ(out[3], 20'833'333u);
    EXPECT_EQ(out[4], 25'000'000u);
    EXPECT_EQ(out[5], 0xDEADBEEFULL);
}

} // namespace

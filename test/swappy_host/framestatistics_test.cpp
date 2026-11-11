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

#include "FrameStatistics.h"

#include <cstdint>

#include "gtest/gtest.h"
#include "swappy/swappy_common.h"

namespace {

using swappy::FrameStatistics;
using swappy::FrameTimings;

constexpr uint64_t k60HzPeriodNs = 16'666'667;

TEST(FrameStatisticsHostTest, AccumulatesHistogramsAndClearResetsBuckets) {
    FrameStatistics stats;
    stats.enableStats(true);

    stats.updateFrameStats(
            FrameTimings{
                    .startFrameTime = 100'000'000,
                    .desiredPresentTime = 116'666'667,
                    .actualPresentTime = 116'666'667,
                    .presentMargin = 2'000'000,
            },
            k60HzPeriodNs);
    stats.updateFrameStats(
            FrameTimings{
                    .startFrameTime = 116'666'667,
                    .desiredPresentTime = 133'333'334,
                    .actualPresentTime = 150'000'001,
                    .presentMargin = k60HzPeriodNs,
            },
            k60HzPeriodNs);

    EXPECT_EQ(stats.lastLatencyRecorded(), 2);

    const SwappyStats snapshot = stats.getStats();
    EXPECT_EQ(snapshot.totalFrames, 2u);
    EXPECT_EQ(snapshot.idleFrames[0], 1u);
    EXPECT_EQ(snapshot.idleFrames[1], 1u);
    EXPECT_EQ(snapshot.lateFrames[0], 1u);
    EXPECT_EQ(snapshot.lateFrames[1], 1u);
    EXPECT_EQ(snapshot.offsetFromPreviousFrame[2], 1u);
    EXPECT_EQ(snapshot.latencyFrames[1], 1u);
    EXPECT_EQ(snapshot.latencyFrames[2], 1u);

    stats.clearStats();
    EXPECT_EQ(stats.getStats().totalFrames, 0u);
}

} // namespace

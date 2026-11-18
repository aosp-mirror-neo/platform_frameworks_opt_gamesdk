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

#include "ClassicPacingImpl.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

using namespace swappy;
using namespace std::chrono_literals;

using std::chrono::nanoseconds;
using TimePoint = FrameDurations::TimePoint;

namespace {

constexpr nanoseconds k60Hz = 16666667ns;
constexpr nanoseconds k120Hz = 8333333ns;
constexpr VotableRefreshPeriods kSetFrameRateVotable{.usesSetFrameRate = true};

// Enough 60 Hz frames to span strictly more than FrameDurations' 2 s sample
// window (> 120 intervals), so hasEnoughSamples() is satisfied.
constexpr int kFramesToFillWindow = 130;

TimePoint feed(FrameDurations& durations, TimePoint start, int count, nanoseconds spacing,
               nanoseconds cpu, nanoseconds gpu = 0ns, bool miss = false) {
    TimePoint now = start;
    for (int i = 0; i < count; ++i) {
        durations.add(FrameDuration(cpu, gpu, miss), now);
        now += spacing;
    }
    return now;
}

RefreshPeriodMap makePhysical60And120HzPeriods() {
    RefreshPeriodMap periods;
    insertRefreshPeriod(periods, k120Hz, 1);
    insertRefreshPeriod(periods, k60Hz, 2);
    return periods;
}

} // namespace

TEST(ClassicPacingImplTest, InsufficientSamplesReturnsEmptyDecision) {
    ClassicPacingImplementation pacing;
    LadderState state = {2, PipelineMode::Off};
    FrameDurations durations;
    ASSERT_FALSE(durations.hasEnoughSamples());

    const PacingResult result =
            pacing.updateSwapInterval(state.autoSwapInterval, state.pipelineMode, durations, k60Hz,
                                      0ns, 50ms, true, kSetFrameRateVotable);

    EXPECT_FALSE(result.configChanged);
    EXPECT_EQ(result.autoSwapInterval, 2);
    EXPECT_EQ(result.pipelineMode, PipelineMode::Off);
    EXPECT_FALSE(result.vote.emitted());
    EXPECT_FALSE(durations.hasEnoughSamples());
}

TEST(ClassicPacingImplTest, OverBudgetAdvancesIntervalAndClearsDurations) {
    ClassicPacingImplementation pacing;
    LadderState state = {1, PipelineMode::On};
    FrameDurations durations;
    feed(durations, TimePoint{}, kFramesToFillWindow, k60Hz, 22ms, 0ns, /*miss=*/true);
    ASSERT_TRUE(durations.hasEnoughSamples());

    const PacingResult result =
            pacing.updateSwapInterval(state.autoSwapInterval, state.pipelineMode, durations, k60Hz,
                                      0ns, 50ms, true, kSetFrameRateVotable);

    EXPECT_TRUE(result.configChanged);
    EXPECT_EQ(result.autoSwapInterval, 2);
    EXPECT_EQ(result.pipelineMode, PipelineMode::On);
    ASSERT_TRUE(result.vote.frameRateHz.has_value());
    EXPECT_NEAR(*result.vote.frameRateHz, 1e9f / 23'000'000.f, 0.05f);
    EXPECT_FALSE(durations.hasEnoughSamples());
}

TEST(ClassicPacingImplTest, SteadyStatePreservesDurations) {
    ClassicPacingImplementation pacing;
    LadderState state = {1, PipelineMode::Off};
    FrameDurations durations;
    feed(durations, TimePoint{}, kFramesToFillWindow, k60Hz, 8ms);
    ASSERT_TRUE(durations.hasEnoughSamples());

    const PacingResult result =
            pacing.updateSwapInterval(state.autoSwapInterval, state.pipelineMode, durations, k60Hz,
                                      0ns, 50ms, true, kSetFrameRateVotable);

    EXPECT_FALSE(result.configChanged);
    EXPECT_EQ(result.autoSwapInterval, 1);
    EXPECT_EQ(result.pipelineMode, PipelineMode::Off);
    ASSERT_TRUE(result.vote.frameRateHz.has_value());
    EXPECT_NEAR(*result.vote.frameRateHz, 1e9f / 9'000'000.f, 0.05f);
    EXPECT_TRUE(durations.hasEnoughSamples());
}

TEST(ClassicPacingImplTest, MissedFramesWithinBudgetRestoresPipeliningWithoutClearingDurations) {
    ClassicPacingImplementation pacing;
    LadderState state = {2, PipelineMode::Off};
    FrameDurations durations;
    // 22ms + 1ms FRAME_MARGIN <= 2 * 16.67ms upper bound: swapSlower restores
    // PipelineMode::On without changing autoSwapInterval or setting configChanged.
    feed(durations, TimePoint{}, kFramesToFillWindow, k60Hz, 22ms, 0ns, /*miss=*/true);
    ASSERT_TRUE(durations.hasEnoughSamples());

    const PacingResult result =
            pacing.updateSwapInterval(state.autoSwapInterval, state.pipelineMode, durations, k60Hz,
                                      0ns, 50ms, true, kSetFrameRateVotable);

    EXPECT_FALSE(result.configChanged);
    EXPECT_EQ(result.autoSwapInterval, 2);
    EXPECT_EQ(result.pipelineMode, PipelineMode::On);
    ASSERT_TRUE(result.vote.frameRateHz.has_value());
    EXPECT_NEAR(*result.vote.frameRateHz, 1e9f / 23'000'000.f, 0.05f);
    EXPECT_TRUE(durations.hasEnoughSamples());
}

TEST(ClassicPacingImplTest, DisplayTimingChangeUsesMeasuredPipelineFrameTimeAndInitialVote) {
    ClassicPacingImplementation pacing;
    LadderState state = {1, PipelineMode::On};
    FrameDurations durations;
    feed(durations, TimePoint{}, kFramesToFillWindow, k60Hz, 22ms);
    ASSERT_TRUE(durations.hasEnoughSamples());
    EXPECT_GT(durations.getAverageFrameTime().getTime(PipelineMode::On), 0ns);

    TimingChange change;
    change.newRefreshPeriod = k120Hz;
    change.swapDuration = k120Hz;

    const PacingResult result =
            pacing.onDisplayTimingsChanged(state.autoSwapInterval, state.pipelineMode, durations,
                                           change, kSetFrameRateVotable);

    EXPECT_FALSE(result.configChanged);
    EXPECT_EQ(result.autoSwapInterval, 3);
    EXPECT_EQ(result.pipelineMode, PipelineMode::On);
    ASSERT_TRUE(result.vote.frameRateHz.has_value());
    EXPECT_NEAR(*result.vote.frameRateHz, 120.0f, 0.05f);
    EXPECT_FALSE(durations.hasEnoughSamples());
}

TEST(ClassicPacingImplTest, DisplayTimingChangeFallsBackWhenNoSamples) {
    ClassicPacingImplementation pacing;
    LadderState state = {1, PipelineMode::Off};
    FrameDurations durations;
    ASSERT_FALSE(durations.hasEnoughSamples());

    TimingChange change;
    change.newRefreshPeriod = k60Hz;
    change.swapDuration = 33333334ns;

    const PacingResult result =
            pacing.onDisplayTimingsChanged(state.autoSwapInterval, state.pipelineMode, durations,
                                           change);

    EXPECT_FALSE(result.configChanged);
    EXPECT_EQ(result.autoSwapInterval, 2);
    EXPECT_EQ(result.pipelineMode, PipelineMode::On);
    EXPECT_FALSE(result.vote.emitted());
}

TEST(ClassicPacingImplTest, DisplayTimingChangeSelectsLegacyDisplayModeAndWindowChangeRearmsVote) {
    ClassicPacingImplementation pacing;
    const RefreshPeriodMap periods = makePhysical60And120HzPeriods();
    const VotableRefreshPeriods legacyVotable{.periods = &periods, .usesSetFrameRate = false};

    FrameDurations durations;
    TimingChange change{
            .newRefreshPeriod = k120Hz,
            .swapDuration = k60Hz,
            .swapDurationChanged = true,
    };
    const PacingResult modeResult =
            pacing.onDisplayTimingsChanged(1, PipelineMode::On, durations, change, legacyVotable);
    EXPECT_EQ(modeResult.autoSwapInterval, 2);
    ASSERT_TRUE(modeResult.vote.displayModeId.has_value());
    EXPECT_EQ(*modeResult.vote.displayModeId, 2);

    const RefreshRateVote firstArrVote =
            pacing.voteRefreshPeriod(k60Hz, k60Hz, kSetFrameRateVotable);
    ASSERT_TRUE(firstArrVote.frameRateHz.has_value());
    EXPECT_EQ(firstArrVote.traceValue, 60);
    EXPECT_FALSE(pacing.voteRefreshPeriod(k60Hz, k60Hz, kSetFrameRateVotable).emitted());

    pacing.onWindowChanged();
    EXPECT_TRUE(pacing.voteRefreshPeriod(k60Hz, k60Hz, kSetFrameRateVotable).emitted());
}

TEST(ClassicPacingImplTest, PolymorphicInvocationThroughPacingImplementation) {
    std::unique_ptr<PacingImplementation> impl = std::make_unique<ClassicPacingImplementation>();

    // Verify polymorphic updateSwapInterval()
    LadderState state = {1, PipelineMode::On};
    FrameDurations durations;
    feed(durations, TimePoint{}, kFramesToFillWindow, k60Hz, 22ms, 0ns, /*miss=*/true);

    const PacingResult result = impl->updateSwapInterval(state.autoSwapInterval, state.pipelineMode,
                                                         durations, k60Hz, 0ns, 50ms, true);
    EXPECT_TRUE(result.configChanged);
    EXPECT_EQ(result.autoSwapInterval, 2);

    // Verify polymorphic onWindowChanged(), voteRefreshPeriod(), and onDisplayTimingsChanged()
    impl->onWindowChanged();
    EXPECT_TRUE(impl->voteRefreshPeriod(k60Hz, k60Hz, kSetFrameRateVotable).emitted());

    TimingChange change;
    change.newRefreshPeriod = k60Hz;
    change.swapDuration = 33333334ns;
    FrameDurations emptyDurations;

    const PacingResult timingResult =
            impl->onDisplayTimingsChanged(state.autoSwapInterval, state.pipelineMode,
                                          emptyDurations, change);
    EXPECT_EQ(timingResult.autoSwapInterval, 2);
    EXPECT_EQ(timingResult.pipelineMode, PipelineMode::On);
}

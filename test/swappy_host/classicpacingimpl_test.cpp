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
constexpr nanoseconds k40Hz = 25000000ns;
constexpr nanoseconds k30Hz = 33333334ns;
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
    EXPECT_TRUE(durations.hasEnoughSamples());
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

TEST(ClassicPacingImplTest, DisplayTimingChangeClampsLongStallToAutoSwapThreshold) {
    ClassicPacingImplementation pacing;
    FrameDurations durations;
    feed(durations, TimePoint{}, kFramesToFillWindow, 75ms, 75ms, 0ns, /*miss=*/true);
    ASSERT_TRUE(durations.hasEnoughSamples());

    TimingChange change;
    change.newRefreshPeriod = k60Hz;
    change.swapDuration = k120Hz;
    change.autoSwapIntervalThreshold = 50ms;

    const PacingResult timingResult =
            pacing.onDisplayTimingsChanged(6, PipelineMode::On, durations, change);

    // 76ms pipelined implies interval 5 on 60Hz (83.3ms), clamped to 3 (50.0ms).
    EXPECT_EQ(timingResult.autoSwapInterval, 3);
    EXPECT_EQ(timingResult.pipelineMode, PipelineMode::On);
    EXPECT_TRUE(durations.hasEnoughSamples());
}

TEST(ClassicPacingImplTest, ThresholdOnlyChangeReportsReclampAsConfigChange) {
    ClassicPacingImplementation pacing;
    FrameDurations durations;
    feed(durations, TimePoint{}, kFramesToFillWindow, 75ms, 75ms, 0ns, /*miss=*/true);
    ASSERT_TRUE(durations.hasEnoughSamples());

    // Tightening the ceiling to 60fps re-clamps interval 4 to 2 on 120Hz.
    TimingChange tightened;
    tightened.oldRefreshPeriod = k120Hz;
    tightened.newRefreshPeriod = k120Hz;
    tightened.swapDuration = k60Hz;
    tightened.autoSwapIntervalThreshold = k60Hz;
    tightened.autoSwapIntervalThresholdChanged = true;
    const PacingResult reclamped =
            pacing.onDisplayTimingsChanged(4, PipelineMode::On, durations, tightened);
    EXPECT_EQ(reclamped.autoSwapInterval, 2);
    EXPECT_TRUE(reclamped.configChanged);

    // Already within the new ceiling: nothing to report.
    const PacingResult unchanged =
            pacing.onDisplayTimingsChanged(2, PipelineMode::On, durations, tightened);
    EXPECT_EQ(unchanged.autoSwapInterval, 2);
    EXPECT_FALSE(unchanged.configChanged);

    // A same-frame app floor change owns the update, so it is not reported.
    TimingChange withFloorChange = tightened;
    withFloorChange.swapDurationChanged = true;
    const PacingResult floorChanged =
            pacing.onDisplayTimingsChanged(4, PipelineMode::On, durations, withFloorChange);
    EXPECT_EQ(floorChanged.autoSwapInterval, 2);
    EXPECT_FALSE(floorChanged.configChanged);
}

TEST(ClassicPacingImplTest, UpdateSwapIntervalClampsRefreshRateVoteToAutoSwapThreshold) {
    ClassicPacingImplementation pacing;
    FrameDurations durations;
    feed(durations, TimePoint{}, kFramesToFillWindow, 75ms, 75ms, 0ns, /*miss=*/true);
    ASSERT_TRUE(durations.hasEnoughSamples());

    const PacingResult downshiftResult =
            pacing.updateSwapInterval(1, PipelineMode::On, durations, k120Hz, k120Hz, 50ms, true,
                                      kSetFrameRateVotable);

    EXPECT_TRUE(downshiftResult.configChanged);
    EXPECT_EQ(downshiftResult.autoSwapInterval, 6);
    ASSERT_TRUE(downshiftResult.vote.frameRateHz.has_value());
    EXPECT_NEAR(*downshiftResult.vote.frameRateHz, 20.0f, 0.05f);
}

TEST(ClassicPacingImplTest, UnpacedAutoSwapThresholdVotesRequestedFloorInsteadOfStallDuration) {
    ClassicPacingImplementation pacing;
    FrameDurations durations;
    feed(durations, TimePoint{}, kFramesToFillWindow, 40ms, 40ms, 0ns, /*miss=*/true);
    ASSERT_TRUE(durations.hasEnoughSamples());

    const PacingResult unpacedResult =
            pacing.updateSwapInterval(2, PipelineMode::On, durations, k120Hz, k60Hz, 0ns, true,
                                      kSetFrameRateVotable);

    EXPECT_FALSE(unpacedResult.configChanged);
    EXPECT_EQ(unpacedResult.autoSwapInterval, 2);
    ASSERT_TRUE(unpacedResult.vote.frameRateHz.has_value());
    EXPECT_NEAR(*unpacedResult.vote.frameRateHz, 60.0f, 0.05f);
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

TEST(ClassicPacingImplTest, DisplayTimingChangeTreatsSubMicrosecond120HzMultipleAsValidFloor) {
    ClassicPacingImplementation pacing;
    // Seed a prior vote (40 Hz) so hasVoted() is true and only !swapIntervalValid
    // would emit a recovery vote to swapDuration (60 Hz).
    ASSERT_TRUE(pacing.voteRefreshPeriod(k40Hz, k60Hz, kSetFrameRateVotable).emitted());

    FrameDurations durations;
    feed(durations, TimePoint{}, kFramesToFillWindow, k60Hz, 15ms);
    ASSERT_TRUE(durations.hasEnoughSamples());

    // 2 * 8,333,333ns = 16,666,666ns, 1ns shy of k60Hz (16,666,667ns) due to
    // integer nanosecond truncation; DURATION_ROUNDING_MARGIN absorbs the 1ns gap.
    const TimingChange changeTo120Hz{
            .oldRefreshPeriod = k60Hz,
            .newRefreshPeriod = k120Hz,
            .swapDuration = k60Hz,
    };
    const PacingResult validResult =
            pacing.onDisplayTimingsChanged(1, PipelineMode::On, durations, changeTo120Hz,
                                           kSetFrameRateVotable);
    EXPECT_EQ(validResult.autoSwapInterval, 2);
    EXPECT_FALSE(validResult.vote.emitted());

    // A genuine undercut (6ms -> interval 1 = 8.33ms < 16.67ms) still clamps to
    // floorInterval (2) and votes back to the 60 Hz floor.
    FrameDurations fastDurations;
    feed(fastDurations, TimePoint{}, kFramesToFillWindow, k60Hz, 6ms);
    ASSERT_TRUE(fastDurations.hasEnoughSamples());

    const PacingResult undercutResult =
            pacing.onDisplayTimingsChanged(1, PipelineMode::On, fastDurations, changeTo120Hz,
                                           kSetFrameRateVotable);
    EXPECT_EQ(undercutResult.autoSwapInterval, 2);
    ASSERT_TRUE(undercutResult.vote.frameRateHz.has_value());
    EXPECT_NEAR(*undercutResult.vote.frameRateHz, 60.0f, 0.05f);
}

TEST(ClassicPacingImplTest, FramePacingResetClearsSamplesRestoresPipelineAndVotesFastestWhenUnset) {
    ClassicPacingImplementation pacing;
    const RefreshPeriodMap periods = makePhysical60And120HzPeriods();
    const VotableRefreshPeriods votable{.periods = &periods, .usesSetFrameRate = true};

    FrameDurations durations;
    feed(durations, TimePoint{}, kFramesToFillWindow, k60Hz, 8ms);
    ASSERT_TRUE(durations.hasEnoughSamples());

    const PacingResult resetResult =
            pacing.onFramePacingReset(durations, k60Hz, 0ns, k60Hz, votable);

    EXPECT_TRUE(resetResult.configChanged);
    EXPECT_EQ(resetResult.autoSwapInterval, 1);
    EXPECT_EQ(resetResult.pipelineMode, PipelineMode::On);
    EXPECT_FALSE(durations.hasEnoughSamples());
    ASSERT_TRUE(resetResult.vote.frameRateHz.has_value());
    EXPECT_NEAR(*resetResult.vote.frameRateHz, 120.0f, 0.05f);
}

TEST(ClassicPacingImplTest, FramePacingResetVotesMinSwapDurationFloorWhenCapped) {
    ClassicPacingImplementation pacing;
    FrameDurations durations;

    const PacingResult resetResult =
            pacing.onFramePacingReset(durations, k120Hz, k60Hz, k60Hz, kSetFrameRateVotable);

    EXPECT_TRUE(resetResult.configChanged);
    EXPECT_EQ(resetResult.autoSwapInterval, 2);
    EXPECT_EQ(resetResult.pipelineMode, PipelineMode::On);
    ASSERT_TRUE(resetResult.vote.frameRateHz.has_value());
    EXPECT_NEAR(*resetResult.vote.frameRateHz, 60.0f, 0.05f);
}

TEST(ClassicPacingImplTest, PassiveTimingChangeRetainsSamplesAndClearsMissesFittingLoosenedBudget) {
    ClassicPacingImplementation pacing;
    FrameDurations durations;
    TimePoint now = feed(durations, TimePoint{}, kFramesToFillWindow - 20, k60Hz, 20ms);
    feed(durations, now, 20, k60Hz, 20ms, 0ns, /*miss=*/true);
    ASSERT_TRUE(durations.hasEnoughSamples());
    ASSERT_GT(durations.getMissedFramePercent(), 10);

    const TimingChange passiveChange{
            .oldRefreshPeriod = k60Hz,
            .newRefreshPeriod = k40Hz,
            .swapDuration = k60Hz,
    };
    const PacingResult passiveResult =
            pacing.onDisplayTimingsChanged(1, PipelineMode::On, durations, passiveChange);
    EXPECT_EQ(passiveResult.autoSwapInterval, 1);
    EXPECT_TRUE(durations.hasEnoughSamples());
    EXPECT_EQ(durations.getMissedFramePercent(), 0);
}

TEST(ClassicPacingImplTest, ExplicitSwapDurationChangeClearsSamples) {
    ClassicPacingImplementation pacing;
    FrameDurations durations;
    feed(durations, TimePoint{}, kFramesToFillWindow, k40Hz, 20ms);
    ASSERT_TRUE(durations.hasEnoughSamples());

    const TimingChange explicitSwapDurationChange{
            .oldRefreshPeriod = k40Hz,
            .newRefreshPeriod = k40Hz,
            .swapDuration = k30Hz,
            .swapDurationChanged = true,
    };
    (void)pacing.onDisplayTimingsChanged(2, PipelineMode::On, durations,
                                         explicitSwapDurationChange);
    EXPECT_FALSE(durations.hasEnoughSamples());
}

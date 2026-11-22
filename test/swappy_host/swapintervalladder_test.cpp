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

// Characterisation tests for the swap interval ladder.
//
// These describe what the shipping ladder does, not what it ideally would. The
// ladder had no tests before; it was extracted from SwappyCommon precisely so
// it could have some, and the lock-in work that follows is going to change it,
// so what it does today needs pinning down first.
//
// Every expectation here was read off the implementation and then confirmed
// against the pre-refactor code by a one-off differential run over 267 million
// enumerated inputs. That run is not committed -- a permanent second copy of
// the ladder is the drift this refactor removes -- so these tests are what
// remains of it. They were sized by re-running the same ten mutations the
// differential was checked against, and catch all ten.
//
// Several assertions look like bugs. They are marked where they do; the point
// of a characterisation test is to record the behaviour, not to endorse it.

#include "SwapIntervalLadder.h"

#include <gtest/gtest.h>

using namespace swappy;
using namespace std::chrono_literals;

using std::chrono::nanoseconds;

namespace {

constexpr nanoseconds k60Hz = 16666667ns;

TEST(SwapIntervalLadderTest, TuningConstantsAreTheShippingValues) {
    EXPECT_EQ(FRAME_DROP_THRESHOLD, 10);
    EXPECT_EQ(NON_PIPELINE_PERCENT, 50);
    EXPECT_EQ(REFRESH_RATE_MARGIN, 500ns);
    EXPECT_EQ(DURATION_ROUNDING_MARGIN, 1us);
    EXPECT_EQ(DEFAULT_AUTO_SWAP_INTERVAL_THRESHOLD, 50ms);
}

// Defaults chosen to be out of the way: no requested swap duration floor, a
// 20 fps ceiling, pipelining allowed to switch itself off.
LadderInput input(nanoseconds cpu, nanoseconds gpu, int missedPercent) {
    LadderInput in;
    in.averageFrameTime = FrameDuration(cpu, gpu, false);
    in.missedFramesPercent = missedPercent;
    in.refreshPeriod = k60Hz;
    in.swapDuration = 0ns;
    in.autoSwapIntervalThreshold = 50ms;
    in.pipelineModeAutoMode = true;
    return in;
}

LadderState state(int32_t interval, PipelineMode mode) {
    LadderState s;
    s.autoSwapInterval = interval;
    s.pipelineMode = mode;
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Entry conditions
// ---------------------------------------------------------------------------

// The ladder must not be asked to decide on a window shorter than the sample
// period. Two of the values it would read are actively misleading when the
// window is short: the average frame time is zero, which reads as "everything
// fits" and upshifts, and the miss percentage divides by an empty deque.
TEST(SwapIntervalLadderTest, NoInputUntilTheSampleWindowIsLongEnough) {
    FrameDurations durations;
    FrameDurations::TimePoint now{};

    durations.add(FrameDuration(5ms, 0ms, false), now);
    EXPECT_FALSE(makeLadderInput(durations, k60Hz, 0ns, 50ms, true).has_value());

    now += 1s;
    durations.add(FrameDuration(5ms, 0ms, false), now);
    EXPECT_FALSE(makeLadderInput(durations, k60Hz, 0ns, 50ms, true).has_value());

    now += 1500ms;
    durations.add(FrameDuration(5ms, 0ms, false), now);
    const auto in = makeLadderInput(durations, k60Hz, 0ns, 50ms, true);
    ASSERT_TRUE(in.has_value());
    EXPECT_EQ(in->averageFrameTime.getCpuTime(), 5ms);
    EXPECT_EQ(in->missedFramesPercent, 0);
}

TEST(SwapIntervalLadderTest, MakeLadderInputCopiesEveryParameterThrough) {
    FrameDurations durations;
    FrameDurations::TimePoint now{};
    durations.add(FrameDuration(5ms, 0ms, false), now);
    now += 2500ms;
    durations.add(FrameDuration(5ms, 0ms, false), now);

    const auto in = makeLadderInput(durations, k60Hz, 3ms, 40ms, false);

    ASSERT_TRUE(in.has_value());
    EXPECT_EQ(in->refreshPeriod, k60Hz);
    EXPECT_EQ(in->swapDuration, 3ms);
    EXPECT_EQ(in->autoSwapIntervalThreshold, 40ms);
    EXPECT_FALSE(in->pipelineModeAutoMode);
}

TEST(SwapIntervalLadderTest, BoundsAreReportedForTheIntervalBeingLeft) {
    const LadderDecision d = decideSwapInterval(state(2, PipelineMode::On), input(5ms, 0ms, 0));

    EXPECT_EQ(d.upperBound, k60Hz * 2);
    EXPECT_EQ(d.lowerBound, k60Hz * 1 - FRAME_MARGIN);
}

// ---------------------------------------------------------------------------
// Swapping slower
// ---------------------------------------------------------------------------

// Above the drop threshold the interval lengthens by one step.
TEST(SwapIntervalLadderTest, MissingFramesLengthensTheSwapInterval) {
    const LadderDecision d = decideSwapInterval(state(1, PipelineMode::On), input(5ms, 0ms, 11));

    EXPECT_EQ(d.action, LadderAction::SwapSlower);
    EXPECT_EQ(d.next.autoSwapInterval, 2);
    EXPECT_TRUE(d.configChanged);
}

// One step at a time is only the fallback. If the measured frame time already
// implies a longer interval within the ceiling, the ladder jumps straight
// there: a 51ms frame on a 60Hz panel needs four refresh periods, not two.
TEST(SwapIntervalLadderTest, SwapSlowerJumpsStraightToTheImpliedInterval) {
    LadderInput in = input(50ms, 0ms, 100);
    in.autoSwapIntervalThreshold = 70ms;

    const LadderDecision d = decideSwapInterval(state(1, PipelineMode::On), in);

    EXPECT_EQ(d.next.autoSwapInterval, 4);
    EXPECT_TRUE(d.configChanged);
}

// Downshifts clamp at the auto-swap ceiling rather than stepping one rung past
// it: on 60Hz, 120Hz, and 30Hz panels with the default 50ms ceiling, slow
// windows do not cross into 66.7ms (15fps) or 58.3ms (17.1fps).
TEST(SwapIntervalLadderTest, SwapSlowerClampsJumpAndStepAtThreshold) {
    // 60Hz: 51ms pipelined implies interval 4 (66.7ms), clamped to 3 (50.0ms).
    const LadderDecision jumped60 =
            decideSwapInterval(state(1, PipelineMode::On), input(50ms, 0ms, 100));
    EXPECT_EQ(jumped60.action, LadderAction::SwapSlower);
    EXPECT_EQ(jumped60.next.autoSwapInterval, 3);
    EXPECT_TRUE(jumped60.configChanged);

    const LadderDecision held60 =
            decideSwapInterval(state(3, PipelineMode::On), input(50ms, 0ms, 100));
    EXPECT_EQ(held60.action, LadderAction::SwapSlower);
    EXPECT_EQ(held60.next.autoSwapInterval, 3);
    EXPECT_FALSE(held60.configChanged);

    // 120Hz: interval 6 is 50.0ms; interval 7 (58.3ms) exceeds the 50ms ceiling.
    LadderInput in120 = input(55ms, 0ms, 100);
    in120.refreshPeriod = 8333333ns;
    EXPECT_EQ(decideSwapInterval(state(1, PipelineMode::On), in120).next.autoSwapInterval, 6);
    const LadderDecision held120 = decideSwapInterval(state(6, PipelineMode::On), in120);
    EXPECT_EQ(held120.next.autoSwapInterval, 6);
    EXPECT_FALSE(held120.configChanged);

    // 30Hz: interval 1 is 33.3ms; interval 2 (66.7ms) exceeds the 50ms ceiling.
    LadderInput in30 = input(55ms, 0ms, 100);
    in30.refreshPeriod = 33333334ns;
    const LadderDecision held30 = decideSwapInterval(state(1, PipelineMode::On), in30);
    EXPECT_EQ(held30.next.autoSwapInterval, 1);
    EXPECT_FALSE(held30.configChanged);

    // 90Hz with a 60fps floor and a 60fps max threshold: rounded floor is
    // interval 2 (22.2ms), and a 30ms hitch stays clamped at interval 2.
    LadderInput in90 = input(30ms, 0ms, 100);
    in90.refreshPeriod = 11111111ns;
    in90.swapDuration = k60Hz;
    in90.autoSwapIntervalThreshold = k60Hz;
    const LadderDecision held90 = decideSwapInterval(state(2, PipelineMode::On), in90);
    EXPECT_EQ(held90.next.autoSwapInterval, 2);
    EXPECT_FALSE(held90.configChanged);
}

TEST(SwapIntervalLadderTest, SwapSlowerClampsCurrentIntervalDownWhenThresholdTightens) {
    LadderInput tightened = input(45ms, 0ms, 50);
    tightened.swapDuration = k60Hz;
    tightened.autoSwapIntervalThreshold = k60Hz * 2;
    const LadderDecision reclamped = decideSwapInterval(state(3, PipelineMode::On), tightened);
    EXPECT_EQ(reclamped.action, LadderAction::SwapSlower);
    EXPECT_EQ(reclamped.next.autoSwapInterval, 2);
    EXPECT_TRUE(reclamped.configChanged);

    // An explicit swapDuration floor above the threshold stays at the floor.
    LadderInput floorAboveThreshold = input(45ms, 0ms, 50);
    floorAboveThreshold.swapDuration = k60Hz * 2;
    floorAboveThreshold.autoSwapIntervalThreshold = 30ms;
    const LadderDecision preservedFloor =
            decideSwapInterval(state(2, PipelineMode::On), floorAboveThreshold);
    EXPECT_EQ(preservedFloor.next.autoSwapInterval, 2);
    EXPECT_FALSE(preservedFloor.configChanged);
}

// autoSwapBounds() owns the floor-versus-ceiling rule that the ladder and the
// pacing implementation clamp through.
TEST(SwapIntervalLadderTest, AutoSwapBoundsLiftCeilingToRoundedFloorAndHoldFloorAboveIt) {
    // Default 50ms ceiling on 120Hz: interval 6 (50.0ms) fits, 7 (58.3ms) does not.
    const AutoSwapBounds at120 = autoSwapBounds(8333333ns, 0ns, 50ms);
    EXPECT_EQ(at120.floor, 1);
    EXPECT_EQ(at120.ceiling, 6);
    EXPECT_TRUE(at120.paced());
    EXPECT_EQ(at120.clamp(7), 6);
    EXPECT_EQ(at120.clamp(4), 4);

    // 60fps floor and ceiling on 90Hz: the floor rounds up to interval 2
    // (22.2ms) and lifts the ceiling with it, so pacing stays active.
    const AutoSwapBounds at90 = autoSwapBounds(11111111ns, k60Hz, k60Hz);
    EXPECT_EQ(at90.floor, 2);
    EXPECT_EQ(at90.ceiling, 2);
    EXPECT_TRUE(at90.paced());

    // A floor above the ceiling holds the floor unpaced; clamp() leaves it.
    const AutoSwapBounds floorAbove = autoSwapBounds(k60Hz, k60Hz * 2, 30ms);
    EXPECT_EQ(floorAbove.floor, 2);
    EXPECT_EQ(floorAbove.ceiling, 1);
    EXPECT_FALSE(floorAbove.paced());
    EXPECT_EQ(floorAbove.clamp(2), 2);

    // A zero threshold disables auto pacing.
    const AutoSwapBounds disabled = autoSwapBounds(k60Hz, k60Hz, 0ns);
    EXPECT_EQ(disabled.ceiling, 0);
    EXPECT_FALSE(disabled.paced());
}

// FRAME_DROP_THRESHOLD is exclusive: exactly 10% missed is tolerated.
// pipelineModeAutoMode is off here so the third branch cannot fire and confuse
// the question being asked.
TEST(SwapIntervalLadderTest, ExactlyAtTheDropThresholdIsTolerated) {
    LadderInput in = input(5ms, 0ms, 10);
    in.pipelineModeAutoMode = false;

    const LadderDecision d = decideSwapInterval(state(1, PipelineMode::On), in);

    EXPECT_EQ(d.action, LadderAction::None);
    EXPECT_EQ(d.next, state(1, PipelineMode::On));
    EXPECT_FALSE(d.configChanged);

    in.missedFramesPercent = 11;
    EXPECT_EQ(decideSwapInterval(state(1, PipelineMode::On), in).next.autoSwapInterval, 2);
}

// The ceiling is `refreshPeriod * targetInterval <= threshold + FRAME_MARGIN`,
// so a threshold one FRAME_MARGIN short of the resulting swap duration still
// permits the step. Tested on the exact boundary because the margin is easy to
// drop.
TEST(SwapIntervalLadderTest, SwapSlowerCeilingIncludesOneFrameMargin) {
    const nanoseconds swapDurationAfterStep = k60Hz * 3;

    LadderInput justAllowed = input(40ms, 0ms, 50);
    justAllowed.autoSwapIntervalThreshold = swapDurationAfterStep - FRAME_MARGIN;
    // 41ms pipelined over a 16.6ms period implies interval 3.
    EXPECT_EQ(decideSwapInterval(state(2, PipelineMode::On), justAllowed).next.autoSwapInterval, 3);

    LadderInput justBlocked = input(40ms, 0ms, 50);
    justBlocked.autoSwapIntervalThreshold = swapDurationAfterStep - FRAME_MARGIN - 1ns;
    const LadderDecision d = decideSwapInterval(state(2, PipelineMode::On), justBlocked);
    EXPECT_EQ(d.next.autoSwapInterval, 2);
    EXPECT_FALSE(d.configChanged);
}

// The slower branch restores pipelining as a side effect, and does not report
// that as a config change. So the caller must apply `next` even when
// configChanged is false -- and the frame-time window is NOT cleared, so the
// samples gathered while unpipelined carry over into the pipelined regime.
//
// This is the one place where `next != current` and `configChanged == false`
// disagree, and it is why LadderDecision keeps them separate.
TEST(SwapIntervalLadderTest, SlowerBranchRestoresPipeliningWithoutReportingAChange) {
    // Interval 2 at 60Hz gives a 33ms budget, which a 6ms frame fits, so the
    // interval cannot lengthen; only the pipelining side effect is left.
    const LadderDecision d = decideSwapInterval(state(2, PipelineMode::Off), input(5ms, 0ms, 50));

    EXPECT_EQ(d.action, LadderAction::SwapSlower);
    EXPECT_EQ(d.next.autoSwapInterval, 2);
    EXPECT_EQ(d.next.pipelineMode, PipelineMode::On);
    EXPECT_FALSE(d.configChanged);
}

TEST(SwapIntervalLadderTest, UnpipelinedAndOverBudgetStillLengthensTheInterval) {
    // 41ms pipelined against a 33.3ms budget at interval 2: the frame does not
    // fit, and the implied interval is 3.
    const LadderDecision d = decideSwapInterval(state(2, PipelineMode::Off), input(40ms, 0ms, 50));

    EXPECT_EQ(d.action, LadderAction::SwapSlower);
    EXPECT_EQ(d.next.autoSwapInterval, 3);
    EXPECT_EQ(d.next.pipelineMode, PipelineMode::On);
    EXPECT_TRUE(d.configChanged);
}

// ---------------------------------------------------------------------------
// Swapping faster
// ---------------------------------------------------------------------------

// With no misses and a frame time well inside the next interval down, the
// ladder descends as far as it can in a single decision rather than one step
// per sample window.
TEST(SwapIntervalLadderTest, HeadroomCollapsesTheIntervalInOneStep) {
    const LadderDecision d = decideSwapInterval(state(3, PipelineMode::On), input(5ms, 0ms, 0));

    EXPECT_EQ(d.action, LadderAction::SwapFaster);
    EXPECT_EQ(d.next.autoSwapInterval, 1);
    EXPECT_EQ(d.next.pipelineMode, PipelineMode::On);
    EXPECT_TRUE(d.configChanged);
}

// The app's requested swap duration is a floor. The comparison is
// `swapDuration <= refreshPeriod * (interval - 1) + DURATION_ROUNDING_MARGIN`,
// non-strict, so a request landing exactly on the boundary still descends.
TEST(SwapIntervalLadderTest, SwapFasterFloorIsInclusiveOfTheRoundingMargin) {
    const nanoseconds boundary = k60Hz + DURATION_ROUNDING_MARGIN;

    LadderInput justAllowed = input(5ms, 0ms, 0);
    justAllowed.swapDuration = boundary;
    EXPECT_EQ(decideSwapInterval(state(2, PipelineMode::On), justAllowed).next.autoSwapInterval, 1);

    LadderInput justBlocked = input(5ms, 0ms, 0);
    justBlocked.swapDuration = boundary + 1ns;
    EXPECT_EQ(decideSwapInterval(state(2, PipelineMode::On), justBlocked).next.autoSwapInterval, 2);
}

// Descending needs the frame time strictly below
// `refreshPeriod * (interval - 1) - FRAME_MARGIN`. Note the minus: the bound is
// a whole frame margin tighter than the interval it is testing, so a frame that
// exactly fills the shorter interval is not considered to fit it.
TEST(SwapIntervalLadderTest, UpshiftBoundIsStrictAndOneFrameMarginTight) {
    const nanoseconds bound = k60Hz * 1 - FRAME_MARGIN;
    // getTime() adds FRAME_MARGIN, so back it out to land on the bound exactly.
    const nanoseconds cpuAtBound = bound - FRAME_MARGIN;

    LadderInput atBound = input(cpuAtBound, 0ms, 0);
    atBound.pipelineModeAutoMode = false;
    const LadderDecision held = decideSwapInterval(state(2, PipelineMode::On), atBound);
    EXPECT_EQ(held.next.autoSwapInterval, 2);
    EXPECT_FALSE(held.configChanged);

    LadderInput belowBound = input(cpuAtBound - 1ns, 0ms, 0);
    belowBound.pipelineModeAutoMode = false;
    EXPECT_EQ(decideSwapInterval(state(2, PipelineMode::On), belowBound).next.autoSwapInterval, 1);
}

// ---------------------------------------------------------------------------
// Turning pipelining off
// ---------------------------------------------------------------------------

// Pipelining is dropped only if the unpipelined frame time, inflated by 50%,
// still fits the current interval.
TEST(SwapIntervalLadderTest, PipeliningIsDroppedOnlyWithFiftyPercentToSpare) {
    // 10ms unpipelined, 15ms inflated, fits the 16.6ms budget.
    const LadderDecision fits = decideSwapInterval(state(1, PipelineMode::On), input(9ms, 0ms, 5));
    EXPECT_EQ(fits.action, LadderAction::PipelineOff);
    EXPECT_EQ(fits.next.pipelineMode, PipelineMode::Off);
    EXPECT_TRUE(fits.configChanged);

    // 12ms unpipelined fits on its own, but 18ms inflated does not.
    const LadderDecision tooTight =
            decideSwapInterval(state(1, PipelineMode::On), input(11ms, 0ms, 5));
    EXPECT_EQ(tooTight.action, LadderAction::None);
    EXPECT_EQ(tooTight.next.pipelineMode, PipelineMode::On);
    EXPECT_FALSE(tooTight.configChanged);
}

// Worth stating on its own: the branches are an if/else chain, and a miss rate
// between 1% and 10% falls past both the slower and faster branches into this
// one. So a game that is dropping frames can have pipelining taken away from
// it, which is not obviously what the threshold was meant to express.
TEST(SwapIntervalLadderTest, PipeliningCanBeDroppedWhileFramesAreStillBeingMissed) {
    for (int missed : {1, 5, 9, 10}) {
        const LadderDecision d =
                decideSwapInterval(state(1, PipelineMode::On), input(9ms, 0ms, missed));
        EXPECT_EQ(d.action, LadderAction::PipelineOff) << "missedFramesPercent = " << missed;
        EXPECT_EQ(d.next.pipelineMode, PipelineMode::Off) << "missedFramesPercent = " << missed;
    }
}

TEST(SwapIntervalLadderTest, PipeliningIsLeftAloneWhenAutoModeIsOff) {
    LadderInput in = input(9ms, 0ms, 5);
    in.pipelineModeAutoMode = false;

    const LadderDecision d = decideSwapInterval(state(1, PipelineMode::On), in);

    EXPECT_EQ(d.action, LadderAction::None);
    EXPECT_EQ(d.next, state(1, PipelineMode::On));
    EXPECT_FALSE(d.configChanged);
}

// Nothing here ever turns pipelining back on: only the slower branch does that.
TEST(SwapIntervalLadderTest, AlreadyUnpipelinedStaysUnpipelined) {
    const LadderDecision d = decideSwapInterval(state(1, PipelineMode::Off), input(9ms, 0ms, 5));

    EXPECT_EQ(d.action, LadderAction::None);
    EXPECT_EQ(d.next, state(1, PipelineMode::Off));
    EXPECT_FALSE(d.configChanged);
}

// ---------------------------------------------------------------------------
// The refresh period hint
// ---------------------------------------------------------------------------

// The hint is the pipelined frame time on every branch, including the one where
// pipelining has just been turned off and the unpipelined time is what the
// frame will actually cost.
TEST(SwapIntervalLadderTest, HintIsThePipelinedFrameTimeWhateverTheBranch) {
    // Pipelined 10ms, unpipelined 14ms, so the two are distinguishable.
    const nanoseconds pipelined = 9ms + FRAME_MARGIN;

    EXPECT_EQ(decideSwapInterval(state(1, PipelineMode::On), input(9ms, 4ms, 50))
                      .preferredRefreshPeriodHint,
              pipelined);
    EXPECT_EQ(decideSwapInterval(state(3, PipelineMode::On), input(9ms, 4ms, 0))
                      .preferredRefreshPeriodHint,
              pipelined);
    EXPECT_EQ(decideSwapInterval(state(2, PipelineMode::On), input(9ms, 4ms, 5))
                      .preferredRefreshPeriodHint,
              pipelined);
    EXPECT_EQ(decideSwapInterval(state(1, PipelineMode::Off), input(9ms, 4ms, 5))
                      .preferredRefreshPeriodHint,
              pipelined);
}

// ---------------------------------------------------------------------------
// calculateSwapInterval
// ---------------------------------------------------------------------------

TEST(SwapIntervalLadderTest, CalculateSwapIntervalRoundsUpBeyondTheRefreshRateMargin) {
    EXPECT_EQ(calculateSwapInterval(k60Hz - 1ns, k60Hz), 1);
    EXPECT_EQ(calculateSwapInterval(k60Hz, k60Hz), 1);
    EXPECT_EQ(calculateSwapInterval(k60Hz * 2, k60Hz), 2);

    // The margin is exclusive: a remainder of exactly REFRESH_RATE_MARGIN is
    // absorbed, one nanosecond more is not.
    EXPECT_EQ(calculateSwapInterval(k60Hz + REFRESH_RATE_MARGIN, k60Hz), 1);
    EXPECT_EQ(calculateSwapInterval(k60Hz + REFRESH_RATE_MARGIN + 1ns, k60Hz), 2);
}

// A frame time shorter than a single refresh period short-circuits to 1 rather
// than falling through the division. Note what this does not cover: a zero
// refresh period fails the short-circuit too (0 < 0 is false) and divides by
// zero, so the caller still owns that precondition.
TEST(SwapIntervalLadderTest, CalculateSwapIntervalShortCircuitsBelowOneRefreshPeriod) {
    EXPECT_EQ(calculateSwapInterval(0ns, k60Hz), 1);
    EXPECT_EQ(calculateSwapInterval(1ns, k60Hz), 1);
}

// ---------------------------------------------------------------------------
// Purity
// ---------------------------------------------------------------------------

// The whole reason for extracting this: the same inputs give the same answer,
// with no clock, no lock and no hidden state between calls.
TEST(SwapIntervalLadderTest, DecideIsAFunctionOfItsArguments) {
    const LadderState current = state(2, PipelineMode::Off);
    const LadderInput in = input(9ms, 4ms, 50);

    const LadderDecision first = decideSwapInterval(current, in);
    for (int i = 0; i < 5; ++i) {
        const LadderDecision again = decideSwapInterval(current, in);
        EXPECT_EQ(again.next, first.next);
        EXPECT_EQ(again.configChanged, first.configChanged);
        EXPECT_EQ(again.action, first.action);
        EXPECT_EQ(again.preferredRefreshPeriodHint, first.preferredRefreshPeriodHint);
    }
}

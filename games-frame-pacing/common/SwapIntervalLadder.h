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

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>

#include "FrameDurations.h"

// Swappy's swap interval ladder: given an averaged frame time and a miss rate,
// decide whether to swap slower, swap faster, or drop out of pipeline mode.
//
// This is the logic that decides a game is a 30 fps game and keeps it there, so
// it is the thing the lock-in tests need to drive. It used to live inside
// SwappyCommon::updateSwapInterval() and its two helpers, reading and writing
// member state directly, which meant it could only be exercised by constructing
// a SwappyCommon -- and that pulls in jni.h, Choreographer and the display
// manager. Nothing here needs any of that: it is a function of the inputs
// below.
//
// The logic is moved unchanged. decideSwapInterval() is a pure function; the caller owns
// the state and applies the result. The only deliberate difference is that the
// SWAPPY_LOGV calls that were interleaved with the decision have moved out to
// the caller, reconstructed from LadderDecision::action -- keeping this header
// free of the Android logging headers is what lets a host test include it.

namespace swappy {

// Added when comparing a duration against a refresh-period multiple, to absorb
// the sub-microsecond error in a measured period. Distinct from FRAME_MARGIN:
// this one is about arithmetic noise, FRAME_MARGIN is a safety budget.
inline constexpr std::chrono::nanoseconds DURATION_ROUNDING_MARGIN = std::chrono::microseconds(1);

// Pipelining is turned off only if the frame fits with this much room to spare,
// as a percentage. Turning it off is hard to undo cheaply, so the test is
// deliberately conservative.
inline constexpr int NON_PIPELINE_PERCENT = 50; // 50%

// Above this percentage of missed frames in the sample window, the current swap
// interval is considered not to fit.
inline constexpr int FRAME_DROP_THRESHOLD = 10; // 10%

// Slack allowed when deciding whether a frame time divides evenly into a
// refresh period.
inline constexpr std::chrono::nanoseconds REFRESH_RATE_MARGIN = std::chrono::nanoseconds(500);

// Default auto-swap ceiling (20 fps), used until the app sets a max auto swap
// interval.
inline constexpr std::chrono::nanoseconds DEFAULT_AUTO_SWAP_INTERVAL_THRESHOLD =
        std::chrono::milliseconds(50);

// The part of Swappy's state that the ladder owns.
struct LadderState {
    int32_t autoSwapInterval = 1;
    PipelineMode pipelineMode = PipelineMode::On;

    bool operator==(const LadderState& other) const {
        return autoSwapInterval == other.autoSwapInterval && pipelineMode == other.pipelineMode;
    }
    bool operator!=(const LadderState& other) const {
        return !(*this == other);
    }
};

// Everything else the decision reads. Built by makeLadderInput() below.
struct LadderInput {
    // Mean over the sample window, not a single frame.
    FrameDuration averageFrameTime;
    int missedFramesPercent = 0;
    std::chrono::nanoseconds refreshPeriod = std::chrono::nanoseconds(0);
    // The floor the app asked for via Swappy_setSwapIntervalNS; the ladder will
    // not swap faster than this.
    std::chrono::nanoseconds swapDuration = std::chrono::nanoseconds(0);
    // The ceiling, i.e. the slowest swap the ladder may descend to.
    std::chrono::nanoseconds autoSwapIntervalThreshold = DEFAULT_AUTO_SWAP_INTERVAL_THRESHOLD;
    bool pipelineModeAutoMode = true;
};

// Which branch of the ladder was taken. Carried out so the caller can log and
// trace what happened without this header knowing how it logs. Note that a
// branch can be taken without configChanged being set -- see LadderDecision.
enum class LadderAction {
    None,
    SwapSlower,
    SwapFaster,
    PipelineOff,
};

struct LadderDecision {
    // The state to adopt. Always valid: equals the input state when nothing
    // changed.
    LadderState next;

    // Whether the frame-time history should now be discarded and the caller's
    // swap configuration re-applied.
    //
    // This is NOT the same as `next != current`. The slower branch can turn
    // pipelining back on without changing the swap interval, and the original
    // code reports that as no change -- so the sample window survives. Callers
    // must apply `next` unconditionally and use this flag only for the
    // clear-and-notify step.
    bool configChanged = false;

    LadderAction action = LadderAction::None;

    // What to pass to the platform as the preferred refresh period. Always the
    // pipelined frame time, whatever branch was taken.
    std::chrono::nanoseconds preferredRefreshPeriodHint = std::chrono::nanoseconds(0);

    std::chrono::nanoseconds upperBound = std::chrono::nanoseconds(0);
    std::chrono::nanoseconds lowerBound = std::chrono::nanoseconds(0);
};

// How many refresh periods a frame of this length needs.
inline int calculateSwapInterval(std::chrono::nanoseconds frameTime,
                                 std::chrono::nanoseconds refreshPeriod) {
    if (frameTime < refreshPeriod) {
        return 1;
    }

    auto div_result = std::div(frameTime.count(), refreshPeriod.count());
    auto framesPerRefresh = div_result.quot;
    auto framesPerRefreshRemainder = div_result.rem;

    return (framesPerRefresh + (framesPerRefreshRemainder > REFRESH_RATE_MARGIN.count() ? 1 : 0));
}

// The auto-swap interval range on a panel. `floor` is the interval the app's
// swap-duration floor needs, and `ceiling` is the longest interval within
// `threshold + FRAME_MARGIN`, or `0` when auto pacing is disabled
// (`threshold <= 0`). Owns the floor-versus-ceiling rule, so callers clamp
// through it instead of comparing the two themselves.
struct AutoSwapBounds {
    int32_t floor = 1;
    int32_t ceiling = 0;

    // False when auto pacing is disabled or the app floor exceeds the ceiling;
    // auto mode then holds the floor unpaced.
    [[nodiscard]] bool paced() const {
        return ceiling >= floor;
    }

    // Caps `interval` at the ceiling while paced; otherwise returns it as is.
    [[nodiscard]] int32_t clamp(int32_t interval) const {
        return paced() ? std::min(interval, ceiling) : interval;
    }
};

// When `swapDuration` is within `threshold + FRAME_MARGIN`, rounding that floor
// up to whole refresh periods (for example, 60fps -> 2 * 11.1ms on 90Hz) lifts
// the ceiling to the floor, which keeps pacing active instead of crossing the
// ceiling.
[[nodiscard]] inline AutoSwapBounds autoSwapBounds(std::chrono::nanoseconds refreshPeriod,
                                                   std::chrono::nanoseconds swapDuration,
                                                   std::chrono::nanoseconds threshold) {
    AutoSwapBounds bounds;
    if (refreshPeriod <= std::chrono::nanoseconds(0)) {
        return bounds;
    }
    if (swapDuration > std::chrono::nanoseconds(0)) {
        bounds.floor = calculateSwapInterval(swapDuration, refreshPeriod);
    }
    if (threshold <= std::chrono::nanoseconds(0)) {
        return bounds;
    }
    const auto maxBudget = (threshold > std::chrono::nanoseconds::max() - FRAME_MARGIN)
            ? std::chrono::nanoseconds::max()
            : threshold + FRAME_MARGIN;
    const int64_t rawIntervals = maxBudget / refreshPeriod;
    bounds.ceiling = (rawIntervals > INT32_MAX) ? INT32_MAX : static_cast<int32_t>(rawIntervals);
    if (swapDuration > std::chrono::nanoseconds(0) && swapDuration <= maxBudget) {
        bounds.ceiling = std::max(bounds.ceiling, bounds.floor);
    }
    return bounds;
}

namespace ladder_internal {

// Is there room to move one step up the ladder without undercutting the swap
// duration the app asked for? Re-evaluated on each step of the swapFaster loop,
// because `interval` changes underneath it.
inline bool swapFasterCondition(const LadderInput& in, int32_t interval) {
    return in.swapDuration <= in.refreshPeriod * (interval - 1) + DURATION_ROUNDING_MARGIN;
}

// Returns true if the swap interval actually changed. Mutates `state`.
inline bool swapSlower(LadderState& state, const LadderInput& in, int newSwapInterval) {
    bool swappedSlower = false;

    const auto upperBound = in.refreshPeriod * state.autoSwapInterval;
    const auto frameFitsUpperBound = in.averageFrameTime.getTime(PipelineMode::On) <= upperBound;
    const AutoSwapBounds bounds =
            autoSwapBounds(in.refreshPeriod, in.swapDuration, in.autoSwapIntervalThreshold);

    // Check if turning on pipeline is not enough. Downshifts clamp at the
    // ceiling (re-clamping an interval above it) and hold while unpaced.
    if ((state.pipelineMode == PipelineMode::On || !frameFitsUpperBound) && bounds.paced()) {
        const int32_t targetInterval =
                bounds.clamp(std::max<int32_t>(newSwapInterval, state.autoSwapInterval + 1));
        swappedSlower = targetInterval != state.autoSwapInterval;
        state.autoSwapInterval = targetInterval;
    }

    // Deliberately outside the branch above, and deliberately not reported as a
    // change: pipelining is restored even when the interval could not move.
    if (state.pipelineMode == PipelineMode::Off) {
        state.pipelineMode = PipelineMode::On;
    }

    return swappedSlower;
}

// Returns true if the swap interval actually changed. Mutates `state`.
inline bool swapFaster(LadderState& state, const LadderInput& in, int newSwapInterval) {
    bool swappedFaster = false;
    int32_t originalAutoSwapInterval = state.autoSwapInterval;
    while (newSwapInterval < state.autoSwapInterval &&
           swapFasterCondition(in, state.autoSwapInterval)) {
        state.autoSwapInterval--;
    }

    if (state.autoSwapInterval != originalAutoSwapInterval) {
        // Since we changed the swap interval, we may need to turn on pipeline
        // mode
        state.pipelineMode = PipelineMode::On;
        swappedFaster = true;
    }

    return swappedFaster;
}

} // namespace ladder_internal

// The ladder. Pure: same inputs, same decision, no clock and no I/O.
inline LadderDecision decideSwapInterval(const LadderState& current, const LadderInput& in) {
    LadderDecision decision;
    decision.next = current;

    const auto pipelineFrameTime = in.averageFrameTime.getTime(PipelineMode::On);
    const auto nonPipelineFrameTime = in.averageFrameTime.getTime(PipelineMode::Off);
    decision.preferredRefreshPeriodHint = pipelineFrameTime;

    // Calculate the new swap interval based on average frame time, assuming we
    // are in pipeline mode (prefer higher swap interval rather than turning off
    // pipeline mode)
    const int newSwapInterval = calculateSwapInterval(pipelineFrameTime, in.refreshPeriod);

    // Define upper and lower bounds based on the swap duration
    const std::chrono::nanoseconds upperBoundForThisRefresh =
            in.refreshPeriod * current.autoSwapInterval;
    const std::chrono::nanoseconds lowerBoundForThisRefresh =
            in.refreshPeriod * (current.autoSwapInterval - 1) - FRAME_MARGIN;

    decision.upperBound = upperBoundForThisRefresh;
    decision.lowerBound = lowerBoundForThisRefresh;

    const auto nonPipelinePercent = (100.f + NON_PIPELINE_PERCENT) / 100.f;

    // Make sure the frame time fits in the current config to avoid missing
    // frames
    if (in.missedFramesPercent > FRAME_DROP_THRESHOLD) {
        decision.action = LadderAction::SwapSlower;
        if (ladder_internal::swapSlower(decision.next, in, newSwapInterval)) {
            decision.configChanged = true;
        }
    }

    // So we shouldn't miss any frames with this config but maybe we can go
    // faster ? we check the pipeline frame time here as we prefer lower swap
    // interval than no pipelining
    else if (in.missedFramesPercent == 0 &&
             ladder_internal::swapFasterCondition(in, current.autoSwapInterval) &&
             pipelineFrameTime < lowerBoundForThisRefresh) {
        decision.action = LadderAction::SwapFaster;
        if (ladder_internal::swapFaster(decision.next, in, newSwapInterval)) {
            decision.configChanged = true;
        }
    }

    // If we reached to this condition it means that we fit into the boundaries.
    // However we might be in pipeline mode and we could turn it off if we still
    // fit. To be very conservative, switch to non-pipeline if frame time * 50%
    // fits
    else if (in.pipelineModeAutoMode && current.pipelineMode == PipelineMode::On &&
             nonPipelineFrameTime * nonPipelinePercent < upperBoundForThisRefresh) {
        decision.action = LadderAction::PipelineOff;
        decision.next.pipelineMode = PipelineMode::Off;
        decision.configChanged = true;
    }

    return decision;
}

// Assembles the decision's inputs from the frame-time window, or returns
// nullopt if the window is too short to decide on.
//
// This exists to make one ordering bug unrepresentable. Before the extraction
// the caller had to test hasEnoughSamples() itself, and two of the values below
// are actively unsafe without it: getAverageFrameTime() returns a zero duration
// (which reads as "everything fits", i.e. upshift) and getMissedFramePercent()
// divides by an empty window.
inline std::optional<LadderInput> makeLadderInput(
        const FrameDurations& durations, std::chrono::nanoseconds refreshPeriod,
        std::chrono::nanoseconds swapDuration, std::chrono::nanoseconds autoSwapIntervalThreshold,
        bool pipelineModeAutoMode) {
    if (!durations.hasEnoughSamples()) {
        return std::nullopt;
    }

    LadderInput in;
    in.averageFrameTime = durations.getAverageFrameTime();
    in.missedFramesPercent = durations.getMissedFramePercent();
    in.refreshPeriod = refreshPeriod;
    in.swapDuration = swapDuration;
    in.autoSwapIntervalThreshold = autoSwapIntervalThreshold;
    in.pipelineModeAutoMode = pipelineModeAutoMode;
    return in;
}

} // namespace swappy

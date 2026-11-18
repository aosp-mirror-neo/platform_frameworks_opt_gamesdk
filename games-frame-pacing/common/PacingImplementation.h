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

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include "DisplayRateController.h"
#include "FrameDurations.h"
#include "SwapIntervalLadder.h"

namespace swappy {

struct RefreshRateVote {
    // Frame rate (in Hz) for ANativeWindow_setFrameRate when outside the deadband.
    std::optional<float> frameRateHz;
    // Display.Mode ID for WindowManager preferredDisplayModeId when the mode changes.
    std::optional<int> displayModeId;
    // Value recorded on the "preferredRefreshPeriod" trace counter when a vote
    // path is active (rounded Hz for setFrameRate, period in ns for Display.Mode).
    std::optional<int64_t> traceValue;

    [[nodiscard]] bool emitted() const noexcept {
        return frameRateHz.has_value() || displayModeId.has_value();
    }
};

struct TimingChange {
    std::chrono::nanoseconds oldRefreshPeriod{0};
    std::chrono::nanoseconds newRefreshPeriod{0};
    std::chrono::nanoseconds swapDuration{0};
    bool swapDurationChanged = false;
    bool autoSwapIntervalEnabled = true;
};

struct PacingResult {
    int32_t autoSwapInterval = 1;
    PipelineMode pipelineMode = PipelineMode::On;
    bool configChanged = false;
    RefreshRateVote vote;
};

class PacingImplementation {
public:
    virtual ~PacingImplementation() = default;

    // Factory method: returns the active pacing strategy instance.
    static std::unique_ptr<PacingImplementation> create();

    // Resets per-window frame-rate vote state when a new ANativeWindow is bound.
    virtual void onWindowChanged() = 0;

    // Evaluates a direct refresh-period vote (such as on frame-pacing reset).
    [[nodiscard]] virtual RefreshRateVote voteRefreshPeriod(
            std::chrono::nanoseconds targetPeriod, std::chrono::nanoseconds minSwapDuration,
            const VotableRefreshPeriods& votableRefreshPeriods = {}) = 0;

    // votableRefreshPeriods.periods must stay valid for the duration of the call.
    virtual PacingResult onDisplayTimingsChanged(
            int32_t currentSwapInterval, PipelineMode currentPipelineMode,
            FrameDurations& durations, const TimingChange& change,
            const VotableRefreshPeriods& votableRefreshPeriods = {}) = 0;

    // votableRefreshPeriods.periods must stay valid for the duration of the call.
    virtual PacingResult updateSwapInterval(
            int32_t currentSwapInterval, PipelineMode currentPipelineMode,
            FrameDurations& durations, std::chrono::nanoseconds refreshPeriod,
            std::chrono::nanoseconds swapDuration,
            std::chrono::nanoseconds autoSwapIntervalThreshold, bool pipelineModeAutoMode,
            const VotableRefreshPeriods& votableRefreshPeriods = {}) = 0;
};

} // namespace swappy

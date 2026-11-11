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
#include <cmath>
#include <optional>
#include <utility>

#include "FrameDurations.h"
#include "RefreshPeriodSet.h"
#include "SwapIntervalLadder.h"

namespace swappy {

// Tracks platform display-rate votes across ANativeWindow_setFrameRate (API 30+)
// and legacy WindowManager preferredDisplayModeId (API 28-29).
class DisplayRateController {
public:
    // 0.1 Hz absorbs 1 ns period rounding while still voting sub-20 Hz ARR
    // transitions (e.g. 16 vs 15 Hz and 11 vs 10 Hz).
    static constexpr float kVoteMarginHz = 0.1f;

    struct ModeSelection {
        std::chrono::nanoseconds refreshPeriod{0};
        int modeId = -1;
        bool changed = false;
    };

    [[nodiscard]] bool hasVoted() const {
        return mLastVotedFrameRateHz > 0.f || mLastModeId >= 0;
    }

    void onWindowChanged() {
        mLastVotedFrameRateHz = 0.f;
    }

    // Returns a new frameRate (in Hz) to pass to ANativeWindow_setFrameRate when
    // the clamped target period moves outside the 0.1 Hz deadband.
    [[nodiscard]] std::optional<float> voteFrameRateHz(std::chrono::nanoseconds targetPeriod,
                                                       std::chrono::nanoseconds minSwapDuration) {
        const auto effectivePeriod = std::max(targetPeriod, minSwapDuration);
        if (effectivePeriod <= std::chrono::nanoseconds::zero()) {
            return std::nullopt;
        }
        const float frameRate = 1e9f / static_cast<float>(effectivePeriod.count());
        if (mLastVotedFrameRateHz > 0.f &&
            std::abs(mLastVotedFrameRateHz - frameRate) <= kVoteMarginHz) {
            return std::nullopt;
        }
        mLastVotedFrameRateHz = frameRate;
        return frameRate;
    }

    [[nodiscard]] float lastVotedFrameRateHz() const {
        return mLastVotedFrameRateHz;
    }

    // Selects the longest physical Display.Mode period whose integer multiple
    // accommodates targetPeriod without swapping faster than minSwapDuration.
    [[nodiscard]] ModeSelection selectDisplayMode(const RefreshPeriodMap* supportedPeriods,
                                                  std::chrono::nanoseconds targetPeriod,
                                                  std::chrono::nanoseconds minSwapDuration) {
        using namespace std::chrono_literals;
        ModeSelection result;
        if (supportedPeriods == nullptr) {
            return result;
        }

        std::pair<std::chrono::nanoseconds, int> bestConfig{0ns, -1};
        std::chrono::nanoseconds minCandidateSwapDuration = 1s;
        for (const auto& [period, modeId] : *supportedPeriods) {
            if (modeId < 0 || period <= 0ns) {
                continue;
            }
            const int interval = calculateSwapInterval(targetPeriod, period);
            const auto candidateSwapDuration = period * interval;
            if (candidateSwapDuration + FRAME_MARGIN < minSwapDuration) {
                continue;
            }
            if (candidateSwapDuration < minCandidateSwapDuration + FRAME_MARGIN) {
                minCandidateSwapDuration = candidateSwapDuration;
                bestConfig = {period, modeId};
            }
        }

        result.refreshPeriod = bestConfig.first;
        result.modeId = bestConfig.second;
        if (bestConfig.second >= 0 && bestConfig.second != mLastModeId) {
            mLastModeId = bestConfig.second;
            result.changed = true;
        }
        return result;
    }

private:
    float mLastVotedFrameRateHz = 0.f;
    int mLastModeId = -1;
};

} // namespace swappy

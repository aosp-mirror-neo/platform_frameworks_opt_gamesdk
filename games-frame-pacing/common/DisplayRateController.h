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
    static constexpr float kVoteMarginHz = 1.f; // 1Hz

    struct ModeSelection {
        std::chrono::nanoseconds refreshPeriod{0};
        int modeId = -1;
        bool changed = false;
    };

    [[nodiscard]] bool hasVoted() const {
        return mLastVotedFrameRateHz != 0.f || mLastModeId >= 0;
    }

    void onWindowChanged() {
        mLastVotedFrameRateHz = 0.f;
    }

    [[nodiscard]] std::optional<float> voteFrameRateHz(std::chrono::nanoseconds targetPeriod,
                                                       std::chrono::nanoseconds minSwapDuration) {
        auto frameRate = 1e9f / targetPeriod.count();
        frameRate = std::min(frameRate, 1e9f / minSwapDuration.count());
        if (std::abs(mLastVotedFrameRateHz - frameRate) > kVoteMarginHz) {
            mLastVotedFrameRateHz = frameRate;
            return frameRate;
        }
        return std::nullopt;
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
            if (modeId < 0) {
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

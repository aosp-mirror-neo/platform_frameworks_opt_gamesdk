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
#include <utility>

#include "FrameDurations.h"
#include "RefreshPeriodSet.h"
#include "SwapIntervalLadder.h"

namespace swappy {

// Converts a positive period to rounded integer millihertz, or returns 0 for
// non-positive periods.
[[nodiscard]] constexpr int64_t periodToMillihertz(std::chrono::nanoseconds period) {
    if (period <= std::chrono::nanoseconds::zero()) {
        return 0;
    }
    constexpr int64_t kMillihertzPerSecondNs = 1'000'000'000'000LL;
    return (kMillihertzPerSecondNs + period.count() / 2) / period.count();
}

// Tracks platform display-rate votes across ANativeWindow_setFrameRate (API 30+)
// and legacy WindowManager preferredDisplayModeId (API 28-29).
class DisplayRateController {
public:
    // A new frameRate vote for a measured frame time must differ from the last
    // vote by more than voteMarginMillihertz(): max(0.1 Hz, 2%). Swappy votes
    // its 2 s rolling-mean frame time on every frame, and frame jitter moves that
    // mean by a few tenths of a hertz, so a fixed 0.1 Hz deadband re-voted
    // several times a second while the game held a steady rate. 2% still lets
    // 1 Hz ARR steps below 50 Hz through (e.g. 16 vs 15 Hz). The 1 ms
    // FRAME_MARGIN in a measured frame time absorbs a 2% slowdown only for
    // periods up to ~49 ms, so a held vote is also dropped once its period no
    // longer covers the frame time without FRAME_MARGIN.
    //
    // Votes clamped to minSwapDuration keep the 0.1 Hz margin, which only
    // absorbs 1 ns period rounding. The app floor is an exact target: a TE
    // divisor panel serves a 59 Hz vote at 48 Hz, so recovery must reach the
    // 60 Hz floor exactly.
    static constexpr int64_t kVoteMarginMillihertz = 100;
    static constexpr int64_t kVoteMarginPerMille = 20;

    [[nodiscard]] static constexpr int64_t voteMarginMillihertz(int64_t lastVotedMillihertz) {
        return std::max(kVoteMarginMillihertz, lastVotedMillihertz * kVoteMarginPerMille / 1000);
    }

    struct ModeSelection {
        std::chrono::nanoseconds refreshPeriod{0};
        int modeId = -1;
        bool changed = false;
    };

    [[nodiscard]] bool hasVoted() const {
        return mLastVotedMillihertz > 0 || mLastModeId >= 0;
    }

    void onWindowChanged() {
        mLastVotedMillihertz = 0;
    }

    // Returns a new frameRate (in Hz) to pass to ANativeWindow_setFrameRate when
    // the clamped target period moves outside the vote margin of the last vote
    // (see voteMarginMillihertz()).
    [[nodiscard]] std::optional<float> voteFrameRateHz(std::chrono::nanoseconds targetPeriod,
                                                       std::chrono::nanoseconds minSwapDuration) {
        const auto effectivePeriod = std::max(targetPeriod, minSwapDuration);
        const int64_t millihertz = periodToMillihertz(effectivePeriod);
        if (millihertz <= 0) {
            return std::nullopt;
        }
        const bool clampedToFloor = targetPeriod <= minSwapDuration;
        const int64_t margin =
                clampedToFloor ? kVoteMarginMillihertz : voteMarginMillihertz(mLastVotedMillihertz);
        // A held vote must also still cover the frame time without FRAME_MARGIN, which
        // the 2% margin stops guaranteeing above ~49 ms.
        const bool heldVoteCoversFrame = clampedToFloor ||
                mLastVotedMillihertz <= periodToMillihertz(targetPeriod - FRAME_MARGIN);
        if (mLastVotedMillihertz > 0 && std::abs(millihertz - mLastVotedMillihertz) <= margin &&
            heldVoteCoversFrame) {
            return std::nullopt;
        }
        mLastVotedMillihertz = millihertz;
        return static_cast<float>(millihertz) / 1000.0f;
    }

    [[nodiscard]] int64_t lastVotedMillihertz() const {
        return mLastVotedMillihertz;
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
    int64_t mLastVotedMillihertz = 0;
    int mLastModeId = -1;
};

// Platform refresh-rate voting path and currently reported refresh periods.
struct VotableRefreshPeriods {
    // Non-owning pointer; null until the platform reports refresh periods.
    const RefreshPeriodMap* periods = nullptr;
    // True when ANativeWindow_setFrameRate is active on a bound window; false
    // selects physical Display.Modes via preferredDisplayModeId.
    bool usesSetFrameRate = false;
};

} // namespace swappy

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

#if defined(__ANDROID__)
#ifndef LOG_TAG
#define LOG_TAG "SwappyCommon"
#endif
#include "SwappyLog.h"
#else
#ifndef SWAPPY_LOGV
#define SWAPPY_LOGV(...)
#endif
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

#include "PacingImplementation.h"

namespace swappy {

class ClassicPacingImplementation final : public PacingImplementation {
public:
    void onWindowChanged() override {
        mDisplayRateController.onWindowChanged();
    }

    RefreshRateVote voteRefreshPeriod(
            std::chrono::nanoseconds targetPeriod, std::chrono::nanoseconds minSwapDuration,
            const VotableRefreshPeriods& votableRefreshPeriods = {}) override {
        if (votableRefreshPeriods.usesSetFrameRate) {
            const auto votedHz =
                    mDisplayRateController.voteFrameRateHz(targetPeriod, minSwapDuration);
            const int64_t traceHz = (mDisplayRateController.lastVotedMillihertz() + 500) / 1000;
            return RefreshRateVote{
                    .frameRateHz = votedHz,
                    .traceValue = traceHz,
            };
        }
        if (votableRefreshPeriods.periods == nullptr) {
            return RefreshRateVote{};
        }
        const auto selection =
                mDisplayRateController.selectDisplayMode(votableRefreshPeriods.periods,
                                                         targetPeriod, minSwapDuration);
        return RefreshRateVote{
                .displayModeId =
                        selection.changed ? std::optional<int>(selection.modeId) : std::nullopt,
                .traceValue = selection.refreshPeriod.count(),
        };
    }

    PacingResult onFramePacingReset(
            FrameDurations& durations, std::chrono::nanoseconds refreshPeriod,
            std::chrono::nanoseconds minSwapDuration,
            std::chrono::nanoseconds fallbackRefreshPeriod,
            const VotableRefreshPeriods& votableRefreshPeriods = {}) override {
        using namespace std::chrono_literals;
        durations.clear();
        std::chrono::nanoseconds resetTarget = minSwapDuration;
        if (resetTarget <= 0ns) {
            resetTarget = std::min(fallbackRefreshPeriod, refreshPeriod);
            if (votableRefreshPeriods.periods != nullptr) {
                const auto fastestIt = votableRefreshPeriods.periods->upper_bound(0ns);
                if (fastestIt != votableRefreshPeriods.periods->end()) {
                    resetTarget = fastestIt->first;
                }
            }
        }
        return PacingResult{
                .autoSwapInterval = calculateSwapInterval(minSwapDuration, refreshPeriod),
                .pipelineMode = PipelineMode::On,
                .configChanged = true,
                .vote = voteRefreshPeriod(resetTarget, minSwapDuration, votableRefreshPeriods),
        };
    }

    PacingResult updateSwapInterval(
            int32_t currentSwapInterval, PipelineMode currentPipelineMode,
            FrameDurations& durations, std::chrono::nanoseconds refreshPeriod,
            std::chrono::nanoseconds swapDuration,
            std::chrono::nanoseconds autoSwapIntervalThreshold, bool pipelineModeAutoMode,
            const VotableRefreshPeriods& votableRefreshPeriods = {}) override {
        using namespace std::chrono_literals;

        const auto input = makeLadderInput(durations, refreshPeriod, swapDuration,
                                           autoSwapIntervalThreshold, pipelineModeAutoMode);
        if (!input) {
            return PacingResult{
                    .autoSwapInterval = currentSwapInterval,
                    .pipelineMode = currentPipelineMode,
                    .configChanged = false,
            };
        }

        const LadderState current = {currentSwapInterval, currentPipelineMode};
        const LadderDecision decision = decideSwapInterval(current, *input);

        SWAPPY_LOGV("mPipelineMode = %d", static_cast<int>(currentPipelineMode));
        SWAPPY_LOGV("Average cpu frame time = %.2f",
                    (input->averageFrameTime.getCpuTime().count()) / 1e6f);
        SWAPPY_LOGV("Average gpu frame time = %.2f",
                    (input->averageFrameTime.getGpuTime().count()) / 1e6f);
        SWAPPY_LOGV("upperBound = %.2f", decision.upperBound.count() / 1e6f);
        SWAPPY_LOGV("lowerBound = %.2f", decision.lowerBound.count() / 1e6f);
        SWAPPY_LOGV("frame missed = %d%%", input->missedFramesPercent);
        SWAPPY_LOGV("pipelineFrameTime = %.2f", decision.preferredRefreshPeriodHint.count() / 1e6f);

        switch (decision.action) {
            case LadderAction::SwapSlower:
                SWAPPY_LOGV("Rendering takes too much time for the given config");
                break;
            case LadderAction::SwapFaster:
                SWAPPY_LOGV("Rendering is much shorter for the given config");
                break;
            case LadderAction::PipelineOff:
                SWAPPY_LOGV("Rendering time fits the current swap interval without pipelining");
                break;
            case LadderAction::None:
                break;
        }

        if (decision.next.autoSwapInterval != currentSwapInterval) {
            SWAPPY_LOGV("Changing Swap interval to %d from %d", decision.next.autoSwapInterval,
                        currentSwapInterval);
        }
        if (decision.next.pipelineMode != currentPipelineMode) {
            SWAPPY_LOGV("Turning %s pipelining",
                        decision.next.pipelineMode == PipelineMode::On ? "on" : "off");
        }

        const AutoSwapBounds bounds =
                autoSwapBounds(refreshPeriod, swapDuration, autoSwapIntervalThreshold);
        const RefreshRateVote vote =
                voteClampedRefreshPeriod(decision.next.autoSwapInterval, bounds,
                                         decision.preferredRefreshPeriodHint, swapDuration,
                                         autoSwapIntervalThreshold, votableRefreshPeriods);

        if (decision.configChanged) {
            durations.clear();
        }

        return PacingResult{
                .autoSwapInterval = decision.next.autoSwapInterval,
                .pipelineMode = decision.next.pipelineMode,
                .configChanged = decision.configChanged,
                .vote = vote,
        };
    }

    PacingResult onDisplayTimingsChanged(
            int32_t currentSwapInterval, PipelineMode currentPipelineMode,
            FrameDurations& durations, const TimingChange& change,
            const VotableRefreshPeriods& votableRefreshPeriods = {}) override {
        using namespace std::chrono_literals;
        if (!change.autoSwapIntervalEnabled) {
            durations.clear();
            const int32_t autoSwapInterval =
                    calculateSwapInterval(change.swapDuration, change.newRefreshPeriod);
            return PacingResult{
                    .autoSwapInterval = autoSwapInterval,
                    .pipelineMode = PipelineMode::On,
                    .configChanged = false,
                    .vote = voteRefreshPeriod(change.swapDuration, change.swapDuration,
                                              votableRefreshPeriods),
            };
        }

        if (change.swapDurationChanged) {
            durations.clear();
        }

        const auto oldPacedPeriod = change.oldRefreshPeriod * currentSwapInterval;
        const auto pipelineFrameTime = durations.getAverageFrameTime().getTime(PipelineMode::On);
        const auto targetDuration =
                pipelineFrameTime != 0ns ? pipelineFrameTime : change.swapDuration;
        int32_t autoSwapInterval = calculateSwapInterval(targetDuration, change.newRefreshPeriod);

        const bool swapIntervalValid =
                change.newRefreshPeriod * autoSwapInterval + DURATION_ROUNDING_MARGIN >=
                change.swapDuration;
        const AutoSwapBounds bounds = autoSwapBounds(change.newRefreshPeriod, change.swapDuration,
                                                     change.autoSwapIntervalThreshold);
        autoSwapInterval = bounds.clamp(swapIntervalValid ? autoSwapInterval : bounds.floor);

        RefreshRateVote vote;
        bool configChanged = false;
        if (change.autoSwapIntervalThresholdChanged && !change.swapDurationChanged) {
            durations.clear();
            // Report the re-clamp so the caller notifies swapIntervalChanged
            // listeners; app floor and refresh-period changes do not.
            configChanged = autoSwapInterval != currentSwapInterval;
            vote = voteClampedRefreshPeriod(autoSwapInterval, bounds, targetDuration,
                                            change.swapDuration, change.autoSwapIntervalThreshold,
                                            votableRefreshPeriods);
        } else {
            if (change.swapDurationChanged || !swapIntervalValid ||
                !mDisplayRateController.hasVoted()) {
                vote = voteClampedRefreshPeriod(autoSwapInterval, bounds, change.swapDuration,
                                                change.swapDuration,
                                                change.autoSwapIntervalThreshold,
                                                votableRefreshPeriods);
            }
            const auto newPacedPeriod = change.newRefreshPeriod * autoSwapInterval;
            const bool budgetLoosened =
                    newPacedPeriod > oldPacedPeriod + DURATION_ROUNDING_MARGIN ||
                    currentPipelineMode == PipelineMode::Off;
            if (budgetLoosened) {
                durations.clearMissesFittingBudget(newPacedPeriod, PipelineMode::On);
            }
        }

        return PacingResult{
                .autoSwapInterval = autoSwapInterval,
                .pipelineMode = PipelineMode::On,
                .configChanged = configChanged,
                .vote = vote,
        };
    }

private:
    RefreshRateVote voteClampedRefreshPeriod(int32_t swapInterval, const AutoSwapBounds& bounds,
                                             std::chrono::nanoseconds targetPeriod,
                                             std::chrono::nanoseconds minSwapDuration,
                                             std::chrono::nanoseconds autoSwapIntervalThreshold,
                                             const VotableRefreshPeriods& votableRefreshPeriods) {
        using namespace std::chrono_literals;
        if (swapInterval > bounds.ceiling) {
            return minSwapDuration > 0ns
                    ? voteRefreshPeriod(minSwapDuration, minSwapDuration, votableRefreshPeriods)
                    : RefreshRateVote{};
        }
        if (targetPeriod <= 0ns) {
            return votableRefreshPeriods.usesSetFrameRate
                    ? RefreshRateVote{}
                    : voteRefreshPeriod(0ns, minSwapDuration, votableRefreshPeriods);
        }
        const auto maxPreferredPeriod = std::max(minSwapDuration, autoSwapIntervalThreshold);
        return voteRefreshPeriod(std::min(targetPeriod, maxPreferredPeriod), minSwapDuration,
                                 votableRefreshPeriods);
    }

    DisplayRateController mDisplayRateController;
};

} // namespace swappy

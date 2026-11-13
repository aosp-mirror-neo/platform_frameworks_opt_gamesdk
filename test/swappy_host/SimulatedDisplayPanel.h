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
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "RefreshPeriodSet.h"
#include "SwapIntervalLadder.h"

namespace swappy::testing {

// Models SurfaceFlinger panel refresh-period selection across discrete modes,
// 240 Hz tearing-effect (TE) VRR divisors, and continuous ARR step tables.
class DisplayPanelModel {
public:
    DisplayPanelModel() = default;

    [[nodiscard]] static DisplayPanelModel openLoop() {
        return DisplayPanelModel{};
    }

    [[nodiscard]] static DisplayPanelModel discrete60And120Hz(int applyLatencyVsyncs = 2) {
        using namespace std::chrono_literals;
        assert(applyLatencyVsyncs >= 1);
        auto periods = std::make_shared<RefreshPeriodMap>();
        insertRefreshPeriod(*periods, 8'333'333ns, 1);
        insertRefreshPeriod(*periods, 16'666'667ns, 2);
        return DisplayPanelModel(std::move(periods), applyLatencyVsyncs,
                                 /*matchDivisorMultiples=*/true);
    }

    [[nodiscard]] static DisplayPanelModel teDivisor240Hz(int applyLatencyVsyncs = 2) {
        using namespace std::chrono_literals;
        assert(applyLatencyVsyncs >= 1);
        auto periods = std::make_shared<RefreshPeriodMap>();
        insertRefreshPeriod(*periods, 8'333'333ns, 1);
        insertRefreshPeriod(*periods, 12'500'000ns, -1);
        insertRefreshPeriod(*periods, 16'666'667ns, 2);
        insertRefreshPeriod(*periods, 20'833'333ns, -1);
        insertRefreshPeriod(*periods, 25'000'000ns, -1);
        insertRefreshPeriod(*periods, 33'333'333ns, -1);
        insertRefreshPeriod(*periods, 41'666'667ns, -1);
        return DisplayPanelModel(std::move(periods), applyLatencyVsyncs,
                                 /*matchDivisorMultiples=*/false);
    }

    [[nodiscard]] static DisplayPanelModel continuousArr(int minHz = 24, int maxHz = 120,
                                                         int applyLatencyVsyncs = 2) {
        assert(applyLatencyVsyncs >= 1);
        const int clampedMinHz = std::max(1, minHz);
        const int clampedMaxHz = std::max(clampedMinHz, maxHz);
        auto periods = std::make_shared<RefreshPeriodMap>();
        for (int hz = clampedMaxHz; hz >= clampedMinHz; --hz) {
            int modeId = -1;
            if (hz == 120) {
                modeId = 1;
            } else if (hz == 60) {
                modeId = 2;
            }
            insertRefreshPeriod(*periods, hzToPeriod(hz), modeId);
        }
        return DisplayPanelModel(std::move(periods), applyLatencyVsyncs,
                                 /*matchDivisorMultiples=*/false);
    }

    [[nodiscard]] bool enabled() const noexcept {
        return mSupportedPeriods != nullptr && !mSupportedPeriods->empty();
    }

    [[nodiscard]] int applyLatencyVsyncs() const noexcept {
        return mApplyLatencyVsyncs;
    }

    [[nodiscard]] std::shared_ptr<RefreshPeriodMap> supportedPeriods() const noexcept {
        return mSupportedPeriods;
    }

    [[nodiscard]] std::optional<std::chrono::nanoseconds> resolveFrameRateVote(
            float votedHz) const noexcept {
        if (!enabled() || !(votedHz > 0.f) || !std::isfinite(votedHz)) {
            return std::nullopt;
        }

        constexpr double kOneSecondNs = 1'000'000'000.0;
        constexpr double kMaxRepresentablePeriodNs = 1e15;
        const double clampedPeriodNs = std::clamp(kOneSecondNs / static_cast<double>(votedHz), 0.0,
                                                  kMaxRepresentablePeriodNs);
        const auto targetPeriod =
                std::chrono::nanoseconds(static_cast<int64_t>(std::llround(clampedPeriodNs)));

        if (!mMatchDivisorMultiples) {
            const auto it = mSupportedPeriods->lower_bound(targetPeriod - DURATION_ROUNDING_MARGIN);
            return (it != mSupportedPeriods->end()) ? it->first
                                                    : mSupportedPeriods->rbegin()->first;
        }

        // Discrete physical modes: pick the longest physical refresh period whose integer
        // multiple achieves the closest paced duration without running faster.
        std::optional<std::chrono::nanoseconds> bestPeriod;
        std::optional<std::chrono::nanoseconds> bestSwapDuration;
        for (const auto& [period, modeId] : *mSupportedPeriods) {
            if (modeId < 0) {
                continue;
            }
            const int interval = calculateSwapInterval(targetPeriod, period);
            const auto candidateSwapDuration = period * interval;
            if (!bestSwapDuration.has_value() ||
                candidateSwapDuration <= *bestSwapDuration + DURATION_ROUNDING_MARGIN) {
                bestSwapDuration = candidateSwapDuration;
                bestPeriod = period;
            }
        }
        return bestPeriod;
    }

    [[nodiscard]] std::optional<std::chrono::nanoseconds> resolveModeId(int modeId) const noexcept {
        if (!enabled() || modeId < 0) {
            return std::nullopt;
        }
        for (const auto& [period, candidateModeId] : *mSupportedPeriods) {
            if (candidateModeId == modeId) {
                return period;
            }
        }
        return std::nullopt;
    }

private:
    DisplayPanelModel(std::shared_ptr<RefreshPeriodMap> supportedPeriods, int applyLatencyVsyncs,
                      bool matchDivisorMultiples) noexcept
          : mSupportedPeriods(std::move(supportedPeriods)),
            mApplyLatencyVsyncs(applyLatencyVsyncs),
            mMatchDivisorMultiples(matchDivisorMultiples) {}

    [[nodiscard]] static std::chrono::nanoseconds hzToPeriod(int hz) noexcept {
        return std::chrono::nanoseconds(
                static_cast<int64_t>(std::llround(1'000'000'000.0 / static_cast<double>(hz))));
    }

    std::shared_ptr<RefreshPeriodMap> mSupportedPeriods;
    int mApplyLatencyVsyncs = 2;
    bool mMatchDivisorMultiples = false;
};

struct PendingPanelSwitch {
    std::chrono::nanoseconds refreshPeriod{0};
    int remainingVsyncs = 0;
};

// Stateful display panel simulation tracking the active refresh period and
// pending VSYNC-deferred mode or ARR rate transitions.
class SimulatedDisplayPanel {
public:
    explicit SimulatedDisplayPanel(
            std::chrono::nanoseconds initialPeriod = std::chrono::nanoseconds(16'666'667),
            DisplayPanelModel model = DisplayPanelModel::openLoop()) noexcept
          : mModel(std::move(model)), mActivePeriod(initialPeriod) {}

    [[nodiscard]] std::shared_ptr<RefreshPeriodMap> supportedPeriods() const noexcept {
        return mModel.supportedPeriods();
    }

    [[nodiscard]] std::chrono::nanoseconds activePeriod() const noexcept {
        return mActivePeriod;
    }

    void onFrameRateVote(float votedHz) noexcept {
        if (const auto nextPeriod = mModel.resolveFrameRateVote(votedHz)) {
            scheduleSwitch(*nextPeriod);
        }
    }

    void onModeIdVote(int modeId) noexcept {
        if (const auto nextPeriod = mModel.resolveModeId(modeId)) {
            scheduleSwitch(*nextPeriod);
        }
    }

    void scheduleSwitch(std::chrono::nanoseconds targetPeriod) noexcept {
        using namespace std::chrono_literals;
        if (targetPeriod <= 0ns) {
            return;
        }
        if (targetPeriod == mActivePeriod) {
            mPendingSwitch.reset();
            return;
        }
        if (mPendingSwitch.has_value() && mPendingSwitch->refreshPeriod == targetPeriod) {
            return;
        }
        mPendingSwitch = PendingPanelSwitch{targetPeriod, mModel.applyLatencyVsyncs()};
    }

    [[nodiscard]] std::optional<std::chrono::nanoseconds> advanceVsync() noexcept {
        if (!mPendingSwitch.has_value() || --mPendingSwitch->remainingVsyncs > 0) {
            return std::nullopt;
        }
        const auto nextPeriod = mPendingSwitch->refreshPeriod;
        mPendingSwitch.reset();
        if (nextPeriod == mActivePeriod) {
            return std::nullopt;
        }
        mActivePeriod = nextPeriod;
        return nextPeriod;
    }

    void forceRefreshPeriod(std::chrono::nanoseconds newRefreshPeriod) noexcept {
        mPendingSwitch.reset();
        mActivePeriod = newRefreshPeriod;
    }

private:
    DisplayPanelModel mModel;
    std::chrono::nanoseconds mActivePeriod;
    std::optional<PendingPanelSwitch> mPendingSwitch;
};

} // namespace swappy::testing

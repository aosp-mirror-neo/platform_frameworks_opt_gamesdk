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
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "Clock.h"
#include "RefreshPeriodSet.h"
#include "SdkVersion.h"
#include "Settings.h"
#include "SimulatedDisplayPanel.h"
#include "SwappyPlatform.h"

struct ANativeWindow {
    int id = 0;
};

namespace swappy::testing {

[[nodiscard]] inline SwappyCommonSettings makeTestSwappySettings(
        std::chrono::nanoseconds refreshPeriod = std::chrono::nanoseconds(16'666'667),
        int sdkInt = 33) {
    using namespace std::chrono_literals;
    return SwappyCommonSettings{
            .sdkVersion = SdkVersion{sdkInt, 0},
            .refreshPeriod = refreshPeriod,
            .appVsyncOffset = 1'000'000ns,
            .sfVsyncOffset = 2'000'000ns,
    };
}

struct PlatformSharedState {
    std::mutex mutex;
    bool valid = true;
    bool startsWithExternalChoreographer = false;
    bool choreographerInitialized = true;
    bool displayManagerInitialized = true;
    bool supportsSetFrameRate = true;
    SwappyCommonSettings initialSettings = makeTestSwappySettings();
    std::shared_ptr<RefreshPeriodMap> supportedPeriods;
    SimulatedDisplayPanel panel{initialSettings.refreshPeriod};

    SwappyPlatform::ChoreographerCallback onChoreographer;
    std::vector<ANativeWindow*> acquiredWindows;
    std::vector<ANativeWindow*> releasedWindows;
    std::vector<std::pair<ANativeWindow*, float>> frameRateVotes;
    std::vector<int> preferredModeIds;
    int postFrameCallbacksCount = 0;
    int switchToExternalChoreographerCount = 0;
    bool choreographerCallbackArmed = false;
};

struct PlatformSnapshot {
    std::vector<ANativeWindow*> acquiredWindows;
    std::vector<ANativeWindow*> releasedWindows;
    std::vector<std::pair<ANativeWindow*, float>> frameRateVotes;
    std::vector<int> preferredModeIds;
    int postFrameCallbacksCount = 0;
    int switchToExternalChoreographerCount = 0;
};

[[nodiscard]] inline PlatformSnapshot snapshotPlatformState(PlatformSharedState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    return PlatformSnapshot{
            .acquiredWindows = state.acquiredWindows,
            .releasedWindows = state.releasedWindows,
            .frameRateVotes = state.frameRateVotes,
            .preferredModeIds = state.preferredModeIds,
            .postFrameCallbacksCount = state.postFrameCallbacksCount,
            .switchToExternalChoreographerCount = state.switchToExternalChoreographerCount,
    };
}

class FakeSwappyPlatform final : public SwappyPlatform {
public:
    explicit FakeSwappyPlatform(std::shared_ptr<PlatformSharedState> state)
          : mState(std::move(state)) {}

    [[nodiscard]] bool isValid() const override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->valid;
    }

    [[nodiscard]] SwappyCommonSettings getInitialSettings() const override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->initialSettings;
    }

    [[nodiscard]] bool startsWithExternalChoreographer() const override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->startsWithExternalChoreographer;
    }

    [[nodiscard]] bool initializeChoreographer(ChoreographerCallback onChoreographer) override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        if (!mState->choreographerInitialized) {
            return false;
        }
        mState->onChoreographer = std::move(onChoreographer);
        return true;
    }

    void postFrameCallbacks() override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        ++mState->postFrameCallbacksCount;
        mState->choreographerCallbackArmed = true;
    }

    void switchToExternalChoreographer(ChoreographerCallback onChoreographer) override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        ++mState->switchToExternalChoreographerCount;
        mState->onChoreographer = std::move(onChoreographer);
    }

    [[nodiscard]] bool initializeDisplayManager() override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->displayManagerInitialized;
    }

    void acquireWindow(ANativeWindow* window) override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        mState->acquiredWindows.push_back(window);
    }

    void releaseWindow(ANativeWindow* window) override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        mState->releasedWindows.push_back(window);
    }

    [[nodiscard]] bool supportsSetFrameRate() const override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->supportsSetFrameRate;
    }

    void setFrameRate(ANativeWindow* window, float frameRate) override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        mState->frameRateVotes.emplace_back(window, frameRate);
        mState->panel.onFrameRateVote(frameRate);
    }

    [[nodiscard]] std::shared_ptr<RefreshPeriodMap> getSupportedRefreshPeriods() override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->supportedPeriods ? mState->supportedPeriods
                                        : mState->panel.supportedPeriods();
    }

    void setPreferredDisplayModeId(int modeId) override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        mState->preferredModeIds.push_back(modeId);
        mState->panel.onModeIdVote(modeId);
    }

private:
    std::shared_ptr<PlatformSharedState> mState;
};

// Coordinates Choreographer VSYNC delivery and deferred panel refresh-period
// switches against a shared PlatformSharedState.
class VsyncPanelTimeline {
public:
    VsyncPanelTimeline(std::shared_ptr<PlatformSharedState> state,
                       const SwappyCommonSettings& settings, const Clock* clock = nullptr)
          : mState(std::move(state)), mClock(clock) {
        std::lock_guard<std::mutex> lock(mState->mutex);
        mState->initialSettings = settings;
        mState->panel.forceRefreshPeriod(settings.refreshPeriod);
    }

    [[nodiscard]] std::chrono::nanoseconds displayPeriod() const {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->panel.activePeriod();
    }

    [[nodiscard]] std::optional<Clock::time_point> nextVsyncAfter(Clock::time_point current) const {
        using namespace std::chrono_literals;
        std::lock_guard<std::mutex> lock(mState->mutex);
        const auto activePeriod = mState->panel.activePeriod();
        if (!mState->choreographerCallbackArmed || activePeriod <= 0ns) {
            return std::nullopt;
        }
        const int64_t periodNs = activePeriod.count();
        const int64_t anchorNs =
                mPhaseAnchorNs.value_or(mState->initialSettings.appVsyncOffset.count());
        const int64_t nowNs = current.time_since_epoch().count();
        const int64_t adjusted = nowNs - anchorNs;
        int64_t k = adjusted / periodNs;
        if (adjusted % periodNs < 0) {
            --k;
        }
        int64_t nextNs = (k + 1) * periodNs + anchorNs;
        while (nextNs <= nowNs) {
            nextNs += periodNs;
        }
        return Clock::time_point(std::chrono::nanoseconds(nextNs));
    }

    void deliverVsync(std::optional<Clock::time_point> vsyncTime = std::nullopt) {
        using namespace std::chrono_literals;
        SwappyPlatform::ChoreographerCallback callback;
        std::chrono::nanoseconds sfToVsyncDelay;
        SwappyCommonSettings timings;
        std::optional<std::chrono::nanoseconds> appliedPanelPeriod;
        {
            std::lock_guard<std::mutex> lock(mState->mutex);
            if (!mState->choreographerCallbackArmed || !mState->onChoreographer) {
                return;
            }
            mState->choreographerCallbackArmed = false;
            callback = mState->onChoreographer;
            timings = mState->initialSettings;
            sfToVsyncDelay = std::max(0ns, mState->panel.activePeriod() - timings.sfVsyncOffset);
            appliedPanelPeriod = mState->panel.advanceVsync();
            if (appliedPanelPeriod.has_value()) {
                const auto switchTime = vsyncTime.has_value()
                        ? vsyncTime
                        : (mClock != nullptr ? std::optional{mClock->now()} : std::nullopt);
                if (switchTime.has_value()) {
                    mPhaseAnchorNs = switchTime->time_since_epoch().count();
                }
            }
        }
        callback(sfToVsyncDelay);
        if (appliedPanelPeriod.has_value()) {
            Settings::getInstance()->setDisplayTimings(
                    {*appliedPanelPeriod, timings.appVsyncOffset, timings.sfVsyncOffset});
        }
    }

    void triggerPassiveDisplayRateChange(std::chrono::nanoseconds newRefreshPeriod,
                                         std::optional<Clock::time_point> now = std::nullopt) {
        SwappyCommonSettings timings;
        {
            std::lock_guard<std::mutex> lock(mState->mutex);
            mState->panel.forceRefreshPeriod(newRefreshPeriod);
            const auto switchTime = now.has_value()
                    ? now
                    : (mClock != nullptr ? std::optional{mClock->now()} : std::nullopt);
            if (switchTime.has_value()) {
                mPhaseAnchorNs = switchTime->time_since_epoch().count();
            }
            timings = mState->initialSettings;
        }
        Settings::getInstance()->setDisplayTimings(
                {newRefreshPeriod, timings.appVsyncOffset, timings.sfVsyncOffset});
    }

private:
    std::shared_ptr<PlatformSharedState> mState;
    const Clock* mClock = nullptr;
    std::optional<int64_t> mPhaseAnchorNs;
};

} // namespace swappy::testing

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
#include <functional>
#include <memory>
#include <optional>

#include "RefreshPeriodSet.h"
#include "SdkVersion.h"

struct ANativeWindow;

namespace swappy {

struct SwappyCommonSettings {
    SdkVersion sdkVersion;

    std::chrono::nanoseconds refreshPeriod;
    std::chrono::nanoseconds appVsyncOffset;
    std::chrono::nanoseconds sfVsyncOffset;
};

// Platform boundary isolating SwappyCommon from Android JNI, NDK ANativeWindow,
// and SwappyDisplayManager APIs.
class SwappyPlatform {
public:
    using ChoreographerCallback =
            std::function<void(std::optional<std::chrono::nanoseconds> sfToVsyncDelay)>;

    virtual ~SwappyPlatform() = default;

    // Returns true if platform initialization succeeded and initial settings are valid.
    [[nodiscard]] virtual bool isValid() const = 0;

    // Returns display timings and SDK version queried during platform initialization.
    [[nodiscard]] virtual SwappyCommonSettings getInitialSettings() const = 0;

    // Returns true if Choreographer starts in external (application-driven) mode.
    [[nodiscard]] virtual bool startsWithExternalChoreographer() const {
        return false;
    }

    // Initializes platform Choreographer dispatch invoking onChoreographer per tick.
    [[nodiscard]] virtual bool initializeChoreographer(ChoreographerCallback onChoreographer) = 0;

    // Schedules frame callbacks while driving Choreographer or servicing an external tick.
    virtual void postFrameCallbacks() = 0;

    // Switches Choreographer dispatch from internal mode to application-driven ticks.
    virtual void switchToExternalChoreographer(ChoreographerCallback onChoreographer) = 0;

    // Initializes display-rate discovery when supported; returns false on initialization failure.
    [[nodiscard]] virtual bool initializeDisplayManager() {
        return true;
    }

    // Retains or releases an ANativeWindow reference across SwappyCommon::setANativeWindow.
    virtual void acquireWindow(ANativeWindow* window) = 0;
    virtual void releaseWindow(ANativeWindow* window) = 0;

    // Returns true when platform frame-rate voting via ANativeWindow_setFrameRate is available.
    [[nodiscard]] virtual bool supportsSetFrameRate() const = 0;
    virtual void setFrameRate(ANativeWindow* window, float frameRate) = 0;

    // Returns supported refresh periods mapped to display mode IDs, or nullptr if unavailable.
    [[nodiscard]] virtual std::shared_ptr<RefreshPeriodMap> getSupportedRefreshPeriods() = 0;
    virtual void setPreferredDisplayModeId(int modeId) = 0;
};

} // namespace swappy

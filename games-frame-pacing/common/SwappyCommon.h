/*
 * Copyright 2018 The Android Open Source Project
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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>

#include "CPUTracer.h"
#include "ChoreographerFilter.h"
#include "Clock.h"
#include "DisplayRateController.h"
#include "FrameDurations.h"
#include "PacingImplementation.h"
#include "RefreshPeriodSet.h"
#include "Settings.h"
#include "SwappyPlatform.h"
#include "SwappyTrace.h"
#include "Thread.h"
#include "swappy/swappy_common.h"

namespace swappy {

using namespace std::chrono_literals;

// Common part between OpenGL and Vulkan implementations.
class SwappyCommon {
public:
    // Aliased at class scope because SwappyGL.cpp and SwappyVkBase.cpp
    // reference SwappyCommon::PipelineMode.
    using PipelineMode = ::swappy::PipelineMode;

    // callbacks to be called during pre/post swap
    struct SwapHandlers {
        std::function<bool()> lastFrameIsComplete;
        std::function<std::chrono::nanoseconds()> getPrevFrameGpuTime;
    };

    explicit SwappyCommon(std::unique_ptr<SwappyPlatform> platform,
                          const Clock& clock = SteadyClock::instance());

    ~SwappyCommon();

    std::chrono::nanoseconds getSwapDuration() const;

    int32_t getAutoSwapInterval() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mAutoSwapInterval;
    }

    void onChoreographer(int64_t frameTimeNanos);

    void onPreSwap(const SwapHandlers& h);

    bool needToSetPresentationTime() {
        return mPresentationTimeNeeded;
    }

    void onPostSwap(const SwapHandlers& h);

    PipelineMode getCurrentPipelineMode() const;

    template <typename... T>
    struct Tracer {
        void (*function)(void*, T...);
        void* userData;
    };

    void addTracerCallbacks(const SwappyTracer& tracer);

    void removeTracerCallbacks(const SwappyTracer& tracer);

    void setAutoSwapInterval(bool enabled);
    void setAutoPipelineMode(bool enabled);
    void setMaxAutoSwapDuration(std::chrono::nanoseconds swapDuration);

    Clock::time_point getPresentationTime() {
        return mPresentationTime;
    }
    std::chrono::nanoseconds getRefreshPeriod() const;

    bool isValid() {
        return mValid;
    }

    std::chrono::nanoseconds getFenceTimeout() const {
        return mFenceTimeout.load(std::memory_order_relaxed);
    }
    void setFenceTimeout(std::chrono::nanoseconds t) {
        mFenceTimeout.store(t, std::memory_order_relaxed);
    }

    void setANativeWindow(ANativeWindow* window);

    void setBufferStuffingFixWait(int32_t nFrames) {
        mBufferStuffingFixWait.store(std::max(0, nFrames), std::memory_order_relaxed);
    }

    int getSupportedRefreshPeriodsNS(uint64_t* out_refreshrates, int allocated_entries);

    void setLastLatencyRecordedCallback(std::function<int32_t()> callback) {
        mLastLatencyRecorded = callback;
    }

    void resetFramePacing();

    void enableFramePacing(bool enable);
    void enableBlockingWait(bool enable);

private:
    // Aliased so the member declarations below can reference them unqualified.
    using FrameDuration = ::swappy::FrameDuration;
    using FrameDurations = ::swappy::FrameDurations;

    void addFrameDuration(FrameDuration duration, PipelineMode swapPipelineMode,
                          bool previousFrameMissed);
    std::chrono::nanoseconds wakeClient(Clock::time_point wakeDeadline,
                                        std::optional<std::chrono::nanoseconds> sfToVsyncDelay);

    // The swap interval ladder itself is in SwapIntervalLadder.h; this is the
    // glue that holds the lock, feeds it state and applies its decision.
    bool updateSwapInterval();
#if AGDK_TRACE != 0
    // Copies the settings that Swappy.Settings reports. Locks mMutex.
    TraceSettingsSnapshot buildTraceSettingsSnapshot() EXCLUDES(mMutex);
#endif
    void preSwapBuffersCallbacks();
    void postSwapBuffersCallbacks();
    void preWaitCallbacks();
    void postWaitCallbacks(std::chrono::nanoseconds cpuTime, std::chrono::nanoseconds gpuTime);
    void startFrameCallbacks(int32_t currentFrame);
    void swapIntervalChangedCallbacks();
    void onSettingsChanged();
    void updateMeasuredSwapDuration(std::chrono::nanoseconds duration);
    void startFrame();
    void waitUntil(int32_t target);
    void waitUntilLocked(int32_t target) REQUIRES(mWaitingMutex);
    void waitUntilTargetFrame();
    void waitOneFrame();
    void setPreferredDisplayModeId(int index) REQUIRES(mMutex);
    // The returned periods pointer stays valid while mMutex is held.
    [[nodiscard]] VotableRefreshPeriods votableRefreshPeriods() const REQUIRES(mMutex);
    void applyRefreshRateVote(const RefreshRateVote& vote) REQUIRES(mMutex);
    [[nodiscard]] bool isAutoSwapWithinThresholdLocked() const REQUIRES(mMutex);
    void refreshSupportedRefreshPeriods() EXCLUDES(mMutex);
    bool updateDisplayTimings();
    bool applyNextTimingSettings() REQUIRES(mMutex);

    // Waits for the next frame, considering both Choreographer and the prior
    // frame's completion
    bool waitForNextFrame(const SwapHandlers& h);

    std::unique_ptr<SwappyPlatform> mPlatform;
    const Clock& mClock;

    mutable std::mutex mMutex;

    SwappyCommonSettings mCommonSettings GUARDED_BY(mMutex){};

    std::unique_ptr<ChoreographerFilter> mChoreographerFilter;

    bool mUsingExternalChoreographer = false;

    std::mutex mWaitingMutex;
    std::condition_variable mWaitingCondition;
    Clock::time_point mCurrentFrameTimestamp GUARDED_BY(mWaitingMutex);
    Clock::time_point mCurrentFrameTargetWakeTime GUARDED_BY(mWaitingMutex){};
    int32_t mCurrentFrame GUARDED_BY(mWaitingMutex) = 0;
    std::optional<std::chrono::nanoseconds> mSfToVsyncDelay GUARDED_BY(mWaitingMutex);
    std::atomic<std::chrono::nanoseconds> mMeasuredSwapDuration;

    Clock::time_point mSwapTime{};

    FrameDurations mFrameDurations GUARDED_BY(mMutex);

    bool mAutoSwapIntervalEnabled GUARDED_BY(mMutex) = true;
    bool mPipelineModeAutoMode GUARDED_BY(mMutex) = true;

    std::chrono::nanoseconds mSwapDuration GUARDED_BY(mMutex) = 0ns;
    int32_t mAutoSwapInterval GUARDED_BY(mMutex);
    std::chrono::nanoseconds mAutoSwapIntervalThreshold GUARDED_BY(mMutex) =
            DEFAULT_AUTO_SWAP_INTERVAL_THRESHOLD;

    std::optional<Clock::time_point> mStartFrameTime;

    struct SwappyTracerCallbacks {
        std::list<Tracer<>> preWait;
        std::list<Tracer<int64_t, int64_t>> postWait;
        std::list<Tracer<>> preSwapBuffers;
        std::list<Tracer<int64_t>> postSwapBuffers;
        std::list<Tracer<int32_t, int64_t>> startFrame;
        std::list<Tracer<>> swapIntervalChanged;
    };

    SwappyTracerCallbacks mInjectedTracers;

    int32_t mTargetFrame = 0;
    Clock::time_point mPresentationTime;
    bool mPresentationTimeNeeded;
    PipelineMode mPipelineMode GUARDED_BY(mMutex) = PipelineMode::On;
    std::optional<PipelineMode> mActiveSwapPipelineMode GUARDED_BY(mMutex);

    bool mValid;

    std::atomic<std::chrono::nanoseconds> mFenceTimeout{std::chrono::nanoseconds(50ms)};

    std::shared_ptr<RefreshPeriodMap> mSupportedRefreshPeriods GUARDED_BY(mMutex);

    struct TimingSettings {
        std::chrono::nanoseconds refreshPeriod = {};
        std::chrono::nanoseconds swapDuration = {};

        static TimingSettings from(const Settings& settings) {
            TimingSettings timingSettings;

            timingSettings.refreshPeriod = settings.getDisplayTimings().refreshPeriod;
            timingSettings.swapDuration = settings.getSwapDuration();
            return timingSettings;
        }

        bool operator!=(const TimingSettings& other) const {
            return (refreshPeriod != other.refreshPeriod) || (swapDuration != other.swapDuration);
        }

        bool operator==(const TimingSettings& other) const {
            return !(*this != other);
        }
    };
    TimingSettings mNextTimingSettings GUARDED_BY(mMutex) = {};
    bool mTimingSettingsNeedUpdate GUARDED_BY(mMutex) = false;
    std::chrono::nanoseconds mNextAutoSwapIntervalThreshold GUARDED_BY(mMutex) =
            DEFAULT_AUTO_SWAP_INTERVAL_THRESHOLD;

    CPUTracer mCPUTracer;

    ANativeWindow* mWindow GUARDED_BY(mMutex) = nullptr;
    bool mWindowChanged GUARDED_BY(mMutex) = false;

    // Callback for last latency recorded - used for buffer stuffing fix.
    // Latency is returned in number of V-syncs
    std::function<int32_t()> mLastLatencyRecorded;

    // If zero, don't apply the double buffering fix. If non-zero, apply
    // the fix after this number of bad frames.
    std::atomic<int32_t> mBufferStuffingFixWait{0};
    // When zero, buffer stuffing fixing may occur.
    // After a fix has been applied, this is non-zero and counts down to avoid
    // consecutive fixes.
    int mBufferStuffingFixCounter = 0;
    // Counts the number of consecutive missed frames (as judged by expected
    // latency).
    int mMissedFrameCounter = 0;

    bool mFramePacingResetRequested GUARDED_BY(mMutex) = false;

    bool mFramePacingToggleRequested GUARDED_BY(mMutex) = false;
    bool mFramePacingEnabled GUARDED_BY(mMutex) = true;
    bool mBlockingWaitEnabled GUARDED_BY(mMutex) = true;

    std::unique_ptr<PacingImplementation> mPacingImpl GUARDED_BY(mMutex) =
            PacingImplementation::create();
    TRACE_MEMBER(TraceSession mTraceSession);
};

} // namespace swappy

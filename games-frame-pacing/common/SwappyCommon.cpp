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

#include "SwappyCommon.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ClassicPacingImpl.h"
#include "Settings.h"
#include "SwapIntervalLadder.h"
#include "Thread.h"
#include "Trace.h"

#define LOG_TAG "SwappyCommon"
#include "SwappyLog.h"

namespace swappy {

using std::chrono::milliseconds;
using std::chrono::nanoseconds;

std::unique_ptr<PacingImplementation> PacingImplementation::create() {
    return std::make_unique<ClassicPacingImplementation>();
}

SwappyCommon::SwappyCommon(std::unique_ptr<SwappyPlatform> platform)
      : mPlatform(std::move(platform)),
        mMeasuredSwapDuration(nanoseconds(0)),
        mAutoSwapInterval(1),
        mValid(false) {
    if (!mPlatform || !mPlatform->isValid()) {
        mPlatform.reset();
        return;
    }

    mCommonSettings = mPlatform->getInitialSettings();

    mChoreographerFilter = std::make_unique<
            ChoreographerFilter>(mCommonSettings.refreshPeriod,
                                 mCommonSettings.sfVsyncOffset - mCommonSettings.appVsyncOffset,
                                 [this](std::optional<std::chrono::nanoseconds> sfToVsyncDelay) {
                                     return wakeClient(sfToVsyncDelay);
                                 });

    mUsingExternalChoreographer = mPlatform->startsWithExternalChoreographer();

    if (!mPlatform->initializeChoreographer(
                [this](std::optional<std::chrono::nanoseconds> sfToVsyncDelay) {
                    mChoreographerFilter->onChoreographer(sfToVsyncDelay);
                })) {
        SWAPPY_LOGE("failed to initialize ChoreographerThread");
        mPlatform.reset();
        return;
    }

    if (!mPlatform->initializeDisplayManager()) {
        mPlatform.reset();
        return;
    }

    Settings::getInstance()->addListener([this]() { onSettingsChanged(); });
    Settings::getInstance()->setDisplayTimings({mCommonSettings.refreshPeriod,
                                                mCommonSettings.appVsyncOffset,
                                                mCommonSettings.sfVsyncOffset});

    mInitialRefreshPeriod = mCommonSettings.refreshPeriod;
    SWAPPY_LOGI("Initialized Swappy with vsyncPeriod=%lld, appOffset=%lld, "
                "sfOffset=%lld",
                (long long)mCommonSettings.refreshPeriod.count(),
                (long long)mCommonSettings.appVsyncOffset.count(),
                (long long)mCommonSettings.sfVsyncOffset.count());
    mValid = true;
}

SwappyCommon::~SwappyCommon() {
    // Remove the settings' listeners before destroying Choreographer objects
    // because the listeners may contain references to the objects
    Settings::getInstance()->removeAllListeners();
    // Destroy the platform first so its Choreographer thread stops invoking
    // mChoreographerFilter before the filter is destroyed.
    mPlatform.reset();
    mChoreographerFilter.reset();

    Settings::reset();
}

nanoseconds SwappyCommon::wakeClient(std::optional<std::chrono::nanoseconds> sfToVsyncDelay) {
    std::lock_guard<std::mutex> lock(mWaitingMutex);
    ++mCurrentFrame;
    // We're attempting to align with SurfaceFlinger's vsync, but it's always
    // better to be a little late than a little early (since a little early
    // could cause our frame to be picked up prematurely), so we pad by an
    // additional millisecond.
    mCurrentFrameTimestamp = std::chrono::steady_clock::now() + mMeasuredSwapDuration.load() + 1ms;

    mSfToVsyncDelay = sfToVsyncDelay;
    mWaitingCondition.notify_all();
    return mMeasuredSwapDuration;
}

void SwappyCommon::onChoreographer(int64_t /*frameTimeNanos*/) {
    TRACE_CALL();

    if (!mPlatform) {
        return;
    }

    if (!mUsingExternalChoreographer) {
        mUsingExternalChoreographer = true;
        mPlatform->switchToExternalChoreographer(
                [this](std::optional<std::chrono::nanoseconds> sfToVsyncDelay) {
                    mChoreographerFilter->onChoreographer(sfToVsyncDelay);
                });
    }

    mPlatform->postFrameCallbacks();
}

bool SwappyCommon::waitForNextFrame(const SwapHandlers& h) {
    int lateFrames = 0;
    bool presentationTimeIsNeeded;

    // We do not want to hold the mutex while waiting, so make a local copy of
    // the flags.
    mMutex.lock();
    bool localAutoSwapIntervalEnabled = mAutoSwapIntervalEnabled;
    bool localFramePacingEnabled = mFramePacingEnabled;

    // We do the blocking wait when pacing or request by the app when not
    // pacing.
    bool localBlockingWaitEnabled = mBlockingWaitEnabled || mFramePacingEnabled;
    mMutex.unlock();

    const nanoseconds cpuTime = (mStartFrameTime.time_since_epoch().count() == 0)
            ? 0ns
            : std::chrono::steady_clock::now() - mStartFrameTime;
    mCPUTracer.endTrace();

    preWaitCallbacks();

    // if we are running slower than the threshold (if auto swap interval is
    // enabled) there is no point to sleep,
    // just let the app run as fast as it can
    if (mCommonSettings.refreshPeriod * mAutoSwapInterval <= mAutoSwapIntervalThreshold.load() ||
        !localAutoSwapIntervalEnabled) {
        if (localFramePacingEnabled) waitUntilTargetFrame();

        if (localBlockingWaitEnabled) {
            // wait for the previous frame to be rendered
            while (!h.lastFrameIsComplete()) {
                lateFrames++;
                waitOneFrame();
            }
        }

        mPresentationTime += lateFrames * mCommonSettings.refreshPeriod;
        presentationTimeIsNeeded = localFramePacingEnabled;
    } else {
        presentationTimeIsNeeded = false;
    }

    // If last frame is not finished, return -1 for GPU time.
    const nanoseconds gpuTime = (h.lastFrameIsComplete()) ? h.getPrevFrameGpuTime() : -1ns;

    // Keep track of durations only if frame pacing is enabled.
    if (localFramePacingEnabled) addFrameDuration({cpuTime, gpuTime, mCurrentFrame > mTargetFrame});

    postWaitCallbacks(cpuTime, gpuTime);

    return presentationTimeIsNeeded;
}

void SwappyCommon::updateDisplayTimings() {
    // grab a pointer to the latest supported refresh rates
    if (mPlatform) {
        mSupportedRefreshPeriods = mPlatform->getSupportedRefreshPeriods();
    }

    std::lock_guard<std::mutex> lock(mMutex);
    SWAPPY_LOGW_ONCE_IF(!mWindow,
                        "ANativeWindow not configured, frame rate will not be "
                        "reported to Android platform");

    if (mFramePacingToggleRequested) {
        // In case frame pacing is toggled, set the update here.
        mFramePacingEnabled = !mFramePacingEnabled;
        mFramePacingToggleRequested = false;
    }
    if (mFramePacingResetRequested) {
        // In case of reset, just issue the update for setting refresh period.
        setPreferredRefreshPeriod(mInitialRefreshPeriod);
        mFramePacingResetRequested = false;
        return;
    }

    if (!mFramePacingEnabled) {
        return;
    }

    if (!mTimingSettingsNeedUpdate && !mWindowChanged) {
        return;
    }

    mTimingSettingsNeedUpdate = false;

    if (!mWindowChanged && mCommonSettings.refreshPeriod == mNextTimingSettings.refreshPeriod &&
        mSwapDuration == mNextTimingSettings.swapDuration) {
        return;
    }

    const TimingChange change{
            .newRefreshPeriod = mNextTimingSettings.refreshPeriod,
            .swapDuration = mSwapDuration,
    };
    mWindowChanged = false;
    mCommonSettings.refreshPeriod = mNextTimingSettings.refreshPeriod;

    const PacingResult timingResult =
            mPacingImpl->onDisplayTimingsChanged(mAutoSwapInterval, mPipelineMode, mFrameDurations,
                                                 change);
    mAutoSwapInterval = timingResult.autoSwapInterval;
    mPipelineMode = timingResult.pipelineMode;

    const bool swapIntervalValid = mNextTimingSettings.refreshPeriod * mAutoSwapInterval >=
            mNextTimingSettings.swapDuration;
    const bool swapIntervalChangedBySettings = mSwapDuration != mNextTimingSettings.swapDuration;

    mSwapDuration = mNextTimingSettings.swapDuration;
    if (!mAutoSwapIntervalEnabled || swapIntervalChangedBySettings || !swapIntervalValid) {
        mAutoSwapInterval = calculateSwapInterval(mSwapDuration, mCommonSettings.refreshPeriod);
        mPipelineMode = PipelineMode::On;
        setPreferredRefreshPeriod(mSwapDuration);
    }

    if (!mDisplayRateController.hasVoted()) {
        setPreferredRefreshPeriod(mSwapDuration);
    }

    mFrameDurations.clear();

    TRACE_INT("mSwapDuration", int(mSwapDuration.count()));
    TRACE_INT("mAutoSwapInterval", mAutoSwapInterval);
    TRACE_INT("mCommonSettings.refreshPeriod", mCommonSettings.refreshPeriod.count());
    TRACE_INT("mPipelineMode", static_cast<int>(mPipelineMode));
}

void SwappyCommon::onPreSwap(const SwapHandlers& h) {
    if (!mUsingExternalChoreographer && mPlatform) {
        mPlatform->postFrameCallbacks();
    }

    // for non pipeline mode where both cpu and gpu work is done at the same
    // stage wait for next frame will happen after swap
    if (mPipelineMode == PipelineMode::On) {
        mPresentationTimeNeeded = waitForNextFrame(h);
    } else {
        mPresentationTimeNeeded = (mCommonSettings.refreshPeriod * mAutoSwapInterval <=
                                   mAutoSwapIntervalThreshold.load());
    }

    mSwapTime = std::chrono::steady_clock::now();
    preSwapBuffersCallbacks();
}

void SwappyCommon::onPostSwap(const SwapHandlers& h) {
    postSwapBuffersCallbacks();

    updateMeasuredSwapDuration(std::chrono::steady_clock::now() - mSwapTime);

    if (mPipelineMode == PipelineMode::Off) {
        waitForNextFrame(h);
    }

    if (updateSwapInterval()) {
        swapIntervalChangedCallbacks();
        TRACE_INT("mPipelineMode", static_cast<int>(mPipelineMode));
        TRACE_INT("mAutoSwapInterval", mAutoSwapInterval);
    }

    updateDisplayTimings();

    startFrame();
}

void SwappyCommon::updateMeasuredSwapDuration(nanoseconds duration) {
    // TODO: The exponential smoothing factor here is arbitrary
    mMeasuredSwapDuration = (mMeasuredSwapDuration.load() * 4 / 5) + duration / 5;

    // Clamp the swap duration to half the refresh period
    //
    // We do this since the swap duration can be a bit noisy during periods such
    // as app startup, which can cause some stuttering as the smoothing catches
    // up with the actual duration. By clamping, we reduce the maximum error
    // which reduces the calibration time.
    if (mMeasuredSwapDuration.load() > (mCommonSettings.refreshPeriod / 2)) {
        mMeasuredSwapDuration.store(mCommonSettings.refreshPeriod / 2);
    }
}

nanoseconds SwappyCommon::getSwapDuration() {
    std::lock_guard<std::mutex> lock(mMutex);
    return mAutoSwapInterval * mCommonSettings.refreshPeriod;
};

void SwappyCommon::addFrameDuration(FrameDuration duration) {
    SWAPPY_LOGV("cpuTime = %.2f", duration.getCpuTime().count() / 1e6f);
    SWAPPY_LOGV("gpuTime = %.2f", duration.getGpuTime().count() / 1e6f);
    SWAPPY_LOGV("frame %s", duration.frameMiss() ? "MISS" : "on time");

    std::lock_guard<std::mutex> lock(mMutex);
    mFrameDurations.add(duration, std::chrono::steady_clock::now());
}

bool SwappyCommon::updateSwapInterval() {
    std::lock_guard<std::mutex> lock(mMutex);

    // A request to reset the frame-pacing is made, so reset the internal swap
    // state to the initial state and clear the frame durations collected.
    if (mFramePacingResetRequested) {
        mAutoSwapInterval = 1;
        mMeasuredSwapDuration = 0ns;
        mSwapDuration = 0ns;
        mCommonSettings.refreshPeriod = mInitialRefreshPeriod;
        mFrameDurations.clear();
        return true;
    }
    if (!mAutoSwapIntervalEnabled) return false;

    const auto result =
            mPacingImpl->updateSwapInterval(mAutoSwapInterval, mPipelineMode, mFrameDurations,
                                            mCommonSettings.refreshPeriod, mSwapDuration,
                                            mAutoSwapIntervalThreshold.load(),
                                            mPipelineModeAutoMode);

    // Applied unconditionally: swapSlower() can restore pipelining without
    // reporting a config change.
    mAutoSwapInterval = result.autoSwapInterval;
    mPipelineMode = result.pipelineMode;
    if (result.preferredRefreshPeriodHint.count() > 0) {
        setPreferredRefreshPeriod(result.preferredRefreshPeriodHint);
    }

    return result.configChanged;
}

template <typename Tracers, typename Func>
void addToTracers(Tracers& tracers, Func func, void* userData) {
    if (func != nullptr) {
        tracers.push_back({func, userData});
    }
}

template <typename Tracers, typename Func>
void removeFromTracers(Tracers& tracers, Func func) {
    if (func != nullptr) {
        for (auto it = tracers.begin(); it != tracers.end();) {
            auto jt = it;
            it++;
            if (jt->function == func) {
                tracers.erase(jt);
            }
        }
    }
}

void SwappyCommon::addTracerCallbacks(const SwappyTracer& tracer) {
    addToTracers(mInjectedTracers.preWait, tracer.preWait, tracer.userData);
    addToTracers(mInjectedTracers.postWait, tracer.postWait, tracer.userData);
    addToTracers(mInjectedTracers.preSwapBuffers, tracer.preSwapBuffers, tracer.userData);
    addToTracers(mInjectedTracers.postSwapBuffers, tracer.postSwapBuffers, tracer.userData);
    addToTracers(mInjectedTracers.startFrame, tracer.startFrame, tracer.userData);
    addToTracers(mInjectedTracers.swapIntervalChanged, tracer.swapIntervalChanged, tracer.userData);
}

void SwappyCommon::removeTracerCallbacks(const SwappyTracer& tracer) {
    removeFromTracers(mInjectedTracers.preWait, tracer.preWait);
    removeFromTracers(mInjectedTracers.postWait, tracer.postWait);
    removeFromTracers(mInjectedTracers.preSwapBuffers, tracer.preSwapBuffers);
    removeFromTracers(mInjectedTracers.postSwapBuffers, tracer.postSwapBuffers);
    removeFromTracers(mInjectedTracers.startFrame, tracer.startFrame);
    removeFromTracers(mInjectedTracers.swapIntervalChanged, tracer.swapIntervalChanged);
}

template <typename T, typename... Args>
void executeTracers(T& tracers, Args... args) {
    for (const auto& tracer : tracers) {
        tracer.function(tracer.userData, std::forward<Args>(args)...);
    }
}

void SwappyCommon::preSwapBuffersCallbacks() {
    executeTracers(mInjectedTracers.preSwapBuffers);
}

void SwappyCommon::postSwapBuffersCallbacks() {
    executeTracers(mInjectedTracers.postSwapBuffers,
                   (int64_t)mPresentationTime.time_since_epoch().count());
}

void SwappyCommon::preWaitCallbacks() {
    executeTracers(mInjectedTracers.preWait);
}

void SwappyCommon::postWaitCallbacks(nanoseconds cpuTime, nanoseconds gpuTime) {
    executeTracers(mInjectedTracers.postWait, cpuTime.count(), gpuTime.count());
}

void SwappyCommon::startFrameCallbacks() {
    executeTracers(mInjectedTracers.startFrame, mCurrentFrame,
                   (int64_t)mPresentationTime.time_since_epoch().count());
}

void SwappyCommon::swapIntervalChangedCallbacks() {
    executeTracers(mInjectedTracers.swapIntervalChanged);
}

void SwappyCommon::setAutoSwapInterval(bool enabled) {
    std::lock_guard<std::mutex> lock(mMutex);
    mAutoSwapIntervalEnabled = enabled;

    // non pipeline mode is not supported when auto mode is disabled
    if (!enabled) {
        mPipelineMode = PipelineMode::On;
        TRACE_INT("mPipelineMode", static_cast<int>(mPipelineMode));
    }
}

void SwappyCommon::setAutoPipelineMode(bool enabled) {
    std::lock_guard<std::mutex> lock(mMutex);
    mPipelineModeAutoMode = enabled;
    TRACE_INT("mPipelineModeAutoMode", mPipelineModeAutoMode);
    if (!enabled) {
        mPipelineMode = PipelineMode::On;
        TRACE_INT("mPipelineMode", static_cast<int>(mPipelineMode));
    }
}

void SwappyCommon::setPreferredDisplayModeId(int modeId) {
    if (!mPlatform || modeId < 0) {
        return;
    }

    mPlatform->setPreferredDisplayModeId(modeId);
}

void SwappyCommon::setPreferredRefreshPeriod(nanoseconds frameTime) {
    if (mPlatform && mPlatform->supportsSetFrameRate() && mWindow) {
        const auto votedHz = mDisplayRateController.voteFrameRateHz(frameTime, mSwapDuration);
        if (votedHz.has_value()) {
            mPlatform->setFrameRate(mWindow, *votedHz);
        }

        const auto frameRate = std::min(1e9f / frameTime.count(), 1e9f / mSwapDuration.count());
        TRACE_INT("preferredRefreshPeriod", (int)frameRate);
    } else {
        if (!mPlatform || !mSupportedRefreshPeriods) {
            return;
        }
        const auto selection =
                mDisplayRateController.selectDisplayMode(mSupportedRefreshPeriods.get(), frameTime,
                                                         mSwapDuration);
        TRACE_INT("preferredRefreshPeriod", selection.refreshPeriod.count());
        if (selection.changed) {
            setPreferredDisplayModeId(selection.modeId);
        }
    }
}

void SwappyCommon::onSettingsChanged() {
    std::lock_guard<std::mutex> lock(mMutex);

    TimingSettings timingSettings = TimingSettings::from(*Settings::getInstance());

    // If display timings has changed, cache the update and apply them on the
    // next frame
    if (timingSettings != mNextTimingSettings) {
        mNextTimingSettings = timingSettings;
        mTimingSettingsNeedUpdate = true;
    }
}

void SwappyCommon::startFrame() {
    TRACE_CALL();

    int32_t currentFrame;
    std::chrono::steady_clock::time_point currentFrameTimestamp;
    std::optional<std::chrono::nanoseconds> sfToVsyncDelay;
    {
        std::unique_lock<std::mutex> lock(mWaitingMutex);
        currentFrame = mCurrentFrame;
        currentFrameTimestamp = mCurrentFrameTimestamp;
        sfToVsyncDelay = mSfToVsyncDelay;
    }

    // Whether to add a wait to fix buffer stuffing.
    bool waitFrame = false;

    const int intervals = (mPipelineMode == PipelineMode::On) ? 2 : 1;

    // Use frame statistics to fix any buffer stuffing
    if (mBufferStuffingFixWait > 0 && mLastLatencyRecorded) {
        int32_t lastLatency = mLastLatencyRecorded();
        int expectedLatency = mAutoSwapInterval * intervals;
        if (sfToVsyncDelay) {
            expectedLatency += *sfToVsyncDelay / mCommonSettings.refreshPeriod;
        }
        TRACE_INT("ExpectedLatency", expectedLatency);
        if (mBufferStuffingFixCounter == 0) {
            if (lastLatency > expectedLatency) {
                mMissedFrameCounter++;
                if (mMissedFrameCounter >= mBufferStuffingFixWait) {
                    waitFrame = true;
                    mBufferStuffingFixCounter = 2 * lastLatency;
                    TRACE_INT("BufferStuffingFix", mBufferStuffingFixCounter);
                }
            } else {
                mMissedFrameCounter = 0;
            }
        } else {
            --mBufferStuffingFixCounter;
            TRACE_INT("BufferStuffingFix", mBufferStuffingFixCounter);
        }
    }
    mTargetFrame = currentFrame + mAutoSwapInterval;
    if (waitFrame) mTargetFrame += 1;

    // If available, use the SF to Vsync delay to target the specific
    // vsync instead of guessing when the vsync is going to be
    if (sfToVsyncDelay) {
        currentFrameTimestamp += *sfToVsyncDelay - mCommonSettings.refreshPeriod / 2 -
                mMeasuredSwapDuration.load() - 1ms;
    }

    // We compute the target time as now
    //   + the time the buffer will be on the GPU and in the queue to the
    //   compositor (1 swap period)
    mPresentationTime =
            currentFrameTimestamp + (mAutoSwapInterval * intervals) * mCommonSettings.refreshPeriod;

    mStartFrameTime = std::chrono::steady_clock::now();
    mCPUTracer.startTrace();

    startFrameCallbacks();
}

void SwappyCommon::waitUntil(int32_t target) {
    TRACE_CALL();
    std::unique_lock<std::mutex> lock(mWaitingMutex);
    mWaitingCondition.wait(lock, [&]() {
        if (mCurrentFrame < target) {
            if (!mUsingExternalChoreographer && mPlatform) {
                mPlatform->postFrameCallbacks();
            }
            return false;
        }
        return true;
    });
}

void SwappyCommon::waitUntilTargetFrame() {
    waitUntil(mTargetFrame);
}

void SwappyCommon::waitOneFrame() {
    waitUntil(mCurrentFrame + 1);
}

void SwappyCommon::setANativeWindow(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mWindow == window) {
        return;
    }

    if (mWindow != nullptr && mPlatform != nullptr) {
        mPlatform->releaseWindow(mWindow);
    }

    mWindow = window;
    if (mWindow != nullptr) {
        if (mPlatform != nullptr) {
            mPlatform->acquireWindow(mWindow);
        }
        mWindowChanged = true;
        mDisplayRateController.onWindowChanged();
    }
}

int SwappyCommon::getSupportedRefreshPeriodsNS(uint64_t* out_refreshrates, int allocated_entries) {
    if (mPlatform) {
        mSupportedRefreshPeriods = mPlatform->getSupportedRefreshPeriods();
    }

    return copyRefreshPeriods(mSupportedRefreshPeriods.get(), out_refreshrates, allocated_entries);
}

void SwappyCommon::resetFramePacing() {
    std::lock_guard<std::mutex> lock(mMutex);

    // Just set the flag here, we actually reset at the end of the frame.
    mFramePacingResetRequested = true;
}

void SwappyCommon::enableFramePacing(bool enable) {
    std::lock_guard<std::mutex> lock(mMutex);

    // Set flags that are applied at the end of the frame.
    if (mFramePacingEnabled != enable) {
        mFramePacingToggleRequested = true;
        mFramePacingResetRequested = true;
    }
}

void SwappyCommon::enableBlockingWait(bool enable) {
    std::lock_guard<std::mutex> lock(mMutex);

    mBlockingWaitEnabled = enable;
}

} // namespace swappy

#ifdef AGDK_COVERAGE
extern "C" void __gcov_dump(void) __attribute__((weak));
extern "C" void __gcov_reset(void) __attribute__((weak));
extern "C" void __gcov_flush(void) __attribute__((weak));

// Flushes and resets gcov counters for this DSO. LLVM 14+ removed __gcov_flush
// in favor of __gcov_dump() + __gcov_reset(); resetting zeroes in-memory arc
// counters so subsequent flushes or atexit destructors do not double-count.
extern "C" __attribute__((used, visibility("default"))) void Swappy_flushCoverage() {
    if (__gcov_dump) {
        __gcov_dump();
        if (__gcov_reset) {
            __gcov_reset();
        }
    } else if (__gcov_flush) {
        __gcov_flush();
    }
}
#endif // AGDK_COVERAGE

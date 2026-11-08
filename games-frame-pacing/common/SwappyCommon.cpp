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

#include <cmath>
#include <cstring>

#include "Settings.h"
#include "SwapIntervalLadder.h"
#include "Thread.h"
#include "Trace.h"

#define LOG_TAG "SwappyCommon"
#include "ClassicPacingImpl.h"
#include "SwappyLog.h"

namespace swappy {

using std::chrono::milliseconds;
using std::chrono::nanoseconds;

#if __ANDROID_API__ < 30
// Define ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_* to allow compilation on older
// versions
enum {
    /**
     * There are no inherent restrictions on the frame rate of this window.
     */
    ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT = 0,
    /**
     * This window is being used to display content with an inherently fixed
     * frame rate, e.g. a video that has a specific frame rate. When the system
     * selects a frame rate other than what the app requested, the app will need
     * to do pull down or use some other technique to adapt to the system's
     * frame rate. The user experience is likely to be worse (e.g. more frame
     * stuttering) than it would be if the system had chosen the app's requested
     * frame rate.
     */
    ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE = 1
};
#endif

bool SwappyCommonSettings::getFromApp(JNIEnv* env, jobject jactivity, SwappyCommonSettings* out) {
    if (out == nullptr) return false;

    SWAPPY_LOGI("Swappy version %d.%d", SWAPPY_MAJOR_VERSION, SWAPPY_MINOR_VERSION);

    out->sdkVersion = getSDKVersion(env);

    jclass activityClass = env->FindClass("android/app/NativeActivity");
    jclass windowManagerClass = env->FindClass("android/view/WindowManager");
    jclass displayClass = env->FindClass("android/view/Display");

    jmethodID getWindowManager =
            env->GetMethodID(activityClass, "getWindowManager", "()Landroid/view/WindowManager;");

    jmethodID getDefaultDisplay =
            env->GetMethodID(windowManagerClass, "getDefaultDisplay", "()Landroid/view/Display;");

    jobject wm = env->CallObjectMethod(jactivity, getWindowManager);
    jobject display = env->CallObjectMethod(wm, getDefaultDisplay);

    jmethodID getRefreshRate = env->GetMethodID(displayClass, "getRefreshRate", "()F");

    const float refreshRateHz = env->CallFloatMethod(display, getRefreshRate);

    jmethodID getAppVsyncOffsetNanos =
            env->GetMethodID(displayClass, "getAppVsyncOffsetNanos", "()J");

    // getAppVsyncOffsetNanos was only added in API 21.
    // Return gracefully if this device doesn't support it.
    if (getAppVsyncOffsetNanos == 0 || env->ExceptionOccurred()) {
        SWAPPY_LOGE("Error while getting method: getAppVsyncOffsetNanos");
        env->ExceptionClear();
        return false;
    }
    const long appVsyncOffsetNanos = env->CallLongMethod(display, getAppVsyncOffsetNanos);

    jmethodID getPresentationDeadlineNanos =
            env->GetMethodID(displayClass, "getPresentationDeadlineNanos", "()J");

    if (getPresentationDeadlineNanos == 0 || env->ExceptionOccurred()) {
        SWAPPY_LOGE("Error while getting method: getPresentationDeadlineNanos");
        return false;
    }

    const long vsyncPresentationDeadlineNanos =
            env->CallLongMethod(display, getPresentationDeadlineNanos);

    const long ONE_MS_IN_NS = 1000 * 1000;
    const long ONE_S_IN_NS = ONE_MS_IN_NS * 1000;

    const long vsyncPeriodNanos = static_cast<long>(ONE_S_IN_NS / refreshRateHz);
    const long sfVsyncOffsetNanos =
            vsyncPeriodNanos - (vsyncPresentationDeadlineNanos - ONE_MS_IN_NS);

    using std::chrono::nanoseconds;
    out->refreshPeriod = nanoseconds(vsyncPeriodNanos);
    out->appVsyncOffset = nanoseconds(appVsyncOffsetNanos);
    out->sfVsyncOffset = nanoseconds(sfVsyncOffsetNanos);

    return true;
}

std::unique_ptr<PacingImplementation> PacingImplementation::create() {
    return std::make_unique<ClassicPacingImplementation>();
}

SwappyCommon::SwappyCommon(JNIEnv* env, jobject jactivity)
      : mJactivity(env->NewGlobalRef(jactivity)),
        mMeasuredSwapDuration(nanoseconds(0)),
        mAutoSwapInterval(1),
        mValid(false) {
    mLibAndroid = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (mLibAndroid == nullptr) {
        SWAPPY_LOGE("FATAL: cannot open libandroid.so: %s", strerror(errno));
        return;
    }

    if (!SwappyCommonSettings::getFromApp(env, mJactivity, &mCommonSettings)) return;

    env->GetJavaVM(&mJVM);

    if (isDeviceUnsupported()) {
        SWAPPY_LOGE("Device is unsupported");
        return;
    }

    if (!SwappyDisplayManager::useSwappyDisplayManager(mCommonSettings.sdkVersion)) {
        mANativeWindow_setFrameRate = reinterpret_cast<PFN_ANativeWindow_setFrameRate>(
                dlsym(mLibAndroid, "ANativeWindow_setFrameRate"));
    }

    mChoreographerFilter = std::make_unique<
            ChoreographerFilter>(mCommonSettings.refreshPeriod,
                                 mCommonSettings.sfVsyncOffset - mCommonSettings.appVsyncOffset,
                                 [this](std::optional<std::chrono::nanoseconds> sfToVsyncDelay) {
                                     return wakeClient(sfToVsyncDelay);
                                 });

    mChoreographerThread = ChoreographerThread::createChoreographerThread(
            ChoreographerThread::Type::Swappy, mJVM, jactivity,
            [this](std::optional<std::chrono::nanoseconds> sfToVsyncDelay) {
                mChoreographerFilter->onChoreographer(sfToVsyncDelay);
            },
            [this] { onRefreshRateChanged(); }, mCommonSettings.sdkVersion);
    if (!mChoreographerThread->isInitialized()) {
        SWAPPY_LOGE("failed to initialize ChoreographerThread");
        return;
    }
    if (USE_DISPLAY_MANAGER &&
        SwappyDisplayManager::usesMinSdkOrLater(mCommonSettings.sdkVersion)) {
        mDisplayManager = std::make_unique<SwappyDisplayManager>(mJVM, jactivity);

        if (!mDisplayManager->isInitialized()) {
            mDisplayManager = nullptr;
            SWAPPY_LOGE("failed to initialize DisplayManager");
            return;
        }
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

// Used by tests
SwappyCommon::SwappyCommon(const SwappyCommonSettings& settings)
      : mJactivity(nullptr),
        mCommonSettings(settings),
        mMeasuredSwapDuration(nanoseconds(0)),
        mAutoSwapInterval(1),
        mValid(true) {
    mChoreographerFilter = std::make_unique<
            ChoreographerFilter>(mCommonSettings.refreshPeriod,
                                 mCommonSettings.sfVsyncOffset - mCommonSettings.appVsyncOffset,
                                 [this](std::optional<std::chrono::nanoseconds> sfToVsyncDelay) {
                                     return wakeClient(sfToVsyncDelay);
                                 });
    mUsingExternalChoreographer = true;
    mChoreographerThread = ChoreographerThread::createChoreographerThread(
            ChoreographerThread::Type::App, nullptr, nullptr,
            [this](std::optional<std::chrono::nanoseconds> sfToVsyncDelay) {
                mChoreographerFilter->onChoreographer(sfToVsyncDelay);
            },
            [] {}, mCommonSettings.sdkVersion);

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
}

SwappyCommon::~SwappyCommon() {
    // Remove the settings' listeners before destroying Choreographer objects
    // because the listeners may contain references to the objects
    Settings::getInstance()->removeAllListeners();
    // destroy all threads first before the other members of this class
    mChoreographerThread.reset();
    mChoreographerFilter.reset();

    Settings::reset();

    if (mJactivity != nullptr) {
        JNIEnv* env;
        mJVM->AttachCurrentThread(&env, nullptr);

        env->DeleteGlobalRef(mJactivity);
    }
}

void SwappyCommon::onRefreshRateChanged() {
    JNIEnv* env;
    mJVM->AttachCurrentThread(&env, nullptr);

    SWAPPY_LOGV("onRefreshRateChanged");

    SwappyCommonSettings settings;
    if (!SwappyCommonSettings::getFromApp(env, mJactivity, &settings)) {
        SWAPPY_LOGE("failed to query display timings");
        return;
    }

    Settings::getInstance()->setDisplayTimings(
            {settings.refreshPeriod, settings.appVsyncOffset, settings.sfVsyncOffset});
    SWAPPY_LOGV("onRefreshRateChanged: refresh rate: %.0fHz",
                1e9f / settings.refreshPeriod.count());
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

void SwappyCommon::onChoreographer(int64_t frameTimeNanos) {
    TRACE_CALL();

    if (!mUsingExternalChoreographer) {
        mUsingExternalChoreographer = true;
        if (mDisplayManager) {
            mDisplayManager->enableRefreshCallbackForwarding();
        }
        mChoreographerThread = ChoreographerThread::createChoreographerThread(
                ChoreographerThread::Type::App, nullptr, nullptr,
                [this](std::optional<std::chrono::nanoseconds> sfToVsyncDelay) {
                    mChoreographerFilter->onChoreographer(sfToVsyncDelay);
                },
                [this] { onRefreshRateChanged(); }, mCommonSettings.sdkVersion);
    }

    mChoreographerThread->postFrameCallbacks();
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
    if (mDisplayManager) {
        mSupportedRefreshPeriods = mDisplayManager->getSupportedRefreshPeriods();
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
    if (!mUsingExternalChoreographer) {
        mChoreographerThread->postFrameCallbacks();
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
    if (!mDisplayManager || modeId < 0) {
        return;
    }

    mDisplayManager->setPreferredDisplayModeId(modeId);
    SWAPPY_LOGV("setPreferredDisplayModeId set to %d", modeId);
}

void SwappyCommon::setPreferredRefreshPeriod(nanoseconds frameTime) {
    if (mANativeWindow_setFrameRate && mWindow) {
        const auto votedHz = mDisplayRateController.voteFrameRateHz(frameTime, mSwapDuration);
        if (votedHz.has_value()) {
            SWAPPY_LOGV("ANativeWindow_setFrameRate(%.2f)", *votedHz);
            mANativeWindow_setFrameRate(mWindow, *votedHz,
                                        ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
        }

        const auto frameRate = std::min(1e9f / frameTime.count(), 1e9f / mSwapDuration.count());
        TRACE_INT("preferredRefreshPeriod", (int)frameRate);
    } else {
        if (!mDisplayManager || !mSupportedRefreshPeriods) {
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
            if (!mUsingExternalChoreographer) {
                mChoreographerThread->postFrameCallbacks();
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

SdkVersion SwappyCommonSettings::getSDKVersion(JNIEnv* env) {
    const jclass buildClass = env->FindClass("android/os/Build$VERSION");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        SWAPPY_LOGE("Failed to get Build.VERSION class");
        return SdkVersion{0, 0};
    }

    const jfieldID sdkInt = env->GetStaticFieldID(buildClass, "SDK_INT", "I");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        SWAPPY_LOGE("Failed to get Build.VERSION.SDK_INT field");
        return SdkVersion{0, 0};
    }

    const jint sdk = env->GetStaticIntField(buildClass, sdkInt);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        SWAPPY_LOGE("Failed to get SDK version");
        return SdkVersion{0, 0};
    }

    jint sdkPreview = 0;
    if (sdk >= 23) {
        const jfieldID previewSdkInt = env->GetStaticFieldID(buildClass, "PREVIEW_SDK_INT", "I");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            SWAPPY_LOGE("Failed to get Build.VERSION.PREVIEW_SDK_INT field");
        }

        sdkPreview = env->GetStaticIntField(buildClass, previewSdkInt);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            SWAPPY_LOGE("Failed to get preview SDK version");
        }
    }

    SWAPPY_LOGI("SDK version = %d preview = %d", sdk, sdkPreview);
    return SdkVersion{sdk, sdkPreview};
}

void SwappyCommon::setANativeWindow(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mWindow == window) {
        return;
    }

    if (mWindow != nullptr) {
        ANativeWindow_release(mWindow);
    }

    mWindow = window;
    if (mWindow != nullptr) {
        ANativeWindow_acquire(mWindow);
        mWindowChanged = true;
        mDisplayRateController.onWindowChanged();
    }
}

namespace {

static std::string GetStaticStringField(JNIEnv* env, jclass clz, const char* name) {
    const jfieldID fieldId = env->GetStaticFieldID(clz, name, "Ljava/lang/String;");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        SWAPPY_LOGE("Failed to get string field %s", name);
        return "";
    }

    const jstring jstr = (jstring)env->GetStaticObjectField(clz, fieldId);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        SWAPPY_LOGE("Failed to get string %s", name);
        return "";
    }
    auto cstr = env->GetStringUTFChars(jstr, nullptr);
    auto length = env->GetStringUTFLength(jstr);
    std::string retValue(cstr, length);
    env->ReleaseStringUTFChars(jstr, cstr);
    env->DeleteLocalRef(jstr);
    return retValue;
}

struct DeviceIdentifier {
    std::string manufacturer;
    std::string model;
    std::string display;
    // Empty fields match against any value and we match the beginning of the
    // input, e.g.
    //  A37 matches A37f, A37fw, etc.
    bool match(const std::string& manufacturer_in, const std::string& model_in,
               const std::string& display_in) {
        if (!matchStartOfString(manufacturer, manufacturer_in)) return false;
        if (!matchStartOfString(model, model_in)) return false;
        if (!matchStartOfString(display, display_in)) return false;
        return true;
    }
    bool matchStartOfString(const std::string& start, const std::string& sample) {
        return start.empty() || start == sample.substr(0, start.length());
    }
};

} // anonymous namespace

bool SwappyCommon::isDeviceUnsupported() {
    JNIEnv* env;
    mJVM->AttachCurrentThread(&env, nullptr);

    // List of unsupported models
    static std::vector<DeviceIdentifier> unsupportedDevices = {{"OPPO", "A37", ""}};

    const jclass buildClass = env->FindClass("android/os/Build");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        SWAPPY_LOGE("Failed to get Build class");
        return false;
    }

    auto manufacturer = GetStaticStringField(env, buildClass, "MANUFACTURER");
    if (manufacturer.empty()) return false;

    auto model = GetStaticStringField(env, buildClass, "MODEL");
    if (model.empty()) return false;

    auto display = GetStaticStringField(env, buildClass, "DISPLAY");
    if (display.empty()) return false;

    for (auto& device : unsupportedDevices) {
        if (device.match(manufacturer, model, display)) return true;
    }

    return false;
}

int SwappyCommon::getSupportedRefreshPeriodsNS(uint64_t* out_refreshrates, int allocated_entries) {
    if (mDisplayManager) {
        mSupportedRefreshPeriods = mDisplayManager->getSupportedRefreshPeriods();
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

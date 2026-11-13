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

#include "AndroidSwappyPlatform.h"

#include <android/native_window.h>
#include <dlfcn.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "ChoreographerThread.h"
#include "Settings.h"
#include "SwappyDisplayManager.h"
#include "SwappyJniUtils.h"
#include "swappy/swappy_common.h"

#define LOG_TAG "SwappyCommon"
#include "SwappyLog.h"

namespace swappy {

namespace {

#if __ANDROID_API__ < 30
// Define ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_* to allow compilation on older
// versions.
enum {
    ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT = 0,
    ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE = 1,
};
#endif

// ANativeWindow_setFrameRate is supported from API 30. To allow compilation for
// minSDK < 30 we need runtime support to call this API.
using PFN_ANativeWindow_setFrameRate = int32_t (*)(ANativeWindow* window, float frameRate,
                                                   int8_t compatibility);

SdkVersion getSdkVersion(JNIEnv* env) {
    ScopedLocalRef<jclass> buildClass(env, env->FindClass("android/os/Build$VERSION"));
    if (env->ExceptionCheck() || buildClass.get() == nullptr) {
        env->ExceptionClear();
        SWAPPY_LOGE("Failed to get Build.VERSION class");
        return SdkVersion{0, 0};
    }

    const jfieldID sdkInt = env->GetStaticFieldID(buildClass.get(), "SDK_INT", "I");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        SWAPPY_LOGE("Failed to get Build.VERSION.SDK_INT field");
        return SdkVersion{0, 0};
    }

    const jint sdk = env->GetStaticIntField(buildClass.get(), sdkInt);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        SWAPPY_LOGE("Failed to get SDK version");
        return SdkVersion{0, 0};
    }

    jint sdkPreview = 0;
    if (sdk >= 23) {
        const jfieldID previewSdkInt =
                env->GetStaticFieldID(buildClass.get(), "PREVIEW_SDK_INT", "I");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            SWAPPY_LOGE("Failed to get Build.VERSION.PREVIEW_SDK_INT field");
        }

        sdkPreview = env->GetStaticIntField(buildClass.get(), previewSdkInt);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            SWAPPY_LOGE("Failed to get preview SDK version");
        }
    }

    SWAPPY_LOGI("SDK version = %d preview = %d", sdk, sdkPreview);
    return SdkVersion{sdk, sdkPreview};
}

bool getSettingsFromApp(JNIEnv* env, jobject jactivity, SwappyCommonSettings* out) {
    if (out == nullptr) {
        return false;
    }

    ScopedLocalRef<jclass> activityClass(env, env->FindClass("android/app/NativeActivity"));
    ScopedLocalRef<jclass> windowManagerClass(env, env->FindClass("android/view/WindowManager"));
    ScopedLocalRef<jclass> displayClass(env, env->FindClass("android/view/Display"));

    jmethodID getWindowManager = env->GetMethodID(activityClass.get(), "getWindowManager",
                                                  "()Landroid/view/WindowManager;");

    jmethodID getDefaultDisplay = env->GetMethodID(windowManagerClass.get(), "getDefaultDisplay",
                                                   "()Landroid/view/Display;");

    ScopedLocalRef<jobject> wm(env, env->CallObjectMethod(jactivity, getWindowManager));
    ScopedLocalRef<jobject> display(env, env->CallObjectMethod(wm.get(), getDefaultDisplay));

    jmethodID getRefreshRate = env->GetMethodID(displayClass.get(), "getRefreshRate", "()F");

    const float refreshRateHz = env->CallFloatMethod(display.get(), getRefreshRate);

    jmethodID getAppVsyncOffsetNanos =
            env->GetMethodID(displayClass.get(), "getAppVsyncOffsetNanos", "()J");

    // getAppVsyncOffsetNanos was only added in API 21.
    // Return gracefully if this device doesn't support it.
    if (getAppVsyncOffsetNanos == 0 || env->ExceptionCheck()) {
        SWAPPY_LOGE("Error while getting method: getAppVsyncOffsetNanos");
        env->ExceptionClear();
        return false;
    }
    const long appVsyncOffsetNanos = env->CallLongMethod(display.get(), getAppVsyncOffsetNanos);

    jmethodID getPresentationDeadlineNanos =
            env->GetMethodID(displayClass.get(), "getPresentationDeadlineNanos", "()J");

    if (getPresentationDeadlineNanos == 0 || env->ExceptionCheck()) {
        SWAPPY_LOGE("Error while getting method: getPresentationDeadlineNanos");
        return false;
    }

    const long vsyncPresentationDeadlineNanos =
            env->CallLongMethod(display.get(), getPresentationDeadlineNanos);

    constexpr long kOneMsInNs = 1000 * 1000;
    constexpr long kOneSInNs = kOneMsInNs * 1000;

    const long vsyncPeriodNanos = static_cast<long>(kOneSInNs / refreshRateHz);
    const long sfVsyncOffsetNanos =
            vsyncPeriodNanos - (vsyncPresentationDeadlineNanos - kOneMsInNs);

    using std::chrono::nanoseconds;
    out->refreshPeriod = nanoseconds(vsyncPeriodNanos);
    out->appVsyncOffset = nanoseconds(appVsyncOffsetNanos);
    out->sfVsyncOffset = nanoseconds(sfVsyncOffsetNanos);

    return true;
}

std::string getStaticStringField(JNIEnv* env, jclass clz, const char* name) {
    const jfieldID fieldId = env->GetStaticFieldID(clz, name, "Ljava/lang/String;");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        SWAPPY_LOGE("Failed to get string field %s", name);
        return "";
    }

    ScopedLocalRef<jstring> jstr(env,
                                 static_cast<jstring>(env->GetStaticObjectField(clz, fieldId)));
    if (env->ExceptionCheck() || jstr.get() == nullptr) {
        env->ExceptionClear();
        SWAPPY_LOGE("Failed to get string %s", name);
        return "";
    }
    const char* cstr = env->GetStringUTFChars(jstr.get(), nullptr);
    const jsize length = env->GetStringUTFLength(jstr.get());
    std::string retValue(cstr, length);
    env->ReleaseStringUTFChars(jstr.get(), cstr);
    return retValue;
}

struct DeviceIdentifier {
    std::string manufacturer;
    std::string model;
    std::string display;

    // Empty fields match against any value and we match the beginning of the
    // input, e.g. A37 matches A37f, A37fw, etc.
    bool match(const std::string& manufacturerIn, const std::string& modelIn,
               const std::string& displayIn) const {
        return matchStartOfString(manufacturer, manufacturerIn) &&
                matchStartOfString(model, modelIn) && matchStartOfString(display, displayIn);
    }

    static bool matchStartOfString(const std::string& start, const std::string& sample) {
        return start.empty() || start == sample.substr(0, start.length());
    }
};

bool isDeviceUnsupported(JavaVM* jvm) {
    if (jvm == nullptr) {
        return false;
    }
    JNIEnv* env = nullptr;
    jvm->AttachCurrentThread(&env, nullptr);
    if (env == nullptr) {
        return false;
    }

    static const std::vector<DeviceIdentifier> kUnsupportedDevices = {{"OPPO", "A37", ""}};

    ScopedLocalRef<jclass> buildClass(env, env->FindClass("android/os/Build"));
    if (env->ExceptionCheck() || buildClass.get() == nullptr) {
        env->ExceptionClear();
        SWAPPY_LOGE("Failed to get Build class");
        return false;
    }

    const std::string manufacturer = getStaticStringField(env, buildClass.get(), "MANUFACTURER");
    if (manufacturer.empty()) {
        return false;
    }

    const std::string model = getStaticStringField(env, buildClass.get(), "MODEL");
    if (model.empty()) {
        return false;
    }

    const std::string display = getStaticStringField(env, buildClass.get(), "DISPLAY");
    if (display.empty()) {
        return false;
    }

    for (const auto& device : kUnsupportedDevices) {
        if (device.match(manufacturer, model, display)) {
            return true;
        }
    }

    return false;
}

class AndroidChoreographerPlatform : public SwappyPlatform {
public:
    void postFrameCallbacks() final {
        if (mChoreographerThread) {
            mChoreographerThread->postFrameCallbacks();
        }
    }

    void acquireWindow(ANativeWindow* window) final {
        ANativeWindow_acquire(window);
    }

    void releaseWindow(ANativeWindow* window) final {
        ANativeWindow_release(window);
    }

protected:
    [[nodiscard]] bool initChoreographerThread(
            ChoreographerThread::Type type, JavaVM* jvm, jobject jactivity,
            ChoreographerCallback onChoreographer,
            ChoreographerThread::RefreshRateChangedCallback onRefreshRateChanged,
            SdkVersion sdkVersion) {
        mChoreographerThread =
                ChoreographerThread::createChoreographerThread(type, jvm, jactivity,
                                                               std::move(onChoreographer),
                                                               std::move(onRefreshRateChanged),
                                                               sdkVersion);
        return mChoreographerThread && mChoreographerThread->isInitialized();
    }

    void resetChoreographerThread() {
        mChoreographerThread.reset();
    }

private:
    std::unique_ptr<ChoreographerThread> mChoreographerThread;
};

class AppSwappyPlatform final : public AndroidChoreographerPlatform {
public:
    explicit AppSwappyPlatform(const SwappyCommonSettings& settings) : mSettings(settings) {}

    [[nodiscard]] bool isValid() const override {
        return true;
    }

    [[nodiscard]] SwappyCommonSettings getInitialSettings() const override {
        return mSettings;
    }

    [[nodiscard]] bool startsWithExternalChoreographer() const override {
        return true;
    }

    [[nodiscard]] bool initializeChoreographer(ChoreographerCallback onChoreographer) override {
        return initChoreographerThread(
                ChoreographerThread::Type::App, nullptr, nullptr, std::move(onChoreographer), [] {},
                mSettings.sdkVersion);
    }

    void switchToExternalChoreographer(ChoreographerCallback /*onChoreographer*/) override {}

    [[nodiscard]] bool supportsSetFrameRate() const override {
        return false;
    }

    void setFrameRate(ANativeWindow* /*window*/, float /*frameRate*/) override {}

    [[nodiscard]] std::shared_ptr<RefreshPeriodMap> getSupportedRefreshPeriods() override {
        return nullptr;
    }

    void setPreferredDisplayModeId(int /*modeId*/) override {}

private:
    SwappyCommonSettings mSettings{};
};

class AndroidSwappyPlatform final : public AndroidChoreographerPlatform {
public:
    AndroidSwappyPlatform(JNIEnv* env, jobject jactivity) {
        SWAPPY_LOGI("Swappy version %d.%d", SWAPPY_MAJOR_VERSION, SWAPPY_MINOR_VERSION);

        if (env == nullptr || jactivity == nullptr) {
            return;
        }

        mJactivity = env->NewGlobalRef(jactivity);
        env->GetJavaVM(&mJVM);

        mLibAndroid = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (mLibAndroid == nullptr) {
            SWAPPY_LOGE("FATAL: cannot open libandroid.so: %s", strerror(errno));
            return;
        }

        mSettings.sdkVersion = getSdkVersion(env);
        if (!getSettingsFromApp(env, mJactivity, &mSettings)) {
            return;
        }

        if (isDeviceUnsupported(mJVM)) {
            SWAPPY_LOGE("Device is unsupported");
            return;
        }

        if (!SwappyDisplayManager::useSwappyDisplayManager(mSettings.sdkVersion)) {
            mANativeWindowSetFrameRate = reinterpret_cast<PFN_ANativeWindow_setFrameRate>(
                    dlsym(mLibAndroid, "ANativeWindow_setFrameRate"));
        }

        mValid = true;
    }

    ~AndroidSwappyPlatform() override {
        // Stop the Choreographer thread before tearing down DisplayManager, JNI refs, and dlopen.
        resetChoreographerThread();
        mDisplayManager.reset();

        if (mJactivity != nullptr && mJVM != nullptr) {
            JNIEnv* env = nullptr;
            mJVM->AttachCurrentThread(&env, nullptr);
            if (env != nullptr) {
                env->DeleteGlobalRef(mJactivity);
            }
        }

        if (mLibAndroid != nullptr) {
            dlclose(mLibAndroid);
        }
    }

    [[nodiscard]] bool isValid() const override {
        return mValid;
    }

    [[nodiscard]] SwappyCommonSettings getInitialSettings() const override {
        return mSettings;
    }

    [[nodiscard]] bool initializeChoreographer(ChoreographerCallback onChoreographer) override {
        return initChoreographerThread(
                ChoreographerThread::Type::Swappy, mJVM, mJactivity, std::move(onChoreographer),
                [this] { onRefreshRateChanged(); }, mSettings.sdkVersion);
    }

    void switchToExternalChoreographer(ChoreographerCallback onChoreographer) override {
        if (mDisplayManager) {
            mDisplayManager->enableRefreshCallbackForwarding();
        }
        (void)initChoreographerThread(
                ChoreographerThread::Type::App, nullptr, nullptr, std::move(onChoreographer), [] {},
                mSettings.sdkVersion);
    }

    [[nodiscard]] bool initializeDisplayManager() override {
        if (SwappyDisplayManager::usesMinSdkOrLater(mSettings.sdkVersion)) {
            mDisplayManager = std::make_unique<SwappyDisplayManager>(mJVM, mJactivity);
            if (!mDisplayManager->isInitialized()) {
                mDisplayManager = nullptr;
                SWAPPY_LOGE("failed to initialize DisplayManager");
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool supportsSetFrameRate() const override {
        return mANativeWindowSetFrameRate != nullptr;
    }

    void setFrameRate(ANativeWindow* window, float frameRate) override {
        if (mANativeWindowSetFrameRate != nullptr && window != nullptr) {
            SWAPPY_LOGV("ANativeWindow_setFrameRate(%.2f)", frameRate);
            mANativeWindowSetFrameRate(window, frameRate,
                                       ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
        }
    }

    [[nodiscard]] std::shared_ptr<RefreshPeriodMap> getSupportedRefreshPeriods() override {
        return mDisplayManager ? mDisplayManager->getSupportedRefreshPeriods() : nullptr;
    }

    void setPreferredDisplayModeId(int modeId) override {
        if (!mDisplayManager || modeId < 0) {
            return;
        }
        mDisplayManager->setPreferredDisplayModeId(modeId);
        SWAPPY_LOGV("setPreferredDisplayModeId set to %d", modeId);
    }

private:
    void onRefreshRateChanged() {
        if (mJVM == nullptr || mJactivity == nullptr) {
            return;
        }
        JNIEnv* env = nullptr;
        mJVM->AttachCurrentThread(&env, nullptr);
        if (env == nullptr) {
            return;
        }

        SWAPPY_LOGV("onRefreshRateChanged");

        SwappyCommonSettings settings{};
        if (!getSettingsFromApp(env, mJactivity, &settings)) {
            SWAPPY_LOGE("failed to query display timings");
            return;
        }

        Settings::getInstance()->setDisplayTimings(
                {settings.refreshPeriod, settings.appVsyncOffset, settings.sfVsyncOffset});
        SWAPPY_LOGV("onRefreshRateChanged: refresh rate: %.0fHz",
                    1e9f / settings.refreshPeriod.count());
    }

    jobject mJactivity = nullptr;
    JavaVM* mJVM = nullptr;
    void* mLibAndroid = nullptr;
    PFN_ANativeWindow_setFrameRate mANativeWindowSetFrameRate = nullptr;
    SwappyCommonSettings mSettings{};
    std::unique_ptr<SwappyDisplayManager> mDisplayManager;
    bool mValid = false;
};

} // namespace

std::unique_ptr<SwappyPlatform> createAndroidSwappyPlatform(JNIEnv* env, jobject jactivity) {
    return std::make_unique<AndroidSwappyPlatform>(env, jactivity);
}

std::unique_ptr<SwappyPlatform> createAppSwappyPlatform(const SwappyCommonSettings& settings) {
    return std::make_unique<AppSwappyPlatform>(settings);
}

} // namespace swappy

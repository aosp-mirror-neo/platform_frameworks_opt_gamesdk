#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <dlfcn.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <media/NdkImageReader.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "ChoreographerShim.h"
#include "SwappyDisplayManager.h"
#include "gcov_flush_environment.h"
#include "swappy/swappyGL.h"
#include "swappy/swappyGL_extra.h"
#include "swappy/swappyVk.h"
#include "vulkan/SwappyVk.h"
#include "vulkan/SwappyVkBase.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "swappy_test", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "swappy_test", __VA_ARGS__)

#define VK_CHECK(result) ASSERT_EQ(VK_SUCCESS, result)

#ifdef NATIVE_COVERAGE
#include <android/log.h>
#include <stdlib.h>
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);
extern "C" void Swappy_dumpCoverage();

class CoverageEnvironment : public ::testing::Environment {
public:
    ~CoverageEnvironment() override {}
    void TearDown() override {
        const char* coverage_dir = getenv("COVERAGE_DIR");
        std::string path = coverage_dir
                ? std::string(coverage_dir) + "/swappy_coverage.profraw"
                : "/data/data/com.swappy.testapp/cache/swappy_coverage.profraw";
        __llvm_profile_set_filename(path.c_str());
        int ret = __llvm_profile_write_file();
        __android_log_print(ANDROID_LOG_INFO, "SwappyTest", "LLVM profile write returned %d", ret);
    }
};
static const ::testing::Environment* const coverage_env =
        ::testing::AddGlobalTestEnvironment(new CoverageEnvironment);
#endif

namespace swappy {
class SwappyDisplayManagerJNI {
public:
    static void onSetSupportedRefreshPeriods(
            jlong, std::shared_ptr<SwappyDisplayManager::RefreshPeriodMap>);
    static void onRefreshPeriodChanged(jlong, long, long, long);
};
} // namespace swappy

namespace {

std::mutex g_refreshRateMutex;
AChoreographer_refreshRateCallback g_refreshRateCallback = nullptr;
void* g_refreshRateData = nullptr;
std::atomic<bool> g_mockThreadRunning{false};
std::atomic<int> g_mockRunningThreads{0};
std::mutex g_mockThreadsMutex;
std::vector<std::thread> g_mockThreads;
std::condition_variable g_mockThreadsCv;

struct MockFrameData {
    int64_t expectedPresentationTimeNanos;
    int64_t deadlineNanos;
};

} // namespace

extern "C" {

void setupMockNDKChoreographer() {
    LOGI("[MockChoreographer] setupMockNDKChoreographer");
    g_mockThreadRunning = true;
}

void cleanupMockNDKChoreographer() {
    LOGI("[MockChoreographer] cleanupMockNDKChoreographer");
    g_mockThreadRunning = false;
    g_mockThreadsCv.notify_all();

    {
        std::lock_guard<std::mutex> lock(g_refreshRateMutex);
        g_refreshRateCallback = nullptr;
        g_refreshRateData = nullptr;
    }

    std::unique_lock<std::mutex> lock(g_mockThreadsMutex);
    g_mockThreadsCv.wait(lock, [] { return g_mockRunningThreads.load() == 0; });
    std::vector<std::thread> toJoin;
    toJoin.swap(g_mockThreads);
    lock.unlock();

    for (auto& t : toJoin) {
        if (t.joinable()) {
            t.join();
        }
    }
}

static int g_dummyChoreographer = 0;
// Mock implementation of functions resolved via dlsym
AChoreographer* Mock_AChoreographer_getInstance() {
    return reinterpret_cast<AChoreographer*>(&g_dummyChoreographer);
}

void Mock_AChoreographer_postFrameCallbackDelayed(AChoreographer* choreographer,
                                                  AChoreographer_frameCallback callback, void* data,
                                                  long delay) {
    LOGI("[MockChoreographer] Mock_AChoreographer_postFrameCallbackDelayed called");
    std::lock_guard<std::mutex> lock(g_mockThreadsMutex);
    if (!g_mockThreadRunning.load()) {
        return;
    }
    g_mockRunningThreads++;
    g_mockThreads.emplace_back([callback, data, delay]() {
        struct ThreadExit {
            ~ThreadExit() {
                std::lock_guard<std::mutex> lock(g_mockThreadsMutex);
                g_mockRunningThreads--;
                g_mockThreadsCv.notify_all();
            }
        } onExit;

        std::unique_lock<std::mutex> cvLock(g_mockThreadsMutex);
        // Default to 16ms to simulate a 60Hz frame timeline if delay is 0
        g_mockThreadsCv.wait_for(cvLock, std::chrono::milliseconds(delay > 0 ? delay : 16),
                                 [] { return !g_mockThreadRunning.load(); });
        cvLock.unlock();

        if (g_mockThreadRunning) {
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            long timeNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
            callback(timeNanos, data);
        }
    });
}

bool triggerMockRefreshRateCallback(int64_t vsyncPeriodNanos) {
    AChoreographer_refreshRateCallback callback = nullptr;
    void* data = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_refreshRateMutex);
        callback = g_refreshRateCallback;
        data = g_refreshRateData;
    }
    if (callback == nullptr) {
        return false;
    }
    callback(vsyncPeriodNanos, data);
    return true;
}

void Mock_AChoreographer_registerRefreshRateCallback(AChoreographer* choreographer,
                                                     AChoreographer_refreshRateCallback callback,
                                                     void* data) {
    LOGI("[MockChoreographer] Mock_AChoreographer_registerRefreshRateCallback called");
    std::lock_guard<std::mutex> lock(g_refreshRateMutex);
    g_refreshRateCallback = callback;
    g_refreshRateData = data;
}

void Mock_AChoreographer_unregisterRefreshRateCallback(AChoreographer* choreographer,
                                                       AChoreographer_refreshRateCallback callback,
                                                       void* data) {
    LOGI("[MockChoreographer] Mock_AChoreographer_unregisterRefreshRateCallback called");
    std::lock_guard<std::mutex> lock(g_refreshRateMutex);
    if (g_refreshRateCallback == callback && g_refreshRateData == data) {
        g_refreshRateCallback = nullptr;
        g_refreshRateData = nullptr;
    }
}

void Mock_AChoreographer_postVsyncCallback(AChoreographer* choreographer,
                                           AChoreographer_vsyncCallback callback, void* data) {
    LOGI("[MockChoreographer] Mock_AChoreographer_postVsyncCallback called");
    std::lock_guard<std::mutex> lock(g_mockThreadsMutex);
    if (!g_mockThreadRunning.load()) {
        return;
    }
    g_mockRunningThreads++;
    g_mockThreads.emplace_back([callback, data]() {
        struct ThreadExit {
            ~ThreadExit() {
                std::lock_guard<std::mutex> lock(g_mockThreadsMutex);
                g_mockRunningThreads--;
                g_mockThreadsCv.notify_all();
            }
        } onExit;

        std::unique_lock<std::mutex> cvLock(g_mockThreadsMutex);
        // Default to 16ms to simulate a 60Hz frame timeline
        g_mockThreadsCv.wait_for(cvLock, std::chrono::milliseconds(16),
                                 [] { return !g_mockThreadRunning.load(); });
        cvLock.unlock();

        if (g_mockThreadRunning) {
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            int64_t timeNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
            MockFrameData frameData;
            frameData.expectedPresentationTimeNanos = timeNanos + 16666666;
            frameData.deadlineNanos = timeNanos + 8333333;
            callback(reinterpret_cast<const AChoreographerFrameCallbackData*>(&frameData), data);
        }
    });
}

size_t Mock_AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(
        const AChoreographerFrameCallbackData* /* data */) {
    return 0;
}

int64_t Mock_AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(
        const AChoreographerFrameCallbackData* data, size_t /* index */) {
    return reinterpret_cast<const MockFrameData*>(data)->expectedPresentationTimeNanos;
}

int64_t Mock_AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(
        const AChoreographerFrameCallbackData* data, size_t /* index */) {
    return reinterpret_cast<const MockFrameData*>(data)->deadlineNanos;
}

} // extern "C"

#ifdef __ANDROID__
// Forward declarations for real functions
extern "C" {
void* __real_dlopen(const char* filename, int flag);
void* __real_dlsym(void* handle, const char* symbol);
int __real_dlclose(void* handle);
}

namespace {
void* g_realLibAndroid = nullptr;
std::once_flag g_realLibAndroidOnce;

void* getRealLibAndroid() {
    std::call_once(g_realLibAndroidOnce, []() {
        g_realLibAndroid = __real_dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    });
    return g_realLibAndroid;
}
} // namespace

extern "C" {

// Intercepts dlopen calls via the -Wl,--wrap=dlopen linker flag defined in CMakeLists.txt.
// This allows us to inject mocked AChoreographer symbols for libandroid.so on SDK 37+.
void* __wrap_dlopen(const char* filename, int flag) {
    LOGI("[__wrap_dlopen] dlopen called for: %s", filename ? filename : "nullptr");
    if (filename && strcmp(filename, "libandroid.so") == 0) {
        LOGI("[__wrap_dlopen] Intercepted libandroid.so");
        return reinterpret_cast<void*>(0x1234abcd);
    }
    return __real_dlopen(filename, flag);
}

void* __wrap_dlsym(void* handle, const char* symbol) {
    LOGI("[__wrap_dlsym] dlsym called for symbol: %s", symbol);
    if (handle == reinterpret_cast<void*>(0x1234abcd)) {
        LOGI("[__wrap_dlsym] Resolving mock symbol: %s", symbol);
#define MOCK_SYM(name) \
    if (strcmp(symbol, #name) == 0) return reinterpret_cast<void*>(Mock_##name)
        MOCK_SYM(AChoreographer_getInstance);
        MOCK_SYM(AChoreographer_postFrameCallbackDelayed);
        MOCK_SYM(AChoreographer_registerRefreshRateCallback);
        MOCK_SYM(AChoreographer_unregisterRefreshRateCallback);
        MOCK_SYM(AChoreographer_postVsyncCallback);
        MOCK_SYM(AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex);
        MOCK_SYM(AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos);
        MOCK_SYM(AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos);
#undef MOCK_SYM

        void* realLib = getRealLibAndroid();
        if (!realLib) {
            LOGI("[__wrap_dlsym] Failed to load real libandroid.so for symbol resolution");
            return nullptr;
        }
        return __real_dlsym(realLib, symbol);
    }
    return __real_dlsym(handle, symbol);
}

int __wrap_dlclose(void* handle) {
    if (handle == reinterpret_cast<void*>(0x1234abcd)) {
        return 0;
    }
    return __real_dlclose(handle);
}

} // extern "C"
#endif // __ANDROID__

namespace android {

namespace swappytest {

class MockJNI {
public:
    MOCK_METHOD(jclass, FindClass, (JNIEnv*, const char*));
    MOCK_METHOD(jmethodID, GetMethodID, (JNIEnv*, jclass, const char*, const char*));
    MOCK_METHOD(jfieldID, GetStaticFieldID, (JNIEnv*, jclass, const char*, const char*));
    MOCK_METHOD(jint, GetStaticIntField, (JNIEnv*, jclass, jfieldID));
    MOCK_METHOD(jobject, CallObjectMethodV, (JNIEnv*, jobject, jmethodID, va_list));
    MOCK_METHOD(jfloat, CallFloatMethodV, (JNIEnv*, jobject, jmethodID, va_list));
    MOCK_METHOD(jlong, CallLongMethodV, (JNIEnv*, jobject, jmethodID, va_list));
    MOCK_METHOD(jboolean, ExceptionCheck, (JNIEnv*));
    MOCK_METHOD(jthrowable, ExceptionOccurred, (JNIEnv*));
    MOCK_METHOD(void, ExceptionClear, (JNIEnv*));
    MOCK_METHOD(jint, GetJavaVM, (JNIEnv*, JavaVM**));
    MOCK_METHOD(jobject, NewGlobalRef, (JNIEnv*, jobject));
    MOCK_METHOD(void, DeleteGlobalRef, (JNIEnv*, jobject));
    MOCK_METHOD(jobject, GetStaticObjectField, (JNIEnv*, jclass, jfieldID));
    MOCK_METHOD(const char*, GetStringUTFChars, (JNIEnv*, jstring, jboolean*));
    MOCK_METHOD(jsize, GetStringUTFLength, (JNIEnv*, jstring));
    MOCK_METHOD(void, ReleaseStringUTFChars, (JNIEnv*, jstring, const char*));
    MOCK_METHOD(void, DeleteLocalRef, (JNIEnv*, jobject));
    MOCK_METHOD(jclass, GetObjectClass, (JNIEnv*, jobject));
    MOCK_METHOD(jstring, NewStringUTF, (JNIEnv*, const char*));
    MOCK_METHOD(jobject, NewObjectV, (JNIEnv*, jclass, jmethodID, va_list));
    MOCK_METHOD(jint, RegisterNatives, (JNIEnv*, jclass, const JNINativeMethod*, jint));
    MOCK_METHOD(void, CallVoidMethodV, (JNIEnv*, jobject, jmethodID, va_list));
};

static MockJNI* gMockJni = nullptr;
static std::atomic<int> gCreatedLocalRefs{0};
static std::atomic<int> gDeletedLocalRefs{0};

static jclass MockFindClass(JNIEnv* env, const char* name) {
    jclass res = gMockJni->FindClass(env, name);
    if (res != nullptr) {
        gCreatedLocalRefs++;
    }
    return res;
}
static jmethodID MockGetMethodID(JNIEnv* env, jclass clazz, const char* name, const char* sig) {
    return gMockJni->GetMethodID(env, clazz, name, sig);
}
static jfieldID MockGetStaticFieldID(JNIEnv* env, jclass clazz, const char* name, const char* sig) {
    return gMockJni->GetStaticFieldID(env, clazz, name, sig);
}
static jint MockGetStaticIntField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    return gMockJni->GetStaticIntField(env, clazz, fieldID);
}
static jobject MockCallObjectMethodV(JNIEnv* env, jobject obj, jmethodID methodID, va_list args) {
    jobject res = gMockJni->CallObjectMethodV(env, obj, methodID, args);
    if (res != nullptr) {
        gCreatedLocalRefs++;
    }
    return res;
}
static jfloat MockCallFloatMethodV(JNIEnv* env, jobject obj, jmethodID methodID, va_list args) {
    return gMockJni->CallFloatMethodV(env, obj, methodID, args);
}
static jlong MockCallLongMethodV(JNIEnv* env, jobject obj, jmethodID methodID, va_list args) {
    return gMockJni->CallLongMethodV(env, obj, methodID, args);
}
static void MockCallVoidMethodV(JNIEnv* env, jobject obj, jmethodID methodID, va_list args) {
    gMockJni->CallVoidMethodV(env, obj, methodID, args);
}
static jboolean MockExceptionCheck(JNIEnv* env) {
    return gMockJni->ExceptionCheck(env);
}
static jthrowable MockExceptionOccurred(JNIEnv* env) {
    jthrowable res = gMockJni->ExceptionOccurred(env);
    if (res != nullptr) {
        gCreatedLocalRefs++;
    }
    return res;
}
static void MockExceptionClear(JNIEnv* env) {
    gMockJni->ExceptionClear(env);
}
static jint MockGetJavaVM(JNIEnv* env, JavaVM** vm) {
    return gMockJni->GetJavaVM(env, vm);
}
static jobject MockNewGlobalRef(JNIEnv* env, jobject obj) {
    return gMockJni->NewGlobalRef(env, obj);
}
static void MockDeleteGlobalRef(JNIEnv* env, jobject obj) {
    gMockJni->DeleteGlobalRef(env, obj);
}
static jobject MockGetStaticObjectField(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    jobject res = gMockJni->GetStaticObjectField(env, clazz, fieldID);
    if (res != nullptr) {
        gCreatedLocalRefs++;
    }
    return res;
}
static const char* MockGetStringUTFChars(JNIEnv* env, jstring string, jboolean* isCopy) {
    return gMockJni->GetStringUTFChars(env, string, isCopy);
}
static jsize MockGetStringUTFLength(JNIEnv* env, jstring string) {
    return gMockJni->GetStringUTFLength(env, string);
}
static void MockReleaseStringUTFChars(JNIEnv* env, jstring string, const char* utf) {
    gMockJni->ReleaseStringUTFChars(env, string, utf);
}
static void MockDeleteLocalRef(JNIEnv* env, jobject localRef) {
    if (localRef != nullptr) {
        gDeletedLocalRefs++;
    }
    gMockJni->DeleteLocalRef(env, localRef);
}
static jclass MockGetObjectClass(JNIEnv* env, jobject obj) {
    jclass res = gMockJni->GetObjectClass(env, obj);
    if (res != nullptr) {
        gCreatedLocalRefs++;
    }
    return res;
}
static jstring MockNewStringUTF(JNIEnv* env, const char* bytes) {
    jstring res = gMockJni->NewStringUTF(env, bytes);
    if (res != nullptr) {
        gCreatedLocalRefs++;
    }
    return res;
}
static jobject MockNewObjectV(JNIEnv* env, jclass clazz, jmethodID methodID, va_list args) {
    jobject res = gMockJni->NewObjectV(env, clazz, methodID, args);
    if (res != nullptr) {
        gCreatedLocalRefs++;
    }
    return res;
}
static jint MockRegisterNatives(JNIEnv* env, jclass clazz, const JNINativeMethod* methods,
                                jint nMethods) {
    return gMockJni->RegisterNatives(env, clazz, methods, nMethods);
}

static _JNIEnv* gMockEnv = nullptr;
static std::mutex gTestSerializationMutex;

static jint MockAttachCurrentThread(JavaVM*, JNIEnv** env, void*) {
    if (env && gMockEnv) {
        *env = gMockEnv;
    }
    return JNI_OK;
}

static jint MockGetEnv(JavaVM*, void** env, jint) {
    if (env && gMockEnv) {
        *env = gMockEnv;
    }
    return JNI_OK;
}

static jint MockDetachCurrentThread(JavaVM*) {
    return JNI_OK;
}

extern "C" JNIEXPORT void JNICALL
Java_com_google_androidgamesdk_ChoreographerCallback_nOnChoreographer(JNIEnv* env, jobject thisObj,
                                                                      jlong cookie,
                                                                      jlong frameTimeNanos);

class AImageReaderSwapchainTestBase : public ::testing::Test {
private:
    std::atomic<jlong> mChoreographerCookie{0};
    std::atomic<int> mRunningThreads{0};
    std::vector<std::thread> mMockThreads;
    std::mutex mMockThreadsMutex;

public:
    AImageReaderSwapchainTestBase() {
        gTestSerializationMutex.lock();
    }

    virtual ~AImageReaderSwapchainTestBase() {
        gTestSerializationMutex.unlock();
    }

    AImageReader* mReader = nullptr;
    ANativeWindow* mWindow = nullptr;

    testing::NiceMock<MockJNI> mMockJni;
    _JNIEnv mEnvStruct;
    JNIInvokeInterface mInvokeInterface = {};
    _JavaVM mJavaVM;
    JNINativeInterface mJniInterface = {};

    void setChoreographerCookie(va_list args) {
        va_list args_copy;
        va_copy(args_copy, args);
        mChoreographerCookie = va_arg(args_copy, jlong);
        va_end(args_copy);
    }

    void spawnMockChoreographerThread() {
        mRunningThreads++;
        std::lock_guard<std::mutex> lock(mMockThreadsMutex);
        mMockThreads.emplace_back([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            jlong cookie = mChoreographerCookie.load();
            if (cookie != 0) {
                auto now = std::chrono::steady_clock::now().time_since_epoch();
                jlong timeNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
                Java_com_google_androidgamesdk_ChoreographerCallback_nOnChoreographer(nullptr,
                                                                                      nullptr,
                                                                                      cookie,
                                                                                      timeNanos);
            }
            mRunningThreads--;
        });
    }

    void setupMockChoreographer() {
        // Override JNI Mock for <init> and postFrameCallback to intercept Choreographer callbacks.
        ON_CALL(mMockJni, GetMethodID(testing::_, testing::_, testing::StrEq("<init>"), testing::_))
                .WillByDefault(testing::Return(reinterpret_cast<jmethodID>(0x100)));
        ON_CALL(mMockJni,
                GetMethodID(testing::_, testing::_, testing::StrEq("postFrameCallback"),
                            testing::_))
                .WillByDefault(testing::Return(reinterpret_cast<jmethodID>(0x101)));

        // When Swappy calls into the choreographer, grab the cookie for our mock thread.
        ON_CALL(mMockJni, NewObjectV(testing::_, testing::_, testing::_, testing::_))
                .WillByDefault(
                        testing::Invoke([this](JNIEnv*, jclass, jmethodID methodID, va_list args) {
                            if (methodID == reinterpret_cast<jmethodID>(0x100)) {
                                setChoreographerCookie(args);
                            }
                            return reinterpret_cast<jobject>(0x8);
                        }));

        // Spin up a mock thread to simulate the Choreographer firing callbacks on frame events.
        ON_CALL(mMockJni, CallVoidMethodV(testing::_, testing::_, testing::_, testing::_))
                .WillByDefault(testing::Invoke(
                        [this](JNIEnv* env, jobject obj, jmethodID methodID, va_list) {
                            if (methodID == reinterpret_cast<jmethodID>(0x101)) {
                                spawnMockChoreographerThread();
                            }
                        }));
    }

    void cleanupMockChoreographer() {
        mChoreographerCookie = 0;
        while (true) {
            std::vector<std::thread> toJoin;
            {
                std::lock_guard<std::mutex> lock(mMockThreadsMutex);
                toJoin.swap(mMockThreads);
            }
            if (toJoin.empty() && mRunningThreads.load() == 0) {
                break;
            }
            for (auto& t : toJoin) {
                if (t.joinable()) {
                    t.join();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void setupMockNDKChoreographer() {
        ::setupMockNDKChoreographer();
    }

    void cleanupMockNDKChoreographer() {
        ::cleanupMockNDKChoreographer();
    }

    void TearDown() override {
        if (mReader) {
            AImageReader_delete(mReader);
            mReader = nullptr;
        }
        mWindow = nullptr;
        cleanupMockChoreographer();
        cleanupMockNDKChoreographer();
        teardownMockJni();
    }

    // Sets up a comprehensive mocked JNI environment that simulates the Android
    // Java runtime structure expected by Swappy during its initialization and lifecycle.
    // This allows testing the C++ logic of Swappy completely isolated from the Java layers,
    // intercepting expected calls like retrieving the SDK version or presentation deadlines.
    // Individual tests can still append their own specific mock behaviors (via ON_CALL overrides)
    // after calling this method.
    void setupMockJni(int sdkVersion) {
        gMockJni = &mMockJni;
        gCreatedLocalRefs = 0;
        gDeletedLocalRefs = 0;

        ON_CALL(mMockJni, FindClass(testing::_, testing::_))
                .WillByDefault(testing::Return(reinterpret_cast<jclass>(0x1)));
        ON_CALL(mMockJni, ExceptionCheck(testing::_)).WillByDefault(testing::Return(JNI_FALSE));
        ON_CALL(mMockJni, ExceptionOccurred(testing::_)).WillByDefault(testing::Return(nullptr));
        ON_CALL(mMockJni,
                GetStaticFieldID(testing::_, testing::_, testing::StrEq("SDK_INT"), testing::_))
                .WillByDefault(testing::Return(reinterpret_cast<jfieldID>(0x1)));
        ON_CALL(mMockJni,
                GetStaticFieldID(testing::_, testing::_, testing::StrEq("PREVIEW_SDK_INT"),
                                 testing::_))
                .WillByDefault(testing::Return(reinterpret_cast<jfieldID>(0x2)));
        ON_CALL(mMockJni,
                GetStaticIntField(testing::_, testing::_, reinterpret_cast<jfieldID>(0x1)))
                .WillByDefault(testing::Return(sdkVersion));
        ON_CALL(mMockJni,
                GetStaticIntField(testing::_, testing::_, reinterpret_cast<jfieldID>(0x2)))
                .WillByDefault(testing::Return(0));

        ON_CALL(mMockJni,
                GetMethodID(testing::_, testing::_, testing::Not(testing::EndsWith("Nanos")),
                            testing::_))
                .WillByDefault(testing::Return(reinterpret_cast<jmethodID>(0x3)));
        ON_CALL(mMockJni,
                GetMethodID(testing::_, testing::_, testing::StrEq("getAppVsyncOffsetNanos"),
                            testing::_))
                .WillByDefault(testing::Return(reinterpret_cast<jmethodID>(0x10)));
        ON_CALL(mMockJni,
                GetMethodID(testing::_, testing::_, testing::StrEq("getPresentationDeadlineNanos"),
                            testing::_))
                .WillByDefault(testing::Return(reinterpret_cast<jmethodID>(0x11)));

        ON_CALL(mMockJni, CallObjectMethodV(testing::_, testing::_, testing::_, testing::_))
                .WillByDefault(testing::Return(reinterpret_cast<jobject>(0x4)));
        ON_CALL(mMockJni, CallFloatMethodV(testing::_, testing::_, testing::_, testing::_))
                .WillByDefault(testing::Return(60.0f));

        ON_CALL(mMockJni,
                CallLongMethodV(testing::_, testing::_, reinterpret_cast<jmethodID>(0x10),
                                testing::_))
                .WillByDefault(testing::Return(1000000));
        ON_CALL(mMockJni,
                CallLongMethodV(testing::_, testing::_, reinterpret_cast<jmethodID>(0x11),
                                testing::_))
                .WillByDefault(testing::Return(16666666));

        mInvokeInterface.AttachCurrentThread = MockAttachCurrentThread;
        mInvokeInterface.DetachCurrentThread = MockDetachCurrentThread;
        mInvokeInterface.GetEnv = MockGetEnv;

        mJavaVM.functions = &mInvokeInterface;

        ON_CALL(mMockJni, GetJavaVM(testing::_, testing::_))
                .WillByDefault(testing::DoAll(testing::SetArgPointee<1>(&mJavaVM),
                                              testing::Return(JNI_OK)));
        ON_CALL(mMockJni, NewGlobalRef(testing::_, testing::_))
                .WillByDefault(testing::ReturnArg<1>());
        ON_CALL(mMockJni, GetStaticObjectField(testing::_, testing::_, testing::_))
                .WillByDefault(testing::Return(reinterpret_cast<jobject>(0x5)));
        ON_CALL(mMockJni, GetStringUTFChars(testing::_, testing::_, testing::_))
                .WillByDefault(testing::Return(""));
        ON_CALL(mMockJni, GetStringUTFLength(testing::_, testing::_))
                .WillByDefault(testing::Return(0));
        ON_CALL(mMockJni, GetObjectClass(testing::_, testing::_))
                .WillByDefault(testing::Return(reinterpret_cast<jclass>(0x6)));
        ON_CALL(mMockJni, NewStringUTF(testing::_, testing::_))
                .WillByDefault(testing::Return(reinterpret_cast<jstring>(0x7)));
        const jmethodID kSwappyDisplayManagerConstructor = reinterpret_cast<jmethodID>(0x21);

        ON_CALL(mMockJni,
                GetMethodID(testing::_, testing::_, testing::StrEq("<init>"),
                            testing::StrEq("(JLandroid/app/Activity;)V")))
                .WillByDefault(testing::Return(kSwappyDisplayManagerConstructor));

        ON_CALL(mMockJni,
                NewObjectV(testing::_, testing::_, kSwappyDisplayManagerConstructor, testing::_))
                .WillByDefault(testing::Invoke([](JNIEnv*, jclass, jmethodID, va_list args) {
                    va_list args_copy;
                    va_copy(args_copy, args);
                    jlong cookie = va_arg(args_copy, jlong);
                    va_end(args_copy);
                    if (cookie != 0) {
                        auto refreshPeriodsMap =
                                std::make_shared<swappy::SwappyDisplayManager::RefreshPeriodMap>();
                        (*refreshPeriodsMap)[std::chrono::nanoseconds(16666666)] = 0;
                        swappy::SwappyDisplayManagerJNI::
                                onSetSupportedRefreshPeriods(cookie, refreshPeriodsMap);
                    }
                    return reinterpret_cast<jobject>(0x8);
                }));
        ON_CALL(mMockJni,
                NewObjectV(testing::_, testing::_, testing::Ne(kSwappyDisplayManagerConstructor),
                           testing::_))
                .WillByDefault(testing::Return(reinterpret_cast<jobject>(0x8)));
        ON_CALL(mMockJni, CallVoidMethodV(testing::_, testing::_, testing::_, testing::_))
                .WillByDefault(testing::Invoke([](JNIEnv*, jobject, jmethodID, va_list) {}));
        ON_CALL(mMockJni, RegisterNatives(testing::_, testing::_, testing::_, testing::_))
                .WillByDefault(testing::Return(JNI_OK));

        mJniInterface.FindClass = MockFindClass;
        mJniInterface.GetMethodID = MockGetMethodID;
        mJniInterface.GetStaticFieldID = MockGetStaticFieldID;
        mJniInterface.GetStaticIntField = MockGetStaticIntField;
        mJniInterface.CallObjectMethodV = MockCallObjectMethodV;
        mJniInterface.CallFloatMethodV = MockCallFloatMethodV;
        mJniInterface.CallLongMethodV = MockCallLongMethodV;
        mJniInterface.CallVoidMethodV = MockCallVoidMethodV;
        mJniInterface.ExceptionCheck = MockExceptionCheck;
        mJniInterface.ExceptionOccurred = MockExceptionOccurred;
        mJniInterface.ExceptionClear = MockExceptionClear;
        mJniInterface.GetJavaVM = MockGetJavaVM;
        mJniInterface.NewGlobalRef = MockNewGlobalRef;
        mJniInterface.DeleteGlobalRef = MockDeleteGlobalRef;
        mJniInterface.GetStaticObjectField = MockGetStaticObjectField;
        mJniInterface.GetStringUTFChars = MockGetStringUTFChars;
        mJniInterface.GetStringUTFLength = MockGetStringUTFLength;
        mJniInterface.ReleaseStringUTFChars = MockReleaseStringUTFChars;
        mJniInterface.DeleteLocalRef = MockDeleteLocalRef;
        mJniInterface.GetObjectClass = MockGetObjectClass;
        mJniInterface.NewStringUTF = MockNewStringUTF;
        mJniInterface.NewObjectV = MockNewObjectV;
        mJniInterface.RegisterNatives = MockRegisterNatives;

        mEnvStruct.functions = &mJniInterface;
        gMockEnv = &mEnvStruct;
    }

    void teardownMockJni() {
        gMockJni = nullptr;
        gMockEnv = nullptr;
    }

    // ------------------------------------------------------
    // Helper methods
    // ------------------------------------------------------

    void createAImageReader(int width, int height, int format, int maxImages,
                            AImageReader_ImageListener* listener = nullptr) {
        media_status_t status = AImageReader_new(width, height, format, maxImages, &mReader);
        ASSERT_EQ(AMEDIA_OK, status) << "Failed to create AImageReader";
        ASSERT_NE(nullptr, mReader) << "AImageReader is null";

        LOGI("AImageReader created with %dx%d, format=%d", width, height, format);
        if (listener) {
            AImageReader_setImageListener(mReader, listener);
        }
    }

    void getANativeWindowFromReader() {
        ASSERT_NE(nullptr, mReader);

        media_status_t status = AImageReader_getWindow(mReader, &mWindow);
        ASSERT_EQ(AMEDIA_OK, status) << "Failed to get ANativeWindow from AImageReader";
        ASSERT_NE(nullptr, mWindow) << "ANativeWindow is null";
        LOGI("ANativeWindow obtained from AImageReader");
    }

    static void onImageAvailable(void*, AImageReader* reader) {
        LOGI("onImageAvailable callback triggered");
        AImage* image = nullptr;
        media_status_t status = AImageReader_acquireLatestImage(reader, &image);
        if (status != AMEDIA_OK || !image) {
            LOGE("Failed to acquire latest image");
            return;
        }
        AImage_delete(image);
        LOGI("Released acquired image");
    }
};

class AImageReaderVulkanSwapchainTest : public AImageReaderSwapchainTestBase {
public:
    VkInstance mVkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysicalDev = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    VkSurfaceKHR mSurface = VK_NULL_HANDLE;
    VkQueue mPresentQueue = VK_NULL_HANDLE;
    uint32_t mPresentQueueFamily = UINT32_MAX;
    VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;

    void createVulkanInstance(std::vector<const char*>& layers) {
        const char* extensions[] = {
                VK_KHR_SURFACE_EXTENSION_NAME,
                VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
                VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        };

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "AImageReader Vulkan Swapchain Test";
        appInfo.applicationVersion = 1;
        appInfo.pEngineName = "TestEngine";
        appInfo.engineVersion = 1;
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo instInfo{};
        instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instInfo.pApplicationInfo = &appInfo;
        instInfo.enabledExtensionCount =
                static_cast<uint32_t>(sizeof(extensions) / sizeof(extensions[0]));
        instInfo.ppEnabledExtensionNames = extensions;
        instInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
        instInfo.ppEnabledLayerNames = layers.data();
        VkResult res = vkCreateInstance(&instInfo, nullptr, &mVkInstance);
        VK_CHECK(res);
        LOGI("Vulkan instance created");
    }

    void createVulkanSurface() {
        ASSERT_NE((VkInstance)VK_NULL_HANDLE, mVkInstance);
        ASSERT_NE((ANativeWindow*)nullptr, mWindow);

        VkAndroidSurfaceCreateInfoKHR surfaceCreateInfo{};
        surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        surfaceCreateInfo.window = mWindow;

        VkResult res =
                vkCreateAndroidSurfaceKHR(mVkInstance, &surfaceCreateInfo, nullptr, &mSurface);
        VK_CHECK(res);
        LOGI("Vulkan surface created from ANativeWindow");
    }

    void pickPhysicalDeviceAndQueueFamily() {
        ASSERT_NE((VkInstance)VK_NULL_HANDLE, mVkInstance);

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(mVkInstance, &deviceCount, nullptr);
        ASSERT_GT(deviceCount, 0U) << "No Vulkan physical devices found!";

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(mVkInstance, &deviceCount, devices.data());

        for (auto& dev : devices) {
            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueProps(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, queueProps.data());

            for (uint32_t i = 0; i < queueFamilyCount; i++) {
                VkBool32 support = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, mSurface, &support);
                if (support == VK_TRUE) {
                    // Found a queue family that can present
                    mPhysicalDev = dev;
                    mPresentQueueFamily = i;

                    LOGI("Physical device found with queue family %u supporting "
                         "present",
                         i);
                    return;
                }
            }
        }

        FAIL() << "No physical device found that supports present to the surface!";
    }

    void createDeviceAndGetQueue(std::vector<const char*>& layers,
                                 std::vector<const char*> inExtensions = {},
                                 void* pNext = nullptr) {
        ASSERT_NE((void*)VK_NULL_HANDLE, mPhysicalDev);
        ASSERT_NE(UINT32_MAX, mPresentQueueFamily);

        float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = mPresentQueueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.pNext = pNext;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
        deviceInfo.ppEnabledLayerNames = layers.data();

        std::vector<const char*> extensions = {
                VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };
        for (auto extension : inExtensions) {
            extensions.push_back(extension);
        }
        deviceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        deviceInfo.ppEnabledExtensionNames = extensions.data();

        VkResult res = vkCreateDevice(mPhysicalDev, &deviceInfo, nullptr, &mDevice);
        VK_CHECK(res);
        LOGI("Logical device created");

        vkGetDeviceQueue(mDevice, mPresentQueueFamily, 0, &mPresentQueue);
        ASSERT_NE((VkQueue)VK_NULL_HANDLE, mPresentQueue);
        LOGI("Acquired present-capable queue");
    }

    void createSwapchain() {
        ASSERT_NE((VkDevice)VK_NULL_HANDLE, mDevice);
        ASSERT_NE((VkSurfaceKHR)VK_NULL_HANDLE, mSurface);

        VkSurfaceCapabilitiesKHR surfaceCaps{};
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mPhysicalDev, mSurface, &surfaceCaps));

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysicalDev, mSurface, &formatCount, nullptr);
        ASSERT_GT(formatCount, 0U);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysicalDev, mSurface, &formatCount, formats.data());

        VkSurfaceFormatKHR chosenFormat = formats[0];
        LOGI("Chosen surface format: %d", chosenFormat.format);

        uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysicalDev, mSurface, &presentModeCount,
                                                  nullptr);
        ASSERT_GT(presentModeCount, 0U);
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(mPhysicalDev, mSurface, &presentModeCount,
                                                  presentModes.data());

        VkPresentModeKHR chosenPresentMode = VK_PRESENT_MODE_FIFO_KHR;
        for (auto mode : presentModes) {
            if (mode == VK_PRESENT_MODE_FIFO_KHR) {
                chosenPresentMode = mode;
                break;
            }
        }

        VkExtent2D swapchainExtent{};
        if (surfaceCaps.currentExtent.width == 0xFFFFFFFF) {
            swapchainExtent.width = 640;  // fallback
            swapchainExtent.height = 480; // fallback
        } else {
            swapchainExtent = surfaceCaps.currentExtent;
        }

        uint32_t desiredImageCount = surfaceCaps.minImageCount + 1;
        if (surfaceCaps.maxImageCount > 0 && desiredImageCount > surfaceCaps.maxImageCount) {
            desiredImageCount = surfaceCaps.maxImageCount;
        }

        VkSwapchainCreateInfoKHR swapchainInfo{};
        swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapchainInfo.surface = mSurface;
        swapchainInfo.minImageCount = desiredImageCount;
        swapchainInfo.imageFormat = chosenFormat.format;
        swapchainInfo.imageColorSpace = chosenFormat.colorSpace;
        swapchainInfo.imageExtent = swapchainExtent;
        swapchainInfo.imageArrayLayers = 1;
        swapchainInfo.imageUsage =
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        swapchainInfo.preTransform = surfaceCaps.currentTransform;
        swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        swapchainInfo.presentMode = chosenPresentMode;
        swapchainInfo.clipped = VK_TRUE;
        swapchainInfo.oldSwapchain = VK_NULL_HANDLE;

        uint32_t queueFamilyIndices[] = {mPresentQueueFamily};
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainInfo.queueFamilyIndexCount = 1;
        swapchainInfo.pQueueFamilyIndices = queueFamilyIndices;

        VkResult res = vkCreateSwapchainKHR(mDevice, &swapchainInfo, nullptr, &mSwapchain);
        if (res == VK_SUCCESS) {
            LOGI("Swapchain created successfully");

            uint32_t swapchainImageCount = 0;
            vkGetSwapchainImagesKHR(mDevice, mSwapchain, &swapchainImageCount, nullptr);
            std::vector<VkImage> swapchainImages(swapchainImageCount);
            vkGetSwapchainImagesKHR(mDevice, mSwapchain, &swapchainImageCount,
                                    swapchainImages.data());
        } else {
            LOGI("Swapchain creation failed");
        }
    }

    void cleanUpSwapchainForTest() {
        if (mSwapchain != VK_NULL_HANDLE) {
            SwappyVk_destroySwapchain(mDevice, mSwapchain);
            vkDestroySwapchainKHR(mDevice, mSwapchain, nullptr);
            mSwapchain = VK_NULL_HANDLE;
        }
        if (mDevice != VK_NULL_HANDLE) {
            SwappyVk_destroyDevice(mDevice);
            vkDestroyDevice(mDevice, nullptr);
            mDevice = VK_NULL_HANDLE;
        }
        if (mSurface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(mVkInstance, mSurface, nullptr);
            mSurface = VK_NULL_HANDLE;
        }
        if (mVkInstance != VK_NULL_HANDLE) {
            vkDestroyInstance(mVkInstance, nullptr);
            mVkInstance = VK_NULL_HANDLE;
        }
    }

    void TearDown() override {
        cleanupMockChoreographer();
        // Must clean up mock threads before destroying the swapchain (which destroys Swappy

        cleanupMockNDKChoreographer();
        cleanUpSwapchainForTest();
        unsetenv("MOCK_VK_GOOGLE_DISPLAY_TIMING");
        AImageReaderSwapchainTestBase::TearDown();
        swappy::SwappyVk::getInstance().clearState();
    }

    void buildSwapchainForTest(std::vector<const char*>& instanceLayers,
                               std::vector<const char*>& deviceLayers) {
        createVulkanInstance(instanceLayers);
        AImageReader_ImageListener listener{};
        listener.context = this;
        listener.onImageAvailable = &AImageReaderSwapchainTestBase::onImageAvailable;
        createAImageReader(640, 480, AIMAGE_FORMAT_PRIVATE, 3, &listener);
        getANativeWindowFromReader();
        createVulkanSurface();
        pickPhysicalDeviceAndQueueFamily();

        createDeviceAndGetQueue(deviceLayers);
        createSwapchain();
    }
};

TEST_F(AImageReaderVulkanSwapchainTest, TestHelperMethods) {
    // Verify that the basic plumbing/helper functions of these tests is
    // working. This doesn't directly test any of the layer code. It only
    // verifies that we can successfully create a swapchain with an AImageReader

    std::vector<const char*> instanceLayers;
    std::vector<const char*> deviceLayers;
    buildSwapchainForTest(instanceLayers, deviceLayers);

    ASSERT_NE(mVkInstance, (VkInstance)VK_NULL_HANDLE);
    ASSERT_NE(mPhysicalDev, (VkPhysicalDevice)VK_NULL_HANDLE);
    ASSERT_NE(mDevice, (VkDevice)VK_NULL_HANDLE);
    ASSERT_NE(mSurface, (VkSurfaceKHR)VK_NULL_HANDLE);
    ASSERT_NE(mSwapchain, (VkSwapchainKHR)VK_NULL_HANDLE);
    cleanUpSwapchainForTest();
}

TEST_F(AImageReaderVulkanSwapchainTest, Initialization) {
    // Set up the mocked JNI environment with an SDK version of 37.
    // This allows us to simulate the Android runtime environment for Swappy.
    setupMockJni(37);

    jobject fakeActivity = reinterpret_cast<jobject>(0x1234);

    // Build swapchain to provide a valid device and swapchain to Swappy
    std::vector<const char*> instanceLayers;
    std::vector<const char*> deviceLayers;
    buildSwapchainForTest(instanceLayers, deviceLayers);

    uint64_t refreshDuration = 0;
    // Verify that Swappy successfully initializes and fetches the refresh cycle duration
    // using the provided mock JNI environment and Vulkan objects.
    bool success = SwappyVk_initAndGetRefreshCycleDuration(gMockEnv, fakeActivity, mPhysicalDev,
                                                           mDevice, mSwapchain, &refreshDuration);

    EXPECT_TRUE(success);

    cleanUpSwapchainForTest();
}

TEST_F(AImageReaderVulkanSwapchainTest, RenderingLoop) {
    // Set up the mocked JNI environment with an SDK version of 23.
    // Different SDK versions trigger different initialization paths in Swappy.
    setupMockJni(23);
    setupMockChoreographer();

    jobject fakeActivity = reinterpret_cast<jobject>(0x1234);

    // Build swapchain to provide a valid device and swapchain to Swappy
    std::vector<const char*> instanceLayers;
    std::vector<const char*> deviceLayers;
    buildSwapchainForTest(instanceLayers, deviceLayers);

    uint64_t refreshDuration = 0;
    bool success = SwappyVk_initAndGetRefreshCycleDuration(gMockEnv, fakeActivity, mPhysicalDev,
                                                           mDevice, mSwapchain, &refreshDuration);

    EXPECT_TRUE(success);

    SwappyVk_setQueueFamilyIndex(mDevice, mPresentQueue, mPresentQueueFamily);
    SwappyVk_setSwapIntervalNS(mDevice, mSwapchain, 16666666);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore imageAvailableSemaphore;
    VK_CHECK(vkCreateSemaphore(mDevice, &semaphoreInfo, nullptr, &imageAvailableSemaphore));

    // Spin for 10 frames, acquiring and presenting to verify the rendering loop
    // correctly interacts with our mocked choreographer.
    for (int i = 0; i < 10; ++i) {
        uint32_t imageIndex;
        VkResult res = vkAcquireNextImageKHR(mDevice, mSwapchain, UINT64_MAX,
                                             imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
            break;
        }
        EXPECT_EQ(res, VK_SUCCESS);

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &imageAvailableSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &mSwapchain;
        presentInfo.pImageIndices = &imageIndex;

        // Use Swappy to present
        res = SwappyVk_queuePresent(mPresentQueue, &presentInfo);
        EXPECT_EQ(res, VK_SUCCESS);
    }

    vkDestroySemaphore(mDevice, imageAvailableSemaphore, nullptr);

    // Clean up existing mock threads before destroying Swappy to prevent them from
    // calling into a destroyed Swappy instance. TearDown() will clean up any
    // additional threads spawned during destruction.
    cleanupMockChoreographer();

    cleanUpSwapchainForTest();
}

// This test verifies that the VK_LAYER_swappy_mock layer correctly emulates the
// VK_GOOGLE_display_timing extension when enabled. It checks if the extension is
// exposed, sets up a swapchain, queues a frame with mock display timing data,
// and confirms that past presentation timing history can be retrieved successfully.
TEST_F(AImageReaderVulkanSwapchainTest, DisplayTimingMockEnabledTest) {
    // Enables the mock timing behavior in the VK_LAYER_swappy_mock layer.
    // The layer reads this environment variable during vkCreateInstance and, when set to "1",
    // actively intercepts presentation calls to record and return mock timing data.
    setenv("MOCK_VK_GOOGLE_DISPLAY_TIMING", "1", 1);

    std::vector<const char*> instanceLayers = {"VK_LAYER_swappy_mock"};
    std::vector<const char*> deviceLayers = {"VK_LAYER_swappy_mock"};
    std::vector<const char*> deviceExtensions = {
            VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
    };

    createVulkanInstance(instanceLayers);
    createAImageReader(640, 480, AIMAGE_FORMAT_PRIVATE, 3);
    getANativeWindowFromReader();
    createVulkanSurface();
    pickPhysicalDeviceAndQueueFamily();

    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(mPhysicalDev, "VK_LAYER_swappy_mock", &extensionCount,
                                         nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(mPhysicalDev, "VK_LAYER_swappy_mock", &extensionCount,
                                         availableExtensions.data());

    bool timingExtSupported = false;
    for (const auto& extension : availableExtensions) {
        if (strcmp(extension.extensionName, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME) == 0) {
            timingExtSupported = true;
            break;
        }
    }

    ASSERT_TRUE(timingExtSupported);

    createDeviceAndGetQueue(deviceLayers, deviceExtensions);
    createSwapchain();

    uint32_t imageIndex;
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore imageAcquiredSemaphore = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSemaphore(mDevice, &semInfo, nullptr, &imageAcquiredSemaphore));

    VkResult res = vkAcquireNextImageKHR(mDevice, mSwapchain, UINT64_MAX, imageAcquiredSemaphore,
                                         VK_NULL_HANDLE, &imageIndex);
    VK_CHECK(res);

    VkPresentTimeGOOGLE presentTime = {1, 1000};
    VkPresentTimesInfoGOOGLE presentTimesInfo = {};
    presentTimesInfo.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;
    presentTimesInfo.swapchainCount = 1;
    presentTimesInfo.pTimes = &presentTime;

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = &presentTimesInfo;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &mSwapchain;
    presentInfo.pImageIndices = &imageIndex;

    res = vkQueuePresentKHR(mPresentQueue, &presentInfo);
    VK_CHECK(res);

    auto pfnGetPastPresentationTimingGOOGLE = (PFN_vkGetPastPresentationTimingGOOGLE)
            vkGetDeviceProcAddr(mDevice, "vkGetPastPresentationTimingGOOGLE");
    ASSERT_NE(pfnGetPastPresentationTimingGOOGLE, nullptr);

    uint32_t timingCount = 0;
    res = pfnGetPastPresentationTimingGOOGLE(mDevice, mSwapchain, &timingCount, nullptr);
    ASSERT_EQ(res, VK_SUCCESS);
    ASSERT_TRUE(timingCount > 0);

    vkDestroySemaphore(mDevice, imageAcquiredSemaphore, nullptr);
    cleanUpSwapchainForTest();
    unsetenv("MOCK_VK_GOOGLE_DISPLAY_TIMING");
}

// This test verifies that the VK_LAYER_swappy_mock layer behaves transparently
// when its mock functionality is explicitly disabled. It ensures that normal Vulkan
// initialization, enumeration, and swapchain creation still succeed without the
// mock layer interfering.
TEST_F(AImageReaderVulkanSwapchainTest, DisplayTimingMockDisabledTest) {
    // Disables the mock timing behavior in the VK_LAYER_swappy_mock layer.
    // When set to "0", the layer acts as a pass-through and will not emulate the
    // VK_GOOGLE_display_timing extension or intercept its related function calls.
    setenv("MOCK_VK_GOOGLE_DISPLAY_TIMING", "0", 1);

    std::vector<const char*> instanceLayers = {};
    std::vector<const char*> deviceLayers = {"VK_LAYER_swappy_mock"};

    createVulkanInstance(instanceLayers);
    createAImageReader(640, 480, AIMAGE_FORMAT_PRIVATE, 3);
    getANativeWindowFromReader();
    createVulkanSurface();
    pickPhysicalDeviceAndQueueFamily();

    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(mPhysicalDev, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(mPhysicalDev, nullptr, &extensionCount,
                                         availableExtensions.data());

    bool timingExtSupported = false;
    for (const auto& extension : availableExtensions) {
        if (strcmp(extension.extensionName, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME) == 0) {
            timingExtSupported = true;
            break;
        }
    }

    std::vector<const char*> deviceExtensions;
    if (timingExtSupported) {
        deviceExtensions.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    }

    createDeviceAndGetQueue(deviceLayers, deviceExtensions);
    createSwapchain();
    cleanUpSwapchainForTest();
    unsetenv("MOCK_VK_GOOGLE_DISPLAY_TIMING");
}

// This test exercises the Swappy Vulkan rendering loop when the VK_GOOGLE_display_timing
// extension is available and enabled via VK_LAYER_swappy_mock. It initializes Swappy and
// queries the refresh cycle duration, sets the present queue and swap interval, and runs a
// 10-frame rendering loop (acquiring and queuing images with SwappyVk_queuePresent) to verify
// stable frame pacing and presentation under VK_GOOGLE_display_timing.
TEST_F(AImageReaderVulkanSwapchainTest, RenderingLoopWithGoogleDisplayTiming) {
    setenv("MOCK_VK_GOOGLE_DISPLAY_TIMING", "1", 1);
    setupMockJni(23);
    setupMockChoreographer();

    jobject fakeActivity = reinterpret_cast<jobject>(0x1234);

    std::vector<const char*> instanceLayers = {"VK_LAYER_swappy_mock"};
    std::vector<const char*> deviceLayers = {"VK_LAYER_swappy_mock"};
    std::vector<const char*> deviceExtensions = {
            VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
    };

    createVulkanInstance(instanceLayers);
    createAImageReader(640, 480, AIMAGE_FORMAT_PRIVATE, 3);
    getANativeWindowFromReader();
    createVulkanSurface();
    pickPhysicalDeviceAndQueueFamily();
    createDeviceAndGetQueue(deviceLayers, deviceExtensions);
    createSwapchain();

    uint64_t refreshDuration = 0;
    EXPECT_TRUE(SwappyVk_initAndGetRefreshCycleDuration(gMockEnv, fakeActivity, mPhysicalDev,
                                                        mDevice, mSwapchain, &refreshDuration));

    SwappyVk_setQueueFamilyIndex(mDevice, mPresentQueue, mPresentQueueFamily);
    SwappyVk_setSwapIntervalNS(mDevice, mSwapchain, SWAPPY_SWAP_60FPS);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore imageAvailableSemaphore;
    VK_CHECK(vkCreateSemaphore(mDevice, &semaphoreInfo, nullptr, &imageAvailableSemaphore));

    for (int i = 0; i < 10; ++i) {
        uint32_t imageIndex;
        ASSERT_EQ(VK_SUCCESS,
                  vkAcquireNextImageKHR(mDevice, mSwapchain, UINT64_MAX, imageAvailableSemaphore,
                                        VK_NULL_HANDLE, &imageIndex));

        SwappyVk_recordFrameStart(mPresentQueue, mSwapchain, imageIndex);

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &imageAvailableSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &mSwapchain;
        presentInfo.pImageIndices = &imageIndex;

        ASSERT_EQ(VK_SUCCESS, SwappyVk_queuePresent(mPresentQueue, &presentInfo));
    }

    vkDestroySemaphore(mDevice, imageAvailableSemaphore, nullptr);
}

// This test provides end-to-end coverage for Swappy's Vulkan C API functions, configuration
// setters/getters, tracer injection, and statistics collection. It verifies device extension
// determination, tests toggling affinity/auto-swap/frame-pacing modes, injects custom tracer
// callbacks, runs a 5-frame presentation loop, and confirms that frame statistics are recorded.
TEST_F(AImageReaderVulkanSwapchainTest, CApiAndStatsTest) {
    setenv("MOCK_VK_GOOGLE_DISPLAY_TIMING", "1", 1);
    setupMockJni(23);
    setupMockChoreographer();

    jobject fakeActivity = reinterpret_cast<jobject>(0x1234);

    std::vector<const char*> instanceLayers = {"VK_LAYER_swappy_mock"};
    std::vector<const char*> deviceLayers = {"VK_LAYER_swappy_mock"};
    std::vector<const char*> deviceExtensions = {
            VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
    };

    createVulkanInstance(instanceLayers);
    createAImageReader(640, 480, AIMAGE_FORMAT_PRIVATE, 3);
    getANativeWindowFromReader();
    createVulkanSurface();
    pickPhysicalDeviceAndQueueFamily();
    createDeviceAndGetQueue(deviceLayers, deviceExtensions);
    createSwapchain();

    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(mPhysicalDev, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(mPhysicalDev, nullptr, &extensionCount,
                                         availableExtensions.data());

    uint32_t requiredCount = 0;
    SwappyVk_determineDeviceExtensions(mPhysicalDev, extensionCount, availableExtensions.data(),
                                       &requiredCount, nullptr);
    std::vector<std::vector<char>> requiredExtensionBuffers(requiredCount,
                                                            std::vector<char>(
                                                                    VK_MAX_EXTENSION_NAME_SIZE));
    std::vector<char*> requiredExtensions(requiredCount);
    for (uint32_t i = 0; i < requiredCount; ++i) {
        requiredExtensions[i] = requiredExtensionBuffers[i].data();
    }
    SwappyVk_determineDeviceExtensions(mPhysicalDev, extensionCount, availableExtensions.data(),
                                       &requiredCount, requiredExtensions.data());

    uint64_t refreshDuration = 0;
    EXPECT_TRUE(SwappyVk_initAndGetRefreshCycleDuration(gMockEnv, fakeActivity, mPhysicalDev,
                                                        mDevice, mSwapchain, &refreshDuration));
    SwappyVk_setWindow(mDevice, mSwapchain, mWindow);
    SwappyVk_setQueueFamilyIndex(mDevice, mPresentQueue, mPresentQueueFamily);
    SwappyVk_setSwapIntervalNS(mDevice, mSwapchain, SWAPPY_SWAP_60FPS);
    EXPECT_EQ(SwappyVk_getSwapIntervalNS(mSwapchain), refreshDuration);

    SwappyVk_setAutoSwapInterval(true);
    SwappyVk_setMaxAutoSwapIntervalNS(SWAPPY_SWAP_30FPS);
    SwappyVk_setAutoPipelineMode(true);
    SwappyVk_setAutoPipelineMode(false);

    bool isEnabled = false;
    EXPECT_TRUE(SwappyVk_isEnabled(mSwapchain, &isEnabled));

    SwappyVk_setFenceTimeoutNS(50000000);
    EXPECT_EQ(SwappyVk_getFenceTimeoutNS(), 50000000U);

    uint64_t refreshRates[5];
    int count = SwappyVk_getSupportedRefreshPeriodsNS(refreshRates, 5, mSwapchain);
    // With SDK version 23 in this mock test, SwappyDisplayManager is disabled (requires SDK >= 28),
    // so 0 supported refresh periods are returned.
    EXPECT_EQ(count, 0);

    SwappyVk_enableFramePacing(mSwapchain, true);
    SwappyVk_enableBlockingWait(mSwapchain, true);
    SwappyVk_resetFramePacing(mSwapchain);
    SwappyVk_setFunctionProvider(nullptr);

    struct TracerCounters {
        int startCount = 0;
        int endCount = 0;
        int swapStartCount = 0;
        int swapEndCount = 0;
    } counters;

    SwappyTracer tracer{};
    tracer.userData = &counters;
    tracer.preWait = [](void* userData) { static_cast<TracerCounters*>(userData)->startCount++; };
    tracer.postWait = [](void* userData, int64_t, int64_t) {
        static_cast<TracerCounters*>(userData)->endCount++;
    };
    tracer.preSwapBuffers = [](void* userData) {
        static_cast<TracerCounters*>(userData)->swapStartCount++;
    };
    tracer.postSwapBuffers = [](void* userData, int64_t) {
        static_cast<TracerCounters*>(userData)->swapEndCount++;
    };
    tracer.startFrame = [](void*, int, int64_t) {};
    tracer.swapIntervalChanged = [](void*) {};

    SwappyVk_injectTracer(&tracer);

    SwappyVk_enableStats(mSwapchain, true);
    SwappyVk_clearStats(mSwapchain);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore imageAvailableSemaphore;
    VK_CHECK(vkCreateSemaphore(mDevice, &semaphoreInfo, nullptr, &imageAvailableSemaphore));

    for (int i = 0; i < 5; ++i) {
        uint32_t imageIndex;
        ASSERT_EQ(VK_SUCCESS,
                  vkAcquireNextImageKHR(mDevice, mSwapchain, UINT64_MAX, imageAvailableSemaphore,
                                        VK_NULL_HANDLE, &imageIndex));

        SwappyVk_recordFrameStart(mPresentQueue, mSwapchain, imageIndex);

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &imageAvailableSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &mSwapchain;
        presentInfo.pImageIndices = &imageIndex;

        ASSERT_EQ(VK_SUCCESS, SwappyVk_queuePresent(mPresentQueue, &presentInfo));
    }

    vkDestroySemaphore(mDevice, imageAvailableSemaphore, nullptr);

    SwappyStats stats{};
    SwappyVk_getStats(mSwapchain, &stats);
    // In a 5-iteration loop with MIN_FRAME_LAG = 5, recordFrameStart on the 5th iteration
    // collects statistics for the 4 previously presented frames.
    EXPECT_EQ(stats.totalFrames, 4U);
    EXPECT_EQ(counters.startCount, 5);
    EXPECT_EQ(counters.endCount, 5);
    EXPECT_EQ(counters.swapStartCount, 5);
    EXPECT_EQ(counters.swapEndCount, 5);

    SwappyVk_uninjectTracer(&tracer);
}

namespace {

static bool sProvider1Closed = false;
static bool sProvider2Closed = false;

static void VKAPI_ATTR mockDummyFence1() {}
static void VKAPI_ATTR mockDummyFence2() {}
static void VKAPI_ATTR mockDummyCommandPool1() {}
static void VKAPI_ATTR mockDummyCommandPool2() {}

static void* mockGetProcAddr1(const char* name) {
    if (strcmp(name, "vkCreateFence") == 0) {
        return reinterpret_cast<void*>(mockDummyFence1);
    }
    if (strcmp(name, "vkCreateCommandPool") == 0) {
        return reinterpret_cast<void*>(mockDummyCommandPool1);
    }
    return nullptr;
}

static void mockClose1() {
    sProvider1Closed = true;
}

static void* mockGetProcAddr2(const char* name) {
    if (strcmp(name, "vkCreateFence") == 0) {
        return reinterpret_cast<void*>(mockDummyFence2);
    }
    if (strcmp(name, "vkCreateCommandPool") == 0) {
        return reinterpret_cast<void*>(mockDummyCommandPool2);
    }
    return nullptr;
}

static void mockClose2() {
    sProvider2Closed = true;
}

} // namespace

TEST_F(AImageReaderVulkanSwapchainTest, SetFunctionProvider_ResetsCachedVulkanFunctions) {
    sProvider1Closed = false;
    sProvider2Closed = false;

    const SwappyVkFunctionProvider provider1{
            []() { return true; },
            mockGetProcAddr1,
            mockClose1,
    };

    const SwappyVkFunctionProvider provider2{
            []() { return true; },
            mockGetProcAddr2,
            mockClose2,
    };

    // 1. Setting provider1 and loading functions resolves pointers from provider1.
    SwappyVk_setFunctionProvider(&provider1);
    swappy::LoadVulkanFunctions(&provider1);

    EXPECT_EQ(reinterpret_cast<void*>(swappy::vkCreateFence),
              reinterpret_cast<void*>(mockDummyFence1));
    EXPECT_EQ(reinterpret_cast<void*>(swappy::vkCreateCommandPool),
              reinterpret_cast<void*>(mockDummyCommandPool1));

    // 2. Switching to provider2 must close provider1 and reset all cached Vulkan function pointers
    // to nullptr.
    SwappyVk_setFunctionProvider(&provider2);
    EXPECT_TRUE(sProvider1Closed);

    EXPECT_EQ(swappy::vkCreateCommandPool, nullptr);
    EXPECT_EQ(swappy::vkDestroyCommandPool, nullptr);
    EXPECT_EQ(swappy::vkCreateFence, nullptr);
    EXPECT_EQ(swappy::vkDestroyFence, nullptr);
    EXPECT_EQ(swappy::vkWaitForFences, nullptr);
    EXPECT_EQ(swappy::vkResetFences, nullptr);
    EXPECT_EQ(swappy::vkCreateSemaphore, nullptr);
    EXPECT_EQ(swappy::vkDestroySemaphore, nullptr);
    EXPECT_EQ(swappy::vkCreateEvent, nullptr);
    EXPECT_EQ(swappy::vkDestroyEvent, nullptr);
    EXPECT_EQ(swappy::vkCmdSetEvent, nullptr);
    EXPECT_EQ(swappy::vkAllocateCommandBuffers, nullptr);
    EXPECT_EQ(swappy::vkFreeCommandBuffers, nullptr);
    EXPECT_EQ(swappy::vkBeginCommandBuffer, nullptr);
    EXPECT_EQ(swappy::vkEndCommandBuffer, nullptr);
    EXPECT_EQ(swappy::vkQueueSubmit, nullptr);

    // 3. Loading functions with provider2 resolves pointers from the new provider instead of
    // keeping stale ones.
    swappy::LoadVulkanFunctions(&provider2);
    EXPECT_EQ(reinterpret_cast<void*>(swappy::vkCreateFence),
              reinterpret_cast<void*>(mockDummyFence2));
    EXPECT_EQ(reinterpret_cast<void*>(swappy::vkCreateCommandPool),
              reinterpret_cast<void*>(mockDummyCommandPool2));

    // 4. Resetting provider to nullptr (default) closes provider2 and resets function pointers to
    // nullptr.
    SwappyVk_setFunctionProvider(nullptr);
    EXPECT_TRUE(sProvider2Closed);
    EXPECT_EQ(swappy::vkCreateFence, nullptr);
    EXPECT_EQ(swappy::vkCreateCommandPool, nullptr);

    // 5. Setting provider to nullptr again when already default is a safe no-op.
    SwappyVk_setFunctionProvider(nullptr);
    EXPECT_EQ(swappy::vkCreateFence, nullptr);

    // 6. Direct invocation of ResetVulkanFunctions() clears any loaded pointers.
    swappy::LoadVulkanFunctions(&provider1);
    EXPECT_NE(swappy::vkCreateFence, nullptr);
    swappy::ResetVulkanFunctions();
    EXPECT_EQ(swappy::vkCreateFence, nullptr);
    EXPECT_EQ(swappy::vkCreateCommandPool, nullptr);
}

#include <dlfcn.h>

static void* gVulkanLib = nullptr;
static bool gCommandBufferBeginCalled = false;
static VkCommandBufferBeginInfo gCapturedCommandBufferBeginInfo = {};

static VkResult TestInterceptBeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                const VkCommandBufferBeginInfo* pBeginInfo) {
    gCommandBufferBeginCalled = true;
    gCapturedCommandBufferBeginInfo = *pBeginInfo;

    __android_log_print(ANDROID_LOG_INFO, "swappy_test",
                        "TestInterceptBeginCommandBuffer: captured flags=%d (simultaneous=%d)",
                        pBeginInfo->flags,
                        (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) != 0);

    if (gVulkanLib == nullptr) {
        gVulkanLib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    auto real_vkBeginCommandBuffer =
            reinterpret_cast<PFN_vkBeginCommandBuffer>(dlsym(gVulkanLib, "vkBeginCommandBuffer"));
    return real_vkBeginCommandBuffer(commandBuffer, pBeginInfo);
}

static void* TestVkFunctionProvider_GetProcAddr(const char* name) {
    if (strcmp(name, "vkBeginCommandBuffer") == 0) {
        return reinterpret_cast<void*>(TestInterceptBeginCommandBuffer);
    }
    if (gVulkanLib == nullptr) {
        gVulkanLib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    return dlsym(gVulkanLib, name);
}

static bool TestVkFunctionProvider_Init() {
    return true;
}

static void TestVkFunctionProvider_Close() {
    if (gVulkanLib != nullptr) {
        dlclose(gVulkanLib);
        gVulkanLib = nullptr;
    }
}

static SwappyVkFunctionProvider gTestVkFunctionProvider =
        {.init = TestVkFunctionProvider_Init,
         .getProcAddr = TestVkFunctionProvider_GetProcAddr,
         .close = TestVkFunctionProvider_Close};

TEST_F(AImageReaderVulkanSwapchainTest, CommandBufferFlags_DoNotUseSimultaneousBit) {
    setupMockJni(23);
    setupMockChoreographer();

    // 1. Inject custom interceptor provider
    SwappyVk_setFunctionProvider(&gTestVkFunctionProvider);

    jobject fakeActivity = reinterpret_cast<jobject>(0x1234);

    std::vector<const char*> instanceLayers;
    std::vector<const char*> deviceLayers;
    buildSwapchainForTest(instanceLayers, deviceLayers);

    gCommandBufferBeginCalled = false;
    memset(&gCapturedCommandBufferBeginInfo, 0, sizeof(gCapturedCommandBufferBeginInfo));

    uint64_t refreshDuration = 0;
    // 2. Initialise Swappy Vk (allocates and records command buffers)
    bool success = SwappyVk_initAndGetRefreshCycleDuration(gMockEnv, fakeActivity, mPhysicalDev,
                                                           mDevice, mSwapchain, &refreshDuration);
    ASSERT_TRUE(success);

    // Trigger sync object creation (calls vkAllocateCommandBuffers and vkBeginCommandBuffer)
    SwappyVk_setQueueFamilyIndex(mDevice, mPresentQueue, mPresentQueueFamily);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore imageAvailableSemaphore;
    VK_CHECK(vkCreateSemaphore(mDevice, &semaphoreInfo, nullptr, &imageAvailableSemaphore));

    uint32_t imageIndex;
    ASSERT_EQ(VK_SUCCESS,
              vkAcquireNextImageKHR(mDevice, mSwapchain, UINT64_MAX, imageAvailableSemaphore,
                                    VK_NULL_HANDLE, &imageIndex));

    SwappyVk_recordFrameStart(mPresentQueue, mSwapchain, imageIndex);

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &imageAvailableSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &mSwapchain;
    presentInfo.pImageIndices = &imageIndex;

    ASSERT_EQ(VK_SUCCESS, SwappyVk_queuePresent(mPresentQueue, &presentInfo));

    vkDestroySemaphore(mDevice, imageAvailableSemaphore, nullptr);

    // 3. Verify vkBeginCommandBuffer was intercepted
    EXPECT_TRUE(gCommandBufferBeginCalled);

    // 4. Assert that the SIMULTANEOUS_USE_BIT (0x00000004) is absent
    EXPECT_EQ(gCapturedCommandBufferBeginInfo.flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
              0U);

    cleanupMockChoreographer();
    cleanUpSwapchainForTest();

    // 5. Restore default function provider
    SwappyVk_setFunctionProvider(nullptr);
}

namespace {

template <typename Handle>
Handle makeFakeVkHandle(uint64_t id) {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(static_cast<uintptr_t>(id));
    } else {
        return static_cast<Handle>(id);
    }
}

struct RecordedQueueSubmit {
    VkSemaphore waitSemaphore = VK_NULL_HANDLE;
    VkSemaphore signalSemaphore = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};

struct RecordedQueuePresent {
    uint32_t imageIndex = UINT32_MAX;
    VkSemaphore waitSemaphore = VK_NULL_HANDLE;
};

class FakeSwapchainPresentEnv {
public:
    FakeSwapchainPresentEnv() {
        sActive = this;
    }

    ~FakeSwapchainPresentEnv() {
        if (sActive == this) {
            sActive = nullptr;
        }
    }

    static const SwappyVkFunctionProvider* provider() {
        return &kProvider;
    }

    VkSemaphore allocateAppAcquireSemaphore() {
        std::lock_guard<std::mutex> lock(mMutex);
        return makeFakeVkHandle<VkSemaphore>(++mNextHandleId);
    }

    void holdNextFenceUnsignaled() {
        std::lock_guard<std::mutex> lock(mMutex);
        mHoldNextSubmittedFenceUnsignaled = true;
    }

    void markHeldFenceSignaled(VkFence fence) {
        std::lock_guard<std::mutex> lock(mMutex);
        mFencesHeldUnsignaled.erase(fence);
        mFenceSignaled[fence] = true;
    }

    void failNextCreateSemaphore(VkResult errorResult) {
        std::lock_guard<std::mutex> lock(mMutex);
        mNextCreateSemaphoreResult = errorResult;
    }

    void waitForPendingFenceWaiters() {
        std::unique_lock<std::mutex> lock(mMutex);
        mFenceWaitCv.wait(lock, [this]() {
            return mWaitForFencesCompletedCount >= mSubmittedFenceCount;
        });
    }

    std::vector<VkSemaphore> createdSemaphores() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mCreatedSemaphores;
    }

    std::vector<VkSemaphore> destroyedSemaphores() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mDestroyedSemaphores;
    }

    std::vector<RecordedQueueSubmit> submits() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mSubmits;
    }

    std::vector<RecordedQueuePresent> presents() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mPresents;
    }

private:
    static bool providerInit() {
        return true;
    }

    static void providerClose() {}

    static VKAPI_ATTR VkResult VKAPI_CALL fakeCreateCommandPool(VkDevice,
                                                                const VkCommandPoolCreateInfo*,
                                                                const VkAllocationCallbacks*,
                                                                VkCommandPool* pCommandPool) {
        std::lock_guard<std::mutex> lock(sActive->mMutex);
        *pCommandPool = makeFakeVkHandle<VkCommandPool>(++sActive->mNextHandleId);
        return VK_SUCCESS;
    }

    static VKAPI_ATTR void VKAPI_CALL fakeDestroyCommandPool(VkDevice, VkCommandPool,
                                                             const VkAllocationCallbacks*) {}

    static VKAPI_ATTR VkResult VKAPI_CALL fakeCreateFence(VkDevice,
                                                          const VkFenceCreateInfo* pCreateInfo,
                                                          const VkAllocationCallbacks*,
                                                          VkFence* pFence) {
        std::lock_guard<std::mutex> lock(sActive->mMutex);
        VkFence fence = makeFakeVkHandle<VkFence>(++sActive->mNextHandleId);
        sActive->mFenceSignaled[fence] = (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
        *pFence = fence;
        return VK_SUCCESS;
    }

    static VKAPI_ATTR void VKAPI_CALL fakeDestroyFence(VkDevice, VkFence fence,
                                                       const VkAllocationCallbacks*) {
        std::lock_guard<std::mutex> lock(sActive->mMutex);
        sActive->mFenceSignaled.erase(fence);
        sActive->mFencesHeldUnsignaled.erase(fence);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL fakeWaitForFences(VkDevice, uint32_t fenceCount,
                                                            const VkFence* pFences, VkBool32,
                                                            uint64_t) {
        std::lock_guard<std::mutex> lock(sActive->mMutex);
        VkResult result = VK_SUCCESS;
        for (uint32_t i = 0; i < fenceCount; ++i) {
            if (sActive->mFencesHeldUnsignaled.count(pFences[i]) > 0) {
                result = VK_TIMEOUT;
            } else {
                sActive->mFenceSignaled[pFences[i]] = true;
            }
        }
        sActive->mWaitForFencesCompletedCount++;
        sActive->mFenceWaitCv.notify_all();
        return result;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL fakeGetFenceStatus(VkDevice, VkFence fence) {
        std::lock_guard<std::mutex> lock(sActive->mMutex);
        auto it = sActive->mFenceSignaled.find(fence);
        if (it != sActive->mFenceSignaled.end() && it->second) {
            return VK_SUCCESS;
        }
        return VK_NOT_READY;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL fakeResetFences(VkDevice, uint32_t fenceCount,
                                                          const VkFence* pFences) {
        std::lock_guard<std::mutex> lock(sActive->mMutex);
        for (uint32_t i = 0; i < fenceCount; ++i) {
            sActive->mFenceSignaled[pFences[i]] = false;
        }
        return VK_SUCCESS;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL fakeCreateSemaphore(VkDevice,
                                                              const VkSemaphoreCreateInfo*,
                                                              const VkAllocationCallbacks*,
                                                              VkSemaphore* pSemaphore) {
        std::lock_guard<std::mutex> lock(sActive->mMutex);
        if (sActive->mNextCreateSemaphoreResult != VK_SUCCESS) {
            VkResult errorResult = sActive->mNextCreateSemaphoreResult;
            sActive->mNextCreateSemaphoreResult = VK_SUCCESS;
            // Output parameters are undefined after a failed call; model a driver that scribbles.
            *pSemaphore = makeFakeVkHandle<VkSemaphore>(0xdeadbeef);
            return errorResult;
        }
        VkSemaphore semaphore = makeFakeVkHandle<VkSemaphore>(++sActive->mNextHandleId);
        sActive->mCreatedSemaphores.push_back(semaphore);
        *pSemaphore = semaphore;
        return VK_SUCCESS;
    }

    static VKAPI_ATTR void VKAPI_CALL fakeDestroySemaphore(VkDevice, VkSemaphore semaphore,
                                                           const VkAllocationCallbacks*) {
        std::lock_guard<std::mutex> lock(sActive->mMutex);
        sActive->mDestroyedSemaphores.push_back(semaphore);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL fakeCreateEvent(VkDevice, const VkEventCreateInfo*,
                                                          const VkAllocationCallbacks*,
                                                          VkEvent* pEvent) {
        std::lock_guard<std::mutex> lock(sActive->mMutex);
        *pEvent = makeFakeVkHandle<VkEvent>(++sActive->mNextHandleId);
        return VK_SUCCESS;
    }

    static VKAPI_ATTR void VKAPI_CALL fakeDestroyEvent(VkDevice, VkEvent,
                                                       const VkAllocationCallbacks*) {}

    static VKAPI_ATTR void VKAPI_CALL fakeCmdSetEvent(VkCommandBuffer, VkEvent,
                                                      VkPipelineStageFlags) {}

    static VKAPI_ATTR VkResult VKAPI_CALL
    fakeAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo* pAllocateInfo,
                               VkCommandBuffer* pCommandBuffers) {
        std::lock_guard<std::mutex> lock(sActive->mMutex);
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
            pCommandBuffers[i] = makeFakeVkHandle<VkCommandBuffer>(++sActive->mNextHandleId);
        }
        return VK_SUCCESS;
    }

    static VKAPI_ATTR void VKAPI_CALL fakeFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t,
                                                             const VkCommandBuffer*) {}

    static VKAPI_ATTR VkResult VKAPI_CALL fakeBeginCommandBuffer(VkCommandBuffer,
                                                                 const VkCommandBufferBeginInfo*) {
        return VK_SUCCESS;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL fakeEndCommandBuffer(VkCommandBuffer) {
        return VK_SUCCESS;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL fakeQueueSubmit(VkQueue, uint32_t submitCount,
                                                          const VkSubmitInfo* pSubmits,
                                                          VkFence fence) {
        std::lock_guard<std::mutex> lock(sActive->mMutex);
        RecordedQueueSubmit record{};
        if (submitCount > 0 && pSubmits != nullptr) {
            if (pSubmits[0].waitSemaphoreCount > 0 && pSubmits[0].pWaitSemaphores != nullptr) {
                record.waitSemaphore = pSubmits[0].pWaitSemaphores[0];
            }
            if (pSubmits[0].signalSemaphoreCount > 0 && pSubmits[0].pSignalSemaphores != nullptr) {
                record.signalSemaphore = pSubmits[0].pSignalSemaphores[0];
            }
        }
        record.fence = fence;
        sActive->mSubmits.push_back(record);

        if (sActive->mHoldNextSubmittedFenceUnsignaled) {
            sActive->mHoldNextSubmittedFenceUnsignaled = false;
            sActive->mFencesHeldUnsignaled.insert(fence);
            sActive->mFenceSignaled[fence] = false;
        } else {
            sActive->mFenceSignaled[fence] = true;
        }
        sActive->mSubmittedFenceCount++;
        return VK_SUCCESS;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL
    fakeQueuePresentKHR(VkQueue, const VkPresentInfoKHR* pPresentInfo) {
        std::lock_guard<std::mutex> lock(sActive->mMutex);
        RecordedQueuePresent record{};
        if (pPresentInfo->swapchainCount > 0 && pPresentInfo->pImageIndices != nullptr) {
            record.imageIndex = pPresentInfo->pImageIndices[0];
        }
        if (pPresentInfo->waitSemaphoreCount > 0 && pPresentInfo->pWaitSemaphores != nullptr) {
            record.waitSemaphore = pPresentInfo->pWaitSemaphores[0];
        }
        sActive->mPresents.push_back(record);
        return VK_SUCCESS;
    }

    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fakeGetDeviceProcAddr(VkDevice,
                                                                          const char* name) {
        if (strcmp(name, "vkQueuePresentKHR") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(fakeQueuePresentKHR);
        }
        return nullptr;
    }

    static void* providerGetProcAddr(const char* name) {
        if (strcmp(name, "vkCreateCommandPool") == 0) {
            return reinterpret_cast<void*>(fakeCreateCommandPool);
        }
        if (strcmp(name, "vkDestroyCommandPool") == 0) {
            return reinterpret_cast<void*>(fakeDestroyCommandPool);
        }
        if (strcmp(name, "vkCreateFence") == 0) {
            return reinterpret_cast<void*>(fakeCreateFence);
        }
        if (strcmp(name, "vkDestroyFence") == 0) {
            return reinterpret_cast<void*>(fakeDestroyFence);
        }
        if (strcmp(name, "vkWaitForFences") == 0) {
            return reinterpret_cast<void*>(fakeWaitForFences);
        }
        if (strcmp(name, "vkGetFenceStatus") == 0) {
            return reinterpret_cast<void*>(fakeGetFenceStatus);
        }
        if (strcmp(name, "vkResetFences") == 0) {
            return reinterpret_cast<void*>(fakeResetFences);
        }
        if (strcmp(name, "vkCreateSemaphore") == 0) {
            return reinterpret_cast<void*>(fakeCreateSemaphore);
        }
        if (strcmp(name, "vkDestroySemaphore") == 0) {
            return reinterpret_cast<void*>(fakeDestroySemaphore);
        }
        if (strcmp(name, "vkCreateEvent") == 0) {
            return reinterpret_cast<void*>(fakeCreateEvent);
        }
        if (strcmp(name, "vkDestroyEvent") == 0) {
            return reinterpret_cast<void*>(fakeDestroyEvent);
        }
        if (strcmp(name, "vkCmdSetEvent") == 0) {
            return reinterpret_cast<void*>(fakeCmdSetEvent);
        }
        if (strcmp(name, "vkAllocateCommandBuffers") == 0) {
            return reinterpret_cast<void*>(fakeAllocateCommandBuffers);
        }
        if (strcmp(name, "vkFreeCommandBuffers") == 0) {
            return reinterpret_cast<void*>(fakeFreeCommandBuffers);
        }
        if (strcmp(name, "vkBeginCommandBuffer") == 0) {
            return reinterpret_cast<void*>(fakeBeginCommandBuffer);
        }
        if (strcmp(name, "vkEndCommandBuffer") == 0) {
            return reinterpret_cast<void*>(fakeEndCommandBuffer);
        }
        if (strcmp(name, "vkQueueSubmit") == 0) {
            return reinterpret_cast<void*>(fakeQueueSubmit);
        }
        if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
            return reinterpret_cast<void*>(fakeGetDeviceProcAddr);
        }
        return nullptr;
    }

    inline static FakeSwapchainPresentEnv* sActive = nullptr;
    inline static const SwappyVkFunctionProvider kProvider = {
            .init = &FakeSwapchainPresentEnv::providerInit,
            .getProcAddr = &FakeSwapchainPresentEnv::providerGetProcAddr,
            .close = &FakeSwapchainPresentEnv::providerClose,
    };

    mutable std::mutex mMutex;
    std::condition_variable mFenceWaitCv;
    uint64_t mNextHandleId = 0x1000;
    std::vector<VkSemaphore> mCreatedSemaphores;
    std::vector<VkSemaphore> mDestroyedSemaphores;
    std::map<VkFence, bool> mFenceSignaled;
    std::set<VkFence> mFencesHeldUnsignaled;
    bool mHoldNextSubmittedFenceUnsignaled = false;
    VkResult mNextCreateSemaphoreResult = VK_SUCCESS;
    uint64_t mSubmittedFenceCount = 0;
    uint64_t mWaitForFencesCompletedCount = 0;
    std::vector<RecordedQueueSubmit> mSubmits;
    std::vector<RecordedQueuePresent> mPresents;
};

class FakeSwapchainSemaphoreSession {
public:
    explicit FakeSwapchainSemaphoreSession(AImageReaderVulkanSwapchainTest* test)
          : mTest(test),
            mPhysicalDevice(makeFakeVkHandle<VkPhysicalDevice>(0x101)),
            mDevice(makeFakeVkHandle<VkDevice>(0x102)),
            mSwapchain(makeFakeVkHandle<VkSwapchainKHR>(0x103)),
            mQueue(makeFakeVkHandle<VkQueue>(0x104)) {
        mTest->setupMockJni(23);
        mTest->setupMockChoreographer();
        SwappyVk_setFunctionProvider(FakeSwapchainPresentEnv::provider());
    }

    ~FakeSwapchainSemaphoreSession() {
        destroySwapchain();
        SwappyVk_setFunctionProvider(nullptr);
    }

    bool initSwappy() {
        uint64_t refreshDuration = 0;
        jobject fakeActivity = reinterpret_cast<jobject>(0x1234);
        if (!SwappyVk_initAndGetRefreshCycleDuration(gMockEnv, fakeActivity, mPhysicalDevice,
                                                     mDevice, mSwapchain, &refreshDuration)) {
            return false;
        }
        SwappyVk_setQueueFamilyIndex(mDevice, mQueue, 0);
        mSwapchainInitialized = true;
        return refreshDuration > 0;
    }

    VkResult presentImage(uint32_t imageIndex, VkSemaphore appWaitSemaphore) {
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &appWaitSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &mSwapchain;
        presentInfo.pImageIndices = &imageIndex;

        VkResult res = SwappyVk_queuePresent(mQueue, &presentInfo);
        // The fence waiter handles syncs in submission order, so once it has waited on the newest
        // fence every older sync has already been handed back for reuse by the next present.
        mEnv.waitForPendingFenceWaiters();
        return res;
    }

    void destroySwapchain() {
        mTest->cleanupMockChoreographer();
        if (mSwapchainInitialized) {
            SwappyVk_destroySwapchain(mDevice, mSwapchain);
            SwappyVk_destroyDevice(mDevice);
            mSwapchainInitialized = false;
        }
    }

    FakeSwapchainPresentEnv& env() {
        return mEnv;
    }

private:
    AImageReaderVulkanSwapchainTest* mTest;
    FakeSwapchainPresentEnv mEnv;
    VkPhysicalDevice mPhysicalDevice;
    VkDevice mDevice;
    VkSwapchainKHR mSwapchain;
    VkQueue mQueue;
    bool mSwapchainInitialized = false;
};

} // namespace

TEST_F(AImageReaderVulkanSwapchainTest,
       QueuePresent_PerSwapchainImageSemaphoresAreDistinctAndReusedAfterReacquire) {
    FakeSwapchainSemaphoreSession session(this);
    ASSERT_TRUE(session.initSwappy());

    const VkSemaphore appAcquireSem0 = session.env().allocateAppAcquireSemaphore();
    const VkSemaphore appAcquireSem1 = session.env().allocateAppAcquireSemaphore();
    const VkSemaphore appAcquireSem2 = session.env().allocateAppAcquireSemaphore();
    const VkSemaphore appAcquireSem3 = session.env().allocateAppAcquireSemaphore();
    const VkSemaphore appAcquireSem4 = session.env().allocateAppAcquireSemaphore();

    // First 5-image swapchain presentation cycle: images 0, 1, 2, 3, 4.
    ASSERT_EQ(VK_SUCCESS, session.presentImage(0U, appAcquireSem0));
    ASSERT_EQ(VK_SUCCESS, session.presentImage(1U, appAcquireSem1));
    ASSERT_EQ(VK_SUCCESS, session.presentImage(2U, appAcquireSem2));
    ASSERT_EQ(VK_SUCCESS, session.presentImage(3U, appAcquireSem3));
    ASSERT_EQ(VK_SUCCESS, session.presentImage(4U, appAcquireSem4));

    const std::vector<VkSemaphore> createdAfterFirstCycle = session.env().createdSemaphores();
    ASSERT_EQ(5U, createdAfterFirstCycle.size());

    // Second 5-image presentation cycle after re-acquire: images 0, 1, 2, 3, 4.
    ASSERT_EQ(VK_SUCCESS, session.presentImage(0U, appAcquireSem0));
    ASSERT_EQ(VK_SUCCESS, session.presentImage(1U, appAcquireSem1));
    ASSERT_EQ(VK_SUCCESS, session.presentImage(2U, appAcquireSem2));
    ASSERT_EQ(VK_SUCCESS, session.presentImage(3U, appAcquireSem3));
    ASSERT_EQ(VK_SUCCESS, session.presentImage(4U, appAcquireSem4));

    // No additional semaphores should be allocated on the second cycle.
    EXPECT_EQ(5U, session.env().createdSemaphores().size());

    const std::vector<RecordedQueueSubmit> submits = session.env().submits();
    const std::vector<RecordedQueuePresent> presents = session.env().presents();
    ASSERT_EQ(10U, submits.size());
    ASSERT_EQ(10U, presents.size());

    // Cycle 1: each swapchain image index signals and waits on its own distinct semaphore.
    EXPECT_EQ(appAcquireSem0, submits[0].waitSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[0], submits[0].signalSemaphore);
    EXPECT_EQ(0U, presents[0].imageIndex);
    EXPECT_EQ(createdAfterFirstCycle[0], presents[0].waitSemaphore);

    EXPECT_EQ(appAcquireSem1, submits[1].waitSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[1], submits[1].signalSemaphore);
    EXPECT_EQ(1U, presents[1].imageIndex);
    EXPECT_EQ(createdAfterFirstCycle[1], presents[1].waitSemaphore);

    EXPECT_EQ(appAcquireSem2, submits[2].waitSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[2], submits[2].signalSemaphore);
    EXPECT_EQ(2U, presents[2].imageIndex);
    EXPECT_EQ(createdAfterFirstCycle[2], presents[2].waitSemaphore);

    EXPECT_EQ(appAcquireSem3, submits[3].waitSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[3], submits[3].signalSemaphore);
    EXPECT_EQ(3U, presents[3].imageIndex);
    EXPECT_EQ(createdAfterFirstCycle[3], presents[3].waitSemaphore);

    EXPECT_EQ(appAcquireSem4, submits[4].waitSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[4], submits[4].signalSemaphore);
    EXPECT_EQ(4U, presents[4].imageIndex);
    EXPECT_EQ(createdAfterFirstCycle[4], presents[4].waitSemaphore);

    // Cycle 2: each re-acquired image index reuses its own semaphore.
    EXPECT_EQ(createdAfterFirstCycle[0], submits[5].signalSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[0], presents[5].waitSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[1], submits[6].signalSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[1], presents[6].waitSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[2], submits[7].signalSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[2], presents[7].waitSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[3], submits[8].signalSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[3], presents[8].waitSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[4], submits[9].signalSemaphore);
    EXPECT_EQ(createdAfterFirstCycle[4], presents[9].waitSemaphore);

    // Teardown destroys all 5 per-image semaphores.
    EXPECT_EQ(0U, session.env().destroyedSemaphores().size());
    session.destroySwapchain();
    EXPECT_EQ(createdAfterFirstCycle, session.env().destroyedSemaphores());
}

TEST_F(AImageReaderVulkanSwapchainTest,
       QueuePresent_SkipsSemaphoreReuseWhilePriorFenceSubmissionPending) {
    FakeSwapchainSemaphoreSession session(this);
    ASSERT_TRUE(session.initSwappy());

    const VkSemaphore appAcquireSem0 = session.env().allocateAppAcquireSemaphore();
    const VkSemaphore appAcquireSem1 = session.env().allocateAppAcquireSemaphore();
    const VkSemaphore appAcquireSem2 = session.env().allocateAppAcquireSemaphore();
    const VkSemaphore appAcquireSem3 = session.env().allocateAppAcquireSemaphore();

    // Frame 0: Present image 1 with normal fence signaling so a signaled fence remains in the
    // free pool when image 0 is later re-presented.
    ASSERT_EQ(VK_SUCCESS, session.presentImage(1U, appAcquireSem0));

    // Frame 1: Present image 0 while holding its submitted fence unsignaled (simulating a fence
    // wait timeout where GPU work for image 0's semaphore is still in flight).
    session.env().holdNextFenceUnsignaled();
    ASSERT_EQ(VK_SUCCESS, session.presentImage(0U, appAcquireSem1));

    const std::vector<VkSemaphore> createdSemaphores = session.env().createdSemaphores();
    ASSERT_EQ(2U, createdSemaphores.size());
    const VkSemaphore image0Semaphore = createdSemaphores[1];
    const VkFence pendingImage0Fence = session.env().submits()[1].fence;

    // Frame 2: Re-presenting image 0 while pendingImage0Fence is still VK_NOT_READY must NOT
    // re-signal image0Semaphore via vkQueueSubmit, and must forward appAcquireSem2 directly to
    // vkQueuePresentKHR.
    ASSERT_EQ(VK_SUCCESS, session.presentImage(0U, appAcquireSem2));
    EXPECT_EQ(2U, session.env().submits().size());
    ASSERT_EQ(3U, session.env().presents().size());
    EXPECT_EQ(0U, session.env().presents()[2].imageIndex);
    EXPECT_EQ(appAcquireSem2, session.env().presents()[2].waitSemaphore);

    // Frame 3: Once pendingImage0Fence signals, presenting image 0 reuses image0Semaphore.
    session.env().markHeldFenceSignaled(pendingImage0Fence);
    ASSERT_EQ(VK_SUCCESS, session.presentImage(0U, appAcquireSem3));
    EXPECT_EQ(2U, session.env().createdSemaphores().size());
    ASSERT_EQ(3U, session.env().submits().size());
    EXPECT_EQ(image0Semaphore, session.env().submits()[2].signalSemaphore);
    ASSERT_EQ(4U, session.env().presents().size());
    EXPECT_EQ(image0Semaphore, session.env().presents()[3].waitSemaphore);
}

TEST_F(AImageReaderVulkanSwapchainTest, QueuePresent_PropagatesCreateSemaphoreFailure) {
    FakeSwapchainSemaphoreSession session(this);
    ASSERT_TRUE(session.initSwappy());

    const VkSemaphore appAcquireSem0 = session.env().allocateAppAcquireSemaphore();
    const VkSemaphore appAcquireSem1 = session.env().allocateAppAcquireSemaphore();

    session.env().failNextCreateSemaphore(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    EXPECT_EQ(VK_ERROR_OUT_OF_DEVICE_MEMORY, session.presentImage(0U, appAcquireSem0));
    EXPECT_EQ(0U, session.env().createdSemaphores().size());
    EXPECT_EQ(0U, session.env().submits().size());
    EXPECT_EQ(0U, session.env().presents().size());

    // The next present creates a fresh semaphore instead of reusing the failed call's undefined
    // output, and the failed attempt did not leak the free fence.
    ASSERT_EQ(VK_SUCCESS, session.presentImage(0U, appAcquireSem1));
    ASSERT_EQ(1U, session.env().createdSemaphores().size());
    ASSERT_EQ(1U, session.env().submits().size());
    ASSERT_EQ(1U, session.env().presents().size());
    EXPECT_EQ(session.env().createdSemaphores()[0], session.env().presents()[0].waitSemaphore);
}

TEST_F(AImageReaderVulkanSwapchainTest,
       QueuePresent_RecycledFenceDoesNotBlockOtherImageSemaphores) {
    FakeSwapchainSemaphoreSession session(this);
    ASSERT_TRUE(session.initSwappy());

    const VkSemaphore appAcquireSem0 = session.env().allocateAppAcquireSemaphore();
    const VkSemaphore appAcquireSem1 = session.env().allocateAppAcquireSemaphore();
    const VkSemaphore appAcquireSem2 = session.env().allocateAppAcquireSemaphore();
    const VkSemaphore appAcquireSem3 = session.env().allocateAppAcquireSemaphore();

    // Images 0 and 1 use the two pooled fences; image 2 then recycles image 0's fence and keeps
    // that new submission in flight.
    ASSERT_EQ(VK_SUCCESS, session.presentImage(0U, appAcquireSem0));
    ASSERT_EQ(VK_SUCCESS, session.presentImage(1U, appAcquireSem1));
    session.env().holdNextFenceUnsignaled();
    ASSERT_EQ(VK_SUCCESS, session.presentImage(2U, appAcquireSem2));
    const std::vector<RecordedQueueSubmit> firstSubmits = session.env().submits();
    ASSERT_EQ(3U, firstSubmits.size());
    ASSERT_EQ(firstSubmits[0].fence, firstSubmits[2].fence);

    // Image 0's own submission has retired, so image 2's in-flight reuse of that fence must not
    // stop image 0's semaphore from being signaled again.
    ASSERT_EQ(VK_SUCCESS, session.presentImage(0U, appAcquireSem3));
    const std::vector<VkSemaphore> createdSemaphores = session.env().createdSemaphores();
    const std::vector<RecordedQueueSubmit> submits = session.env().submits();
    const std::vector<RecordedQueuePresent> presents = session.env().presents();
    ASSERT_EQ(3U, createdSemaphores.size());
    ASSERT_EQ(4U, submits.size());
    EXPECT_EQ(createdSemaphores[0], submits[3].signalSemaphore);
    ASSERT_EQ(4U, presents.size());
    EXPECT_EQ(createdSemaphores[0], presents[3].waitSemaphore);
}

class AImageReaderEGLSwapchainTest : public AImageReaderSwapchainTestBase {
public:
    EGLDisplay mDisplay = EGL_NO_DISPLAY;
    EGLSurface mSurface = EGL_NO_SURFACE;
    EGLContext mContext = EGL_NO_CONTEXT;

    void createEGLContext() {
        mDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        ASSERT_NE(mDisplay, EGL_NO_DISPLAY);

        EGLint major, minor;
        ASSERT_TRUE(eglInitialize(mDisplay, &major, &minor));

        const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE,
                                        EGL_OPENGL_ES2_BIT,
                                        EGL_SURFACE_TYPE,
                                        EGL_WINDOW_BIT,
                                        EGL_BLUE_SIZE,
                                        8,
                                        EGL_GREEN_SIZE,
                                        8,
                                        EGL_RED_SIZE,
                                        8,
                                        EGL_NONE};

        EGLConfig config;
        EGLint numConfigs;
        ASSERT_TRUE(eglChooseConfig(mDisplay, configAttribs, &config, 1, &numConfigs));
        ASSERT_GT(numConfigs, 0);

        const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        mContext = eglCreateContext(mDisplay, config, EGL_NO_CONTEXT, contextAttribs);
        ASSERT_NE(mContext, EGL_NO_CONTEXT);

        mSurface = eglCreateWindowSurface(mDisplay, config, mWindow, nullptr);
        ASSERT_NE(mSurface, EGL_NO_SURFACE);

        ASSERT_TRUE(eglMakeCurrent(mDisplay, mSurface, mSurface, mContext));
    }

    void cleanUpEGLForTest() {
        if (mDisplay != EGL_NO_DISPLAY) {
            eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (mSurface != EGL_NO_SURFACE) {
                eglDestroySurface(mDisplay, mSurface);
            }
            if (mContext != EGL_NO_CONTEXT) {
                eglDestroyContext(mDisplay, mContext);
            }
            eglTerminate(mDisplay);
        }
        mDisplay = EGL_NO_DISPLAY;
        mSurface = EGL_NO_SURFACE;
        mContext = EGL_NO_CONTEXT;
    }

    void TearDown() override {
        cleanupMockChoreographer();
        cleanupMockNDKChoreographer();
        SwappyGL_destroy();
        cleanUpEGLForTest();
        AImageReaderSwapchainTestBase::TearDown();
    }

    void buildEGLForTest() {
        AImageReader_ImageListener listener{};
        listener.context = this;
        listener.onImageAvailable = &AImageReaderSwapchainTestBase::onImageAvailable;
        createAImageReader(640, 480, AIMAGE_FORMAT_RGBA_8888, 3, &listener);
        getANativeWindowFromReader();
        createEGLContext();
    }
};

TEST_F(AImageReaderEGLSwapchainTest, TestHelperMethods) {
    buildEGLForTest();

    ASSERT_NE(mDisplay, EGL_NO_DISPLAY);
    ASSERT_NE(mSurface, EGL_NO_SURFACE);
    ASSERT_NE(mContext, EGL_NO_CONTEXT);
}

TEST_F(AImageReaderEGLSwapchainTest, Initialization) {
    setupMockJni(37);

    jobject fakeActivity = reinterpret_cast<jobject>(0x1234);

    buildEGLForTest();

    bool success = SwappyGL_init(gMockEnv, fakeActivity);
    EXPECT_TRUE(success);

    EXPECT_TRUE(SwappyGL_setWindow(mWindow));

    SwappyGL_destroy();
}

TEST_F(AImageReaderEGLSwapchainTest, RenderingLoop) {
    // Set up the mocked JNI environment with an SDK version of 23.
    setupMockJni(23);
    setupMockChoreographer();

    jobject fakeActivity = reinterpret_cast<jobject>(0x1234);

    buildEGLForTest();

    bool success = SwappyGL_init(gMockEnv, fakeActivity);
    EXPECT_TRUE(success);

    EXPECT_TRUE(SwappyGL_setWindow(mWindow));
    SwappyGL_setSwapIntervalNS(SWAPPY_SWAP_60FPS);

    for (int i = 0; i < 10; ++i) {
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        SwappyGL_recordFrameStart(mDisplay, mSurface);
        EXPECT_TRUE(SwappyGL_swap(mDisplay, mSurface));
    }

    // Clean up existing mock threads before destroying Swappy to prevent them from
    // calling into a destroyed Swappy instance. TearDown() will clean up any
    // additional threads spawned during destruction.
    cleanupMockChoreographer();

    SwappyGL_destroy();
}

TEST_F(AImageReaderEGLSwapchainTest, CApiAndStatsTest) {
    setupMockJni(23);
    setupMockChoreographer();

    jobject fakeActivity = reinterpret_cast<jobject>(0x1234);
    buildEGLForTest();

    EXPECT_TRUE(SwappyGL_init(gMockEnv, fakeActivity));
    EXPECT_TRUE(SwappyGL_setWindow(mWindow));

    SwappyGL_setSwapIntervalNS(SWAPPY_SWAP_60FPS);
    EXPECT_EQ(SwappyGL_getSwapIntervalNS(), SWAPPY_SWAP_60FPS);
    EXPECT_GT(SwappyGL_getRefreshPeriodNanos(), 0U);

    SwappyGL_setUseAffinity(true);
    EXPECT_TRUE(SwappyGL_getUseAffinity());
    SwappyGL_setUseAffinity(false);
    EXPECT_FALSE(SwappyGL_getUseAffinity());

    SwappyGL_setAutoSwapInterval(true);
    SwappyGL_setMaxAutoSwapIntervalNS(SWAPPY_SWAP_30FPS);
    SwappyGL_setAutoPipelineMode(true);
    SwappyGL_setAutoPipelineMode(false);

    EXPECT_TRUE(SwappyGL_isEnabled());

    SwappyGL_setFenceTimeoutNS(50000000);
    EXPECT_EQ(SwappyGL_getFenceTimeoutNS(), 50000000U);

    SwappyGL_setBufferStuffingFixWait(2);

    uint64_t refreshRates[5];
    int count = SwappyGL_getSupportedRefreshPeriodsNS(refreshRates, 5);
    EXPECT_GE(count, 0);

    SwappyGL_enableFramePacing(true);
    SwappyGL_enableBlockingWait(true);
    SwappyGL_resetFramePacing();

    // These counter variables are static so that the lambdas below can remain
    // non-capturing (and thus convertible to C-style function pointers).
    static int tracerStartCount = 0;
    static int tracerEndCount = 0;
    static int tracerSwapStartCount = 0;
    static int tracerSwapEndCount = 0;
    tracerStartCount = 0;
    tracerEndCount = 0;
    tracerSwapStartCount = 0;
    tracerSwapEndCount = 0;

    SwappyTracer tracer{};
    tracer.preWait = [](void*) { tracerStartCount++; };
    tracer.postWait = [](void*, int64_t, int64_t) { tracerEndCount++; };
    tracer.preSwapBuffers = [](void*) { tracerSwapStartCount++; };
    tracer.postSwapBuffers = [](void*, int64_t) { tracerSwapEndCount++; };
    tracer.startFrame = [](void*, int, int64_t) {};
    tracer.swapIntervalChanged = [](void*) {};

    SwappyGL_injectTracer(&tracer);

    SwappyGL_enableStats(true);
    SwappyGL_clearStats();

    for (int i = 0; i < 5; ++i) {
        SwappyGL_recordFrameStart(mDisplay, mSurface);
        EXPECT_TRUE(SwappyGL_swap(mDisplay, mSurface));
    }

    SwappyStats stats{};
    SwappyGL_getStats(&stats);
    // For offscreen AImageReader surfaces, EGL presentation timestamps are either
    // unsupported or remain pending, so totalFrames is expected to be 0.
    EXPECT_EQ(stats.totalFrames, 0U);
    EXPECT_EQ(tracerStartCount, 5);
    EXPECT_EQ(tracerEndCount, 5);
    EXPECT_EQ(tracerSwapStartCount, 5);
    EXPECT_EQ(tracerSwapEndCount, 5);

    SwappyGL_uninjectTracer(&tracer);

    // Clean up existing mock threads before destroying Swappy to prevent them from
    // calling into a destroyed Swappy instance. TearDown() will clean up any
    // additional threads spawned during destruction.
    cleanupMockChoreographer();
    SwappyGL_destroy();
}

TEST_F(AImageReaderVulkanSwapchainTest, RenderingLoopSDK37) {
    // Set up the mocked JNI environment with an SDK version of 37 (Android U+).
    // This will force Swappy to use NDKChoreographerThread instead of JavaChoreographerThread.
    setupMockJni(37);
    setupMockNDKChoreographer();

    jobject fakeActivity = reinterpret_cast<jobject>(0x1234);

    // Build swapchain to provide a valid device and swapchain to Swappy
    std::vector<const char*> instanceLayers;
    std::vector<const char*> deviceLayers;
    buildSwapchainForTest(instanceLayers, deviceLayers);

    uint64_t refreshDuration = 0;
    bool success = SwappyVk_initAndGetRefreshCycleDuration(gMockEnv, fakeActivity, mPhysicalDev,
                                                           mDevice, mSwapchain, &refreshDuration);

    EXPECT_TRUE(success);

    SwappyVk_setQueueFamilyIndex(mDevice, mPresentQueue, mPresentQueueFamily);
    SwappyVk_setSwapIntervalNS(mDevice, mSwapchain, 16666666);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore imageAvailableSemaphore;
    VK_CHECK(vkCreateSemaphore(mDevice, &semaphoreInfo, nullptr, &imageAvailableSemaphore));

    // Spin for 10 frames, acquiring and presenting to verify the rendering loop
    // correctly interacts with our mocked NDK choreographer.
    for (int i = 0; i < 10; ++i) {
        LOGI("[Test] Frame %d starting", i);
        uint32_t imageIndex;
        VkResult res = vkAcquireNextImageKHR(mDevice, mSwapchain, UINT64_MAX,
                                             imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
        ASSERT_EQ(res, VK_SUCCESS);
        SwappyVk_recordFrameStart(mPresentQueue, mSwapchain, imageIndex);

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &imageAvailableSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &mSwapchain;
        presentInfo.pImageIndices = &imageIndex;

        // Use Swappy to present
        res = SwappyVk_queuePresent(mPresentQueue, &presentInfo);
        EXPECT_EQ(res, VK_SUCCESS);
        LOGI("[Test] Frame %d presented, res = %d", i, res);
    }

    vkDestroySemaphore(mDevice, imageAvailableSemaphore, nullptr);
}

TEST_F(AImageReaderEGLSwapchainTest, RefreshRateCallbacksReleaseLocalRefsOnSdk37) {
    setupMockJni(37);
    setupMockNDKChoreographer();
    buildEGLForTest();

    ASSERT_TRUE(SwappyGL_init(gMockEnv, reinterpret_cast<jobject>(0x1234)));
    const int createdAfterInit = gCreatedLocalRefs.load();
    const int netLocalRefsAfterInit = createdAfterInit - gDeletedLocalRefs.load();

    // Exceed ART's 512-entry JNI local reference table limit across callbacks.
    constexpr int kCallbackIterations = 600;
    for (int i = 0; i < kCallbackIterations; ++i) {
        ASSERT_TRUE(triggerMockRefreshRateCallback(11'111'111));
    }

    EXPECT_GT(gCreatedLocalRefs.load(), createdAfterInit);
    EXPECT_EQ(gCreatedLocalRefs.load() - gDeletedLocalRefs.load(), netLocalRefsAfterInit);

    cleanupMockNDKChoreographer();
    SwappyGL_destroy();
}

TEST_F(AImageReaderEGLSwapchainTest, RenderingLoopSDK37) {
    // Set up the mocked JNI environment with an SDK version of 37 (Android U+).
    // This will force Swappy to use NDKChoreographerThread instead of JavaChoreographerThread.
    setupMockJni(37);
    setupMockNDKChoreographer();

    jobject fakeActivity = reinterpret_cast<jobject>(0x1234);
    buildEGLForTest();

    EXPECT_TRUE(SwappyGL_init(gMockEnv, fakeActivity));
    EXPECT_TRUE(SwappyGL_setWindow(mWindow));

    SwappyGL_setSwapIntervalNS(SWAPPY_SWAP_60FPS);

    for (int i = 0; i < 10; ++i) {
        LOGI("[Test] Frame %d starting", i);
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        SwappyGL_recordFrameStart(mDisplay, mSurface);
        bool res = SwappyGL_swap(mDisplay, mSurface);
        EXPECT_TRUE(res);
        LOGI("[Test] Frame %d swapped, res = %d", i, res);
    }
    cleanupMockNDKChoreographer();
    SwappyGL_destroy();
}

TEST_F(AImageReaderEGLSwapchainTest, DisablesRenderAheadOnDisplayManagerInit) {
    setupMockJni(37);
    setupMockNDKChoreographer();
    buildEGLForTest();

    const jmethodID kDisableRenderAhead = reinterpret_cast<jmethodID>(0x30);
    int disableRenderAheadCalls = 0;

    ON_CALL(mMockJni,
            GetMethodID(testing::_, testing::_, testing::StrEq("disableRenderAhead"),
                        testing::StrEq("()V")))
            .WillByDefault(testing::Return(kDisableRenderAhead));
    ON_CALL(mMockJni, CallVoidMethodV(testing::_, testing::_, kDisableRenderAhead, testing::_))
            .WillByDefault(testing::Invoke(
                    [&](JNIEnv*, jobject, jmethodID, va_list) { ++disableRenderAheadCalls; }));

    jobject fakeActivity = reinterpret_cast<jobject>(0x1234);
    EXPECT_TRUE(SwappyGL_init(gMockEnv, fakeActivity));
    EXPECT_EQ(disableRenderAheadCalls, 1);

    cleanupMockNDKChoreographer();
    SwappyGL_destroy();
}

TEST_F(AImageReaderEGLSwapchainTest, ToleratesMissingDisableRenderAheadMethod) {
    setupMockJni(37);
    setupMockNDKChoreographer();
    buildEGLForTest();

    bool pendingException = false;
    int exceptionClears = 0;

    ON_CALL(mMockJni,
            GetMethodID(testing::_, testing::_, testing::StrEq("disableRenderAhead"),
                        testing::StrEq("()V")))
            .WillByDefault(testing::Invoke([&](JNIEnv*, jclass, const char*, const char*) {
                pendingException = true;
                return nullptr;
            }));
    ON_CALL(mMockJni, ExceptionCheck(testing::_)).WillByDefault(testing::Invoke([&](JNIEnv*) {
        return pendingException ? JNI_TRUE : JNI_FALSE;
    }));
    ON_CALL(mMockJni, ExceptionClear(testing::_)).WillByDefault(testing::Invoke([&](JNIEnv*) {
        pendingException = false;
        ++exceptionClears;
    }));

    jobject fakeActivity = reinterpret_cast<jobject>(0x1234);
    EXPECT_TRUE(SwappyGL_init(gMockEnv, fakeActivity));
    EXPECT_FALSE(pendingException);
    EXPECT_EQ(exceptionClears, 1);

    cleanupMockNDKChoreographer();
    SwappyGL_destroy();
}

} // namespace swappytest
} // namespace android

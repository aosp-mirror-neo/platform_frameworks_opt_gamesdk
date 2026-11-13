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

#include "JNIUtil.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#include "ScopedLocalRef.h"
#include "fake_jni_env.h"
#include "gtest/gtest.h"

#if defined(__ELF__)
__asm__(".section .rodata\n"
        ".globl _binary_classes_dex_start\n"
        "_binary_classes_dex_start:\n"
        ".byte 0x64, 0x65, 0x78, 0x0a, 0x30, 0x33, 0x35, 0x00\n"
        ".globl _binary_classes_dex_end\n"
        "_binary_classes_dex_end:\n"
        ".previous\n");
#elif defined(__APPLE__)
__asm__(".section __TEXT,__const\n"
        ".globl __binary_classes_dex_start\n"
        "__binary_classes_dex_start:\n"
        ".byte 0x64, 0x65, 0x78, 0x0a, 0x30, 0x33, 0x35, 0x00\n"
        ".globl __binary_classes_dex_end\n"
        "__binary_classes_dex_end:\n"
        ".previous\n");
#else
#error "Unsupported object format for fake _binary_classes_dex_start/_end test symbols"
#endif

namespace {

thread_local int gFakeSdkVersion = 28;

constexpr const char* kTargetClass = "com/google/androidgamesdk/SwappyDisplayManager";

void stubNativeCallback() {}

JNINativeMethod kTestNativeMethods[] = {
        {const_cast<char*>("nOnRefreshPeriodChanged"), const_cast<char*>("(JJJJ)V"),
         reinterpret_cast<void*>(&stubNativeCallback)},
        {const_cast<char*>("nSetSupportedRefreshPeriods"), const_cast<char*>("(J[J[I)V"),
         reinterpret_cast<void*>(&stubNativeCallback)},
};

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        static std::atomic<uint64_t> sCounter{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
        const auto pid = static_cast<long long>(_getpid());
#else
        const auto pid = static_cast<long long>(getpid());
#endif
        const uint64_t seq = sCounter.fetch_add(1, std::memory_order_relaxed);
        mPath = std::filesystem::temp_directory_path() /
                ("gamesdk_jniutil_test_" + std::to_string(pid) + "_" + std::to_string(tick) + "_" +
                 std::to_string(seq));
        std::filesystem::create_directories(mPath);
    }

    ~ScopedTempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(mPath, ec);
    }

    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

    std::string str() const {
        return mPath.string();
    }

private:
    std::filesystem::path mPath;
};

class JniUtilHostTest : public ::testing::Test {
protected:
    void TearDown() override {
        gFakeSdkVersion = 28;
    }

    static void expectCleanJniState(const swappy::testing_support::FakeJniEnv& fakeJni) {
        EXPECT_EQ(fakeJni.liveLocalRefCount(), 0);
        EXPECT_EQ(fakeJni.invalidLocalRefDeletes(), 0);
        EXPECT_EQ(fakeJni.callsWithPendingException(), 0);
        EXPECT_FALSE(fakeJni.hasPendingException());
        EXPECT_EQ(fakeJni.activeUtfBorrowCount(), 0);
    }
};

} // namespace

namespace gamesdk {

std::string GetSystemProp(const char* key, const char* default_value) {
    if (key != nullptr && std::strcmp(key, "ro.build.version.sdk") == 0) {
        return std::to_string(gFakeSdkVersion);
    }
    return default_value != nullptr ? default_value : "";
}

int GetSystemPropAsInt(const char* key, int default_value) {
    if (key != nullptr && std::strcmp(key, "ro.build.version.sdk") == 0) {
        return gFakeSdkVersion;
    }
    return default_value;
}

bool GetSystemPropAsBool(const char* key, bool default_value) {
    return GetSystemPropAsInt(key, default_value ? 1 : 0) != 0;
}

} // namespace gamesdk

namespace {

TEST_F(JniUtilHostTest, RejectsNullArgumentsWithoutCreatingLocalRefs) {
    swappy::testing_support::FakeJniEnv fakeJni;

    EXPECT_EQ(gamesdk::loadClass(nullptr, fakeJni.activity(), kTargetClass, kTestNativeMethods, 2),
              nullptr);
    EXPECT_EQ(gamesdk::loadClass(fakeJni.env(), nullptr, kTargetClass, kTestNativeMethods, 2),
              nullptr);
    EXPECT_EQ(gamesdk::loadClass(fakeJni.env(), fakeJni.activity(), nullptr, kTestNativeMethods, 2),
              nullptr);
    EXPECT_EQ(gamesdk::loadClass(fakeJni.env(), fakeJni.activity(), kTargetClass, nullptr, 2),
              nullptr);

    EXPECT_EQ(fakeJni.totalCreatedLocalRefs(), 0);
    expectCleanJniState(fakeJni);
}

TEST_F(JniUtilHostTest, LoadsFromPrimaryClassLoaderAndReleasesAllIntermediateLocalRefs) {
    swappy::testing_support::FakeJniEnv fakeJni;
    fakeJni.addPrimaryClass(kTargetClass);

    gamesdk::ScopedLocalRef<jclass> loaded(fakeJni.env(),
                                           gamesdk::loadClass(fakeJni.env(), fakeJni.activity(),
                                                              kTargetClass, kTestNativeMethods, 2));

    ASSERT_NE(loaded.get(), nullptr);
    EXPECT_EQ(fakeJni.liveLocalRefCount(), 1);
    EXPECT_EQ(fakeJni.registeredNativeMethodCount(), 2);
    EXPECT_EQ(fakeJni.inMemoryDexLoaderCreations(), 0);
    EXPECT_EQ(fakeJni.pathDexLoaderCreations(), 0);

    loaded = gamesdk::ScopedLocalRef<jclass>(fakeJni.env(), nullptr);
    expectCleanJniState(fakeJni);
}

TEST_F(JniUtilHostTest,
       LoadsFromInMemoryDexClassLoaderOnSdk28AndReleasesDirectByteBufferAndLoaderRefs) {
    gFakeSdkVersion = 28;
    swappy::testing_support::FakeJniEnv fakeJni;
    fakeJni.addEmbeddedDexClass(kTargetClass);

    gamesdk::ScopedLocalRef<jclass> loaded(fakeJni.env(),
                                           gamesdk::loadClass(fakeJni.env(), fakeJni.activity(),
                                                              kTargetClass, kTestNativeMethods, 2));

    ASSERT_NE(loaded.get(), nullptr);
    EXPECT_EQ(fakeJni.liveLocalRefCount(), 1);
    EXPECT_EQ(fakeJni.inMemoryDexLoaderCreations(), 1);
    EXPECT_EQ(fakeJni.pathDexLoaderCreations(), 0);
    EXPECT_EQ(fakeJni.lastDirectByteBufferCapacity(), 8);
    EXPECT_EQ(fakeJni.registeredNativeMethodCount(), 2);

    loaded = gamesdk::ScopedLocalRef<jclass>(fakeJni.env(), nullptr);
    expectCleanJniState(fakeJni);
}

TEST_F(JniUtilHostTest,
       LoadsFromPathClassLoaderOnSdk24DeletesTempDexFileAndReleasesAllTempFileLocalRefs) {
    gFakeSdkVersion = 24;
    ScopedTempDirectory tempDir;
    swappy::testing_support::FakeJniEnv fakeJni;
    fakeJni.setCacheDirPath(tempDir.str());
    fakeJni.addEmbeddedDexClass(kTargetClass);

    gamesdk::ScopedLocalRef<jclass> loaded(fakeJni.env(),
                                           gamesdk::loadClass(fakeJni.env(), fakeJni.activity(),
                                                              kTargetClass, kTestNativeMethods, 2));

    ASSERT_NE(loaded.get(), nullptr);
    EXPECT_EQ(fakeJni.liveLocalRefCount(), 1);
    EXPECT_EQ(fakeJni.inMemoryDexLoaderCreations(), 0);
    EXPECT_EQ(fakeJni.pathDexLoaderCreations(), 1);
    EXPECT_EQ(fakeJni.registeredNativeMethodCount(), 2);
    EXPECT_FALSE(fakeJni.lastCreatedTempFilePath().empty());
    EXPECT_FALSE(std::filesystem::exists(fakeJni.lastCreatedTempFilePath()));
    EXPECT_EQ(fakeJni.lastLoadedDexFileContents(), std::string("dex\n035\0", 8));

    loaded = gamesdk::ScopedLocalRef<jclass>(fakeJni.env(), nullptr);
    expectCleanJniState(fakeJni);
}

TEST_F(JniUtilHostTest,
       FallsBackToPathClassLoaderWhenInMemoryDexClassLoaderMissingOnSdk28AndReleasesAllRefs) {
    gFakeSdkVersion = 28;
    ScopedTempDirectory tempDir;
    swappy::testing_support::FakeJniEnv fakeJni;
    fakeJni.setInMemoryDexClassLoaderAvailable(false);
    fakeJni.setCacheDirPath(tempDir.str());
    fakeJni.addEmbeddedDexClass(kTargetClass);

    gamesdk::ScopedLocalRef<jclass> loaded(fakeJni.env(),
                                           gamesdk::loadClass(fakeJni.env(), fakeJni.activity(),
                                                              kTargetClass, kTestNativeMethods, 2));

    ASSERT_NE(loaded.get(), nullptr);
    EXPECT_EQ(fakeJni.liveLocalRefCount(), 1);
    EXPECT_EQ(fakeJni.inMemoryDexLoaderCreations(), 0);
    EXPECT_EQ(fakeJni.pathDexLoaderCreations(), 1);
    EXPECT_FALSE(std::filesystem::exists(fakeJni.lastCreatedTempFilePath()));

    loaded = gamesdk::ScopedLocalRef<jclass>(fakeJni.env(), nullptr);
    expectCleanJniState(fakeJni);
}

TEST_F(JniUtilHostTest, ReleasesAllLocalRefsWhenTargetClassMissingFromPrimaryAndInMemoryDex) {
    gFakeSdkVersion = 28;
    swappy::testing_support::FakeJniEnv fakeJni;

    jclass loaded = gamesdk::loadClass(fakeJni.env(), fakeJni.activity(), kTargetClass,
                                       kTestNativeMethods, 2);

    EXPECT_EQ(loaded, nullptr);
    expectCleanJniState(fakeJni);
}

TEST_F(JniUtilHostTest,
       ReleasesAllLocalRefsAndDeletesTempFileWhenTargetClassMissingFromPathClassLoader) {
    gFakeSdkVersion = 24;
    ScopedTempDirectory tempDir;
    swappy::testing_support::FakeJniEnv fakeJni;
    fakeJni.setCacheDirPath(tempDir.str());

    jclass loaded = gamesdk::loadClass(fakeJni.env(), fakeJni.activity(), kTargetClass,
                                       kTestNativeMethods, 2);

    EXPECT_EQ(loaded, nullptr);
    EXPECT_FALSE(fakeJni.lastCreatedTempFilePath().empty());
    EXPECT_FALSE(std::filesystem::exists(fakeJni.lastCreatedTempFilePath()));
    expectCleanJniState(fakeJni);
}

TEST_F(JniUtilHostTest, ReleasesAllLocalRefsWhenGetClassLoaderThrowsOrReturnsNull) {
    swappy::testing_support::FakeJniEnv throwingEnv;
    throwingEnv.failGetClassLoaderWithException();
    EXPECT_EQ(gamesdk::loadClass(throwingEnv.env(), throwingEnv.activity(), kTargetClass,
                                 kTestNativeMethods, 2),
              nullptr);
    expectCleanJniState(throwingEnv);

    swappy::testing_support::FakeJniEnv nullLoaderEnv;
    nullLoaderEnv.returnNullFromGetClassLoader();
    EXPECT_EQ(gamesdk::loadClass(nullLoaderEnv.env(), nullLoaderEnv.activity(), kTargetClass,
                                 kTestNativeMethods, 2),
              nullptr);
    expectCleanJniState(nullLoaderEnv);
}

TEST_F(JniUtilHostTest, ReleasesAllLocalRefsWhenDexBufferOrLoaderConstructionFails) {
    gFakeSdkVersion = 28;
    ScopedTempDirectory tempDir;

    swappy::testing_support::FakeJniEnv oomNameEnv;
    oomNameEnv.setCacheDirPath(tempDir.str());
    oomNameEnv.addEmbeddedDexClass(kTargetClass);
    oomNameEnv.failNewStringUTFWithException("dalvik/system/InMemoryDexClassLoader");
    EXPECT_EQ(gamesdk::loadClass(oomNameEnv.env(), oomNameEnv.activity(), kTargetClass,
                                 kTestNativeMethods, 2),
              nullptr);
    EXPECT_EQ(oomNameEnv.pathDexLoaderCreations(), 0);
    EXPECT_EQ(oomNameEnv.exceptionDescribeCount(), 1);
    expectCleanJniState(oomNameEnv);

    swappy::testing_support::FakeJniEnv nullBufferEnv;
    nullBufferEnv.failNewDirectByteBuffer();
    EXPECT_EQ(gamesdk::loadClass(nullBufferEnv.env(), nullBufferEnv.activity(), kTargetClass,
                                 kTestNativeMethods, 2),
              nullptr);
    expectCleanJniState(nullBufferEnv);

    swappy::testing_support::FakeJniEnv failingCtorEnv;
    failingCtorEnv.failDexClassLoaderConstruction();
    EXPECT_EQ(gamesdk::loadClass(failingCtorEnv.env(), failingCtorEnv.activity(), kTargetClass,
                                 kTestNativeMethods, 2),
              nullptr);
    expectCleanJniState(failingCtorEnv);
}

TEST_F(JniUtilHostTest, ReleasesAllLocalRefsWhenTempFileCreationOrRegisterNativesFails) {
    gFakeSdkVersion = 24;
    ScopedTempDirectory tempDir;

    swappy::testing_support::FakeJniEnv tempFailEnv;
    tempFailEnv.setCacheDirPath(tempDir.str());
    tempFailEnv.failCreateTempFileWithException();
    EXPECT_EQ(gamesdk::loadClass(tempFailEnv.env(), tempFailEnv.activity(), kTargetClass,
                                 kTestNativeMethods, 2),
              nullptr);
    expectCleanJniState(tempFailEnv);

    swappy::testing_support::FakeJniEnv registerFailEnv;
    registerFailEnv.addPrimaryClass(kTargetClass);
    registerFailEnv.failRegisterNativesWithException();
    EXPECT_EQ(gamesdk::loadClass(registerFailEnv.env(), registerFailEnv.activity(), kTargetClass,
                                 kTestNativeMethods, 2),
              nullptr);
    expectCleanJniState(registerFailEnv);
}

TEST_F(JniUtilHostTest, CoexistingFakeJniEnvsIsolateLocalRefLifetime) {
    swappy::testing_support::FakeJniEnv firstEnv;
    firstEnv.addPrimaryClass(kTargetClass);

    gamesdk::ScopedLocalRef<jclass> firstLoaded(firstEnv.env(), nullptr);
    {
        swappy::testing_support::FakeJniEnv secondEnv;
        secondEnv.addPrimaryClass(kTargetClass);

        firstLoaded = gamesdk::ScopedLocalRef<jclass>(firstEnv.env(),
                                                      gamesdk::loadClass(firstEnv.env(),
                                                                         firstEnv.activity(),
                                                                         kTargetClass,
                                                                         kTestNativeMethods, 2));
        ASSERT_NE(firstLoaded.get(), nullptr);
        EXPECT_EQ(firstEnv.liveLocalRefCount(), 1);
        EXPECT_EQ(secondEnv.totalCreatedLocalRefs(), 0);

        {
            gamesdk::ScopedLocalRef<jclass> secondLoaded(secondEnv.env(),
                                                         gamesdk::loadClass(secondEnv.env(),
                                                                            secondEnv.activity(),
                                                                            kTargetClass,
                                                                            kTestNativeMethods, 2));
            ASSERT_NE(secondLoaded.get(), nullptr);
            EXPECT_EQ(secondEnv.liveLocalRefCount(), 1);
        }
        expectCleanJniState(secondEnv);
    }

    EXPECT_EQ(firstEnv.liveLocalRefCount(), 1);
    firstLoaded = gamesdk::ScopedLocalRef<jclass>(firstEnv.env(), nullptr);
    expectCleanJniState(firstEnv);
}

} // namespace

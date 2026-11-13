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

#include <type_traits>

#include "SwappyJniUtils.h"
#include "gtest/gtest.h"

namespace {

class FakeJavaVm {
public:
    explicit FakeJavaVm(bool initiallyAttached) : mAttached(initiallyAttached) {
        sActiveVm = this;
        mJniTable.ExceptionCheck = &FakeJavaVm::exceptionCheck;
        mJniTable.ExceptionClear = &FakeJavaVm::exceptionClear;
        mEnv.functions = &mJniTable;

        mInvokeTable.GetEnv = &FakeJavaVm::getEnv;
        mInvokeTable.AttachCurrentThread = &FakeJavaVm::attachCurrentThread;
        mInvokeTable.DetachCurrentThread = &FakeJavaVm::detachCurrentThread;
        mVm.functions = &mInvokeTable;
    }

    ~FakeJavaVm() {
        if (sActiveVm == this) {
            sActiveVm = nullptr;
        }
    }

    FakeJavaVm(const FakeJavaVm&) = delete;
    FakeJavaVm& operator=(const FakeJavaVm&) = delete;

    JavaVM* vm() noexcept {
        return &mVm;
    }

    JNIEnv* expectedEnv() noexcept {
        return &mEnv;
    }

    void setFailAttach(bool failAttach) noexcept {
        mFailAttach = failAttach;
    }

    void setGetEnvError(jint getEnvError) noexcept {
        mGetEnvError = getEnvError;
    }

    void setPendingException(bool pending) noexcept {
        mPendingException = pending;
    }

    bool isAttached() const noexcept {
        return mAttached;
    }

    bool hasPendingException() const noexcept {
        return mPendingException;
    }

    bool detachedWithPendingException() const noexcept {
        return mDetachedWithPendingException;
    }

    int attachCount() const noexcept {
        return mAttachCount;
    }

    int detachCount() const noexcept {
        return mDetachCount;
    }

private:
    using NativeInterface =
            std::remove_const_t<std::remove_pointer_t<decltype(std::declval<JNIEnv>().functions)>>;
    using InvokeInterface =
            std::remove_const_t<std::remove_pointer_t<decltype(std::declval<JavaVM>().functions)>>;

    static jboolean JNICALL exceptionCheck(JNIEnv*) {
        return sActiveVm->mPendingException ? JNI_TRUE : JNI_FALSE;
    }

    static void JNICALL exceptionClear(JNIEnv*) {
        sActiveVm->mPendingException = false;
    }

    static jint JNICALL getEnv(JavaVM*, void** penv, jint) {
        if (sActiveVm->mGetEnvError != JNI_OK) {
            if (penv != nullptr) {
                *penv = nullptr;
            }
            return sActiveVm->mGetEnvError;
        }
        if (!sActiveVm->mAttached) {
            if (penv != nullptr) {
                *penv = nullptr;
            }
            return JNI_EDETACHED;
        }
        if (penv != nullptr) {
            *penv = &sActiveVm->mEnv;
        }
        return JNI_OK;
    }

#ifdef __ANDROID__
    static jint JNICALL attachCurrentThread(JavaVM*, JNIEnv** penv, void*) {
#else
    static jint JNICALL attachCurrentThread(JavaVM*, void** penv, void*) {
#endif
        ++sActiveVm->mAttachCount;
        if (sActiveVm->mFailAttach) {
            if (penv != nullptr) {
                *penv = nullptr;
            }
            return JNI_ERR;
        }
        sActiveVm->mAttached = true;
        if (penv != nullptr) {
            *penv = &sActiveVm->mEnv;
        }
        return JNI_OK;
    }

    static jint JNICALL detachCurrentThread(JavaVM*) {
        ++sActiveVm->mDetachCount;
        if (sActiveVm->mPendingException) {
            sActiveVm->mDetachedWithPendingException = true;
        }
        sActiveVm->mAttached = false;
        return JNI_OK;
    }

    inline static thread_local FakeJavaVm* sActiveVm = nullptr;

    NativeInterface mJniTable{};
    JNIEnv mEnv{};
    InvokeInterface mInvokeTable{};
    JavaVM mVm{};
    bool mAttached = false;
    bool mFailAttach = false;
    jint mGetEnvError = JNI_OK;
    bool mPendingException = false;
    bool mDetachedWithPendingException = false;
    int mAttachCount = 0;
    int mDetachCount = 0;
};

TEST(ScopedJniEnvHostTest, NullJavaVmReturnsNullEnv) {
    swappy::ScopedJniEnv scopedEnv(nullptr);

    EXPECT_EQ(scopedEnv.get(), nullptr);
}

TEST(ScopedJniEnvHostTest, AlreadyAttachedThreadReusesEnvWithoutDetaching) {
    FakeJavaVm fakeVm(/*initiallyAttached=*/true);

    {
        swappy::ScopedJniEnv scopedEnv(fakeVm.vm());
        EXPECT_EQ(scopedEnv.get(), fakeVm.expectedEnv());
    }

    EXPECT_TRUE(fakeVm.isAttached());
    EXPECT_EQ(fakeVm.attachCount(), 0);
    EXPECT_EQ(fakeVm.detachCount(), 0);
}

TEST(ScopedJniEnvHostTest, DetachedThreadAttachesOnceAcrossNestedScopesAndDetachesOnOuterExit) {
    FakeJavaVm fakeVm(/*initiallyAttached=*/false);

    {
        swappy::ScopedJniEnv outerEnv(fakeVm.vm());
        ASSERT_EQ(outerEnv.get(), fakeVm.expectedEnv());
        EXPECT_TRUE(fakeVm.isAttached());

        {
            swappy::ScopedJniEnv innerEnv(fakeVm.vm());
            EXPECT_EQ(innerEnv.get(), fakeVm.expectedEnv());
        }

        // Inner guard saw the thread already attached, so outer scope remains attached.
        EXPECT_TRUE(fakeVm.isAttached());
        EXPECT_EQ(fakeVm.detachCount(), 0);
    }

    EXPECT_FALSE(fakeVm.isAttached());
    EXPECT_EQ(fakeVm.attachCount(), 1);
    EXPECT_EQ(fakeVm.detachCount(), 1);
}

TEST(ScopedJniEnvHostTest, ClearsPendingExceptionBeforeDetachingThread) {
    FakeJavaVm fakeVm(/*initiallyAttached=*/false);

    {
        swappy::ScopedJniEnv scopedEnv(fakeVm.vm());
        ASSERT_NE(scopedEnv.get(), nullptr);
        fakeVm.setPendingException(true);
    }

    EXPECT_FALSE(fakeVm.isAttached());
    EXPECT_FALSE(fakeVm.hasPendingException());
    EXPECT_FALSE(fakeVm.detachedWithPendingException());
    EXPECT_EQ(fakeVm.detachCount(), 1);
}

TEST(ScopedJniEnvHostTest, FailedAttachReturnsNullWithoutDetaching) {
    FakeJavaVm fakeVm(/*initiallyAttached=*/false);
    fakeVm.setFailAttach(true);

    {
        swappy::ScopedJniEnv scopedEnv(fakeVm.vm());
        EXPECT_EQ(scopedEnv.get(), nullptr);
    }

    EXPECT_FALSE(fakeVm.isAttached());
    EXPECT_EQ(fakeVm.attachCount(), 1);
    EXPECT_EQ(fakeVm.detachCount(), 0);
}

TEST(ScopedJniEnvHostTest, GetEnvVersionErrorReturnsNullWithoutAttachingOrDetaching) {
    FakeJavaVm fakeVm(/*initiallyAttached=*/false);
    fakeVm.setGetEnvError(JNI_EVERSION);

    {
        swappy::ScopedJniEnv scopedEnv(fakeVm.vm());
        EXPECT_EQ(scopedEnv.get(), nullptr);
    }

    EXPECT_FALSE(fakeVm.isAttached());
    EXPECT_EQ(fakeVm.attachCount(), 0);
    EXPECT_EQ(fakeVm.detachCount(), 0);
}

} // namespace

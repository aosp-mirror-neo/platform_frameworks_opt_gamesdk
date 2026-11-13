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

#include <jni.h>

#include "ScopedLocalRef.h"
#include "SwappyLog.h"

#pragma push_macro("LOG_TAG")
#ifndef LOG_TAG
#define LOG_TAG "SwappyCommon"
#endif

namespace swappy {

using gamesdk::ScopedLocalRef;

// Resolves the current thread's JNIEnv*, attaching to `jvm` when the thread is
// detached and detaching on scope exit only when this guard attached it.
class ScopedJniEnv {
public:
    explicit ScopedJniEnv(JavaVM* jvm) {
        if (jvm == nullptr) {
            return;
        }
        const jint result = jvm->GetEnv(reinterpret_cast<void**>(&mEnv), JNI_VERSION_1_6);
        if (result == JNI_OK) {
            return;
        }
        if (result != JNI_EDETACHED) {
            mEnv = nullptr;
            return;
        }
#ifdef __ANDROID__
        const jint attachResult = jvm->AttachCurrentThread(&mEnv, nullptr);
#else
        const jint attachResult =
                jvm->AttachCurrentThread(reinterpret_cast<void**>(&mEnv), nullptr);
#endif
        if (attachResult == JNI_OK) {
            mJvmToDetach = jvm;
        } else {
            mEnv = nullptr;
        }
    }

    ~ScopedJniEnv() {
        if (mJvmToDetach != nullptr) {
            if (mEnv != nullptr && mEnv->ExceptionCheck()) {
                mEnv->ExceptionClear();
                SWAPPY_LOGE("Cleared unhandled JNI exception before DetachCurrentThread");
            }
            mJvmToDetach->DetachCurrentThread();
        }
    }

    ScopedJniEnv(const ScopedJniEnv&) = delete;
    ScopedJniEnv& operator=(const ScopedJniEnv&) = delete;

    JNIEnv* get() const noexcept {
        return mEnv;
    }

private:
    JavaVM* mJvmToDetach = nullptr;
    JNIEnv* mEnv = nullptr;
};

// Logs and clears the exception left pending by the JNI call described by
// `operation`, so that no exception ever leaks to the caller of Swappy.
// Returns whether an exception was pending.
inline bool consumePendingException(JNIEnv* env, const char* operation) {
    if (!env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionClear();
    SWAPPY_LOGE("Exception while calling %s", operation);
    return true;
}

// Returns whether `result`, the outcome of the JNI lookup or call described by
// `operation`, is non-null and did not throw.
inline bool isUsableResult(JNIEnv* env, const void* result, const char* operation) {
    if (consumePendingException(env, operation)) {
        return false;
    }
    if (result == nullptr) {
        SWAPPY_LOGE("Failed to get %s", operation);
        return false;
    }
    return true;
}

inline bool isInstanceOf(JNIEnv* env, jobject object, const char* className) {
    if (object == nullptr) {
        return false;
    }
    ScopedLocalRef<jclass> objectClass(env, env->FindClass(className));
    if (!isUsableResult(env, objectClass.get(), className)) {
        return false;
    }

    const jboolean result = env->IsInstanceOf(object, objectClass.get());
    return !consumePendingException(env, "IsInstanceOf") && result == JNI_TRUE;
}

} // namespace swappy

#pragma pop_macro("LOG_TAG")

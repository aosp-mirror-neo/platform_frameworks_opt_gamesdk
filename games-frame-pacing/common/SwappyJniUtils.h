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

namespace swappy {

// Owns a JNI local reference (jobject, jclass, jstring, jthrowable) and
// deletes it on scope exit so native threads without a Java caller frame do
// not exhaust ART's local reference table.
template <typename T>
class ScopedLocalRef {
public:
    ScopedLocalRef(JNIEnv* env, T ref) : mEnv(env), mRef(ref) {}
    ~ScopedLocalRef() {
        if (mRef != nullptr) {
            mEnv->DeleteLocalRef(mRef);
        }
    }

    ScopedLocalRef(const ScopedLocalRef&) = delete;
    ScopedLocalRef& operator=(const ScopedLocalRef&) = delete;

    ScopedLocalRef(ScopedLocalRef&& other) noexcept : mEnv(other.mEnv), mRef(other.mRef) {
        other.mRef = nullptr;
    }

    ScopedLocalRef& operator=(ScopedLocalRef&& other) noexcept {
        if (this != &other) {
            if (mRef != nullptr) {
                mEnv->DeleteLocalRef(mRef);
            }
            mEnv = other.mEnv;
            mRef = other.mRef;
            other.mRef = nullptr;
        }
        return *this;
    }

    T get() const noexcept {
        return mRef;
    }

    T release() noexcept {
        T ref = mRef;
        mRef = nullptr;
        return ref;
    }

private:
    JNIEnv* mEnv;
    T mRef;
};

} // namespace swappy

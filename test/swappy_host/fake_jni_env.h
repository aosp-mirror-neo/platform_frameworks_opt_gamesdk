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

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace swappy::testing_support {

// Stateful in-memory JNIEnv fake that tracks every local reference, UTF borrow,
// and pending-exception violation across JNIUtil.h calls.
class FakeJniEnv {
public:
    FakeJniEnv() {
        mTable.reserved0 = this;
        mTable.FindClass = &FakeJniEnv::findClass;
        mTable.ExceptionDescribe = &FakeJniEnv::exceptionDescribe;
        mTable.ExceptionClear = &FakeJniEnv::exceptionClear;
        mTable.ExceptionCheck = &FakeJniEnv::exceptionCheck;
        mTable.DeleteLocalRef = &FakeJniEnv::deleteLocalRef;
        mTable.NewObjectV = &FakeJniEnv::newObjectV;
        mTable.GetObjectClass = &FakeJniEnv::getObjectClass;
        mTable.GetMethodID = &FakeJniEnv::getMethodID;
        mTable.CallObjectMethodV = &FakeJniEnv::callObjectMethodV;
        mTable.GetStaticMethodID = &FakeJniEnv::getStaticMethodID;
        mTable.CallStaticObjectMethodV = &FakeJniEnv::callStaticObjectMethodV;
        mTable.NewStringUTF = &FakeJniEnv::newStringUTF;
        mTable.GetStringUTFChars = &FakeJniEnv::getStringUTFChars;
        mTable.ReleaseStringUTFChars = &FakeJniEnv::releaseStringUTFChars;
        mTable.RegisterNatives = &FakeJniEnv::registerNatives;
        mTable.NewDirectByteBuffer = &FakeJniEnv::newDirectByteBuffer;
        mEnv.functions = &mTable;

        mActivity = allocateNonLocalObject(RefKind::kActivity, "android/app/Activity");
    }

    FakeJniEnv(const FakeJniEnv&) = delete;
    FakeJniEnv& operator=(const FakeJniEnv&) = delete;

    JNIEnv* env() noexcept {
        return &mEnv;
    }

    jobject activity() const noexcept {
        return mActivity;
    }

    void addPrimaryClass(const std::string& name) {
        mPrimaryClasses.insert(name);
    }

    void addEmbeddedDexClass(const std::string& name) {
        mDexClasses.insert(name);
    }

    void setInMemoryDexClassLoaderAvailable(bool available) {
        mHasInMemoryDexClassLoader = available;
    }

    void setPathClassLoaderAvailable(bool available) {
        mHasPathClassLoader = available;
    }

    void setCacheDirPath(std::string path) {
        mCacheDirPath = std::move(path);
    }

    void failGetClassLoaderWithException() {
        mFailGetClassLoader = true;
    }

    void returnNullFromGetClassLoader() {
        mReturnNullClassLoader = true;
    }

    void failNewDirectByteBuffer() {
        mFailNewDirectByteBuffer = true;
    }

    void failDexClassLoaderConstruction() {
        mFailDexLoaderInit = true;
    }

    void failCreateTempFileWithException() {
        mFailCreateTempFile = true;
    }

    void failRegisterNativesWithException() {
        mFailRegisterNatives = true;
    }

    void failNewStringUTFWithException(std::string target) {
        mFailNewStringUTFTarget = std::move(target);
    }

    int liveLocalRefCount() const {
        return static_cast<int>(
                std::count_if(mRefs.begin(), mRefs.end(), [](const std::unique_ptr<FakeRef>& ref) {
                    return ref->isLocal && ref->alive;
                }));
    }

    int totalCreatedLocalRefs() const noexcept {
        return mCreatedLocalRefs;
    }

    int invalidLocalRefDeletes() const noexcept {
        return mInvalidLocalRefDeletes;
    }

    int callsWithPendingException() const noexcept {
        return mCallsWithPendingException;
    }

    int exceptionDescribeCount() const noexcept {
        return mExceptionDescribes;
    }

    bool hasPendingException() const noexcept {
        return mPendingException;
    }

    int activeUtfBorrowCount() const noexcept {
        return mActiveUtfBorrows;
    }

    int registeredNativeMethodCount() const noexcept {
        return mRegisteredNativeMethods;
    }

    jlong lastDirectByteBufferCapacity() const noexcept {
        return mLastDirectByteBufferCapacity;
    }

    int inMemoryDexLoaderCreations() const noexcept {
        return mInMemoryDexLoaderCreations;
    }

    int pathDexLoaderCreations() const noexcept {
        return mPathDexLoaderCreations;
    }

    const std::string& lastCreatedTempFilePath() const noexcept {
        return mLastCreatedTempFilePath;
    }

    const std::string& lastLoadedDexFileContents() const noexcept {
        return mLastLoadedDexFileContents;
    }

private:
    enum class RefKind {
        kActivity,
        kPrimaryClassLoader,
        kInMemoryDexClassLoader,
        kPathDexClassLoader,
        kClass,
        kString,
        kFile,
        kByteBuffer,
    };

    struct FakeRef {
        RefKind kind;
        std::string text;
        bool isLocal = true;
        bool alive = true;
    };

    struct FakeMethod {
        std::string ownerClass;
        std::string name;
        std::string signature;
        bool isStatic = false;
    };

    using NativeInterface =
            std::remove_const_t<std::remove_pointer_t<decltype(std::declval<JNIEnv>().functions)>>;

    jobject allocateLocalRef(RefKind kind, std::string text) {
        auto ref = std::make_unique<FakeRef>(FakeRef{kind, std::move(text), true, true});
        FakeRef* raw = ref.get();
        mRefs.push_back(std::move(ref));
        ++mCreatedLocalRefs;
        return reinterpret_cast<jobject>(raw);
    }

    jobject allocateNonLocalObject(RefKind kind, std::string text) {
        auto ref = std::make_unique<FakeRef>(FakeRef{kind, std::move(text), false, true});
        FakeRef* raw = ref.get();
        mRefs.push_back(std::move(ref));
        return reinterpret_cast<jobject>(raw);
    }

    FakeRef* lookupLiveRef(jobject handle) {
        if (handle == nullptr) {
            return nullptr;
        }
        auto* candidate = reinterpret_cast<FakeRef*>(handle);
        for (const auto& owned : mRefs) {
            if (owned.get() == candidate && owned->alive) {
                return candidate;
            }
        }
        return nullptr;
    }

    FakeMethod* internMethod(std::string ownerClass, std::string name, std::string signature,
                             bool isStatic) {
        for (const auto& method : mMethods) {
            if (method->ownerClass == ownerClass && method->name == name &&
                method->signature == signature && method->isStatic == isStatic) {
                return method.get();
            }
        }
        auto method = std::make_unique<FakeMethod>(
                FakeMethod{std::move(ownerClass), std::move(name), std::move(signature), isStatic});
        FakeMethod* raw = method.get();
        mMethods.push_back(std::move(method));
        return raw;
    }

    bool checkNoPendingException() {
        if (mPendingException) {
            ++mCallsWithPendingException;
            return false;
        }
        return true;
    }

    static FakeJniEnv* fromEnv(JNIEnv* env) noexcept {
        return static_cast<FakeJniEnv*>(env->functions->reserved0);
    }

    static jclass JNICALL findClass(JNIEnv* env, const char* name) {
        FakeJniEnv* self = fromEnv(env);
        if (!self->checkNoPendingException() || name == nullptr) {
            return nullptr;
        }
        if (std::string(name) == "java/lang/ClassLoader" || std::string(name) == "java/io/File") {
            return static_cast<jclass>(self->allocateLocalRef(RefKind::kClass, name));
        }
        self->mPendingException = true;
        return nullptr;
    }

    static void JNICALL exceptionDescribe(JNIEnv* env) {
        ++fromEnv(env)->mExceptionDescribes;
    }

    static void JNICALL exceptionClear(JNIEnv* env) {
        fromEnv(env)->mPendingException = false;
    }

    static jboolean JNICALL exceptionCheck(JNIEnv* env) {
        return fromEnv(env)->mPendingException ? JNI_TRUE : JNI_FALSE;
    }

    static void JNICALL deleteLocalRef(JNIEnv* env, jobject localRef) {
        if (localRef == nullptr) {
            return;
        }
        FakeJniEnv* self = fromEnv(env);
        auto* candidate = reinterpret_cast<FakeRef*>(localRef);
        for (const auto& owned : self->mRefs) {
            if (owned.get() == candidate && owned->isLocal && owned->alive) {
                owned->alive = false;
                return;
            }
        }
        ++self->mInvalidLocalRefDeletes;
    }

    static jclass JNICALL getObjectClass(JNIEnv* env, jobject obj) {
        FakeJniEnv* self = fromEnv(env);
        if (!self->checkNoPendingException()) {
            return nullptr;
        }
        FakeRef* ref = self->lookupLiveRef(obj);
        if (ref == nullptr) {
            self->mPendingException = true;
            return nullptr;
        }
        return static_cast<jclass>(self->allocateLocalRef(RefKind::kClass, ref->text));
    }

    static jmethodID JNICALL getMethodID(JNIEnv* env, jclass clazz, const char* name,
                                         const char* sig) {
        FakeJniEnv* self = fromEnv(env);
        if (!self->checkNoPendingException()) {
            return nullptr;
        }
        FakeRef* classRef = self->lookupLiveRef(clazz);
        if (classRef == nullptr || name == nullptr || sig == nullptr) {
            self->mPendingException = true;
            return nullptr;
        }
        return reinterpret_cast<jmethodID>(
                self->internMethod(classRef->text, name, sig, /*isStatic=*/false));
    }

    static jmethodID JNICALL getStaticMethodID(JNIEnv* env, jclass clazz, const char* name,
                                               const char* sig) {
        FakeJniEnv* self = fromEnv(env);
        if (!self->checkNoPendingException()) {
            return nullptr;
        }
        FakeRef* classRef = self->lookupLiveRef(clazz);
        if (classRef == nullptr || name == nullptr || sig == nullptr) {
            self->mPendingException = true;
            return nullptr;
        }
        return reinterpret_cast<jmethodID>(
                self->internMethod(classRef->text, name, sig, /*isStatic=*/true));
    }

    static jobject JNICALL callObjectMethodV(JNIEnv* env, jobject obj, jmethodID methodID,
                                             va_list args) {
        FakeJniEnv* self = fromEnv(env);
        if (!self->checkNoPendingException()) {
            return nullptr;
        }
        FakeRef* receiver = self->lookupLiveRef(obj);
        auto* method = reinterpret_cast<FakeMethod*>(methodID);
        if (receiver == nullptr || method == nullptr) {
            self->mPendingException = true;
            return nullptr;
        }
        if (method->name == "getClassLoader") {
            if (self->mFailGetClassLoader) {
                self->mPendingException = true;
                return nullptr;
            }
            if (self->mReturnNullClassLoader) {
                return nullptr;
            }
            return self->allocateLocalRef(RefKind::kPrimaryClassLoader, "PrimaryClassLoader");
        }
        if (method->name == "getCacheDir") {
            if (self->mCacheDirPath.empty()) {
                self->mPendingException = true;
                return nullptr;
            }
            return self->allocateLocalRef(RefKind::kFile, self->mCacheDirPath);
        }
        if (method->name == "getPath") {
            return self->allocateLocalRef(RefKind::kString, receiver->text);
        }
        if (method->name == "loadClass") {
            jstring nameArg = va_arg(args, jstring);
            FakeRef* nameRef = self->lookupLiveRef(nameArg);
            if (nameRef == nullptr) {
                self->mPendingException = true;
                return nullptr;
            }
            const std::string& requested = nameRef->text;
            if (receiver->kind == RefKind::kPrimaryClassLoader) {
                if (requested == "dalvik/system/InMemoryDexClassLoader") {
                    if (self->mHasInMemoryDexClassLoader) {
                        return self->allocateLocalRef(RefKind::kClass, requested);
                    }
                    self->mPendingException = true;
                    return nullptr;
                }
                if (requested == "dalvik/system/PathClassLoader") {
                    if (self->mHasPathClassLoader) {
                        return self->allocateLocalRef(RefKind::kClass, requested);
                    }
                    self->mPendingException = true;
                    return nullptr;
                }
                if (self->mPrimaryClasses.count(requested) > 0) {
                    return self->allocateLocalRef(RefKind::kClass, requested);
                }
                self->mPendingException = true;
                return nullptr;
            }
            if (receiver->kind == RefKind::kInMemoryDexClassLoader ||
                receiver->kind == RefKind::kPathDexClassLoader) {
                if (receiver->kind == RefKind::kPathDexClassLoader) {
                    std::ifstream dexStream(receiver->text, std::ios::binary);
                    self->mLastLoadedDexFileContents.assign(std::istreambuf_iterator<char>(
                                                                    dexStream),
                                                            std::istreambuf_iterator<char>());
                }
                if (self->mDexClasses.count(requested) > 0) {
                    return self->allocateLocalRef(RefKind::kClass, requested);
                }
                self->mPendingException = true;
                return nullptr;
            }
        }
        self->mPendingException = true;
        return nullptr;
    }

    static jobject JNICALL callStaticObjectMethodV(JNIEnv* env, jclass clazz, jmethodID methodID,
                                                   va_list args) {
        FakeJniEnv* self = fromEnv(env);
        if (!self->checkNoPendingException()) {
            return nullptr;
        }
        FakeRef* classRef = self->lookupLiveRef(clazz);
        auto* method = reinterpret_cast<FakeMethod*>(methodID);
        if (classRef == nullptr || method == nullptr || method->name != "createTempFile") {
            self->mPendingException = true;
            return nullptr;
        }
        jstring prefixArg = va_arg(args, jstring);
        jstring suffixArg = va_arg(args, jstring);
        jobject dirArg = va_arg(args, jobject);
        FakeRef* prefixRef = self->lookupLiveRef(prefixArg);
        FakeRef* suffixRef = self->lookupLiveRef(suffixArg);
        FakeRef* dirRef = self->lookupLiveRef(dirArg);
        if (self->mFailCreateTempFile || prefixRef == nullptr || suffixRef == nullptr ||
            dirRef == nullptr) {
            self->mPendingException = true;
            return nullptr;
        }
        std::string fullPath =
                dirRef->text + "/" + prefixRef->text + "_generated" + suffixRef->text;
        std::ofstream created(fullPath, std::ios::binary);
        if (!created.good()) {
            self->mPendingException = true;
            return nullptr;
        }
        self->mLastCreatedTempFilePath = fullPath;
        return self->allocateLocalRef(RefKind::kFile, std::move(fullPath));
    }

    static jobject JNICALL newObjectV(JNIEnv* env, jclass clazz, jmethodID methodID, va_list args) {
        FakeJniEnv* self = fromEnv(env);
        if (!self->checkNoPendingException()) {
            return nullptr;
        }
        FakeRef* classRef = self->lookupLiveRef(clazz);
        auto* method = reinterpret_cast<FakeMethod*>(methodID);
        if (self->mFailDexLoaderInit || classRef == nullptr || method == nullptr ||
            method->name != "<init>") {
            self->mPendingException = true;
            return nullptr;
        }
        if (classRef->text == "dalvik/system/InMemoryDexClassLoader") {
            jobject bufferArg = va_arg(args, jobject);
            jobject parentArg = va_arg(args, jobject);
            if (self->lookupLiveRef(bufferArg) == nullptr ||
                self->lookupLiveRef(parentArg) == nullptr) {
                self->mPendingException = true;
                return nullptr;
            }
            ++self->mInMemoryDexLoaderCreations;
            return self->allocateLocalRef(RefKind::kInMemoryDexClassLoader,
                                          "InMemoryDexClassLoader");
        }
        if (classRef->text == "dalvik/system/PathClassLoader") {
            jstring pathArg = va_arg(args, jstring);
            jobject parentArg = va_arg(args, jobject);
            FakeRef* pathRef = self->lookupLiveRef(pathArg);
            if (pathRef == nullptr || self->lookupLiveRef(parentArg) == nullptr) {
                self->mPendingException = true;
                return nullptr;
            }
            ++self->mPathDexLoaderCreations;
            return self->allocateLocalRef(RefKind::kPathDexClassLoader, pathRef->text);
        }
        self->mPendingException = true;
        return nullptr;
    }

    static jstring JNICALL newStringUTF(JNIEnv* env, const char* bytes) {
        FakeJniEnv* self = fromEnv(env);
        if (!self->checkNoPendingException() || bytes == nullptr) {
            return nullptr;
        }
        if (!self->mFailNewStringUTFTarget.empty() && self->mFailNewStringUTFTarget == bytes) {
            self->mPendingException = true;
            return nullptr;
        }
        return static_cast<jstring>(self->allocateLocalRef(RefKind::kString, bytes));
    }

    static const char* JNICALL getStringUTFChars(JNIEnv* env, jstring string, jboolean* isCopy) {
        FakeJniEnv* self = fromEnv(env);
        if (!self->checkNoPendingException()) {
            return nullptr;
        }
        FakeRef* strRef = self->lookupLiveRef(string);
        if (strRef == nullptr) {
            self->mPendingException = true;
            return nullptr;
        }
        if (isCopy != nullptr) {
            *isCopy = JNI_FALSE;
        }
        ++self->mActiveUtfBorrows;
        return strRef->text.c_str();
    }

    static void JNICALL releaseStringUTFChars(JNIEnv* env, jstring string, const char* utf) {
        FakeJniEnv* self = fromEnv(env);
        FakeRef* strRef = self->lookupLiveRef(string);
        if (strRef != nullptr && utf == strRef->text.c_str()) {
            --self->mActiveUtfBorrows;
        }
    }

    static jint JNICALL registerNatives(JNIEnv* env, jclass clazz, const JNINativeMethod* methods,
                                        jint nMethods) {
        FakeJniEnv* self = fromEnv(env);
        if (!self->checkNoPendingException()) {
            return JNI_ERR;
        }
        if (self->mFailRegisterNatives || self->lookupLiveRef(clazz) == nullptr ||
            (nMethods > 0 && methods == nullptr)) {
            self->mPendingException = true;
            return JNI_ERR;
        }
        self->mRegisteredNativeMethods += nMethods;
        return JNI_OK;
    }

    static jobject JNICALL newDirectByteBuffer(JNIEnv* env, void* address, jlong capacity) {
        FakeJniEnv* self = fromEnv(env);
        if (!self->checkNoPendingException()) {
            return nullptr;
        }
        if (self->mFailNewDirectByteBuffer || address == nullptr || capacity <= 0) {
            return nullptr;
        }
        self->mLastDirectByteBufferCapacity = capacity;
        return self->allocateLocalRef(RefKind::kByteBuffer, "DirectByteBuffer");
    }

    NativeInterface mTable{};
    JNIEnv mEnv{};
    jobject mActivity = nullptr;
    std::vector<std::unique_ptr<FakeRef>> mRefs;
    std::vector<std::unique_ptr<FakeMethod>> mMethods;
    std::unordered_set<std::string> mPrimaryClasses;
    std::unordered_set<std::string> mDexClasses;
    std::string mCacheDirPath;
    std::string mLastCreatedTempFilePath;
    std::string mLastLoadedDexFileContents;
    std::string mFailNewStringUTFTarget;

    bool mHasInMemoryDexClassLoader = true;
    bool mHasPathClassLoader = true;
    bool mFailGetClassLoader = false;
    bool mReturnNullClassLoader = false;
    bool mFailNewDirectByteBuffer = false;
    bool mFailDexLoaderInit = false;
    bool mFailCreateTempFile = false;
    bool mFailRegisterNatives = false;
    bool mPendingException = false;

    int mCreatedLocalRefs = 0;
    int mInvalidLocalRefDeletes = 0;
    int mCallsWithPendingException = 0;
    int mExceptionDescribes = 0;
    int mActiveUtfBorrows = 0;
    int mRegisteredNativeMethods = 0;
    int mInMemoryDexLoaderCreations = 0;
    int mPathDexLoaderCreations = 0;
    jlong mLastDirectByteBufferCapacity = 0;
};

} // namespace swappy::testing_support

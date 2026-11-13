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

#include <jni.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "Log.h"
#include "ScopedLocalRef.h"
#include "system_utils.h"

// The code in this file will dynamically load Java classes from a binary
// resource linked to the library. The binary data is in DEX format, accessible
// using the _binary_classes_dex_start and _binary_classes_dex_end linker
// symbols.

// If you make AGDK classes available on the Java classpath, e.g. by adding them
// to your game/engine's own Java component, you do not need to add the binary
// resource and can instead define ANDROIDGAMESDK_NO_BINARY_DEX_LINKAGE which
// will avoid the linker requiring these symbols.

#ifndef ANDROIDGAMESDK_NO_BINARY_DEX_LINKAGE
extern const char _binary_classes_dex_start;
extern const char _binary_classes_dex_end;
#endif

namespace gamesdk {

inline bool clearAndDescribePendingException(JNIEnv* env) {
    if (!env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

inline ScopedLocalRef<jclass> loadClassWithLoader(JNIEnv* env, jobject classLoaderObj,
                                                  jmethodID loadClassMethod, jstring className,
                                                  bool describeException) {
    ScopedLocalRef<jclass> loadedClass(env,
                                       static_cast<jclass>(env->CallObjectMethod(classLoaderObj,
                                                                                 loadClassMethod,
                                                                                 className)));
    if (env->ExceptionCheck()) {
        if (describeException) {
            env->ExceptionDescribe();
        }
        env->ExceptionClear();
        return ScopedLocalRef<jclass>(env, nullptr);
    }
    return loadedClass;
}

#ifndef ANDROIDGAMESDK_NO_BINARY_DEX_LINKAGE

inline bool saveBytesToFile(std::string fileName, const char* bytes, size_t size) {
    std::ofstream save_file(fileName, std::ios::binary);
    if (save_file.good()) {
        save_file.write(bytes, size);
        return true;
    }
    return false;
}

inline bool deleteFile(std::string fileName) {
    if (remove(fileName.c_str()) != 0)
        return false;
    else
        return true;
}

inline size_t getEmbeddedDexSize() {
    return static_cast<size_t>(reinterpret_cast<uintptr_t>(&_binary_classes_dex_end) -
                               reinterpret_cast<uintptr_t>(&_binary_classes_dex_start));
}

class TempFileGuard {
public:
    explicit TempFileGuard(const std::string& path) : mPath(path) {}
    ~TempFileGuard() {
        if (!mPath.empty()) {
            deleteFile(mPath);
        }
    }

    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;

private:
    std::string mPath;
};

inline bool createTempFile(JNIEnv* env, jobject activity, const char* ext,
                           std::string& tempFileName) {
    ScopedLocalRef<jclass> activityClass(env, env->GetObjectClass(activity));
    if (clearAndDescribePendingException(env) || activityClass.get() == nullptr) {
        return false;
    }

    jmethodID getCacheDir =
            env->GetMethodID(activityClass.get(), "getCacheDir", "()Ljava/io/File;");
    if (clearAndDescribePendingException(env) || getCacheDir == nullptr) {
        return false;
    }

    ScopedLocalRef<jobject> cacheDir(env, env->CallObjectMethod(activity, getCacheDir));
    if (clearAndDescribePendingException(env)) {
        return false;
    }

    ScopedLocalRef<jclass> fileClass(env, env->FindClass("java/io/File"));
    if (clearAndDescribePendingException(env) || fileClass.get() == nullptr) {
        return false;
    }

    jmethodID createTempFileMethod =
            env->GetStaticMethodID(fileClass.get(), "createTempFile",
                                   "(Ljava/lang/String;Ljava/lang/String;Ljava/"
                                   "io/File;)Ljava/io/File;");
    if (clearAndDescribePendingException(env) || createTempFileMethod == nullptr) {
        return false;
    }

    ScopedLocalRef<jstring> prefix(env, env->NewStringUTF("ags"));
    if (clearAndDescribePendingException(env) || prefix.get() == nullptr) {
        return false;
    }

    ScopedLocalRef<jstring> suffix(env, env->NewStringUTF(ext));
    if (clearAndDescribePendingException(env) || suffix.get() == nullptr) {
        return false;
    }

    ScopedLocalRef<jobject> tempFile(env,
                                     env->CallStaticObjectMethod(fileClass.get(),
                                                                 createTempFileMethod, prefix.get(),
                                                                 suffix.get(), cacheDir.get()));
    if (clearAndDescribePendingException(env) || tempFile.get() == nullptr) {
        return false;
    }

    jmethodID getPath = env->GetMethodID(fileClass.get(), "getPath", "()Ljava/lang/String;");
    if (clearAndDescribePendingException(env) || getPath == nullptr) {
        return false;
    }

    ScopedLocalRef<jstring> pathString(env,
                                       static_cast<jstring>(
                                               env->CallObjectMethod(tempFile.get(), getPath)));
    if (clearAndDescribePendingException(env) || pathString.get() == nullptr) {
        return false;
    }

    const char* path = env->GetStringUTFChars(pathString.get(), nullptr);
    if (clearAndDescribePendingException(env) || path == nullptr) {
        return false;
    }
    tempFileName.assign(path);
    env->ReleaseStringUTFChars(pathString.get(), path);
    return true;
}

inline ScopedLocalRef<jclass> loadClassWithInMemoryDexLoader(JNIEnv* env,
                                                             jclass inMemoryLoaderClass,
                                                             jobject parentClassLoader,
                                                             jmethodID loadClassMethod,
                                                             jstring className, const char* name) {
    jmethodID constructor = env->GetMethodID(inMemoryLoaderClass, "<init>",
                                             "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (clearAndDescribePendingException(env) || constructor == nullptr) {
        return ScopedLocalRef<jclass>(env, nullptr);
    }

    ScopedLocalRef<jobject> byteBuffer(env,
                                       env->NewDirectByteBuffer(const_cast<char*>(
                                                                        &_binary_classes_dex_start),
                                                                static_cast<jlong>(
                                                                        getEmbeddedDexSize())));
    if (clearAndDescribePendingException(env) || byteBuffer.get() == nullptr) {
        return ScopedLocalRef<jclass>(env, nullptr);
    }

    ScopedLocalRef<jobject> dexLoader(env,
                                      env->NewObject(inMemoryLoaderClass, constructor,
                                                     byteBuffer.get(), parentClassLoader));
    if (clearAndDescribePendingException(env) || dexLoader.get() == nullptr) {
        return ScopedLocalRef<jclass>(env, nullptr);
    }

    ScopedLocalRef<jclass> targetClass =
            loadClassWithLoader(env, dexLoader.get(), loadClassMethod, className,
                                /*describeException=*/true);
    if (targetClass.get() == nullptr) {
        ALOGE("Unable to find %s class", name);
    }
    return targetClass;
}

inline ScopedLocalRef<jclass> loadClassWithPathDexLoader(JNIEnv* env, jobject activity,
                                                         jobject parentClassLoader,
                                                         jmethodID loadClassMethod,
                                                         jstring className, const char* name) {
    ScopedLocalRef<jstring> pathLoaderName(env, env->NewStringUTF("dalvik/system/PathClassLoader"));
    if (clearAndDescribePendingException(env) || pathLoaderName.get() == nullptr) {
        return ScopedLocalRef<jclass>(env, nullptr);
    }
    ScopedLocalRef<jclass> pathLoaderClass =
            loadClassWithLoader(env, parentClassLoader, loadClassMethod, pathLoaderName.get(),
                                /*describeException=*/true);
    if (pathLoaderClass.get() == nullptr) {
        ALOGE("Unable to find dalvik/system/PathClassLoader.");
        return ScopedLocalRef<jclass>(env, nullptr);
    }

    jmethodID constructor = env->GetMethodID(pathLoaderClass.get(), "<init>",
                                             "(Ljava/lang/String;Ljava/lang/ClassLoader;)V");
    if (clearAndDescribePendingException(env) || constructor == nullptr) {
        return ScopedLocalRef<jclass>(env, nullptr);
    }

    std::string tempPath;
    if (!createTempFile(env, activity, ".dex", tempPath)) {
        ALOGE("Unable to create a temporary file to store DEX with "
              "Java classes.");
        return ScopedLocalRef<jclass>(env, nullptr);
    }
    TempFileGuard tempFileGuard(tempPath);

    if (!saveBytesToFile(tempPath, &_binary_classes_dex_start, getEmbeddedDexSize())) {
        ALOGE("Unable to write to %s file.", tempPath.c_str());
        return ScopedLocalRef<jclass>(env, nullptr);
    }

    ScopedLocalRef<jstring> dexPathString(env, env->NewStringUTF(tempPath.c_str()));
    if (clearAndDescribePendingException(env) || dexPathString.get() == nullptr) {
        return ScopedLocalRef<jclass>(env, nullptr);
    }
    ScopedLocalRef<jobject> dexLoader(env,
                                      env->NewObject(pathLoaderClass.get(), constructor,
                                                     dexPathString.get(), parentClassLoader));
    if (clearAndDescribePendingException(env) || dexLoader.get() == nullptr) {
        return ScopedLocalRef<jclass>(env, nullptr);
    }

    ScopedLocalRef<jclass> targetClass =
            loadClassWithLoader(env, dexLoader.get(), loadClassMethod, className,
                                /*describeException=*/true);
    if (targetClass.get() == nullptr) {
        ALOGE("Unable to find %s class", name);
    }
    return targetClass;
}

inline ScopedLocalRef<jclass> loadClassFromDexBytes(JNIEnv* env, jobject activity,
                                                    jobject parentClassLoader,
                                                    jmethodID loadClassMethod, jstring className,
                                                    const char* name) {
    const int sdkVersion = gamesdk::GetSystemPropAsInt("ro.build.version.sdk");
    if (sdkVersion >= 26) {
        ScopedLocalRef<jstring> inMemoryLoaderName(env,
                                                   env->NewStringUTF(
                                                           "dalvik/system/InMemoryDexClassLoader"));
        if (clearAndDescribePendingException(env) || inMemoryLoaderName.get() == nullptr) {
            return ScopedLocalRef<jclass>(env, nullptr);
        }
        ScopedLocalRef<jclass> inMemoryLoaderClass =
                loadClassWithLoader(env, parentClassLoader, loadClassMethod,
                                    inMemoryLoaderName.get(), /*describeException=*/false);
        if (inMemoryLoaderClass.get() != nullptr) {
            return loadClassWithInMemoryDexLoader(env, inMemoryLoaderClass.get(), parentClassLoader,
                                                  loadClassMethod, className, name);
        }
    }
    return loadClassWithPathDexLoader(env, activity, parentClassLoader, loadClassMethod, className,
                                      name);
}

#endif // #ifndef ANDROIDGAMESDK_NO_BINARY_DEX_LINKAGE

static jclass loadClass(JNIEnv* env, jobject activity, const char* name,
                        JNINativeMethod* nativeMethods, size_t nativeMethodsSize) {
    /*
     *   1. Get a classloader from activity
     *   2. Try to create the requested class from the activity classloader
     *   3. If step 2 not successful then get a classloader for dex bytes (in
     *      memory or file)
     *   4. If step 2 or step 3 is successful then register native methods
     */
    if (!env || !activity || !name) {
        return nullptr;
    }
    if (nativeMethodsSize > 0 && !nativeMethods) {
        return nullptr;
    }

    ScopedLocalRef<jclass> activityClass(env, env->GetObjectClass(activity));
    if (clearAndDescribePendingException(env) || activityClass.get() == nullptr) {
        return nullptr;
    }
    ScopedLocalRef<jclass> classLoaderClass(env, env->FindClass("java/lang/ClassLoader"));
    if (clearAndDescribePendingException(env) || classLoaderClass.get() == nullptr) {
        return nullptr;
    }
    jmethodID getClassLoader =
            env->GetMethodID(activityClass.get(), "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (clearAndDescribePendingException(env) || getClassLoader == nullptr) {
        return nullptr;
    }
    ScopedLocalRef<jobject> classLoaderObj(env, env->CallObjectMethod(activity, getClassLoader));
    if (clearAndDescribePendingException(env) || classLoaderObj.get() == nullptr) {
        return nullptr;
    }
    jmethodID loadClassMethod = env->GetMethodID(classLoaderClass.get(), "loadClass",
                                                 "(Ljava/lang/String;)Ljava/lang/Class;");
    if (clearAndDescribePendingException(env) || loadClassMethod == nullptr) {
        return nullptr;
    }
    ScopedLocalRef<jstring> className(env, env->NewStringUTF(name));
    if (clearAndDescribePendingException(env) || className.get() == nullptr) {
        return nullptr;
    }

    ScopedLocalRef<jclass> targetClass =
            loadClassWithLoader(env, classLoaderObj.get(), loadClassMethod, className.get(),
                                /*describeException=*/false);
    bool loadedFromDexBytes = false;
    if (targetClass.get() == nullptr) {
#ifdef ANDROIDGAMESDK_NO_BINARY_DEX_LINKAGE
        ALOGE("Couldn't find class %s and ANDROIDGAMESDK_NO_BINARY_DEX_LINKAGE "
              "is defined",
              name);
        return nullptr;
#else
        targetClass = loadClassFromDexBytes(env, activity, classLoaderObj.get(), loadClassMethod,
                                            className.get(), name);
        if (targetClass.get() == nullptr) {
            return nullptr;
        }
        loadedFromDexBytes = true;
#endif // #ifdef ANDROIDGAMESDK_NO_BINARY_DEX_LINKAGE
    }

    if (nativeMethodsSize > 0) {
        const jint registerResult = env->RegisterNatives(targetClass.get(), nativeMethods,
                                                         static_cast<jint>(nativeMethodsSize));
        if (clearAndDescribePendingException(env) || registerResult != JNI_OK) {
            return nullptr;
        }
    }
    if (loadedFromDexBytes) {
        ALOGI("Using internal %s class from dex bytes.", name);
    }
    return targetClass.release();
}

} // namespace gamesdk

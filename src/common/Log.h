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

#ifdef __ANDROID__
#include <android/log.h>
#else
#include <cstdio>
#endif

#include <string>

#ifdef __ANDROID__
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#else
#define ALOGE(...)                                               \
    do {                                                         \
        if (false) (void)std::snprintf(nullptr, 0, __VA_ARGS__); \
    } while (0)
#define ALOGW(...) ALOGE(__VA_ARGS__)
#define ALOGI(...) ALOGE(__VA_ARGS__)
#endif
#define ALOGW_ONCE_IF(cond, ...)     \
    do {                             \
        if (cond) {                  \
            ALOGW_ONCE(__VA_ARGS__); \
        }                            \
    } while (0)
#define ALOGW_ONCE(...)                                        \
    do {                                                       \
        static bool alogw_once##__FILE__##__LINE__##__ = true; \
        if (alogw_once##__FILE__##__LINE__##__) {              \
            alogw_once##__FILE__##__LINE__##__ = false;        \
            ALOGW(__VA_ARGS__);                                \
        }                                                      \
    } while (0)
#define ALOGE_ONCE(...)                                        \
    do {                                                       \
        static bool aloge_once##__FILE__##__LINE__##__ = true; \
        if (aloge_once##__FILE__##__LINE__##__) {              \
            aloge_once##__FILE__##__LINE__##__ = false;        \
            ALOGE(__VA_ARGS__);                                \
        }                                                      \
    } while (0)

#if !defined(NDEBUG) && defined(__ANDROID__)
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#else
#define ALOGV(...)
#endif

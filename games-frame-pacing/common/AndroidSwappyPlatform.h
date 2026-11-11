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

#include <memory>

#include "SwappyPlatform.h"

namespace swappy {

// Creates the Android SwappyPlatform backed by JNI, NDK, and SwappyDisplayManager.
// Returns a platform object whose isValid() is false if initialization fails.
std::unique_ptr<SwappyPlatform> createAndroidSwappyPlatform(JNIEnv* env, jobject jactivity);

// Creates an Android SwappyPlatform for external Choreographer tests without JNI/display hooks.
std::unique_ptr<SwappyPlatform> createAppSwappyPlatform(const SwappyCommonSettings& settings);

} // namespace swappy

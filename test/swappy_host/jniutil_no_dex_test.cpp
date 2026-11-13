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

#define ANDROIDGAMESDK_NO_BINARY_DEX_LINKAGE
#include "JNIUtil.h"
#include "ScopedLocalRef.h"
#include "fake_jni_env.h"
#include "gtest/gtest.h"

namespace {

constexpr const char* kTargetClass = "com/google/androidgamesdk/ChoreographerCallback";

void stubNativeCallback() {}

JNINativeMethod kTestNativeMethods[] = {
        {const_cast<char*>("nOnChoreographer"), const_cast<char*>("(JJ)V"),
         reinterpret_cast<void*>(&stubNativeCallback)},
};

TEST(JniUtilNoDexLinkageHostTest, LoadsPrimaryClassAndReleasesLocalRefsWithoutBinaryDex) {
    swappy::testing_support::FakeJniEnv fakeJni;
    fakeJni.addPrimaryClass(kTargetClass);

    gamesdk::ScopedLocalRef<jclass> loaded(fakeJni.env(),
                                           gamesdk::loadClass(fakeJni.env(), fakeJni.activity(),
                                                              kTargetClass, kTestNativeMethods, 1));

    ASSERT_NE(loaded.get(), nullptr);
    EXPECT_EQ(fakeJni.liveLocalRefCount(), 1);
    EXPECT_EQ(fakeJni.registeredNativeMethodCount(), 1);

    loaded = gamesdk::ScopedLocalRef<jclass>(fakeJni.env(), nullptr);
    EXPECT_EQ(fakeJni.liveLocalRefCount(), 0);
    EXPECT_FALSE(fakeJni.hasPendingException());
}

TEST(JniUtilNoDexLinkageHostTest, ReturnsNullAndReleasesLocalRefsWhenPrimaryClassMissing) {
    swappy::testing_support::FakeJniEnv fakeJni;

    jclass loaded = gamesdk::loadClass(fakeJni.env(), fakeJni.activity(), kTargetClass,
                                       kTestNativeMethods, 1);

    EXPECT_EQ(loaded, nullptr);
    EXPECT_EQ(fakeJni.liveLocalRefCount(), 0);
    EXPECT_EQ(fakeJni.callsWithPendingException(), 0);
    EXPECT_FALSE(fakeJni.hasPendingException());
}

} // namespace

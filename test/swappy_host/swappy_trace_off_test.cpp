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

#include <gtest/gtest.h>

#include <cstdint>

#include "SwappyTrace.h"
#include "Trace.h"

#if !defined(AGDK_TRACE) || AGDK_TRACE != 0
#error "swappy_trace_off_test.cpp must be compiled with -DAGDK_TRACE=0"
#endif

namespace {

struct MockSwappyCommonLayout {
    uint64_t baselineField = 0;
    TRACE_MEMBER(swappy::TraceSession mTraceSession);
};

static_assert(sizeof(MockSwappyCommonLayout) == sizeof(uint64_t),
              "TRACE_MEMBER must add 0 bytes when AGDK_TRACE == 0");

TEST(SwappyTraceOffHostTest, AllSwappyTraceMacrosCompileOutWithZeroSizeAndZeroSideEffects) {
    [[maybe_unused]] MockSwappyCommonLayout holder;

    int sideEffects = 0;
    // mTraceSession is compiled out of MockSwappyCommonLayout when AGDK_TRACE == 0,
    // verifying that SWAPPY_TRACE_SETTINGS strips all arguments at preprocess time.
    SWAPPY_TRACE_SETTINGS(holder.mTraceSession, ++sideEffects);
    SWAPPY_TRACE_WORK_TIMES(++sideEffects, ++sideEffects);
    SWAPPY_TRACE_WAKE_ERROR(++sideEffects);
    SWAPPY_TRACE_PRESENT_TARGET(++sideEffects, ++sideEffects, ++sideEffects,
                                swappy::TraceBackend::GL);
    SWAPPY_TRACE_PRESENT_ACTUAL(++sideEffects, ++sideEffects, ++sideEffects, ++sideEffects,
                                swappy::TraceBackend::Vulkan);

    EXPECT_EQ(sideEffects, 0);
}

} // namespace

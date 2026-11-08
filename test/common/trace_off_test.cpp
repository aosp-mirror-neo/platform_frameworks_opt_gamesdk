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
#include <memory>
#include <type_traits>

#include "Trace.h"

#if !defined(TRACE) || TRACE != 0
#error "trace_off_test.cpp must be compiled with -DTRACE=0"
#endif

namespace {

static_assert(std::is_trivially_destructible_v<gamesdk::ScopedTrace>,
              "ScopedTrace stub must be trivially destructible when TRACE == 0");

struct MockTraceOffLayout {
    uint64_t baselineField = 0;
    TRACE_MEMBER(uint64_t, traceField);
};

static_assert(sizeof(MockTraceOffLayout) == sizeof(uint64_t),
              "TRACE_MEMBER must add 0 bytes when TRACE == 0");

TEST(TraceOffHostTest, MacrosCompileOutWithoutEvaluatingArguments) {
    static_assert(!TRACE_ENABLED(), "TRACE_ENABLED() must be false when TRACE == 0");
    EXPECT_FALSE(TRACE_ENABLED());

    MockTraceOffLayout holder;
    EXPECT_EQ(holder.baselineField, 0u);

    int sideEffects = 0;
    TRACE_CALL();
    {
        TRACE_SCOPE((++sideEffects, "IgnoredScope"));
    }
    TRACE_INT((++sideEffects, "IgnoredCounter"), ++sideEffects);
    TRACE_INSTANT("IgnoredInstant %d", ++sideEffects);

    EXPECT_EQ(sideEffects, 0);
}

TEST(TraceOffHostTest, TraceAndScopedTraceNoOpStubsSupportDirectConsumers) {
    std::unique_ptr<gamesdk::Trace> ownedTrace = gamesdk::Trace::create();
    ASSERT_NE(ownedTrace, nullptr);
    EXPECT_FALSE(ownedTrace->isAvailable());
    EXPECT_FALSE(ownedTrace->isEnabled());
    EXPECT_FALSE(ownedTrace->supportsBasicATrace());
    EXPECT_FALSE(ownedTrace->supportsFullATrace());

    ownedTrace->beginSection("TFTrace");
    ownedTrace->endSection();
    ownedTrace->beginAsyncSection("TFAsync", 101);
    ownedTrace->endAsyncSection("TFAsync", 101);
    ownedTrace->setCounter("TFCounter", 42);
    ownedTrace->instant("TFInstant %d", 7);

    gamesdk::Trace* singletonTrace = gamesdk::Trace::getInstance();
    ASSERT_NE(singletonTrace, nullptr);
    EXPECT_FALSE(singletonTrace->isAvailable());
    EXPECT_FALSE(singletonTrace->isEnabled());

    gamesdk::ScopedTrace directScope("DirectScopedTrace");
}

} // namespace

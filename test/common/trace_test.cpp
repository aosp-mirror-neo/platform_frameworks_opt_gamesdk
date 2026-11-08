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

#include "Trace.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>

#include "TraceTestHelper.h"

namespace {

using gamesdk::Trace;
using RecordedCallbacks = gamesdk::test::RecordedTraceCallbacks;
using gamesdk::test::ScopedMockTrace;

struct MockInstrumentedLayout {
    uint64_t baselineField = 0;
    TRACE_MEMBER(uint64_t, traceField) = 42;
};

static_assert(sizeof(MockInstrumentedLayout) == 2 * sizeof(uint64_t),
              "TRACE_MEMBER must declare the member when TRACE != 0");

TEST(TraceHostTest, DefaultInstanceAndMacrosAreSafeNoOpsOnHost) {
    Trace* hostTrace = Trace::getInstance();
    ASSERT_NE(hostTrace, nullptr);
    EXPECT_FALSE(hostTrace->isAvailable());
    EXPECT_FALSE(hostTrace->isEnabled());
    EXPECT_FALSE(hostTrace->supportsBasicATrace());
    EXPECT_FALSE(hostTrace->supportsFullATrace());
    EXPECT_FALSE(TRACE_ENABLED());

    int evaluated = 0;
    TRACE_CALL();
    {
        TRACE_SCOPE((++evaluated, "ScopeWhenDisabled"));
    }
    TRACE_INT((++evaluated, "CounterWhenDisabled"), ++evaluated);
    TRACE_INSTANT("Instant %d", ++evaluated);
    EXPECT_EQ(evaluated, 0);
}

TEST(TraceHostTest, CustomCallbacksDispatchWhenEnabledAndGateWhenDisabled) {
    RecordedCallbacks::reset(true);
    Trace trace(&RecordedCallbacks::beginSection, &RecordedCallbacks::endSection,
                &RecordedCallbacks::isEnabled, &RecordedCallbacks::beginAsyncSection,
                &RecordedCallbacks::endAsyncSection, &RecordedCallbacks::setCounter);

    EXPECT_TRUE(trace.isAvailable());
    EXPECT_TRUE(trace.supportsBasicATrace());
    EXPECT_TRUE(trace.supportsFullATrace());
    EXPECT_TRUE(trace.isEnabled());

    trace.beginSection("Trace::WaitPhase");
    trace.endSection();
    trace.beginAsyncSection("AsyncStep", 7);
    trace.endAsyncSection("AsyncStep", 7);
    trace.setCounter("Trace.CpuTimeUs", 4200);

    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 4u);
    EXPECT_EQ(RecordedCallbacks::sEvents[0], "B:Trace::WaitPhase");
    EXPECT_EQ(RecordedCallbacks::sEvents[1], "E");
    EXPECT_EQ(RecordedCallbacks::sEvents[2], "S:AsyncStep:7");
    EXPECT_EQ(RecordedCallbacks::sEvents[3], "F:AsyncStep:7");
    ASSERT_EQ(RecordedCallbacks::sCounters.size(), 1u);
    EXPECT_EQ(RecordedCallbacks::sCounters[0].first, "Trace.CpuTimeUs");
    EXPECT_EQ(RecordedCallbacks::sCounters[0].second, 4200);

    RecordedCallbacks::reset(false);
    EXPECT_FALSE(trace.isEnabled());
    trace.beginAsyncSection("IgnoredAsync", 8);
    trace.endAsyncSection("IgnoredAsync", 8);
    trace.setCounter("IgnoredCounter", 99);
    EXPECT_TRUE(RecordedCallbacks::sEvents.empty());
    EXPECT_TRUE(RecordedCallbacks::sCounters.empty());
}

TEST(TraceHostTest, InstantEmitsFormattedSectionWhenEnabledAndGatesWhenInactive) {
    RecordedCallbacks::reset(true);
    Trace trace(&RecordedCallbacks::beginSection, &RecordedCallbacks::endSection,
                &RecordedCallbacks::isEnabled, &RecordedCallbacks::beginAsyncSection,
                &RecordedCallbacks::endAsyncSection, &RecordedCallbacks::setCounter);

    trace.instant("Trace.Target f=%d si=%d be=%s", 42, 2, "vk");
    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 2u);
    EXPECT_EQ(RecordedCallbacks::sEvents[0], "B:Trace.Target f=42 si=2 be=vk");
    EXPECT_EQ(RecordedCallbacks::sEvents[1], "E");

    RecordedCallbacks::reset(false);
    trace.instant("Trace.Target f=%d", 43);
    EXPECT_TRUE(RecordedCallbacks::sEvents.empty());

    RecordedCallbacks::reset(true);
    Trace unavailableTrace;
    EXPECT_FALSE(unavailableTrace.isAvailable());
    unavailableTrace.instant("Trace.Target f=%d", 44);
    EXPECT_TRUE(RecordedCallbacks::sEvents.empty());
}

TEST(TraceHostTest, InstantTruncatesLongFormattedNamesSafely) {
    RecordedCallbacks::reset(true);
    Trace trace(&RecordedCallbacks::beginSection, &RecordedCallbacks::endSection,
                &RecordedCallbacks::isEnabled, &RecordedCallbacks::beginAsyncSection,
                &RecordedCallbacks::endAsyncSection, &RecordedCallbacks::setCounter);

    const std::string longPayload(320, 'x');
    trace.instant("Prefix:%s", longPayload.c_str());

    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 2u);
    EXPECT_EQ(RecordedCallbacks::sEvents[0].size(), 2u + 255u);
    EXPECT_EQ(RecordedCallbacks::sEvents[0].substr(0, 9), "B:Prefix:");
    EXPECT_EQ(RecordedCallbacks::sEvents[0].substr(9), std::string(255 - 7, 'x'));
    EXPECT_EQ(RecordedCallbacks::sEvents[1], "E");
}

TEST(TraceHostTest, ScopedTraceIgnoresNullNameWhenEnabled) {
    ScopedMockTrace mockTrace(true);
    {
        gamesdk::ScopedTrace nullScope(nullptr);
    }
    EXPECT_TRUE(RecordedCallbacks::sEvents.empty());
}

TEST(TraceHostTest, MacrosDispatchWhenEnabledAndSkipArgumentsWhenDisabled) {
    MockInstrumentedLayout holder;
    EXPECT_EQ(holder.baselineField, 0u);
    EXPECT_EQ(holder.traceField, 42u);

    ScopedMockTrace mockTrace(true);
    EXPECT_TRUE(TRACE_ENABLED());
    int evaluated = 0;

    {
        TRACE_SCOPE((++evaluated, "OuterScope"));
        TRACE_INT("MetricA", (++evaluated, 123));
        TRACE_INSTANT("Marker v=%d", (++evaluated, 7));
    }

    EXPECT_EQ(evaluated, 3);
    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 4u);
    EXPECT_EQ(RecordedCallbacks::sEvents[0], "B:OuterScope");
    EXPECT_EQ(RecordedCallbacks::sEvents[1], "B:Marker v=7");
    EXPECT_EQ(RecordedCallbacks::sEvents[2], "E");
    EXPECT_EQ(RecordedCallbacks::sEvents[3], "E");
    ASSERT_EQ(RecordedCallbacks::sCounters.size(), 1u);
    EXPECT_EQ(RecordedCallbacks::sCounters[0],
              std::make_pair(std::string("MetricA"), int64_t{123}));

    RecordedCallbacks::reset(false);
    EXPECT_FALSE(TRACE_ENABLED());
    evaluated = 0;
    {
        TRACE_SCOPE((++evaluated, "SkippedScope"));
        TRACE_INT("SkippedMetric", ++evaluated);
        TRACE_INSTANT("SkippedMarker v=%d", ++evaluated);
    }
    EXPECT_EQ(evaluated, 0);
    EXPECT_TRUE(RecordedCallbacks::sEvents.empty());
    EXPECT_TRUE(RecordedCallbacks::sCounters.empty());
}

} // namespace

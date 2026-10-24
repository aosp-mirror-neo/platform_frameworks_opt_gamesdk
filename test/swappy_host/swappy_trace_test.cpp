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

#include <charconv>
#include <chrono>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "SwappyTrace.h"
#include "TraceTestHelper.h"

namespace {

using namespace std::chrono_literals;
using swappy::PipelineMode;
using swappy::TraceBackend;
using swappy::TracePresentTargets;
using swappy::TraceSession;
using swappy::TraceSettingsSnapshot;
using RecordedCallbacks = gamesdk::test::RecordedTraceCallbacks;
using gamesdk::test::ScopedMockTrace;

static_assert(sizeof(TraceSession) <= 56, "TraceSession must stay compact (<= 56 bytes)");

// Stands in for SwappyCommon: owns the settings and the TraceSession, and
// counts how many snapshots SWAPPY_TRACE_SETTINGS builds.
struct SettingsHarness {
    SettingsHarness() {
        mSettings.swapDurationNs = 16666667;
        mSettings.refreshPeriodNs = 16666666;
        mSettings.targetPeriodNs = 33333332;
        mSettings.autoSwapIntervalThresholdNs = 50000000;
        mSettings.fenceTimeoutNs = 50000000;
        mSettings.autoSwapInterval = 2;
    }

    TraceSettingsSnapshot snapshot() {
        ++mSnapshotCount;
        return mSettings;
    }

    void step() {
        SWAPPY_TRACE_SETTINGS(mTraceSession, snapshot());
    }

    TraceSettingsSnapshot mSettings;
    int mSnapshotCount = 0;
    TRACE_MEMBER(TraceSession mTraceSession);
};

// Swappy.Settings event for the initial SettingsHarness settings.
constexpr char kInitialSettingsEvent[] =
        "B:Swappy.Settings impl=Classic(drop=10%,pipeOff=50%) swap=16.67ms refresh=16.67ms "
        "autoSI=1 autoPM=1 pipeline=auto maxAuto=50ms pacing=1 block=1 fence=50ms "
        "stuffFix=0 affinity=1";

// Checks the six baseline counters that follow each Swappy.Settings event.
void expectBaselineCounters(int64_t autoPipelineMode, int64_t pipelineMode, int64_t targetPeriodNs,
                            int64_t autoSwapInterval, int64_t refreshPeriodNs,
                            int64_t swapDurationNs) {
    ASSERT_EQ(RecordedCallbacks::sCounters.size(), 6u);
    EXPECT_EQ(RecordedCallbacks::sCounters[0],
              std::make_pair(std::string("mPipelineModeAutoMode"), autoPipelineMode));
    EXPECT_EQ(RecordedCallbacks::sCounters[1],
              std::make_pair(std::string("mPipelineMode"), pipelineMode));
    EXPECT_EQ(RecordedCallbacks::sCounters[2],
              std::make_pair(std::string("Swappy.TargetPeriodNs"), targetPeriodNs));
    EXPECT_EQ(RecordedCallbacks::sCounters[3],
              std::make_pair(std::string("mAutoSwapInterval"), autoSwapInterval));
    EXPECT_EQ(RecordedCallbacks::sCounters[4],
              std::make_pair(std::string("mCommonSettings.refreshPeriod"), refreshPeriodNs));
    EXPECT_EQ(RecordedCallbacks::sCounters[5],
              std::make_pair(std::string("mSwapDuration"), swapDurationNs));
}

TEST(SwappyTraceHostTest, SettingsAttachEmitsSnapshotAndSeedsBaselineCounters) {
    ScopedMockTrace mockTrace(true);
    SettingsHarness s;

    s.step();

    EXPECT_EQ(s.mSnapshotCount, 1);
    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 2u);
    EXPECT_EQ(RecordedCallbacks::sEvents[0], kInitialSettingsEvent);
    EXPECT_EQ(RecordedCallbacks::sEvents[1], "E");
    expectBaselineCounters(1, 1, 33333332, 2, 16666666, 16666667);
}

TEST(SwappyTraceHostTest, SettingsUnchangedWhileAttachedEmitNothing) {
    ScopedMockTrace mockTrace(true);
    SettingsHarness s;
    s.step();
    RecordedCallbacks::reset(true);

    s.step();

    EXPECT_EQ(s.mSnapshotCount, 2);
    EXPECT_TRUE(RecordedCallbacks::sEvents.empty());
    EXPECT_TRUE(RecordedCallbacks::sCounters.empty());
}

TEST(SwappyTraceHostTest, SettingsPipelineFlipUnderAutoPipelineModeEmitsNothing) {
    ScopedMockTrace mockTrace(true);
    SettingsHarness s;
    s.step();
    RecordedCallbacks::reset(true);

    s.mSettings.pipelineMode = PipelineMode::Off;
    s.step();

    EXPECT_EQ(s.mSnapshotCount, 2);
    EXPECT_TRUE(RecordedCallbacks::sEvents.empty());
    EXPECT_TRUE(RecordedCallbacks::sCounters.empty());
}

TEST(SwappyTraceHostTest, SettingsManualPipelineFlipReEmitsSnapshotAndCounters) {
    ScopedMockTrace mockTrace(true);
    SettingsHarness s;
    s.mSettings.autoPipelineMode = false;
    s.mSettings.pipelineMode = PipelineMode::On;
    s.step();
    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 2u);
    EXPECT_NE(RecordedCallbacks::sEvents[0].find(" autoPM=0 pipeline=on "), std::string::npos)
            << RecordedCallbacks::sEvents[0];
    RecordedCallbacks::reset(true);

    s.mSettings.pipelineMode = PipelineMode::Off;
    s.step();

    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 2u);
    EXPECT_NE(RecordedCallbacks::sEvents[0].find(" autoPM=0 pipeline=off "), std::string::npos)
            << RecordedCallbacks::sEvents[0];
    EXPECT_EQ(RecordedCallbacks::counterValues("mPipelineMode"),
              std::vector<int64_t>{static_cast<int64_t>(PipelineMode::Off)});
}

TEST(SwappyTraceHostTest, SettingsChangeWhileAttachedReEmitsSnapshotAndCounters) {
    ScopedMockTrace mockTrace(true);
    SettingsHarness s;
    s.step();
    RecordedCallbacks::reset(true);

    s.mSettings.swapDurationNs = 8333333;
    s.mSettings.refreshPeriodNs = 8333333;
    s.mSettings.targetPeriodNs = 8333333;
    s.mSettings.autoSwapInterval = 1;
    s.mSettings.autoPipelineMode = false;
    s.mSettings.pipelineMode = PipelineMode::Off;
    s.mSettings.useAffinity = false;
    s.step();

    EXPECT_EQ(s.mSnapshotCount, 2);
    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 2u);
    EXPECT_EQ(RecordedCallbacks::sEvents[0],
              "B:Swappy.Settings impl=Classic(drop=10%,pipeOff=50%) swap=8.33ms refresh=8.33ms "
              "autoSI=1 autoPM=0 pipeline=off maxAuto=50ms pacing=1 block=1 fence=50ms "
              "stuffFix=0 affinity=0");
    EXPECT_EQ(RecordedCallbacks::sEvents[1], "E");
    expectBaselineCounters(0, 0, 8333333, 1, 8333333, 8333333);
}

TEST(SwappyTraceHostTest, SettingsReportPipelineOnWhenAutoPipelineModeIsOff) {
    ScopedMockTrace mockTrace(true);
    SettingsHarness s;
    s.mSettings.autoPipelineMode = false;
    s.mSettings.pipelineMode = PipelineMode::On;

    s.step();

    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 2u);
    EXPECT_EQ(RecordedCallbacks::sEvents[0],
              "B:Swappy.Settings impl=Classic(drop=10%,pipeOff=50%) swap=16.67ms refresh=16.67ms "
              "autoSI=1 autoPM=0 pipeline=on maxAuto=50ms pacing=1 block=1 fence=50ms "
              "stuffFix=0 affinity=1");
    EXPECT_EQ(RecordedCallbacks::sEvents[1], "E");
    expectBaselineCounters(0, 1, 33333332, 2, 16666666, 16666667);
}

struct SettingsKeyCase {
    const char* name;
    void (*mutate)(TraceSettingsSnapshot&);
    const char* expectedField;
};

class SwappyTraceSettingsKeyTest : public ::testing::TestWithParam<SettingsKeyCase> {};

TEST_P(SwappyTraceSettingsKeyTest, ReEmitsSnapshotOnceWhenOneKeyFieldChanges) {
    ScopedMockTrace mockTrace(true);
    SettingsHarness s;
    s.step();
    RecordedCallbacks::reset(true);

    GetParam().mutate(s.mSettings);
    s.step();

    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 2u);
    EXPECT_NE(RecordedCallbacks::sEvents[0].find(GetParam().expectedField), std::string::npos)
            << RecordedCallbacks::sEvents[0];
    EXPECT_EQ(RecordedCallbacks::sEvents[1], "E");
    EXPECT_EQ(RecordedCallbacks::sCounters.size(), 6u);

    // The session keeps the new key: the same settings do not emit again.
    RecordedCallbacks::reset(true);
    s.step();
    EXPECT_TRUE(RecordedCallbacks::sEvents.empty());
    EXPECT_TRUE(RecordedCallbacks::sCounters.empty());
}

INSTANTIATE_TEST_SUITE_P(
        KeyFields, SwappyTraceSettingsKeyTest,
        ::testing::Values(SettingsKeyCase{"autoSwapIntervalThresholdNs",
                                          [](TraceSettingsSnapshot& settings) {
                                              settings.autoSwapIntervalThresholdNs = 40000000;
                                          },
                                          " maxAuto=40ms "},
                          SettingsKeyCase{"fenceTimeoutNs",
                                          [](TraceSettingsSnapshot& settings) {
                                              settings.fenceTimeoutNs = 20000000;
                                          },
                                          " fence=20ms "},
                          SettingsKeyCase{"bufferStuffingFixWait",
                                          [](TraceSettingsSnapshot& settings) {
                                              settings.bufferStuffingFixWait = 3;
                                          },
                                          " stuffFix=3 "},
                          SettingsKeyCase{"autoSwapIntervalEnabled",
                                          [](TraceSettingsSnapshot& settings) {
                                              settings.autoSwapIntervalEnabled = false;
                                          },
                                          " autoSI=0 "},
                          SettingsKeyCase{"framePacingEnabled",
                                          [](TraceSettingsSnapshot& settings) {
                                              settings.framePacingEnabled = false;
                                          },
                                          " pacing=0 "},
                          SettingsKeyCase{"blockingWaitEnabled",
                                          [](TraceSettingsSnapshot& settings) {
                                              settings.blockingWaitEnabled = false;
                                          },
                                          " block=0 "},
                          SettingsKeyCase{"swapDurationNs",
                                          [](TraceSettingsSnapshot& settings) {
                                              settings.swapDurationNs = 33333333;
                                          },
                                          " swap=33.33ms "},
                          SettingsKeyCase{"refreshPeriodNs",
                                          [](TraceSettingsSnapshot& settings) {
                                              settings.refreshPeriodNs = 8333333;
                                          },
                                          " refresh=8.33ms "},
                          SettingsKeyCase{"useAffinity",
                                          [](TraceSettingsSnapshot& settings) {
                                              settings.useAffinity = false;
                                          },
                                          " affinity=0"}),
        [](const ::testing::TestParamInfo<SettingsKeyCase>& info) {
            return std::string(info.param.name);
        });

TEST(SwappyTraceHostTest, SettingsDetachSkipsSnapshot) {
    ScopedMockTrace mockTrace(true);
    SettingsHarness s;
    s.step();

    // Perfetto detaches: the macro must not build a snapshot.
    RecordedCallbacks::reset(false);
    s.step();

    EXPECT_EQ(s.mSnapshotCount, 1);
    EXPECT_TRUE(RecordedCallbacks::sEvents.empty());
    EXPECT_TRUE(RecordedCallbacks::sCounters.empty());
}

TEST(SwappyTraceHostTest, SettingsReattachReEmitsUnchangedSnapshotAndCounters) {
    ScopedMockTrace mockTrace(true);
    SettingsHarness s;
    s.step();
    RecordedCallbacks::reset(false);
    s.step();

    // Perfetto attaches again mid-run with the same settings.
    RecordedCallbacks::reset(true);
    s.step();

    EXPECT_EQ(s.mSnapshotCount, 2);
    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 2u);
    EXPECT_EQ(RecordedCallbacks::sEvents[0], kInitialSettingsEvent);
    EXPECT_EQ(RecordedCallbacks::sEvents[1], "E");
    expectBaselineCounters(1, 1, 33333332, 2, 16666666, 16666667);
}

TEST(SwappyTraceHostTest, WorkTimesEmitCpuAndGpuCountersAndSkipNonPositiveGpuTime) {
    ScopedMockTrace mockTrace(true);

    SWAPPY_TRACE_WORK_TIMES(4200us, 6800us);
    SWAPPY_TRACE_WORK_TIMES(3100us, -1ns);
    // Vulkan reports 0ns until the first fence completes.
    SWAPPY_TRACE_WORK_TIMES(2000us, 0ns);

    ASSERT_EQ(RecordedCallbacks::sCounters.size(), 4u);
    EXPECT_EQ(RecordedCallbacks::sCounters[0],
              std::make_pair(std::string("Swappy.CpuTimeUs"), int64_t{4200}));
    EXPECT_EQ(RecordedCallbacks::sCounters[1],
              std::make_pair(std::string("Swappy.GpuTimeUs"), int64_t{6800}));
    EXPECT_EQ(RecordedCallbacks::sCounters[2],
              std::make_pair(std::string("Swappy.CpuTimeUs"), int64_t{3100}));
    EXPECT_EQ(RecordedCallbacks::sCounters[3],
              std::make_pair(std::string("Swappy.CpuTimeUs"), int64_t{2000}));
}

TEST(SwappyTraceHostTest, WakeErrorEmitsMicrosecondCounter) {
    ScopedMockTrace mockTrace(true);

    SWAPPY_TRACE_WAKE_ERROR(345600ns);
    SWAPPY_TRACE_WAKE_ERROR(-12000ns);

    ASSERT_EQ(RecordedCallbacks::sCounters.size(), 2u);
    EXPECT_EQ(RecordedCallbacks::sCounters[0],
              std::make_pair(std::string("Swappy.WakeErrorUs"), int64_t{345}));
    EXPECT_EQ(RecordedCallbacks::sCounters[1],
              std::make_pair(std::string("Swappy.WakeErrorUs"), int64_t{-12}));
}

TEST(SwappyTraceHostTest, WakeErrorForTargetIsLatenessPastLatestDeadlineWhenWokenOnTargetTick) {
    const std::chrono::steady_clock::time_point latestTickDeadline(1'000ms);
    const std::chrono::steady_clock::time_point now(1'000ms + 300us);

    EXPECT_EQ(swappy::wakeErrorForTarget(now, latestTickDeadline, 7, 7, 16ms), 300us);
}

TEST(SwappyTraceHostTest, WakeErrorForTargetStepsBackOneRefreshPeriodPerExtraTick) {
    const std::chrono::steady_clock::time_point latestTickDeadline(1'000ms);
    const std::chrono::steady_clock::time_point now(1'000ms + 300us);

    EXPECT_EQ(swappy::wakeErrorForTarget(now, latestTickDeadline, 9, 7, 16ms), 32ms + 300us);
    EXPECT_EQ(swappy::wakeErrorForTarget(now, latestTickDeadline, 10, 7, 16ms), 48ms + 300us);
}

TEST(SwappyTraceHostTest, WakeErrorForTargetIsNegativeWhenWokenBeforeDeadline) {
    const std::chrono::steady_clock::time_point latestTickDeadline(1'000ms);
    const std::chrono::steady_clock::time_point now(1'000ms - 200us);

    EXPECT_EQ(swappy::wakeErrorForTarget(now, latestTickDeadline, 7, 7, 16ms), -200us);
}

// Checks a whole present event: prefix up to "n=", the steady_clock reading
// in nanoseconds that the event reports, and suffix. Stores the reading in
// nowNs if given.
void expectPresentEvent(const std::string& event, const std::string& prefix,
                        const std::string& suffix, int64_t* nowNs = nullptr) {
    ASSERT_GE(event.size(), prefix.size() + suffix.size()) << event;
    EXPECT_EQ(event.substr(0, prefix.size()), prefix);
    EXPECT_EQ(event.substr(event.size() - suffix.size()), suffix);
    const std::string now =
            event.substr(prefix.size(), event.size() - prefix.size() - suffix.size());
    ASSERT_FALSE(now.empty()) << event;
    ASSERT_EQ(now.find_first_not_of("0123456789"), std::string::npos) << event;
    int64_t parsedNow = 0;
    const char* const end = now.data() + now.size();
    const auto [ptr, ec] = std::from_chars(now.data(), end, parsedNow);
    ASSERT_TRUE(ec == std::errc{} && ptr == end) << event;
    if (nowNs != nullptr) {
        *nowNs = parsedNow;
    }
}

TEST(SwappyTraceHostTest, PresentTargetEmitsLeadCounterAndInstant) {
    ScopedMockTrace mockTrace(true);

    const auto before = std::chrono::steady_clock::now();
    const auto targetTp = before + 12ms;
    SWAPPY_TRACE_PRESENT_TARGET(9, targetTp, 2, TraceBackend::Vulkan);
    const auto after = std::chrono::steady_clock::now();

    // The event reports the steady_clock reading (n=) that the lead counter
    // uses. The expected lead comes from that reading, so the result does not
    // depend on how long this thread waits for the CPU.
    const int64_t targetNs = targetTp.time_since_epoch().count();
    int64_t nowNs = -1;
    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 2u);
    expectPresentEvent(RecordedCallbacks::sEvents[0],
                       "B:Swappy.Target f=9 t=" + std::to_string(targetNs) + " n=", " si=2 be=vk",
                       &nowNs);
    EXPECT_EQ(RecordedCallbacks::sEvents[1], "E");
    EXPECT_GE(nowNs, before.time_since_epoch().count());
    EXPECT_LE(nowNs, after.time_since_epoch().count());

    ASSERT_EQ(RecordedCallbacks::sCounters.size(), 1u);
    EXPECT_EQ(RecordedCallbacks::sCounters[0],
              std::make_pair(std::string("Swappy.TargetLeadUs"), (targetNs - nowNs) / 1000));
}

TEST(SwappyTraceHostTest, PresentActualOmitsMarginWhenNegative) {
    ScopedMockTrace mockTrace(true);

    // GL passive actual present (marginNs = -1 omits PresentMarginUs).
    SWAPPY_TRACE_PRESENT_ACTUAL(9, 100500000, 100000000, -1, TraceBackend::GL);
    ASSERT_EQ(RecordedCallbacks::sCounters.size(), 1u);
    EXPECT_EQ(RecordedCallbacks::sCounters[0],
              std::make_pair(std::string("Swappy.PresentErrorUs"), int64_t{500}));
    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 2u);
    expectPresentEvent(RecordedCallbacks::sEvents[0],
                       "B:Swappy.Actual f=9 t=100500000 d=100000000 n=", " be=gl");
    EXPECT_EQ(RecordedCallbacks::sEvents[1], "E");
}

TEST(SwappyTraceHostTest, PresentActualEmitsErrorAndMargin) {
    ScopedMockTrace mockTrace(true);

    // Vulkan passive actual present (marginNs >= 0 emits PresentMarginUs).
    SWAPPY_TRACE_PRESENT_ACTUAL(10, 200000000, 201000000, 2500000, TraceBackend::Vulkan);
    ASSERT_EQ(RecordedCallbacks::sCounters.size(), 2u);
    EXPECT_EQ(RecordedCallbacks::sCounters[0],
              std::make_pair(std::string("Swappy.PresentErrorUs"), int64_t{-1000}));
    EXPECT_EQ(RecordedCallbacks::sCounters[1],
              std::make_pair(std::string("Swappy.PresentMarginUs"), int64_t{2500}));
    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 2u);
    expectPresentEvent(RecordedCallbacks::sEvents[0],
                       "B:Swappy.Actual f=10 t=200000000 d=201000000 n=", " be=vk");
    EXPECT_EQ(RecordedCallbacks::sEvents[1], "E");
}

TEST(SwappyTraceHostTest, PresentActualSkipsNonPositiveActualTimestamps) {
    ScopedMockTrace mockTrace(true);

    // Dropped GL frames report EGL_TIMESTAMP_INVALID_ANDROID (-1); zero is also invalid.
    SWAPPY_TRACE_PRESENT_ACTUAL(11, -1, 100000000, 2000000, TraceBackend::GL);
    SWAPPY_TRACE_PRESENT_ACTUAL(12, 0, 100000000, 2000000, TraceBackend::Vulkan);

    EXPECT_TRUE(RecordedCallbacks::sCounters.empty());
    EXPECT_TRUE(RecordedCallbacks::sEvents.empty());
}

TEST(SwappyTraceHostTest, PresentActualOmitsPresentErrorWhenDesiredTimestampIsNonPositive) {
    ScopedMockTrace mockTrace(true);

    SWAPPY_TRACE_PRESENT_ACTUAL(13, 150000000, 0, 1800000, TraceBackend::Vulkan);
    SWAPPY_TRACE_PRESENT_ACTUAL(14, 166666667, -1, -1, TraceBackend::GL);

    ASSERT_EQ(RecordedCallbacks::sCounters.size(), 1u);
    EXPECT_EQ(RecordedCallbacks::sCounters[0],
              std::make_pair(std::string("Swappy.PresentMarginUs"), int64_t{1800}));
    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 4u);
    EXPECT_NE(RecordedCallbacks::sEvents[0].find("B:Swappy.Actual f=13 t=150000000 d=0 "),
              std::string::npos);
    EXPECT_EQ(RecordedCallbacks::sEvents[1], "E");
    EXPECT_NE(RecordedCallbacks::sEvents[2].find("B:Swappy.Actual f=14 t=166666667 d=-1 "),
              std::string::npos);
    EXPECT_EQ(RecordedCallbacks::sEvents[3], "E");
}

TEST(SwappyTraceHostTest, PresentTargetsGiveZeroDesiredTimeForFramesWithoutRequestedTime) {
    ScopedMockTrace mockTrace(true);
    TracePresentTargets targets;

    // Swappy requested a present time for frame 20 only. Frame 21 stands for
    // a frame where Swappy skipped eglPresentationTimeANDROID.
    targets.record(20, 100000000);
    SWAPPY_TRACE_PRESENT_ACTUAL(20, 101000000, targets.targetFor(20), -1, TraceBackend::GL);
    SWAPPY_TRACE_PRESENT_ACTUAL(21, 117666667, targets.targetFor(21), -1, TraceBackend::GL);

    EXPECT_EQ(RecordedCallbacks::counterValues("Swappy.PresentErrorUs"),
              std::vector<int64_t>{1000});
    ASSERT_EQ(RecordedCallbacks::sEvents.size(), 4u);
    EXPECT_NE(RecordedCallbacks::sEvents[0].find("B:Swappy.Actual f=20 t=101000000 d=100000000 "),
              std::string::npos);
    EXPECT_NE(RecordedCallbacks::sEvents[2].find("B:Swappy.Actual f=21 t=117666667 d=0 "),
              std::string::npos);
}

TEST(SwappyTraceHostTest, PresentTargetsDoNotGiveAnOldTargetToTheFrameThatReusesItsSlot) {
    TracePresentTargets targets;
    const uint64_t reusingFrame = 20 + TracePresentTargets::kCapacity;

    targets.record(20, 100000000);
    EXPECT_EQ(targets.targetFor(reusingFrame), 0);

    targets.record(reusingFrame, 366666667);
    EXPECT_EQ(targets.targetFor(reusingFrame), 366666667);
    EXPECT_EQ(targets.targetFor(20), 0);
}

TEST(SwappyTraceHostTest, PresentTargetsResetWhenSurfaceChanges) {
    TracePresentTargets targets;
    const int surfaceA = 0;
    const int surfaceB = 0;

    targets.record(50, 100000000, &surfaceA);
    ASSERT_EQ(targets.targetFor(50, &surfaceA), 100000000);
    EXPECT_EQ(targets.targetFor(50, &surfaceB), 0);

    // Frame id 60 >= 50 isolates the surface != mLastSurface reset branch.
    targets.observeNextFrame(60, &surfaceB);
    EXPECT_EQ(targets.targetFor(50, &surfaceA), 0);
}

TEST(SwappyTraceHostTest, PresentTargetsResetWhenFrameIdRewinds) {
    TracePresentTargets targets;
    const int surfaceA = 0;

    targets.record(50, 100000000, &surfaceA);
    ASSERT_EQ(targets.targetFor(50, &surfaceA), 100000000);

    targets.observeNextFrame(1, &surfaceA);
    EXPECT_EQ(targets.targetFor(50, &surfaceA), 0);
}

TEST(SwappyTraceHostTest, PresentTargetsGiveNoTargetToAnObservedFrameWithoutRecord) {
    TracePresentTargets targets;
    const int surface = 0;

    targets.record(1, 100000000, &surface);
    targets.observeNextFrame(1, &surface);

    EXPECT_EQ(targets.targetFor(1, &surface), 0);
}

TEST(SwappyTraceHostTest, SwappyMacrosSkipArgumentsWhenTracingDisabled) {
    ScopedMockTrace mockTrace(false);

    // Disabled runtime tracing skips argument evaluation for all Swappy macros.
    const auto targetTp = std::chrono::steady_clock::now() + 12ms;
    int evals = 0;
    // The snapshot expression is where SwappyCommon locks its mutex, so a
    // skipped expression also means that the lock is not taken.
    TraceSession session;
    SWAPPY_TRACE_SETTINGS(session, (++evals, TraceSettingsSnapshot{}));
    SWAPPY_TRACE_WORK_TIMES((++evals, 1ms), (++evals, 2ms));
    SWAPPY_TRACE_WAKE_ERROR((++evals, 500us));
    SWAPPY_TRACE_PRESENT_TARGET(++evals, targetTp, ++evals, TraceBackend::GL);
    SWAPPY_TRACE_PRESENT_ACTUAL(++evals, ++evals, ++evals, ++evals, TraceBackend::Vulkan);
    EXPECT_EQ(evals, 0);
    EXPECT_TRUE(RecordedCallbacks::sEvents.empty());
    EXPECT_TRUE(RecordedCallbacks::sCounters.empty());
}

} // namespace

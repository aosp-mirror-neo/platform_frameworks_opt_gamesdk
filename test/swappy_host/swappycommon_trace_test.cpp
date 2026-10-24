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

#include "SwappyCommon.h"
#include "SwappyTrace.h"
#include "Trace.h"

#if !defined(AGDK_TRACE) || AGDK_TRACE == 0
#error "swappycommon_trace_test.cpp must be compiled with AGDK_TRACE != 0"
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "Clock.h"
#include "FakeSwappyPlatform.h"
#include "Settings.h"
#include "SimulatedClock.h"
#include "TraceTestHelper.h"
#include "gtest/gtest.h"
#include "swappy/swappy_common.h"

namespace {

using namespace std::chrono_literals;
using gamesdk::test::RecordedTraceCallbacks;
using gamesdk::test::ScopedMockTrace;
using swappy::Clock;
using swappy::PipelineMode;
using swappy::Settings;
using swappy::SwappyCommon;
using swappy::SwappyCommonSettings;
using swappy::testing::FakeSwappyPlatform;
using swappy::testing::makeTestSwappySettings;
using swappy::testing::PlatformSharedState;
using swappy::testing::SimulatedClock;
using swappy::testing::VsyncPanelTimeline;

constexpr auto k60HzPeriod = 16'666'667ns;
constexpr auto k30HzPeriod = 33'333'334ns;
constexpr auto k120HzPeriod = 8'333'333ns;

SwappyCommon::SwapHandlers readySwapHandlers(std::chrono::nanoseconds gpuTime = 4ms) {
    return SwappyCommon::SwapHandlers{
            .lastFrameIsComplete = [] { return true; },
            .getPrevFrameGpuTime = [gpuTime] { return gpuTime; },
    };
}

class SwappyTraceHarness {
public:
    explicit SwappyTraceHarness(SwappyCommonSettings settings = makeTestSwappySettings(),
                                Clock::time_point initialTime = Clock::time_point{1s})
          : mState(std::make_shared<PlatformSharedState>()),
            mClock(initialTime),
            mTimeline(mState, settings, &mClock) {
        mClock.setVsyncCallbacks(
                [this](Clock::time_point current) { return mTimeline.nextVsyncAfter(current); },
                [this](Clock::time_point vsyncTime) { mTimeline.deliverVsync(vsyncTime); });

        mSwappy = std::make_unique<SwappyCommon>(std::make_unique<FakeSwappyPlatform>(mState),
                                                 mClock);
        EXPECT_TRUE(mSwappy->isValid());
        EXPECT_TRUE(mClock.waitForWorkersSettled());
    }

    ~SwappyTraceHarness() {
        reset();
    }

    void reset() {
        if (mSwappy) {
            mClock.shutdown();
            mSwappy.reset();
        }
    }

    SwappyCommon& swappy() {
        return *mSwappy;
    }

    SimulatedClock& clock() {
        return mClock;
    }

    void triggerPassiveDisplayRateChange(std::chrono::nanoseconds newRefreshPeriod) {
        mTimeline.triggerPassiveDisplayRateChange(newRefreshPeriod);
        EXPECT_TRUE(mClock.waitForWorkersSettled());
    }

    void runFrame(std::chrono::nanoseconds cpuWork, std::chrono::nanoseconds gpuWork,
                  std::chrono::nanoseconds swapCpuCost = 500us) {
        mClock.advanceBy(cpuWork);
        const auto handlers = readySwapHandlers(gpuWork);
        mSwappy->onPreSwap(handlers);
        mClock.advanceBy(swapCpuCost);
        mSwappy->onPostSwap(handlers);
    }

    void runFrames(int frameCount, std::chrono::nanoseconds cpuWork,
                   std::chrono::nanoseconds gpuWork) {
        for (int frame = 0; frame < frameCount; ++frame) {
            runFrame(cpuWork, gpuWork);
        }
    }

private:
    std::shared_ptr<PlatformSharedState> mState;
    SimulatedClock mClock;
    VsyncPanelTimeline mTimeline;
    std::unique_ptr<SwappyCommon> mSwappy;
};

using CounterList = std::vector<std::pair<std::string, int64_t>>;

std::vector<std::string> sliceEventsFrom(size_t startIndex) {
    std::vector<std::string> events = RecordedTraceCallbacks::eventsSnapshot();
    if (startIndex >= events.size()) {
        return {};
    }
    return {events.begin() + static_cast<std::ptrdiff_t>(startIndex), events.end()};
}

CounterList sliceCountersFrom(size_t startIndex) {
    CounterList counters = RecordedTraceCallbacks::countersSnapshot();
    if (startIndex >= counters.size()) {
        return {};
    }
    return {counters.begin() + static_cast<std::ptrdiff_t>(startIndex), counters.end()};
}

std::vector<std::string> extractSettingsEvents(const std::vector<std::string>& events) {
    static constexpr char kPrefix[] = "B:Swappy.Settings ";
    std::vector<std::string> result;
    for (const std::string& event : events) {
        if (event.rfind(kPrefix, 0) == 0) {
            result.push_back(event);
        }
    }
    return result;
}

std::vector<int64_t> filterCounterValues(const CounterList& counters,
                                         const std::string& counterName) {
    std::vector<int64_t> values;
    for (const auto& [name, value] : counters) {
        if (name == counterName) {
            values.push_back(value);
        }
    }
    return values;
}

size_t countBeginEvents(const std::vector<std::string>& events, const std::string& name) {
    const std::string target = "B:" + name;
    return static_cast<size_t>(std::count(events.begin(), events.end(), target));
}

std::vector<std::string>::const_iterator findScopeEnd(
        std::vector<std::string>::const_iterator begin,
        std::vector<std::string>::const_iterator end) {
    int depth = 0;
    for (auto it = begin; it != end; ++it) {
        if (it->rfind("B:", 0) == 0) {
            ++depth;
        } else if (*it == "E") {
            --depth;
            if (depth == 0) {
                return it;
            }
        }
    }
    return end;
}

void expectBaselineCountersSuffix(const CounterList& counters, int64_t autoPipelineMode,
                                  int64_t pipelineMode, int64_t targetPeriodNs,
                                  int64_t autoSwapInterval, int64_t refreshPeriodNs,
                                  int64_t swapDurationNs) {
    ASSERT_GE(counters.size(), 6u);
    const size_t base = counters.size() - 6u;
    EXPECT_EQ(counters[base + 0],
              std::make_pair(std::string("mPipelineModeAutoMode"), autoPipelineMode));
    EXPECT_EQ(counters[base + 1], std::make_pair(std::string("mPipelineMode"), pipelineMode));
    EXPECT_EQ(counters[base + 2],
              std::make_pair(std::string("Swappy.TargetPeriodNs"), targetPeriodNs));
    EXPECT_EQ(counters[base + 3],
              std::make_pair(std::string("mAutoSwapInterval"), autoSwapInterval));
    EXPECT_EQ(counters[base + 4],
              std::make_pair(std::string("mCommonSettings.refreshPeriod"), refreshPeriodNs));
    EXPECT_EQ(counters[base + 5], std::make_pair(std::string("mSwapDuration"), swapDurationNs));
}

void expectBalancedTraceScopes() {
    const auto allEvents = RecordedTraceCallbacks::eventsSnapshot();
    const size_t begins = static_cast<size_t>(
            std::count_if(allEvents.begin(), allEvents.end(),
                          [](const std::string& event) { return event.rfind("B:", 0) == 0; }));
    const size_t ends = static_cast<size_t>(std::count(allEvents.begin(), allEvents.end(), "E"));
    EXPECT_EQ(begins, ends);
}

constexpr char kDefaultSettingsEvent[] =
        "B:Swappy.Settings impl=Classic(drop=10%,pipeOff=50%) swap=16.67ms refresh=16.67ms "
        "autoSI=1 autoPM=1 pipeline=auto maxAuto=50ms pacing=1 block=1 fence=50ms "
        "stuffFix=0 affinity=1";

class SwappyCommonTraceHostTest : public ::testing::Test {
protected:
    void SetUp() override {
        Settings::reset();
    }

    void TearDown() override {
        Settings::reset();
    }
};

TEST_F(SwappyCommonTraceHostTest,
       OnPostSwapEmitsSettingsSnapshotAndBaselineCountersThenDeduplicates) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});

    const auto handlers = readySwapHandlers(4ms);
    harness.swappy().onPreSwap(handlers);

    const size_t eventsBeforeFirstPostSwap = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t countersBeforeFirstPostSwap = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPostSwap(handlers);

    const auto firstEvents = sliceEventsFrom(eventsBeforeFirstPostSwap);
    const auto firstCounters = sliceCountersFrom(countersBeforeFirstPostSwap);
    const auto settings = extractSettingsEvents(firstEvents);
    ASSERT_EQ(settings.size(), 1u);
    EXPECT_EQ(settings[0], kDefaultSettingsEvent);
    expectBaselineCountersSuffix(firstCounters, 1, 1, k60HzPeriod.count(), 1, k60HzPeriod.count(),
                                 k60HzPeriod.count());

    // A second frame with unchanged settings must not re-emit Swappy.Settings or counters.
    harness.clock().advanceBy(2ms);
    harness.swappy().onPreSwap(handlers);
    harness.clock().advanceBy(500us);

    const size_t eventsBeforeSecondPostSwap = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t countersBeforeSecondPostSwap = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPostSwap(handlers);

    EXPECT_TRUE(extractSettingsEvents(sliceEventsFrom(eventsBeforeSecondPostSwap)).empty());
    EXPECT_TRUE(sliceCountersFrom(countersBeforeSecondPostSwap).empty());
}

struct SettingsMutationCase {
    const char* name;
    std::function<void(SwappyCommon&)> mutate;
    const char* expectedSettingsEvent;
    int64_t expectedAutoPipelineMode;
    int64_t expectedTargetPeriodNs;
    int64_t expectedAutoSwapInterval;
    int64_t expectedSwapDurationNs;

    friend void PrintTo(const SettingsMutationCase& mutation, std::ostream* os) {
        *os << mutation.name;
    }
};

class SwappyCommonTraceSettingsMutationTest
      : public SwappyCommonTraceHostTest,
        public ::testing::WithParamInterface<SettingsMutationCase> {};

TEST_P(SwappyCommonTraceSettingsMutationTest, OnPostSwapReEmitsSettingsAfterMutation) {
    const SettingsMutationCase& mutation = GetParam();
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});
    harness.runFrame(2ms, 2ms);

    mutation.mutate(harness.swappy());
    const auto handlers = readySwapHandlers(2ms);
    harness.clock().advanceBy(2ms);
    harness.swappy().onPreSwap(handlers);
    harness.clock().advanceBy(500us);

    const size_t eventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t counterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPostSwap(handlers);

    EXPECT_EQ(extractSettingsEvents(sliceEventsFrom(eventStart)),
              std::vector<std::string>{mutation.expectedSettingsEvent});
    expectBaselineCountersSuffix(sliceCountersFrom(counterStart), mutation.expectedAutoPipelineMode,
                                 1, mutation.expectedTargetPeriodNs,
                                 mutation.expectedAutoSwapInterval, 16'666'667,
                                 mutation.expectedSwapDurationNs);
}

INSTANTIATE_TEST_SUITE_P(
        Mutations, SwappyCommonTraceSettingsMutationTest,
        ::testing::Values(
                SettingsMutationCase{"SwapDuration",
                                     [](SwappyCommon&) {
                                         Settings::getInstance()->setSwapDuration(33'333'334);
                                     },
                                     "B:Swappy.Settings impl=Classic(drop=10%,pipeOff=50%) "
                                     "swap=33.33ms "
                                     "refresh=16.67ms autoSI=1 autoPM=1 pipeline=auto maxAuto=50ms "
                                     "pacing=1 "
                                     "block=1 fence=50ms stuffFix=0 affinity=1",
                                     1, 33'333'334, 2, 33'333'334},
                SettingsMutationCase{"MaxAutoSwapDuration",
                                     [](SwappyCommon& swappy) {
                                         swappy.setMaxAutoSwapDuration(40ms);
                                     },
                                     "B:Swappy.Settings impl=Classic(drop=10%,pipeOff=50%) "
                                     "swap=16.67ms "
                                     "refresh=16.67ms autoSI=1 autoPM=1 pipeline=auto maxAuto=40ms "
                                     "pacing=1 "
                                     "block=1 fence=50ms stuffFix=0 affinity=1",
                                     1, 16'666'667, 1, 16'666'667},
                SettingsMutationCase{"FenceTimeout",
                                     [](SwappyCommon& swappy) { swappy.setFenceTimeout(20ms); },
                                     "B:Swappy.Settings impl=Classic(drop=10%,pipeOff=50%) "
                                     "swap=16.67ms "
                                     "refresh=16.67ms autoSI=1 autoPM=1 pipeline=auto maxAuto=50ms "
                                     "pacing=1 "
                                     "block=1 fence=20ms stuffFix=0 affinity=1",
                                     1, 16'666'667, 1, 16'666'667},
                SettingsMutationCase{"BufferStuffingFixWait",
                                     [](SwappyCommon& swappy) {
                                         swappy.setBufferStuffingFixWait(3);
                                     },
                                     "B:Swappy.Settings impl=Classic(drop=10%,pipeOff=50%) "
                                     "swap=16.67ms "
                                     "refresh=16.67ms autoSI=1 autoPM=1 pipeline=auto maxAuto=50ms "
                                     "pacing=1 "
                                     "block=1 fence=50ms stuffFix=3 affinity=1",
                                     1, 16'666'667, 1, 16'666'667},
                SettingsMutationCase{"BlockingWait",
                                     [](SwappyCommon& swappy) { swappy.enableBlockingWait(false); },
                                     "B:Swappy.Settings impl=Classic(drop=10%,pipeOff=50%) "
                                     "swap=16.67ms "
                                     "refresh=16.67ms autoSI=1 autoPM=1 pipeline=auto maxAuto=50ms "
                                     "pacing=1 "
                                     "block=0 fence=50ms stuffFix=0 affinity=1",
                                     1, 16'666'667, 1, 16'666'667},
                SettingsMutationCase{"AutoSwapInterval",
                                     [](SwappyCommon& swappy) {
                                         swappy.setAutoSwapInterval(false);
                                     },
                                     "B:Swappy.Settings impl=Classic(drop=10%,pipeOff=50%) "
                                     "swap=16.67ms "
                                     "refresh=16.67ms autoSI=0 autoPM=1 pipeline=auto maxAuto=50ms "
                                     "pacing=1 "
                                     "block=1 fence=50ms stuffFix=0 affinity=1",
                                     1, 16'666'667, 1, 16'666'667},
                SettingsMutationCase{"AutoPipelineMode",
                                     [](SwappyCommon& swappy) {
                                         swappy.setAutoPipelineMode(false);
                                     },
                                     "B:Swappy.Settings impl=Classic(drop=10%,pipeOff=50%) "
                                     "swap=16.67ms "
                                     "refresh=16.67ms autoSI=1 autoPM=0 pipeline=on maxAuto=50ms "
                                     "pacing=1 "
                                     "block=1 fence=50ms stuffFix=0 affinity=1",
                                     0, 16'666'667, 1, 16'666'667},
                SettingsMutationCase{"FramePacing",
                                     [](SwappyCommon& swappy) { swappy.enableFramePacing(false); },
                                     "B:Swappy.Settings impl=Classic(drop=10%,pipeOff=50%) "
                                     "swap=16.67ms "
                                     "refresh=16.67ms autoSI=1 autoPM=1 pipeline=auto maxAuto=50ms "
                                     "pacing=0 "
                                     "block=1 fence=50ms stuffFix=0 affinity=1",
                                     1, 16'666'667, 1, 16'666'667}),
        [](const ::testing::TestParamInfo<SettingsMutationCase>& info) {
            return std::string(info.param.name);
        });

TEST_F(SwappyCommonTraceHostTest, OnPostSwapEmitsSettingsWithAffinityDisabled) {
    ScopedMockTrace mockTrace(true);
    Settings::getInstance()->setUseAffinity(false);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});

    harness.runFrame(2ms, 2ms);

    EXPECT_EQ(extractSettingsEvents(RecordedTraceCallbacks::eventsSnapshot()),
              std::vector<std::string>{
                      "B:Swappy.Settings impl=Classic(drop=10%,pipeOff=50%) swap=16.67ms "
                      "refresh=16.67ms autoSI=1 autoPM=1 pipeline=auto maxAuto=50ms pacing=1 "
                      "block=1 fence=50ms stuffFix=0 affinity=0"});
    expectBaselineCountersSuffix(RecordedTraceCallbacks::countersSnapshot(), 1, 1, 16'666'667, 1,
                                 16'666'667, 16'666'667);
}

TEST_F(SwappyCommonTraceHostTest, AutomaticPipelineFlipToOffDoesNotReEmitSettings) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});
    harness.runFrame(2ms, 2ms);
    const size_t eventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t counterStart = RecordedTraceCallbacks::countersSnapshot().size();

    harness.runFrames(150, 2ms, 2ms);

    ASSERT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::Off);
    EXPECT_TRUE(extractSettingsEvents(sliceEventsFrom(eventStart)).empty());
    EXPECT_EQ(filterCounterValues(sliceCountersFrom(counterStart), "mPipelineMode"),
              std::vector<int64_t>{0});
}

TEST_F(SwappyCommonTraceHostTest, OnPostSwapEmitsSettingsAndTargetPeriodFor120HzRefreshPeriod) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k120HzPeriod), Clock::time_point{1s});

    harness.runFrame(2ms, 2ms);

    const auto settings = extractSettingsEvents(RecordedTraceCallbacks::eventsSnapshot());
    ASSERT_EQ(settings.size(), 1u);
    EXPECT_NE(settings[0].find(" swap=16.67ms refresh=8.33ms "), std::string::npos) << settings[0];
    // The target period is two 120Hz periods, which differs from the swap
    // duration by 1ns.
    expectBaselineCountersSuffix(RecordedTraceCallbacks::countersSnapshot(), 1, 1, 16'666'666, 2,
                                 k120HzPeriod.count(), k60HzPeriod.count());
}

TEST_F(SwappyCommonTraceHostTest, OnPostSwapReEmitsSettingsOnReattachAfterDetach) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});

    harness.runFrame(2ms, 2ms);
    ASSERT_EQ(extractSettingsEvents(RecordedTraceCallbacks::eventsSnapshot()).size(), 1u);

    const auto handlers = readySwapHandlers(2ms);

    // Detach Perfetto and run a frame: no Swappy trace events or counters may be emitted, and
    // once onPreSwap joins CPUTracer, onPostSwap produces zero events at all.
    RecordedTraceCallbacks::reset(false);
    harness.clock().advanceBy(2ms);
    harness.swappy().onPreSwap(handlers);
    harness.clock().advanceBy(500us);
    const size_t detachPostSwapEventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    harness.swappy().onPostSwap(handlers);

    const auto detachEvents = RecordedTraceCallbacks::eventsSnapshot();
    EXPECT_TRUE(extractSettingsEvents(detachEvents).empty());
    EXPECT_EQ(countBeginEvents(detachEvents, "Swappy::WaitPhase"), 0u);
    EXPECT_TRUE(sliceEventsFrom(detachPostSwapEventStart).empty());
    EXPECT_TRUE(RecordedTraceCallbacks::countersSnapshot().empty());

    // Re-attach Perfetto with identical settings: Swappy.Settings and 6 counters must re-emit.
    RecordedTraceCallbacks::reset(true);
    harness.clock().advanceBy(2ms);
    harness.swappy().onPreSwap(handlers);
    harness.clock().advanceBy(500us);

    const size_t reattachEventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t reattachCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPostSwap(handlers);

    const auto reattachSettings = extractSettingsEvents(sliceEventsFrom(reattachEventStart));
    ASSERT_EQ(reattachSettings.size(), 1u);
    EXPECT_EQ(reattachSettings[0], kDefaultSettingsEvent);
    expectBaselineCountersSuffix(sliceCountersFrom(reattachCounterStart), 1, 1, k60HzPeriod.count(),
                                 1, k60HzPeriod.count(), k60HzPeriod.count());

    // Subsequent frame while still attached must deduplicate again.
    harness.clock().advanceBy(2ms);
    harness.swappy().onPreSwap(handlers);
    harness.clock().advanceBy(500us);
    const size_t dedupEventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t dedupCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPostSwap(handlers);
    EXPECT_TRUE(extractSettingsEvents(sliceEventsFrom(dedupEventStart)).empty());
    EXPECT_TRUE(sliceCountersFrom(dedupCounterStart).empty());
}

TEST_F(SwappyCommonTraceHostTest, ZeroTraceAndSnapshotWorkWhenTracingDisabled) {
    ScopedMockTrace mockTrace(false);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});

    harness.runFrame(2ms, 2ms);
    harness.swappy().setMaxAutoSwapDuration(40ms);
    harness.swappy().setFenceTimeout(20ms);
    harness.swappy().setBufferStuffingFixWait(2);
    harness.swappy().setAutoPipelineMode(false);
    Settings::getInstance()->setSwapDuration(k30HzPeriod.count());

    int pollCount = 0;
    const SwappyCommon::SwapHandlers slipOnceHandlers{
            .lastFrameIsComplete = [&pollCount] { return ++pollCount >= 2; },
            .getPrevFrameGpuTime = [] { return 6ms; },
    };
    harness.clock().advanceBy(3ms);
    harness.swappy().onPreSwap(slipOnceHandlers);
    harness.clock().advanceBy(500us);
    harness.swappy().onPostSwap(slipOnceHandlers);

    harness.reset();
    EXPECT_TRUE(RecordedTraceCallbacks::eventsSnapshot().empty());
    EXPECT_TRUE(RecordedTraceCallbacks::countersSnapshot().empty());
}

TEST_F(SwappyCommonTraceHostTest, WaitForNextFrameEmitsWaitPhaseAndWorkTimesWithoutFenceWait) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});
    harness.runFrame(2ms, 2ms);

    harness.clock().advanceBy(4500us);
    const size_t eventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t counterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPreSwap(readySwapHandlers(6200us));
    harness.clock().advanceBy(500us);
    harness.swappy().onPostSwap(readySwapHandlers(6200us));

    const auto events = sliceEventsFrom(eventStart);
    EXPECT_EQ(countBeginEvents(events, "Swappy::WaitPhase"), 1u);
    EXPECT_EQ(countBeginEvents(events, "Swappy::GpuFenceWait"), 0u);

    const auto counters = sliceCountersFrom(counterStart);
    EXPECT_EQ(filterCounterValues(counters, "Swappy.CpuTimeUs"), std::vector<int64_t>{4500});
    EXPECT_EQ(filterCounterValues(counters, "Swappy.GpuTimeUs"), std::vector<int64_t>{6200});

    // Verify all trace sections opened across the run closed cleanly after shutdown.
    harness.reset();
    expectBalancedTraceScopes();
}

TEST_F(SwappyCommonTraceHostTest, WaitForNextFrameEmitsGpuFenceWaitWhenBlockingWaitLoops) {
    // Warm up Frame 1 untraced so its async CPUTracer thread cannot emit an out-of-band 'E'
    // inside Frame 2's WaitPhase or GpuFenceWait scope bounds.
    ScopedMockTrace mockTrace(false);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});
    harness.runFrame(2ms, 2ms);
    RecordedTraceCallbacks::reset(true);

    int pollCount = 0;
    const SwappyCommon::SwapHandlers slipTwiceHandlers{
            .lastFrameIsComplete = [&pollCount] { return ++pollCount >= 3; },
            .getPrevFrameGpuTime = [] { return 11500us; },
    };

    harness.clock().advanceBy(3ms);
    const size_t eventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t counterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPreSwap(slipTwiceHandlers);
    harness.clock().advanceBy(500us);
    harness.swappy().onPostSwap(slipTwiceHandlers);

    const auto events = sliceEventsFrom(eventStart);
    EXPECT_EQ(countBeginEvents(events, "Swappy::WaitPhase"), 1u);
    EXPECT_EQ(countBeginEvents(events, "Swappy::GpuFenceWait"), 1u);

    const auto waitPhaseIt = std::find(events.begin(), events.end(), "B:Swappy::WaitPhase");
    ASSERT_NE(waitPhaseIt, events.end());
    const auto waitPhaseEndIt = findScopeEnd(waitPhaseIt, events.end());
    ASSERT_NE(waitPhaseEndIt, events.end());

    const auto fenceWaitIt = std::find(events.begin(), events.end(), "B:Swappy::GpuFenceWait");
    ASSERT_NE(fenceWaitIt, events.end());
    const auto fenceWaitEndIt = findScopeEnd(fenceWaitIt, events.end());
    ASSERT_NE(fenceWaitEndIt, events.end());

    EXPECT_LT(waitPhaseEndIt, fenceWaitIt);

    size_t waitOneFrameCallsInsideFenceWait = 0;
    for (auto it = fenceWaitIt; it != fenceWaitEndIt; ++it) {
        if (it->rfind("B:", 0) == 0 &&
            it->find("SwappyCommon::waitOneFrame()") != std::string::npos) {
            ++waitOneFrameCallsInsideFenceWait;
        }
    }
    EXPECT_EQ(waitOneFrameCallsInsideFenceWait, 2u);

    const auto counters = sliceCountersFrom(counterStart);
    EXPECT_EQ(filterCounterValues(counters, "Swappy.CpuTimeUs"), std::vector<int64_t>{3000});
    EXPECT_EQ(filterCounterValues(counters, "Swappy.GpuTimeUs"), std::vector<int64_t>{11500});

    harness.reset();
    expectBalancedTraceScopes();
}

TEST_F(SwappyCommonTraceHostTest, WaitForNextFrameOmitsGpuTimeWhenZeroOrIncompleteNonBlocking) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});
    harness.runFrame(2ms, 2ms);

    // Zero GPU time omits Swappy.GpuTimeUs while still emitting Swappy.CpuTimeUs.
    harness.clock().advanceBy(3200us);
    const size_t zeroGpuCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPreSwap(readySwapHandlers(0ns));
    const auto zeroGpuCounters = sliceCountersFrom(zeroGpuCounterStart);
    EXPECT_EQ(filterCounterValues(zeroGpuCounters, "Swappy.CpuTimeUs"), std::vector<int64_t>{3200});
    EXPECT_TRUE(filterCounterValues(zeroGpuCounters, "Swappy.GpuTimeUs").empty());
    harness.clock().advanceBy(500us);
    harness.swappy().onPostSwap(readySwapHandlers(0ns));

    // Disable frame pacing and blocking wait: skips WaitPhase and GpuFenceWait, and an
    // incomplete last frame yields gpuTime = -1ns so Swappy.GpuTimeUs is omitted.
    harness.swappy().enableFramePacing(false);
    harness.swappy().enableBlockingWait(false);
    harness.runFrame(2ms, 2ms);

    const SwappyCommon::SwapHandlers incompleteHandlers{
            .lastFrameIsComplete = [] { return false; },
            .getPrevFrameGpuTime = [] { return 9ms; },
    };
    harness.clock().advanceBy(2700us);
    const size_t noPaceEventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t noPaceCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPreSwap(incompleteHandlers);

    const auto noPaceEvents = sliceEventsFrom(noPaceEventStart);
    EXPECT_EQ(countBeginEvents(noPaceEvents, "Swappy::WaitPhase"), 0u);
    EXPECT_EQ(countBeginEvents(noPaceEvents, "Swappy::GpuFenceWait"), 0u);

    const auto noPaceCounters = sliceCountersFrom(noPaceCounterStart);
    EXPECT_EQ(filterCounterValues(noPaceCounters, "Swappy.CpuTimeUs"), std::vector<int64_t>{2700});
    EXPECT_TRUE(filterCounterValues(noPaceCounters, "Swappy.GpuTimeUs").empty());
    EXPECT_TRUE(filterCounterValues(noPaceCounters, "Swappy.WakeErrorUs").empty());

    harness.clock().advanceBy(500us);
    harness.swappy().onPostSwap(incompleteHandlers);
}

TEST_F(SwappyCommonTraceHostTest, WaitUntilEmitsWakeErrorOnlyWhenWaitingUsingLatchedDeadline) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});

    // Frame 1: mCurrentFrame == mTargetFrame == 0, so waitUntil does not wait and skips WakeError.
    const size_t initialCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPreSwap(readySwapHandlers(2ms));
    EXPECT_TRUE(filterCounterValues(sliceCountersFrom(initialCounterStart), "Swappy.WakeErrorUs")
                        .empty());
    // Frame 1's swap cost produces mMeasuredSwapDuration > 0, which shifts Frame 3's
    // wakeDeadline in ChoreographerFilter. Frame 2 itself avoids colliding with vsyncCandidate
    // (preserving SimulatedClock's 350us wake delay) because initialTime=1s starts ~1ms ahead of
    // the appVsyncOffset-aligned vsync, leaving >350us margin.
    harness.clock().advanceBy(1ms);
    harness.swappy().onPostSwap(readySwapHandlers(2ms));

    // Frame 2: mCurrentFrame < mTargetFrame with 350us simulated wake latency past wakeDeadline.
    harness.clock().setSleepWakeDelay(350us);
    harness.clock().advanceBy(2ms);
    const size_t waitedCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPreSwap(readySwapHandlers(2ms));
    EXPECT_EQ(filterCounterValues(sliceCountersFrom(waitedCounterStart), "Swappy.WakeErrorUs"),
              std::vector<int64_t>{350});
    harness.clock().advanceBy(500us);
    harness.swappy().onPostSwap(readySwapHandlers(2ms));

    // Frame 3: advance past the next VSync before onPreSwap so mCurrentFrame >= mTargetFrame > 0.
    // WaitPhase runs, but waitUntil skips Swappy.WakeErrorUs because it did not need to wait.
    harness.clock().setSleepWakeDelay(0ns);
    harness.clock().advanceBy(20ms);
    const size_t noWaitEventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t noWaitCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPreSwap(readySwapHandlers(2ms));
    EXPECT_EQ(countBeginEvents(sliceEventsFrom(noWaitEventStart), "Swappy::WaitPhase"), 1u);
    EXPECT_TRUE(filterCounterValues(sliceCountersFrom(noWaitCounterStart), "Swappy.WakeErrorUs")
                        .empty());
    harness.clock().advanceBy(500us);
    harness.swappy().onPostSwap(readySwapHandlers(2ms));
}

TEST_F(SwappyCommonTraceHostTest, WaitUntilComparesWakeTimeWithDeadlineOfTargetTick) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});
    harness.swappy().setAutoSwapInterval(false);
    Settings::getInstance()->setSwapDuration((3 * k60HzPeriod).count());
    harness.runFrame(2ms, 2ms);
    harness.runFrame(2ms, 2ms);
    RecordedTraceCallbacks::reset(true);

    harness.runFrame(2ms, 2ms);
    EXPECT_EQ(RecordedTraceCallbacks::counterValues("Swappy.WakeErrorUs"), std::vector<int64_t>{0});

    // The target tick passed during the CPU work, so the frame does not wait.
    RecordedTraceCallbacks::reset(true);
    harness.runFrame(4 * k60HzPeriod, 2ms);
    EXPECT_TRUE(RecordedTraceCallbacks::counterValues("Swappy.WakeErrorUs").empty());
}

TEST_F(SwappyCommonTraceHostTest, UpdateSwapIntervalEmitsTargetPeriodNsOnStepDownAndStepUp) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});
    harness.runFrame(2ms, 2ms);
    ASSERT_EQ(harness.swappy().getAutoSwapInterval(), 1);

    // Step down to 30Hz (autoSwapInterval == 2).
    const size_t stepDownEventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t stepDownCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    for (int frame = 0; frame < 200 && harness.swappy().getAutoSwapInterval() == 1; ++frame) {
        harness.runFrame(20ms, 6ms);
    }
    ASSERT_EQ(harness.swappy().getAutoSwapInterval(), 2);

    const auto stepDownCounters = sliceCountersFrom(stepDownCounterStart);
    EXPECT_EQ(filterCounterValues(stepDownCounters, "Swappy.TargetPeriodNs"),
              std::vector<int64_t>{(2 * k60HzPeriod).count()});
    EXPECT_EQ(filterCounterValues(stepDownCounters, "mAutoSwapInterval"), std::vector<int64_t>{2});
    EXPECT_TRUE(extractSettingsEvents(sliceEventsFrom(stepDownEventStart)).empty());

    // Step back up to 60Hz (autoSwapInterval == 1).
    const size_t stepUpEventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t stepUpCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    for (int frame = 0; frame < 200 && harness.swappy().getAutoSwapInterval() == 2; ++frame) {
        harness.runFrame(3ms, 3ms);
    }
    ASSERT_EQ(harness.swappy().getAutoSwapInterval(), 1);

    const auto stepUpCounters = sliceCountersFrom(stepUpCounterStart);
    EXPECT_EQ(filterCounterValues(stepUpCounters, "Swappy.TargetPeriodNs"),
              std::vector<int64_t>{k60HzPeriod.count()});
    EXPECT_EQ(filterCounterValues(stepUpCounters, "mAutoSwapInterval"), std::vector<int64_t>{1});
    EXPECT_TRUE(extractSettingsEvents(sliceEventsFrom(stepUpEventStart)).empty());
}

TEST_F(SwappyCommonTraceHostTest, UpdateSwapIntervalEmitsPipelineModeWithoutConfigChanged) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});

    for (int frame = 0;
         frame < 200 && harness.swappy().getCurrentPipelineMode() == PipelineMode::On; ++frame) {
        harness.runFrame(2ms, 2ms);
    }
    ASSERT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::Off);
    ASSERT_EQ(harness.swappy().getAutoSwapInterval(), 1);

    int swapIntervalChangedCount = 0;
    SwappyTracer tracer{};
    tracer.userData = &swapIntervalChangedCount;
    tracer.swapIntervalChanged = [](void* data) { ++(*static_cast<int*>(data)); };
    harness.swappy().addTracerCallbacks(tracer);

    // Light frames that still fit inside 1 refresh period pipelined, but miss deadlines due to
    // a 1-frame GPU fence slip, restore PipelineMode::On with configChanged == false.
    size_t restoreEventStart = 0;
    size_t restoreCounterStart = 0;
    for (int frame = 0; frame < 200; ++frame) {
        harness.clock().advanceBy(2ms);
        bool fenceChecked = false;
        const SwappyCommon::SwapHandlers slipOnceHandlers{
                .lastFrameIsComplete =
                        [&fenceChecked] {
                            if (!fenceChecked) {
                                fenceChecked = true;
                                return false;
                            }
                            return true;
                        },
                .getPrevFrameGpuTime = [] { return 2ms; },
        };
        harness.swappy().onPreSwap(slipOnceHandlers);
        harness.clock().advanceBy(500us);

        restoreEventStart = RecordedTraceCallbacks::eventsSnapshot().size();
        restoreCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
        harness.swappy().onPostSwap(slipOnceHandlers);
        if (harness.swappy().getCurrentPipelineMode() == PipelineMode::On) {
            break;
        }
    }

    ASSERT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::On);
    EXPECT_EQ(harness.swappy().getAutoSwapInterval(), 1);
    EXPECT_EQ(swapIntervalChangedCount, 0);

    const auto restoreCounters = sliceCountersFrom(restoreCounterStart);
    EXPECT_EQ(filterCounterValues(restoreCounters, "mPipelineMode"),
              std::vector<int64_t>{static_cast<int64_t>(PipelineMode::On)});
    EXPECT_TRUE(filterCounterValues(restoreCounters, "mAutoSwapInterval").empty());
    EXPECT_TRUE(filterCounterValues(restoreCounters, "Swappy.TargetPeriodNs").empty());
    EXPECT_TRUE(extractSettingsEvents(sliceEventsFrom(restoreEventStart)).empty());
}

TEST_F(SwappyCommonTraceHostTest, UpdateDisplayTimingsEmitsTargetPeriodNsOnWindowAndTimingChanges) {
    ScopedMockTrace mockTrace(true);
    ANativeWindow window{7};
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});
    harness.runFrame(2ms, 2ms);

    // Setting a window triggers updateDisplayTimings() without altering SettingsKey.
    harness.swappy().setANativeWindow(&window);
    harness.clock().advanceBy(2ms);
    harness.swappy().onPreSwap(readySwapHandlers(2ms));
    harness.clock().advanceBy(500us);

    const size_t windowEventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t windowCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPostSwap(readySwapHandlers(2ms));

    EXPECT_TRUE(extractSettingsEvents(sliceEventsFrom(windowEventStart)).empty());
    EXPECT_EQ(filterCounterValues(sliceCountersFrom(windowCounterStart), "Swappy.TargetPeriodNs"),
              std::vector<int64_t>{k60HzPeriod.count()});

    // Switching display refresh period to 120Hz while keeping 60fps swap duration yields
    // autoSwapInterval = 2 and TargetPeriodNs = 2 * k120HzPeriod.
    harness.triggerPassiveDisplayRateChange(k120HzPeriod);
    harness.clock().advanceBy(2ms);
    harness.swappy().onPreSwap(readySwapHandlers(2ms));
    harness.clock().advanceBy(500us);

    const size_t timingEventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t timingCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPostSwap(readySwapHandlers(2ms));

    EXPECT_EQ(harness.swappy().getAutoSwapInterval(), 2);
    const auto timingSettings = extractSettingsEvents(sliceEventsFrom(timingEventStart));
    ASSERT_EQ(timingSettings.size(), 1u);
    EXPECT_NE(timingSettings[0].find(" refresh=8.33ms "), std::string::npos) << timingSettings[0];

    const auto timingCounters = sliceCountersFrom(timingCounterStart);
    EXPECT_EQ(filterCounterValues(timingCounters, "Swappy.TargetPeriodNs"),
              (std::vector<int64_t>{(2 * k120HzPeriod).count(), (2 * k120HzPeriod).count()}));
    EXPECT_EQ(filterCounterValues(timingCounters, "mAutoSwapInterval"),
              (std::vector<int64_t>{2, 2}));
    EXPECT_EQ(filterCounterValues(timingCounters, "mCommonSettings.refreshPeriod"),
              (std::vector<int64_t>{k120HzPeriod.count(), k120HzPeriod.count()}));
}

TEST_F(SwappyCommonTraceHostTest, ResetFramePacingEmitsTargetPeriodNsAndRestoresBaseline) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});
    harness.runFrame(2ms, 2ms);

    for (int frame = 0; frame < 200 && harness.swappy().getAutoSwapInterval() == 1; ++frame) {
        harness.runFrame(20ms, 6ms);
    }
    ASSERT_EQ(harness.swappy().getAutoSwapInterval(), 2);

    harness.swappy().resetFramePacing();
    harness.clock().advanceBy(2ms);
    harness.swappy().onPreSwap(readySwapHandlers(2ms));
    harness.clock().advanceBy(500us);

    const size_t resetEventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    const size_t resetCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPostSwap(readySwapHandlers(2ms));

    EXPECT_EQ(harness.swappy().getAutoSwapInterval(), 1);
    EXPECT_EQ(harness.swappy().getSwapDuration(), k60HzPeriod);

    // Reset preserves the configured swapDuration floor, so the Swappy.Settings
    // snapshot key is unchanged and only the reset transition counters are emitted.
    const auto resetSettings = extractSettingsEvents(sliceEventsFrom(resetEventStart));
    EXPECT_TRUE(resetSettings.empty());

    const auto resetCounters = sliceCountersFrom(resetCounterStart);
    ASSERT_EQ(resetCounters.size(), 3u);
    EXPECT_EQ(resetCounters[0],
              std::make_pair(std::string("Swappy.TargetPeriodNs"), k60HzPeriod.count()));
    EXPECT_EQ(resetCounters[1], std::make_pair(std::string("mPipelineMode"), int64_t{1}));
    EXPECT_EQ(resetCounters[2], std::make_pair(std::string("mAutoSwapInterval"), int64_t{1}));
}

} // namespace

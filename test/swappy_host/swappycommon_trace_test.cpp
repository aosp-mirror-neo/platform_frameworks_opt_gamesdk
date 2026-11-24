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
#include "Trace.h"

#if !defined(AGDK_TRACE) || AGDK_TRACE == 0
#error "swappycommon_trace_test.cpp must be compiled with AGDK_TRACE != 0"
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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
constexpr auto k120HzPeriod = 8'333'333ns;

// Suppresses TRACE_ENABLED() inside SwappyCommon::startFrame() so CPUTracer
// does not spawn an unclocked worker thread under SimulatedClock.
class ScopedMockTrace {
public:
    explicit ScopedMockTrace(bool enabled) {
        sStartFrameDepth = 0;
        sScopeIsStartFrame.clear();
        RecordedTraceCallbacks::reset(enabled);
        mPrevious = gamesdk::TraceTestPeer::swapInstance(
                std::make_unique<gamesdk::Trace>(&beginSection, &endSection, &isEnabled,
                                                 &RecordedTraceCallbacks::beginAsyncSection,
                                                 &RecordedTraceCallbacks::endAsyncSection,
                                                 &RecordedTraceCallbacks::setCounter));
    }

    ~ScopedMockTrace() {
        gamesdk::TraceTestPeer::swapInstance(std::move(mPrevious));
        RecordedTraceCallbacks::reset(false);
        sStartFrameDepth = 0;
        sScopeIsStartFrame.clear();
    }

private:
    inline static thread_local int sStartFrameDepth = 0;
    inline static thread_local std::vector<bool> sScopeIsStartFrame;

    static void beginSection(const char* name) {
        const bool isStartFrame =
                std::string_view(name).find("SwappyCommon::startFrame") != std::string_view::npos;
        sScopeIsStartFrame.push_back(isStartFrame);
        if (isStartFrame) {
            ++sStartFrameDepth;
        }
        RecordedTraceCallbacks::beginSection(name);
    }

    static void endSection() {
        if (!sScopeIsStartFrame.empty()) {
            if (sScopeIsStartFrame.back()) {
                --sStartFrameDepth;
            }
            sScopeIsStartFrame.pop_back();
        }
        RecordedTraceCallbacks::endSection();
    }

    static bool isEnabled() {
        return sStartFrameDepth == 0 && RecordedTraceCallbacks::isEnabled();
    }

    std::unique_ptr<gamesdk::Trace> mPrevious;
};

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

void expectBalancedTraceScopes() {
    const auto allEvents = RecordedTraceCallbacks::eventsSnapshot();
    const size_t begins = static_cast<size_t>(
            std::count_if(allEvents.begin(), allEvents.end(),
                          [](const std::string& event) { return event.rfind("B:", 0) == 0; }));
    const size_t ends = static_cast<size_t>(std::count(allEvents.begin(), allEvents.end(), "E"));
    EXPECT_EQ(begins, ends);
}

int64_t currentAutoSwapInterval(const SwappyCommon& swappy) {
    return swappy.getSwapDuration() / swappy.getRefreshPeriod();
}

class SwappyCommonTraceHostTest : public ::testing::Test {
protected:
    void SetUp() override {
        Settings::reset();
    }

    void TearDown() override {
        Settings::reset();
    }
};

TEST_F(SwappyCommonTraceHostTest, WaitForNextFrameEmitsWaitPhaseWithoutFenceWait) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});
    harness.runFrame(2ms, 2ms);

    harness.clock().advanceBy(4500us);
    const size_t eventStart = RecordedTraceCallbacks::eventsSnapshot().size();
    harness.swappy().onPreSwap(readySwapHandlers(6200us));
    harness.clock().advanceBy(500us);
    harness.swappy().onPostSwap(readySwapHandlers(6200us));

    const auto events = sliceEventsFrom(eventStart);
    EXPECT_EQ(countBeginEvents(events, "Swappy::WaitPhase"), 1u);
    EXPECT_EQ(countBeginEvents(events, "Swappy::GpuFenceWait"), 0u);

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

    harness.reset();
    expectBalancedTraceScopes();
}

TEST_F(SwappyCommonTraceHostTest, UpdateSwapIntervalEmitsTargetPeriodNsOnStepDownAndStepUp) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});
    harness.runFrame(2ms, 2ms);
    ASSERT_EQ(currentAutoSwapInterval(harness.swappy()), 1);

    // Step down to 30Hz (autoSwapInterval == 2).
    const size_t stepDownCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    for (int frame = 0; frame < 200 && currentAutoSwapInterval(harness.swappy()) == 1; ++frame) {
        harness.runFrame(20ms, 6ms);
    }
    ASSERT_EQ(currentAutoSwapInterval(harness.swappy()), 2);

    const auto stepDownCounters = sliceCountersFrom(stepDownCounterStart);
    EXPECT_EQ(filterCounterValues(stepDownCounters, "Swappy.TargetPeriodNs"),
              std::vector<int64_t>{(2 * k60HzPeriod).count()});
    EXPECT_EQ(filterCounterValues(stepDownCounters, "mAutoSwapInterval"), std::vector<int64_t>{2});

    // Step back up to 60Hz (autoSwapInterval == 1).
    const size_t stepUpCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    for (int frame = 0; frame < 200 && currentAutoSwapInterval(harness.swappy()) == 2; ++frame) {
        harness.runFrame(3ms, 3ms);
    }
    ASSERT_EQ(currentAutoSwapInterval(harness.swappy()), 1);

    const auto stepUpCounters = sliceCountersFrom(stepUpCounterStart);
    EXPECT_EQ(filterCounterValues(stepUpCounters, "Swappy.TargetPeriodNs"),
              std::vector<int64_t>{k60HzPeriod.count()});
    EXPECT_EQ(filterCounterValues(stepUpCounters, "mAutoSwapInterval"), std::vector<int64_t>{1});
}

TEST_F(SwappyCommonTraceHostTest, UpdateSwapIntervalEmitsPipelineModeWithoutConfigChanged) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});

    for (int frame = 0;
         frame < 200 && harness.swappy().getCurrentPipelineMode() == PipelineMode::On; ++frame) {
        harness.runFrame(2ms, 2ms);
    }
    ASSERT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::Off);
    ASSERT_EQ(currentAutoSwapInterval(harness.swappy()), 1);

    int swapIntervalChangedCount = 0;
    SwappyTracer tracer{};
    tracer.userData = &swapIntervalChangedCount;
    tracer.swapIntervalChanged = [](void* data) { ++(*static_cast<int*>(data)); };
    harness.swappy().addTracerCallbacks(tracer);

    // Light frames that still fit inside 1 refresh period pipelined, but miss deadlines due to
    // a 1-frame GPU fence slip, restore PipelineMode::On with configChanged == false.
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

        restoreCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
        harness.swappy().onPostSwap(slipOnceHandlers);
        if (harness.swappy().getCurrentPipelineMode() == PipelineMode::On) {
            break;
        }
    }

    ASSERT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::On);
    EXPECT_EQ(currentAutoSwapInterval(harness.swappy()), 1);
    EXPECT_EQ(swapIntervalChangedCount, 0);

    const auto restoreCounters = sliceCountersFrom(restoreCounterStart);
    EXPECT_EQ(filterCounterValues(restoreCounters, "mPipelineMode"),
              std::vector<int64_t>{static_cast<int64_t>(PipelineMode::On)});
    EXPECT_TRUE(filterCounterValues(restoreCounters, "mAutoSwapInterval").empty());
    EXPECT_TRUE(filterCounterValues(restoreCounters, "Swappy.TargetPeriodNs").empty());
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

    const size_t windowCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPostSwap(readySwapHandlers(2ms));

    EXPECT_EQ(filterCounterValues(sliceCountersFrom(windowCounterStart), "Swappy.TargetPeriodNs"),
              std::vector<int64_t>{k60HzPeriod.count()});

    // Switching display refresh period to 120Hz while keeping 60fps swap duration yields
    // autoSwapInterval = 2 and TargetPeriodNs = 2 * k120HzPeriod.
    harness.triggerPassiveDisplayRateChange(k120HzPeriod);
    harness.clock().advanceBy(2ms);
    harness.swappy().onPreSwap(readySwapHandlers(2ms));
    harness.clock().advanceBy(500us);

    const size_t timingCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPostSwap(readySwapHandlers(2ms));

    EXPECT_EQ(currentAutoSwapInterval(harness.swappy()), 2);

    const auto timingCounters = sliceCountersFrom(timingCounterStart);
    EXPECT_EQ(filterCounterValues(timingCounters, "Swappy.TargetPeriodNs"),
              std::vector<int64_t>{(2 * k120HzPeriod).count()});
    EXPECT_EQ(filterCounterValues(timingCounters, "mAutoSwapInterval"), std::vector<int64_t>{2});
    EXPECT_EQ(filterCounterValues(timingCounters, "mCommonSettings.refreshPeriod"),
              std::vector<int64_t>{k120HzPeriod.count()});
}

TEST_F(SwappyCommonTraceHostTest, ResetFramePacingEmitsTargetPeriodNsAndRestoresBaseline) {
    ScopedMockTrace mockTrace(true);
    SwappyTraceHarness harness(makeTestSwappySettings(k60HzPeriod), Clock::time_point{1s});
    harness.runFrame(2ms, 2ms);

    for (int frame = 0; frame < 200 && currentAutoSwapInterval(harness.swappy()) == 1; ++frame) {
        harness.runFrame(20ms, 6ms);
    }
    ASSERT_EQ(currentAutoSwapInterval(harness.swappy()), 2);

    harness.swappy().resetFramePacing();
    harness.clock().advanceBy(2ms);
    harness.swappy().onPreSwap(readySwapHandlers(2ms));
    harness.clock().advanceBy(500us);

    const size_t resetCounterStart = RecordedTraceCallbacks::countersSnapshot().size();
    harness.swappy().onPostSwap(readySwapHandlers(2ms));

    EXPECT_EQ(currentAutoSwapInterval(harness.swappy()), 1);
    EXPECT_EQ(harness.swappy().getSwapDuration(), k60HzPeriod);

    const auto resetCounters = sliceCountersFrom(resetCounterStart);
    ASSERT_EQ(resetCounters.size(), 3u);
    EXPECT_EQ(resetCounters[0],
              std::make_pair(std::string("Swappy.TargetPeriodNs"), k60HzPeriod.count()));
    EXPECT_EQ(resetCounters[1], std::make_pair(std::string("mPipelineMode"), int64_t{1}));
    EXPECT_EQ(resetCounters[2], std::make_pair(std::string("mAutoSwapInterval"), int64_t{1}));
}

} // namespace

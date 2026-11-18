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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "Clock.h"
#include "FakeSwappyPlatform.h"
#include "RefreshPeriodSet.h"
#include "Settings.h"
#include "SimulatedClock.h"
#include "SimulatedDisplayPanel.h"
#include "SimulatedGpuQueue.h"
#include "SwappyPlatform.h"
#include "gtest/gtest.h"
#include "swappy/swappy_common.h"

namespace {

using namespace std::chrono_literals;
using swappy::Clock;
using swappy::insertRefreshPeriod;
using swappy::PipelineMode;
using swappy::RefreshPeriodMap;
using swappy::Settings;
using swappy::SwappyCommon;
using swappy::SwappyCommonSettings;
using swappy::SwappyPlatform;
using swappy::testing::DisplayPanelModel;
using swappy::testing::FakeSwappyPlatform;
using swappy::testing::makeTestSwappySettings;
using swappy::testing::PlatformSharedState;
using swappy::testing::PlatformSnapshot;
using swappy::testing::SimulatedClock;
using swappy::testing::SimulatedDisplayPanel;
using swappy::testing::SimulatedGpuQueue;
using swappy::testing::snapshotPlatformState;
using swappy::testing::VsyncPanelTimeline;

constexpr auto k60HzPeriod = 16'666'667ns;
constexpr auto k30HzPeriod = 33'333'334ns;
constexpr auto k90HzPeriod = 11'111'111ns;
constexpr auto k120HzPeriod = 8'333'333ns;
constexpr auto kFastTickPeriod = 1ms;

SwappyCommonSettings makeSettings(std::chrono::nanoseconds refreshPeriod = k60HzPeriod,
                                  int sdkInt = 33) {
    return makeTestSwappySettings(refreshPeriod, sdkInt);
}

SwappyCommon::SwapHandlers readySwapHandlers(std::chrono::nanoseconds gpuTime = 4ms) {
    return SwappyCommon::SwapHandlers{
            .lastFrameIsComplete = [] { return true; },
            .getPrevFrameGpuTime = [gpuTime] { return gpuTime; },
    };
}

[[nodiscard]] bool triggerChoreographer(
        PlatformSharedState& state,
        std::optional<std::chrono::nanoseconds> sfToVsyncDelay = std::nullopt) {
    SwappyPlatform::ChoreographerCallback callback;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        callback = state.onChoreographer;
    }
    if (!callback) {
        return false;
    }
    callback(sfToVsyncDelay);
    return true;
}

[[nodiscard]] bool runPacedSwaps(SwappyCommon& swappy, PlatformSharedState& state, int swapCount,
                                 const SwappyCommon::SwapHandlers& handlers) {
    for (int i = 0; i < swapCount; ++i) {
        swappy.onPreSwap(handlers);
        swappy.onPostSwap(handlers);
        if (!triggerChoreographer(state)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool exerciseConcurrentPacingReaders(SwappyCommon& swappy,
                                                   const SwappyCommon& constSwappy, int swapCount) {
    std::atomic<bool> readerReady{false};
    std::atomic<bool> startSignal{false};
    std::atomic<bool> stopSignal{false};
    std::atomic<bool> observedInvalidState{false};

    std::thread reader([&constSwappy, &readerReady, &startSignal, &stopSignal,
                        &observedInvalidState] {
        readerReady.store(true, std::memory_order_release);
        while (!startSignal.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        do {
            const auto refreshPeriod = constSwappy.getRefreshPeriod();
            const auto swapDuration = constSwappy.getSwapDuration();
            const auto pipelineMode = constSwappy.getCurrentPipelineMode();
            if ((refreshPeriod != kFastTickPeriod && refreshPeriod != 2 * kFastTickPeriod) ||
                swapDuration < kFastTickPeriod ||
                (pipelineMode != PipelineMode::On && pipelineMode != PipelineMode::Off)) {
                observedInvalidState.store(true, std::memory_order_relaxed);
            }
        } while (!stopSignal.load(std::memory_order_acquire));
    });

    const auto handlers = readySwapHandlers(200us);
    while (!readerReady.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    startSignal.store(true, std::memory_order_release);
    for (int i = 0; i < swapCount; ++i) {
        const auto nextPeriod = (i % 2 == 0) ? (2 * kFastTickPeriod) : kFastTickPeriod;
        Settings::getInstance()->setDisplayTimings({nextPeriod, 0ns, 0ns});
        swappy.setAutoSwapInterval(false);
        swappy.setAutoSwapInterval(true);
        swappy.setAutoPipelineMode((i % 2) == 0);
        swappy.onPreSwap(handlers);
        swappy.onPostSwap(handlers);
    }
    stopSignal.store(true, std::memory_order_release);
    reader.join();

    return !observedInvalidState.load(std::memory_order_relaxed);
}

[[nodiscard]] bool exerciseConcurrentSupportedRefreshPeriodQueries(SwappyCommon& swappy,
                                                                   int swapCount,
                                                                   int expectedPeriodCount) {
    std::atomic<bool> readerReady{false};
    std::atomic<bool> startSignal{false};
    std::atomic<bool> stopSignal{false};
    std::atomic<bool> observedUnexpectedCount{false};

    std::thread reader([&swappy, &readerReady, &startSignal, &stopSignal, &observedUnexpectedCount,
                        expectedPeriodCount] {
        readerReady.store(true, std::memory_order_release);
        while (!startSignal.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        do {
            if (swappy.getSupportedRefreshPeriodsNS(nullptr, 0) != expectedPeriodCount) {
                observedUnexpectedCount.store(true, std::memory_order_relaxed);
            }
        } while (!stopSignal.load(std::memory_order_acquire));
    });

    const auto handlers = readySwapHandlers(200us);
    while (!readerReady.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    startSignal.store(true, std::memory_order_release);
    for (int i = 0; i < swapCount; ++i) {
        swappy.onPreSwap(handlers);
        swappy.onPostSwap(handlers);
    }
    stopSignal.store(true, std::memory_order_release);
    reader.join();

    return !observedUnexpectedCount.load(std::memory_order_relaxed);
}

void runConcurrentSettingsUpdateRound(SwappyCommon& swappy,
                                      const SwappyCommon::SwapHandlers& handlers) {
    Settings::getInstance()->setDisplayTimings({2 * kFastTickPeriod, 0ns, 0ns});
    Settings::getInstance()->setSwapDuration(std::chrono::nanoseconds(2 * kFastTickPeriod).count());
    swappy.onPreSwap(handlers);
    swappy.onPostSwap(handlers);

    std::atomic<bool> writerReady{false};
    std::atomic<bool> startSignal{false};
    std::thread displayWriter([&writerReady, &startSignal] {
        writerReady.store(true, std::memory_order_release);
        while (!startSignal.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        Settings::getInstance()->setDisplayTimings({kFastTickPeriod, 0ns, 0ns});
    });

    while (!writerReady.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    startSignal.store(true, std::memory_order_release);
    Settings::getInstance()->setSwapDuration(std::chrono::nanoseconds(4 * kFastTickPeriod).count());
    displayWriter.join();

    swappy.onPreSwap(handlers);
    swappy.onPostSwap(handlers);
}

void verifyConcurrentSettingsUpdates(SwappyCommon& swappy, int rounds) {
    const auto handlers = readySwapHandlers(200us);
    for (int round = 0; round < rounds; ++round) {
        SCOPED_TRACE(round);
        runConcurrentSettingsUpdateRound(swappy, handlers);
        ASSERT_EQ(swappy.getRefreshPeriod(), kFastTickPeriod);
        ASSERT_EQ(swappy.getSwapDuration(), 4 * kFastTickPeriod);
    }
}

class SwappyCommonHostTest : public ::testing::Test {
protected:
    void SetUp() override {
        Settings::reset();
    }

    void TearDown() override {
        Settings::reset();
    }
};

TEST_F(SwappyCommonHostTest, NullPlatformMarksSwappyCommonInvalid) {
    SwappyCommon swappy(std::unique_ptr<SwappyPlatform>{});
    EXPECT_FALSE(swappy.isValid());
}

TEST_F(SwappyCommonHostTest, InvalidPlatformMarksSwappyCommonInvalid) {
    auto state = std::make_shared<PlatformSharedState>();
    state->valid = false;
    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    EXPECT_FALSE(swappy.isValid());
}

TEST_F(SwappyCommonHostTest, UninitializedChoreographerMarksSwappyCommonInvalid) {
    auto state = std::make_shared<PlatformSharedState>();
    state->choreographerInitialized = false;
    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    EXPECT_FALSE(swappy.isValid());
}

TEST_F(SwappyCommonHostTest, UninitializedDisplayManagerMarksSwappyCommonInvalid) {
    auto state = std::make_shared<PlatformSharedState>();
    state->displayManagerInitialized = false;
    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    EXPECT_FALSE(swappy.isValid());
}

TEST_F(SwappyCommonHostTest, OnChoreographerIsNoOpAfterPlatformInitFailure) {
    auto state = std::make_shared<PlatformSharedState>();
    state->valid = false;
    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    ASSERT_FALSE(swappy.isValid());

    swappy.onChoreographer(0);

    const PlatformSnapshot snapshot = snapshotPlatformState(*state);
    EXPECT_EQ(snapshot.switchToExternalChoreographerCount, 0);
    EXPECT_EQ(snapshot.postFrameCallbacksCount, 0);
}

TEST_F(SwappyCommonHostTest, WindowLifecycleAcquiresReleasesAndDeduplicatesSameWindow) {
    auto state = std::make_shared<PlatformSharedState>();
    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    ASSERT_TRUE(swappy.isValid());

    ANativeWindow windowA{1};
    ANativeWindow windowB{2};

    swappy.setANativeWindow(&windowA);
    swappy.setANativeWindow(&windowA);
    PlatformSnapshot snapshot = snapshotPlatformState(*state);
    ASSERT_EQ(snapshot.acquiredWindows.size(), 1u);
    EXPECT_EQ(snapshot.acquiredWindows[0], &windowA);
    EXPECT_TRUE(snapshot.releasedWindows.empty());

    swappy.setANativeWindow(&windowB);
    snapshot = snapshotPlatformState(*state);
    ASSERT_EQ(snapshot.acquiredWindows.size(), 2u);
    EXPECT_EQ(snapshot.acquiredWindows[1], &windowB);
    ASSERT_EQ(snapshot.releasedWindows.size(), 1u);
    EXPECT_EQ(snapshot.releasedWindows[0], &windowA);

    swappy.setANativeWindow(nullptr);
    snapshot = snapshotPlatformState(*state);
    ASSERT_EQ(snapshot.releasedWindows.size(), 2u);
    EXPECT_EQ(snapshot.releasedWindows[1], &windowB);
}

TEST_F(SwappyCommonHostTest, DestructorReleasesRetainedWindowBeforePlatformTeardown) {
    auto state = std::make_shared<PlatformSharedState>();
    ANativeWindow window{11};

    {
        SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
        ASSERT_TRUE(swappy.isValid());
        swappy.setANativeWindow(&window);
    }

    const PlatformSnapshot snapshot = snapshotPlatformState(*state);
    ASSERT_EQ(snapshot.acquiredWindows.size(), 1u);
    EXPECT_EQ(snapshot.acquiredWindows[0], &window);
    ASSERT_EQ(snapshot.releasedWindows.size(), 1u);
    EXPECT_EQ(snapshot.releasedWindows[0], &window);
}

TEST_F(SwappyCommonHostTest, DestructorDoesNotDoubleReleaseClearedWindow) {
    auto state = std::make_shared<PlatformSharedState>();
    ANativeWindow window{12};

    {
        SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
        ASSERT_TRUE(swappy.isValid());
        swappy.setANativeWindow(&window);
        swappy.setANativeWindow(nullptr);
    }

    const PlatformSnapshot snapshot = snapshotPlatformState(*state);
    ASSERT_EQ(snapshot.acquiredWindows.size(), 1u);
    ASSERT_EQ(snapshot.releasedWindows.size(), 1u);
    EXPECT_EQ(snapshot.releasedWindows[0], &window);
}

TEST_F(SwappyCommonHostTest, DestructorDoesNotReleaseWhenWindowWasNeverSet) {
    auto state = std::make_shared<PlatformSharedState>();

    {
        SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
        ASSERT_TRUE(swappy.isValid());
    }

    const PlatformSnapshot snapshot = snapshotPlatformState(*state);
    EXPECT_TRUE(snapshot.acquiredWindows.empty());
    EXPECT_TRUE(snapshot.releasedWindows.empty());
}

TEST_F(SwappyCommonHostTest, GetSupportedRefreshPeriodsCopiesSortedPeriodsFromPlatform) {
    auto state = std::make_shared<PlatformSharedState>();
    state->supportedPeriods = std::make_shared<RefreshPeriodMap>();
    insertRefreshPeriod(*state->supportedPeriods, k60HzPeriod, 10);
    insertRefreshPeriod(*state->supportedPeriods, k120HzPeriod, 20);

    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    ASSERT_TRUE(swappy.isValid());

    EXPECT_EQ(swappy.getSupportedRefreshPeriodsNS(nullptr, 0), 2);

    uint64_t outPeriods[2] = {};
    EXPECT_EQ(swappy.getSupportedRefreshPeriodsNS(outPeriods, 2), 2);
    EXPECT_EQ(outPeriods[0], static_cast<uint64_t>(k120HzPeriod.count()));
    EXPECT_EQ(outPeriods[1], static_cast<uint64_t>(k60HzPeriod.count()));
}

TEST_F(SwappyCommonHostTest, SetWindowTriggersSetFrameRateVoteOnFirstSwap) {
    auto state = std::make_shared<PlatformSharedState>();
    state->supportsSetFrameRate = true;

    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    ASSERT_TRUE(swappy.isValid());
    EXPECT_EQ(swappy.getRefreshPeriod(), k60HzPeriod);

    ANativeWindow window{7};
    swappy.setANativeWindow(&window);

    const auto handlers = readySwapHandlers();
    swappy.onPreSwap(handlers);
    swappy.onPostSwap(handlers);
    EXPECT_EQ(swappy.getSwapDuration(), k60HzPeriod);

    const PlatformSnapshot snapshot = snapshotPlatformState(*state);
    EXPECT_GE(snapshot.postFrameCallbacksCount, 1);
    ASSERT_EQ(snapshot.frameRateVotes.size(), 1u);
    EXPECT_EQ(snapshot.frameRateVotes[0].first, &window);
    EXPECT_NEAR(snapshot.frameRateVotes[0].second, 60.0f, 0.05f);
    EXPECT_TRUE(snapshot.preferredModeIds.empty());
}

TEST_F(SwappyCommonHostTest, DisplayManagerModeSelectionVotesModeIdWhenSetFrameRateUnsupported) {
    auto state = std::make_shared<PlatformSharedState>();
    state->initialSettings = makeSettings(k120HzPeriod, 29);
    state->supportsSetFrameRate = false;
    state->supportedPeriods = std::make_shared<RefreshPeriodMap>();
    insertRefreshPeriod(*state->supportedPeriods, k120HzPeriod, 1);
    insertRefreshPeriod(*state->supportedPeriods, k60HzPeriod, 2);

    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    ASSERT_TRUE(swappy.isValid());

    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_60FPS);

    const auto handlers = readySwapHandlers();
    swappy.onPreSwap(handlers);
    swappy.onPostSwap(handlers);

    const PlatformSnapshot snapshot = snapshotPlatformState(*state);
    EXPECT_TRUE(snapshot.frameRateVotes.empty());
    ASSERT_EQ(snapshot.preferredModeIds.size(), 1u);
    EXPECT_EQ(snapshot.preferredModeIds[0], 2);
}

TEST_F(SwappyCommonHostTest,
       InternalModeSwitchesToExternalChoreographerOnceAndStopsPreSwapPosting) {
    auto state = std::make_shared<PlatformSharedState>();
    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    ASSERT_TRUE(swappy.isValid());

    swappy.onChoreographer(0);
    PlatformSnapshot snapshot = snapshotPlatformState(*state);
    EXPECT_EQ(snapshot.switchToExternalChoreographerCount, 1);
    EXPECT_EQ(snapshot.postFrameCallbacksCount, 1);

    swappy.onChoreographer(k60HzPeriod.count());
    snapshot = snapshotPlatformState(*state);
    EXPECT_EQ(snapshot.switchToExternalChoreographerCount, 1);
    EXPECT_EQ(snapshot.postFrameCallbacksCount, 2);

    // Once external Choreographer is active, onPreSwap does not re-post callbacks.
    swappy.onPreSwap(readySwapHandlers());
    snapshot = snapshotPlatformState(*state);
    EXPECT_EQ(snapshot.postFrameCallbacksCount, 2);
}

TEST_F(SwappyCommonHostTest, ExternalStartModeDoesNotSwitchOrPostOnPreSwap) {
    auto state = std::make_shared<PlatformSharedState>();
    state->startsWithExternalChoreographer = true;
    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    ASSERT_TRUE(swappy.isValid());

    swappy.onPreSwap(readySwapHandlers());
    PlatformSnapshot snapshot = snapshotPlatformState(*state);
    EXPECT_EQ(snapshot.postFrameCallbacksCount, 0);

    swappy.onChoreographer(0);
    snapshot = snapshotPlatformState(*state);
    EXPECT_EQ(snapshot.switchToExternalChoreographerCount, 0);
    EXPECT_EQ(snapshot.postFrameCallbacksCount, 1);
}

TEST_F(SwappyCommonHostTest, InitializeChoreographerCallbackDrivesFilterAcrossSwaps) {
    auto state = std::make_shared<PlatformSharedState>();
    state->initialSettings = makeSettings(kFastTickPeriod);
    state->initialSettings.appVsyncOffset = 0ns;
    state->initialSettings.sfVsyncOffset = 0ns;
    Settings::getInstance()->setSwapDuration(kFastTickPeriod.count());

    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    ASSERT_TRUE(swappy.isValid());

    // ChoreographerTimer stops self-ticking after 7 identical frames, so completing
    // 10 paced swaps verifies initializeChoreographer's callback drives ChoreographerFilter.
    EXPECT_TRUE(runPacedSwaps(swappy, *state, 10, readySwapHandlers(500us)));
}

TEST_F(SwappyCommonHostTest, SwitchToExternalChoreographerCallbackDrivesFilterAcrossSwaps) {
    auto state = std::make_shared<PlatformSharedState>();
    state->initialSettings = makeSettings(kFastTickPeriod);
    state->initialSettings.appVsyncOffset = 0ns;
    state->initialSettings.sfVsyncOffset = 0ns;
    Settings::getInstance()->setSwapDuration(kFastTickPeriod.count());

    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    ASSERT_TRUE(swappy.isValid());

    swappy.onChoreographer(0);
    EXPECT_EQ(snapshotPlatformState(*state).switchToExternalChoreographerCount, 1);

    EXPECT_TRUE(runPacedSwaps(swappy, *state, 10, readySwapHandlers(500us)));
}

struct TracerCounts {
    int preWait = 0;
    int postWait = 0;
    int preSwap = 0;
    int postSwap = 0;
    int startFrame = 0;
};

TEST_F(SwappyCommonHostTest, TracerCallbacksInvokeAndRemoveCleanly) {
    auto state = std::make_shared<PlatformSharedState>();
    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    ASSERT_TRUE(swappy.isValid());

    TracerCounts counts{};
    SwappyTracer tracer{};
    tracer.userData = &counts;
    tracer.preWait = [](void* data) { ++static_cast<TracerCounts*>(data)->preWait; };
    tracer.postWait = [](void* data, int64_t, int64_t) {
        ++static_cast<TracerCounts*>(data)->postWait;
    };
    tracer.preSwapBuffers = [](void* data) { ++static_cast<TracerCounts*>(data)->preSwap; };
    tracer.postSwapBuffers = [](void* data, int64_t) {
        ++static_cast<TracerCounts*>(data)->postSwap;
    };
    tracer.startFrame = [](void* data, int, int64_t) {
        ++static_cast<TracerCounts*>(data)->startFrame;
    };

    swappy.addTracerCallbacks(tracer);
    const auto handlers = readySwapHandlers();
    swappy.onPreSwap(handlers);
    swappy.onPostSwap(handlers);

    EXPECT_EQ(counts.preWait, 1);
    EXPECT_EQ(counts.postWait, 1);
    EXPECT_EQ(counts.preSwap, 1);
    EXPECT_EQ(counts.postSwap, 1);
    EXPECT_EQ(counts.startFrame, 1);

    swappy.removeTracerCallbacks(tracer);
    // FakeSwappyPlatform does not tick Choreographer automatically; exceed the auto-swap
    // threshold so the second swap exercises tracer removal without waiting on mTargetFrame.
    swappy.setMaxAutoSwapDuration(0ns);
    swappy.onPreSwap(handlers);
    swappy.onPostSwap(handlers);

    EXPECT_EQ(counts.preWait, 1);
    EXPECT_EQ(counts.postWait, 1);
    EXPECT_EQ(counts.preSwap, 1);
    EXPECT_EQ(counts.postSwap, 1);
    EXPECT_EQ(counts.startFrame, 1);
}

class SimulatedSwappyHarness {
public:
    explicit SimulatedSwappyHarness(SwappyCommonSettings settings = makeSettings(),
                                    Clock::time_point initialTime = Clock::time_point{},
                                    DisplayPanelModel panelModel = DisplayPanelModel::openLoop(),
                                    bool supportsSetFrameRate = true)
          : mState(std::make_shared<PlatformSharedState>()),
            mClock(initialTime),
            mTimeline(mState, settings, &mClock) {
        {
            std::lock_guard<std::mutex> lock(mState->mutex);
            mState->supportsSetFrameRate = supportsSetFrameRate;
            mState->panel = SimulatedDisplayPanel(settings.refreshPeriod, std::move(panelModel));
        }
        mClock.setVsyncCallbacks(
                [this](Clock::time_point current) { return mTimeline.nextVsyncAfter(current); },
                [this](Clock::time_point vsyncTime) { mTimeline.deliverVsync(vsyncTime); });

        mSwappy = std::make_unique<SwappyCommon>(std::make_unique<FakeSwappyPlatform>(mState),
                                                 mClock);
        EXPECT_TRUE(mSwappy->isValid());
        EXPECT_TRUE(mClock.waitForWorkersSettled());
    }

    ~SimulatedSwappyHarness() {
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

    std::shared_ptr<PlatformSharedState> platformState() {
        return mState;
    }

    void runFrame(std::chrono::nanoseconds cpuWork, std::chrono::nanoseconds gpuWork,
                  std::chrono::nanoseconds swapCpuCost = 500us) {
        mClock.advanceBy(cpuWork);
        mGpuQueue.submit(mClock.now(), gpuWork);
        const SwappyCommon::SwapHandlers handlers{
                .lastFrameIsComplete =
                        [this] {
                            return mGpuQueue.lastFrameIsComplete(mClock.now(),
                                                                 mSwappy->getCurrentPipelineMode());
                        },
                .getPrevFrameGpuTime = [this] { return mGpuQueue.lastFenceTime(mClock.now()); },
        };
        mSwappy->onPreSwap(handlers);
        mClock.advanceBy(swapCpuCost);
        mSwappy->onPostSwap(handlers);
    }

    void runFrames(int frameCount, std::chrono::nanoseconds cpuWork,
                   std::chrono::nanoseconds gpuWork, std::chrono::nanoseconds swapCpuCost = 500us) {
        for (int frame = 0; frame < frameCount; ++frame) {
            runFrame(cpuWork, gpuWork, swapCpuCost);
        }
    }

    void runCombinedHitchCycles(int cycleCount, int lightFrames, std::chrono::nanoseconds lightCpu,
                                std::chrono::nanoseconds lightGpu, int spikeFrames,
                                std::chrono::nanoseconds spikeCpu,
                                std::chrono::nanoseconds spikeGpu) {
        for (int cycle = 0; cycle < cycleCount; ++cycle) {
            runFrames(lightFrames, lightCpu, lightGpu);
            runFrames(spikeFrames, spikeCpu, spikeGpu);
        }
    }

    [[nodiscard]] std::chrono::nanoseconds displayPeriod() const {
        return mTimeline.displayPeriod();
    }

    void triggerPassiveDisplayRateChange(std::chrono::nanoseconds newRefreshPeriod) {
        mTimeline.triggerPassiveDisplayRateChange(newRefreshPeriod);
        EXPECT_TRUE(mClock.waitForWorkersSettled());
    }

    void runDisplayJitterBursts(int burstCount, int framesPerPhase,
                                std::chrono::nanoseconds cpuWork,
                                std::chrono::nanoseconds gpuWork) {
        const std::chrono::nanoseconds kPhases[] = {k120HzPeriod, k90HzPeriod, k60HzPeriod};
        for (int burst = 0; burst < burstCount; ++burst) {
            triggerPassiveDisplayRateChange(kPhases[burst % 3]);
            runFrames(framesPerPhase, cpuWork, gpuWork);
        }
    }

private:
    std::shared_ptr<PlatformSharedState> mState;
    SimulatedClock mClock;
    SimulatedGpuQueue mGpuQueue;
    VsyncPanelTimeline mTimeline;
    std::unique_ptr<SwappyCommon> mSwappy;
};

TEST_F(SwappyCommonHostTest, EpochZeroStartFramePacesSecondFrameWithoutTreatingZeroAsSentinel) {
    SimulatedSwappyHarness harness(makeSettings(), Clock::time_point{});

    int64_t recordedCpuTimeNs = -1;
    SwappyTracer tracer{};
    tracer.userData = &recordedCpuTimeNs;
    tracer.postWait = [](void* data, int64_t cpuTimeNs, int64_t) {
        *static_cast<int64_t*>(data) = cpuTimeNs;
    };
    harness.swappy().addTracerCallbacks(tracer);

    const auto handlers = readySwapHandlers(2ms);
    harness.swappy().onPreSwap(handlers);
    harness.swappy().onPostSwap(handlers);
    EXPECT_EQ(harness.clock().now(), Clock::time_point{});

    harness.clock().advanceBy(4ms);
    harness.swappy().onPreSwap(handlers);

    EXPECT_EQ(recordedCpuTimeNs, std::chrono::nanoseconds(4ms).count());
    EXPECT_GT(harness.clock().now(), Clock::time_point{4ms});
    EXPECT_TRUE(harness.swappy().needToSetPresentationTime());
}

TEST_F(SwappyCommonHostTest, LightWorkloadSwitchesPipelineOffAndPacesAt60Hz) {
    SimulatedSwappyHarness harness(makeSettings(), Clock::time_point{1s});
    EXPECT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::On);

    for (int frame = 0; frame < 150; ++frame) {
        harness.runFrame(2ms, 2ms);
    }

    EXPECT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::Off);
    EXPECT_EQ(harness.swappy().getSwapDuration(), k60HzPeriod);

    const Clock::time_point firstPresent = harness.swappy().getPresentationTime();
    harness.runFrame(2ms, 2ms);
    const Clock::time_point secondPresent = harness.swappy().getPresentationTime();
    EXPECT_NEAR((secondPresent - firstPresent).count(), k60HzPeriod.count(), 1'000'000);
}

TEST_F(SwappyCommonHostTest, HeavyWorkloadStepsDownTo30HzVotesSetFrameRateAndRecoversTo60Hz) {
    SimulatedSwappyHarness harness(makeSettings(), Clock::time_point{1s});
    ANativeWindow window{42};
    harness.swappy().setANativeWindow(&window);

    int intervalChangeCallbacks = 0;
    SwappyTracer tracer{};
    tracer.userData = &intervalChangeCallbacks;
    tracer.swapIntervalChanged = [](void* data) { ++(*static_cast<int*>(data)); };
    harness.swappy().addTracerCallbacks(tracer);

    for (int frame = 0; frame < 150; ++frame) {
        harness.runFrame(20ms, 6ms);
    }

    EXPECT_NEAR(harness.swappy().getSwapDuration().count(), k30HzPeriod.count(), 100);
    EXPECT_GE(intervalChangeCallbacks, 1);
    {
        const auto snapshot = snapshotPlatformState(*harness.platformState());
        ASSERT_FALSE(snapshot.frameRateVotes.empty());
        // DisplayRateController votes pipelineFrameTime (~21ms => ~47.6Hz) at swapInterval=2
        // before discrete-ladder rate voting is layered on.
        EXPECT_LT(snapshot.frameRateVotes.back().second, 55.0f);
        EXPECT_GT(snapshot.frameRateVotes.back().second, 29.0f);
    }

    const int callbacksAt30Hz = intervalChangeCallbacks;
    for (int frame = 0; frame < 150; ++frame) {
        harness.runFrame(3ms, 3ms);
    }

    EXPECT_EQ(harness.swappy().getSwapDuration(), k60HzPeriod);
    EXPECT_GT(intervalChangeCallbacks, callbacksAt30Hz);
    const auto snapshot = snapshotPlatformState(*harness.platformState());
    EXPECT_NEAR(snapshot.frameRateVotes.back().second, 60.0f, 0.05f);
}

TEST_F(SwappyCommonHostTest, IncompleteGpuFenceSlipsOneFrameAndReportsGpuTimeWhenComplete) {
    SimulatedSwappyHarness harness(makeSettings(), Clock::time_point{1s});

    int64_t lastGpuTimeNs = 0;
    SwappyTracer tracer{};
    tracer.userData = &lastGpuTimeNs;
    tracer.postWait = [](void* data, int64_t, int64_t gpuTimeNs) {
        *static_cast<int64_t*>(data) = gpuTimeNs;
    };
    harness.swappy().addTracerCallbacks(tracer);

    harness.runFrame(3ms, 3ms);

    int pollCount = 0;
    const SwappyCommon::SwapHandlers slipOnceHandlers{
            .lastFrameIsComplete = [&pollCount] { return ++pollCount >= 2; },
            .getPrevFrameGpuTime = [] { return 12ms; },
    };
    harness.clock().advanceBy(3ms);
    const Clock::time_point beforeSlipSwap = harness.clock().now();
    harness.swappy().onPreSwap(slipOnceHandlers);
    EXPECT_GE(harness.clock().now() - beforeSlipSwap, k60HzPeriod);
    EXPECT_EQ(lastGpuTimeNs, std::chrono::nanoseconds(12ms).count());
    harness.swappy().onPostSwap(slipOnceHandlers);
}

TEST_F(SwappyCommonHostTest, BufferStuffingFixWaitAddsExtraFrameAfterConsecutiveHighLatencyFrames) {
    SimulatedSwappyHarness harness(makeSettings(), Clock::time_point{1s});
    for (int i = 0; i < 8; ++i) {
        harness.runFrame(3ms, 3ms);
    }

    int latestFrameIndex = 0;
    SwappyTracer tracer{};
    tracer.userData = &latestFrameIndex;
    tracer.startFrame = [](void* data, int currentFrame, int64_t) {
        *static_cast<int*>(data) = currentFrame;
    };
    harness.swappy().addTracerCallbacks(tracer);

    harness.swappy().setBufferStuffingFixWait(2);
    harness.swappy().setLastLatencyRecordedCallback([] { return 4; });

    // Two consecutive high-latency frames (expected latency = 2 VSyncs) arm
    // mTargetFrame += 1 at the end of the second frame so the next frame waits 2 VSyncs.
    harness.runFrame(3ms, 3ms);
    harness.runFrame(3ms, 3ms);
    const int frameBeforeExtraWait = latestFrameIndex;
    const Clock::time_point presentBeforeExtraWait = harness.swappy().getPresentationTime();

    harness.runFrame(3ms, 3ms);
    EXPECT_EQ(latestFrameIndex - frameBeforeExtraWait, 2);
    EXPECT_NEAR((harness.swappy().getPresentationTime() - presentBeforeExtraWait).count(),
                (2 * k60HzPeriod).count(), 1'000'000);
}

TEST_F(SwappyCommonHostTest, EnableFramePacingTogglesNeedToSetPresentationTime) {
    SimulatedSwappyHarness harness(makeSettings(), Clock::time_point{1s});
    harness.runFrame(3ms, 3ms);

    harness.swappy().enableFramePacing(false);
    harness.runFrame(3ms, 3ms);
    harness.clock().advanceBy(3ms);
    harness.swappy().onPreSwap(readySwapHandlers(3ms));
    EXPECT_FALSE(harness.swappy().needToSetPresentationTime());
    harness.swappy().onPostSwap(readySwapHandlers(3ms));

    harness.swappy().enableFramePacing(true);
    harness.runFrame(3ms, 3ms);
    harness.clock().advanceBy(3ms);
    harness.swappy().onPreSwap(readySwapHandlers(3ms));
    EXPECT_TRUE(harness.swappy().needToSetPresentationTime());
    harness.swappy().onPostSwap(readySwapHandlers(3ms));
}

TEST_F(SwappyCommonHostTest, ResetFramePacingRestoresInitialRefreshPeriodAndAutoSwapInterval) {
    SimulatedSwappyHarness harness(makeSettings(), Clock::time_point{1s});
    for (int frame = 0; frame < 150; ++frame) {
        harness.runFrame(20ms, 6ms);
    }
    EXPECT_NEAR(harness.swappy().getSwapDuration().count(), k30HzPeriod.count(), 100);

    harness.swappy().resetFramePacing();
    harness.runFrame(3ms, 3ms);
    EXPECT_EQ(harness.swappy().getSwapDuration(), k60HzPeriod);
}

TEST_F(SwappyCommonHostTest, MultiVsyncWaitReArmsPlatformCallbacksAcrossAllTargetVsyncs) {
    SimulatedSwappyHarness harness(makeSettings(), Clock::time_point{1s});
    harness.swappy().setAutoSwapInterval(false);
    Settings::getInstance()->setSwapDuration((12 * k60HzPeriod).count());

    int latestFrameIndex = 0;
    SwappyTracer tracer{};
    tracer.userData = &latestFrameIndex;
    tracer.startFrame = [](void* data, int currentFrame, int64_t) {
        *static_cast<int*>(data) = currentFrame;
    };
    harness.swappy().addTracerCallbacks(tracer);

    harness.runFrame(2ms, 2ms);
    const int frameIndexBeforeLongWait = latestFrameIndex;

    harness.runFrame(2ms, 2ms);
    EXPECT_GE(latestFrameIndex - frameIndexBeforeLongWait, 12);
    EXPECT_GE(snapshotPlatformState(*harness.platformState()).postFrameCallbacksCount, 12);
}

TEST_F(SwappyCommonHostTest, InvalidPlatformRemainsInvalidAndHandlesEntryPointsSafely) {
    auto state = std::make_shared<PlatformSharedState>();
    state->valid = false;
    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));

    EXPECT_FALSE(swappy.isValid());

    ANativeWindow window{1};
    swappy.setANativeWindow(&window);
    swappy.onChoreographer(1'000'000);
    EXPECT_EQ(swappy.getSupportedRefreshPeriodsNS(nullptr, 0), 0);
    swappy.resetFramePacing();
    swappy.enableFramePacing(false);
    swappy.enableBlockingWait(true);
    swappy.setANativeWindow(nullptr);

    const PlatformSnapshot snapshot = snapshotPlatformState(*state);
    EXPECT_TRUE(snapshot.acquiredWindows.empty());
    EXPECT_TRUE(snapshot.releasedWindows.empty());
    EXPECT_EQ(snapshot.postFrameCallbacksCount, 0);
}

TEST_F(SwappyCommonHostTest, GpuBoundWorkloadStepsDownTo30HzAndRecoversTo60Hz) {
    SimulatedSwappyHarness harness(makeSettings(), Clock::time_point{1s});
    ANativeWindow window{77};
    harness.swappy().setANativeWindow(&window);

    int64_t lastReportedGpuNs = 0;
    SwappyTracer tracer{};
    tracer.userData = &lastReportedGpuNs;
    tracer.postWait = [](void* data, int64_t, int64_t gpuTimeNs) {
        *static_cast<int64_t*>(data) = gpuTimeNs;
    };
    harness.swappy().addTracerCallbacks(tracer);

    // 4ms CPU fits inside 60Hz, while 22ms serialized GPU work overruns the 16.67ms
    // pipelined budget and blocks lastFrameIsComplete across VSYNC boundaries.
    harness.runFrames(150, 4ms, 22ms);

    EXPECT_NEAR(harness.swappy().getSwapDuration().count(), k30HzPeriod.count(), 100);
    EXPECT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::On);
    EXPECT_EQ(lastReportedGpuNs, std::chrono::nanoseconds(22ms).count());

    harness.runFrames(150, 4ms, 4ms);

    EXPECT_EQ(harness.swappy().getSwapDuration(), k60HzPeriod);
    EXPECT_EQ(lastReportedGpuNs, std::chrono::nanoseconds(4ms).count());
}

TEST_F(SwappyCommonHostTest, UnpipelinedGpuOverrunSlipsFrameAndRestoresPipelineModeAt60Hz) {
    SimulatedSwappyHarness harness(makeSettings(), Clock::time_point{1s});
    harness.runFrames(150, 2ms, 2ms);
    ASSERT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::Off);

    // In PipelineMode::Off, 5ms CPU + 14ms GPU (19ms sequential) exceeds 16.67ms in
    // onPostSwap's waitForNextFrame, while pipelined max(5ms, 14ms) + 1ms = 15ms fits
    // 60Hz, so SwapSlower restores PipelineMode::On without dropping to 30Hz.
    harness.runFrames(150, 5ms, 14ms);

    EXPECT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::On);
    EXPECT_EQ(harness.swappy().getSwapDuration(), k60HzPeriod);
}

TEST_F(SwappyCommonHostTest, ContinuousArrPanelCollapsesSwapIntervalToOneAtVotedRate) {
    SimulatedSwappyHarness harness(makeSettings(k120HzPeriod, 36), Clock::time_point{1s},
                                   DisplayPanelModel::continuousArr(24, 120, 2));
    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_60FPS);
    ANativeWindow window{101};
    harness.swappy().setANativeWindow(&window);

    // 4ms CPU + 19ms GPU => 20ms pipelined hint (50.0 Hz vote). Once the ARR panel
    // switches to 50 Hz (20ms period), Swappy paces at swapInterval=1 (20ms == refreshPeriod).
    harness.runFrames(200, 4ms, 19ms);

    EXPECT_EQ(harness.displayPeriod(), 20'000'000ns);
    EXPECT_EQ(harness.swappy().getRefreshPeriod(), 20'000'000ns);
    EXPECT_EQ(harness.swappy().getSwapDuration(), 20'000'000ns);
}

TEST_F(SwappyCommonHostTest, TeDivisor240HzPanelAppliesRateSwitchAfterVsyncLatency) {
    SimulatedSwappyHarness harness(makeSettings(k120HzPeriod, 36), Clock::time_point{1s},
                                   DisplayPanelModel::teDivisor240Hz(/*applyLatencyVsyncs=*/3));
    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_60FPS);
    ANativeWindow window{102};
    harness.swappy().setANativeWindow(&window);

    // Frame 1's onPostSwap votes 60 Hz (mAutoSwapInterval=2, remainingVsyncs=3); Frame 2
    // waits 2 VSYNCs (remainingVsyncs: 3 -> 1), keeping the panel at 120 Hz until Frame 3.
    harness.runFrame(2ms, 2ms);
    harness.runFrame(2ms, 2ms);
    EXPECT_EQ(harness.displayPeriod(), k120HzPeriod);

    harness.runFrame(2ms, 2ms);
    EXPECT_EQ(harness.displayPeriod(), k60HzPeriod);

    // A 19ms GPU load (20ms pipelined hint => 50 Hz vote) snaps to the 48 Hz TE divisor
    // (20,833,333 ns) and runs at swapInterval=1 on that 48 Hz refresh period.
    harness.runFrames(200, 4ms, 19ms);
    EXPECT_EQ(harness.displayPeriod(), 20'833'333ns);
    EXPECT_EQ(harness.swappy().getSwapDuration(), 20'833'333ns);
}

TEST_F(SwappyCommonHostTest,
       RepeatedVotesResolvingToSamePanelPeriodDoNotResetApplyLatencyCountdown) {
    SimulatedSwappyHarness harness(makeSettings(k120HzPeriod, 36), Clock::time_point{1s},
                                   DisplayPanelModel::teDivisor240Hz(/*applyLatencyVsyncs=*/7));
    ANativeWindow window{103};
    harness.swappy().setANativeWindow(&window);

    // Distinct swapDuration floors (~55.6 Hz, ~52.6 Hz, 50.0 Hz) each exceed the 0.1 Hz
    // vote deadband and emit setFrameRate calls across 6 elapsed VSYNCs (3 VSYNCs/frame),
    // all resolving to 20,833,333 ns without restarting the 7-VSYNC apply countdown.
    Settings::getInstance()->setSwapDuration(18'000'000);
    harness.runFrame(2ms, 2ms);

    Settings::getInstance()->setSwapDuration(19'000'000);
    harness.runFrame(2ms, 2ms);

    Settings::getInstance()->setSwapDuration(20'000'000);
    harness.runFrame(2ms, 2ms);
    EXPECT_EQ(harness.displayPeriod(), k120HzPeriod);

    harness.runFrame(2ms, 2ms);
    const auto snapshot = snapshotPlatformState(*harness.platformState());
    EXPECT_GE(snapshot.frameRateVotes.size(), 3u);
    EXPECT_EQ(harness.displayPeriod(), 20'833'333ns);
}

TEST_F(SwappyCommonHostTest, DiscretePanelSwitchesLegacyModeIdAndHandlesPassiveThermalCap) {
    SimulatedSwappyHarness harness(makeSettings(k120HzPeriod, 29), Clock::time_point{1s},
                                   DisplayPanelModel::discrete60And120Hz(1),
                                   /*supportsSetFrameRate=*/false);
    Settings::getInstance()->setSwapDuration(k120HzPeriod.count());

    // 4ms CPU + 20ms GPU (21ms pipelined hint => ~47.6 Hz): on discrete {120Hz, 60Hz},
    // 120Hz*3 = 25ms (40fps) beats 60Hz*2 = 33.3ms (30fps), keeping mode 1 (120Hz) at interval=3.
    harness.runFrames(350, 4ms, 20ms);
    EXPECT_EQ(harness.displayPeriod(), k120HzPeriod);
    EXPECT_EQ(harness.swappy().getSwapDuration(), 3 * k120HzPeriod);

    // Simulate an OS thermal policy capping the physical panel from 120Hz down to 60Hz.
    harness.triggerPassiveDisplayRateChange(k60HzPeriod);
    harness.runFrames(150, 4ms, 20ms);

    EXPECT_EQ(harness.displayPeriod(), k60HzPeriod);
    EXPECT_EQ(harness.swappy().getRefreshPeriod(), k60HzPeriod);
    EXPECT_NEAR(harness.swappy().getSwapDuration().count(), (2 * k60HzPeriod).count(), 100);
}

TEST_F(SwappyCommonHostTest, PanelRateSwitchAdvancesNextVsyncByFullNewRefreshPeriod) {
    auto state = std::make_shared<PlatformSharedState>();
    state->panel =
            SimulatedDisplayPanel(k120HzPeriod,
                                  DisplayPanelModel::discrete60And120Hz(/*applyLatencyVsyncs=*/1));
    state->choreographerCallbackArmed = true;
    state->onChoreographer = [](std::optional<std::chrono::nanoseconds>) {};
    VsyncPanelTimeline timeline(state, makeSettings(k120HzPeriod, 36));

    const auto switchVsync = timeline.nextVsyncAfter(Clock::time_point{1s});
    ASSERT_TRUE(switchVsync.has_value());
    state->panel.scheduleSwitch(k60HzPeriod);
    timeline.deliverVsync(*switchVsync);

    state->choreographerCallbackArmed = true;
    const auto postSwitchVsync = timeline.nextVsyncAfter(*switchVsync);
    ASSERT_TRUE(postSwitchVsync.has_value());
    EXPECT_EQ(*postSwitchVsync - *switchVsync, k60HzPeriod);
}

TEST_F(SwappyCommonHostTest, DiscretePanelSetFrameRateSelectsModeMatchingTargetMultiple) {
    SimulatedSwappyHarness harness(makeSettings(k120HzPeriod, 36), Clock::time_point{1s},
                                   DisplayPanelModel::discrete60And120Hz(1));
    Settings::getInstance()->setSwapDuration(k120HzPeriod.count());
    ANativeWindow window{104};
    harness.swappy().setANativeWindow(&window);

    // Under setFrameRate (~47.6 Hz vote), 120Hz*3 (25ms) beats 60Hz*2 (33.3ms), keeping 120Hz;
    // a 30fps floor (30.0 Hz vote) switches the discrete panel to 60Hz at interval=2.
    harness.runFrames(200, 4ms, 20ms);
    EXPECT_EQ(harness.displayPeriod(), k120HzPeriod);
    EXPECT_EQ(harness.swappy().getSwapDuration(), 3 * k120HzPeriod);

    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_30FPS);
    harness.runFrames(4, 3ms, 3ms);
    EXPECT_EQ(harness.displayPeriod(), k60HzPeriod);
    EXPECT_NEAR(harness.swappy().getSwapDuration().count(), (2 * k60HzPeriod).count(), 100);
}

TEST_F(SwappyCommonHostTest, DiscreteLegacyPanelSelects60HzModeIdFor30HzTarget) {
    SimulatedSwappyHarness harness(makeSettings(k120HzPeriod, 29), Clock::time_point{1s},
                                   DisplayPanelModel::discrete60And120Hz(1),
                                   /*supportsSetFrameRate=*/false);
    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_30FPS);

    // First swap votes mode 2 (60Hz) for the 30fps swapDuration floor, switching the
    // panel to 60Hz and pacing at swapInterval=2 (~33.33ms).
    harness.runFrames(4, 3ms, 3ms);

    const auto snapshot = snapshotPlatformState(*harness.platformState());
    ASSERT_FALSE(snapshot.preferredModeIds.empty());
    EXPECT_EQ(snapshot.preferredModeIds.back(), 2);
    EXPECT_EQ(harness.displayPeriod(), k60HzPeriod);
    EXPECT_EQ(harness.swappy().getRefreshPeriod(), k60HzPeriod);
    EXPECT_NEAR(harness.swappy().getSwapDuration().count(), (2 * k60HzPeriod).count(), 100);
}

TEST_F(SwappyCommonHostTest,
       SingleFrameCpuAndGpuHitchAtFivePercentDoesNotDoubleCountIntoThirtyHzDownshift) {
    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_60FPS);
    SimulatedSwappyHarness harness(makeSettings(k60HzPeriod, 36), Clock::time_point{1s});

    // 17 normal frames (6ms CPU, 6ms GPU) + 1 combined CPU+GPU hitch frame (35ms CPU, 28ms GPU)
    // is a 5.6% hitch rate (<= 10% FRAME_DROP_THRESHOLD). Because the hitch is not double-counted
    // against its successor during GPU fence wait, pacing remains at 60Hz instead of dropping to
    // 30Hz.
    harness.runCombinedHitchCycles(8, 17, 6ms, 6ms, 1, 35ms, 28ms);

    EXPECT_EQ(harness.swappy().getRefreshPeriod(), k60HzPeriod);
    EXPECT_EQ(harness.swappy().getSwapDuration(), k60HzPeriod);
}

TEST_F(SwappyCommonHostTest, GpuOnlyHitchesAboveTenPercentStillStepDownTo30Hz) {
    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_60FPS);
    SimulatedSwappyHarness harness(makeSettings(k60HzPeriod, 36), Clock::time_point{1s});

    // 7 normal frames (6ms CPU, 6ms GPU) + 1 GPU-only hitch frame (6ms CPU, 28ms GPU) is a
    // 12.5% hitch rate (> 10% FRAME_DROP_THRESHOLD). 16 cycles (2.4s) crosses the 2s sample
    // window and steps pacing down to 30Hz.
    harness.runCombinedHitchCycles(16, 7, 6ms, 6ms, 1, 6ms, 28ms);

    EXPECT_EQ(harness.swappy().getSwapDuration(), k30HzPeriod);
}

TEST_F(SwappyCommonHostTest, GpuHitchFollowedByLongerCpuHitchCountsBothMissesAndStepsDownTo30Hz) {
    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_60FPS);
    SimulatedSwappyHarness harness(makeSettings(k60HzPeriod, 36), Clock::time_point{1s});

    // 15 healthy frames + 1 GPU hitch (6ms CPU, 28ms GPU) + 1 longer CPU hitch (35ms CPU, 6ms GPU)
    // is 2 missed frames out of 17 (11.8% > 10% FRAME_DROP_THRESHOLD). Even though the 28ms GPU
    // fence retires during the successor's 35ms CPU stall (lateFrames == 0), both overrunning
    // frames must be counted so pacing steps down to 30Hz after the 2s sample window (~2.53s).
    for (int cycle = 0; cycle < 8; ++cycle) {
        harness.runFrames(15, 6ms, 6ms);
        harness.runFrame(6ms, 28ms);
        harness.runFrame(35ms, 6ms);
    }

    EXPECT_EQ(harness.swappy().getSwapDuration(), k30HzPeriod);
}

TEST_F(SwappyCommonHostTest, ExtraVsyncDuringSingleFenceWaitDoesNotDoubleCountIntoCurrentFrame) {
    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_60FPS);
    SimulatedSwappyHarness harness(makeSettings(k60HzPeriod, 36), Clock::time_point{1s});

    // 17 healthy frames + 1 GPU-only hitch is a 5.6% hitch rate (<= 10% FRAME_DROP_THRESHOLD).
    // Simulate OS scheduling delay where mCurrentFrame advances by two vsyncs during a single
    // waitOneFrame() iteration (lateFrames == 1); the healthy successor whose CPU finished on time
    // must not be charged a second miss.
    for (int cycle = 0; cycle < 8; ++cycle) {
        harness.runFrames(16, 6ms, 6ms);
        harness.runFrame(6ms, 24ms);

        harness.clock().advanceBy(6ms);
        int checkCount = 0;
        const SwappyCommon::SwapHandlers handlers{
                .lastFrameIsComplete =
                        [&] {
                            ++checkCount;
                            if (checkCount == 1) {
                                return false;
                            }
                            if (checkCount == 2) {
                                harness.clock().advanceBy(k60HzPeriod);
                                EXPECT_TRUE(triggerChoreographer(*harness.platformState()));
                                EXPECT_TRUE(harness.clock().waitForWorkersSettled());
                                harness.clock().advanceBy(k60HzPeriod);
                                EXPECT_TRUE(triggerChoreographer(*harness.platformState()));
                                EXPECT_TRUE(harness.clock().waitForWorkersSettled());
                            }
                            return true;
                        },
                .getPrevFrameGpuTime = [] { return 24ms; },
        };
        harness.swappy().onPreSwap(handlers);
        EXPECT_EQ(checkCount, 3);
        harness.clock().advanceBy(500us);
        harness.swappy().onPostSwap(handlers);
    }

    EXPECT_EQ(harness.swappy().getRefreshPeriod(), k60HzPeriod);
    EXPECT_EQ(harness.swappy().getSwapDuration(), k60HzPeriod);
}

TEST_F(SwappyCommonHostTest, ConcurrentReadersObserveConsistentPacingStateUnderTimingUpdates) {
    auto state = std::make_shared<PlatformSharedState>();
    state->initialSettings = makeSettings(kFastTickPeriod);
    state->initialSettings.appVsyncOffset = 0ns;
    state->initialSettings.sfVsyncOffset = 0ns;
    Settings::getInstance()->setSwapDuration(kFastTickPeriod.count());

    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    ASSERT_TRUE(swappy.isValid());
    swappy.setMaxAutoSwapDuration(0ns);

    EXPECT_TRUE(exerciseConcurrentPacingReaders(swappy, swappy, 64));
}

TEST_F(SwappyCommonHostTest, ConcurrentDisplayTimingAndSwapDurationUpdatesPreserveLatestState) {
    auto state = std::make_shared<PlatformSharedState>();
    state->initialSettings = makeSettings(kFastTickPeriod);
    state->initialSettings.appVsyncOffset = 0ns;
    state->initialSettings.sfVsyncOffset = 0ns;
    Settings::getInstance()->setSwapDuration(std::chrono::nanoseconds(kFastTickPeriod).count());

    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    ASSERT_TRUE(swappy.isValid());
    swappy.setMaxAutoSwapDuration(0ns);

    verifyConcurrentSettingsUpdates(swappy, 32);
}

TEST_F(SwappyCommonHostTest,
       SupportedRefreshPeriodQueriesFromAppThreadDuringFramesReturnAllPeriods) {
    auto state = std::make_shared<PlatformSharedState>();
    state->initialSettings = makeSettings(kFastTickPeriod);
    state->initialSettings.appVsyncOffset = 0ns;
    state->initialSettings.sfVsyncOffset = 0ns;
    state->supportedPeriods = std::make_shared<RefreshPeriodMap>();
    insertRefreshPeriod(*state->supportedPeriods, k120HzPeriod, 1);
    insertRefreshPeriod(*state->supportedPeriods, k60HzPeriod, 2);
    Settings::getInstance()->setSwapDuration(kFastTickPeriod.count());

    SwappyCommon swappy(std::make_unique<FakeSwappyPlatform>(state));
    ASSERT_TRUE(swappy.isValid());
    swappy.setMaxAutoSwapDuration(0ns);

    // Each frame refreshes the supported periods on the render thread while an app thread
    // queries them; both sides go through mMutex, which swappy_host_tsan_test checks.
    EXPECT_TRUE(exerciseConcurrentSupportedRefreshPeriodQueries(swappy, 64,
                                                                /*expectedPeriodCount=*/2));
}

struct ActiveSwapModeProbe {
    SwappyCommon* swappy = nullptr;
    int remainingIncompleteChecks = 1;
    std::chrono::nanoseconds gpuTime = 4ms;
    std::function<void()> onFirstIncompleteCheck;
    int completeChecks = 0;
    int gpuTimeReads = 0;
    std::vector<PipelineMode> modesDuringCompleteChecks;
    std::vector<PipelineMode> modesDuringGpuTimeReads;

    [[nodiscard]] SwappyCommon::SwapHandlers handlers() {
        return SwappyCommon::SwapHandlers{
                .lastFrameIsComplete =
                        [this] {
                            ++completeChecks;
                            modesDuringCompleteChecks.push_back(swappy->getCurrentPipelineMode());
                            if (remainingIncompleteChecks > 0) {
                                --remainingIncompleteChecks;
                                if (completeChecks == 1 && onFirstIncompleteCheck) {
                                    onFirstIncompleteCheck();
                                }
                                return false;
                            }
                            return true;
                        },
                .getPrevFrameGpuTime =
                        [this] {
                            ++gpuTimeReads;
                            modesDuringGpuTimeReads.push_back(swappy->getCurrentPipelineMode());
                            return gpuTime;
                        },
        };
    }
};

TEST_F(SwappyCommonHostTest,
       DisablingAutoPipelineModeBetweenPreAndPostSwapCompletesNonPipelinedWait) {
    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_60FPS);
    SimulatedSwappyHarness harness(makeSettings(k60HzPeriod), Clock::time_point{1s});
    harness.runFrames(130, 2ms, 2ms);
    ASSERT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::Off);

    int postWaitCalls = 0;
    int64_t postWaitGpuNs = -1;
    SwappyTracer tracer{};
    std::pair<int*, int64_t*> waitData{&postWaitCalls, &postWaitGpuNs};
    tracer.userData = &waitData;
    tracer.postWait = [](void* data, int64_t, int64_t gpuNs) {
        auto* pair = static_cast<std::pair<int*, int64_t*>*>(data);
        ++(*pair->first);
        *pair->second = gpuNs;
    };
    harness.swappy().addTracerCallbacks(tracer);

    ActiveSwapModeProbe probe{.swappy = &harness.swappy()};
    const SwappyCommon::SwapHandlers handlers = probe.handlers();

    harness.clock().advanceBy(2ms);
    harness.swappy().onPreSwap(handlers);
    EXPECT_EQ(probe.completeChecks, 0);
    EXPECT_EQ(postWaitCalls, 0);

    harness.swappy().setAutoPipelineMode(false);
    EXPECT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::Off);

    harness.clock().advanceBy(500us);
    harness.swappy().onPostSwap(handlers);

    EXPECT_EQ(postWaitCalls, 1);
    EXPECT_EQ(postWaitGpuNs, std::chrono::nanoseconds(4ms).count());
    EXPECT_EQ(probe.completeChecks, 3);
    EXPECT_EQ(probe.gpuTimeReads, 1);
    EXPECT_EQ(probe.modesDuringCompleteChecks,
              (std::vector<PipelineMode>{PipelineMode::Off, PipelineMode::Off, PipelineMode::Off}));
    EXPECT_EQ(probe.modesDuringGpuTimeReads, (std::vector<PipelineMode>{PipelineMode::Off}));
    EXPECT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::On);
}

TEST_F(SwappyCommonHostTest, DisablingAutoSwapIntervalDuringPostSwapWaitKeepsNonPipelinedHandlers) {
    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_60FPS);
    SimulatedSwappyHarness harness(makeSettings(k60HzPeriod), Clock::time_point{1s});
    harness.runFrames(130, 2ms, 2ms);
    ASSERT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::Off);

    int postWaitCalls = 0;
    SwappyTracer tracer{};
    tracer.userData = &postWaitCalls;
    tracer.postWait = [](void* data, int64_t, int64_t) { ++(*static_cast<int*>(data)); };
    harness.swappy().addTracerCallbacks(tracer);

    ActiveSwapModeProbe probe{
            .swappy = &harness.swappy(),
            .onFirstIncompleteCheck = [&harness] { harness.swappy().setAutoSwapInterval(false); },
    };
    const SwappyCommon::SwapHandlers handlers = probe.handlers();

    harness.clock().advanceBy(2ms);
    harness.swappy().onPreSwap(handlers);
    harness.clock().advanceBy(500us);
    harness.swappy().onPostSwap(handlers);

    EXPECT_EQ(postWaitCalls, 1);
    EXPECT_EQ(probe.completeChecks, 3);
    EXPECT_EQ(probe.gpuTimeReads, 1);
    EXPECT_EQ(probe.modesDuringCompleteChecks,
              (std::vector<PipelineMode>{PipelineMode::Off, PipelineMode::Off, PipelineMode::Off}));
    EXPECT_EQ(probe.modesDuringGpuTimeReads, (std::vector<PipelineMode>{PipelineMode::Off}));
    EXPECT_EQ(harness.swappy().getCurrentPipelineMode(), PipelineMode::On);
}

TEST_F(SwappyCommonHostTest, SubTwoSecondDisplayRateJitterDoesNotStarveAutoPacingDownshift) {
    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_60FPS);
    SimulatedSwappyHarness harness(makeSettings(k120HzPeriod), Clock::time_point{1s});

    // Each 35-frame phase at 25ms spans ~0.89s (< 2.0s). Across 3 phases (~2.7s ending on 60Hz),
    // retained samples reach 2.0s and step auto-pacing down instead of starving forever.
    harness.runDisplayJitterBursts(/*burstCount=*/3, /*framesPerPhase=*/35, 25ms, 4ms);

    EXPECT_EQ(harness.swappy().getRefreshPeriod(), k60HzPeriod);
    EXPECT_EQ(harness.swappy().getSwapDuration(), 2 * k60HzPeriod);
}

TEST_F(SwappyCommonHostTest,
       ExternalDisplayDropTo30HzClearsResolved60HzMissesWithoutDoubleDownshift) {
    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_60FPS);
    SimulatedSwappyHarness harness(makeSettings(k60HzPeriod), Clock::time_point{1s});

    // 80 frames of 20ms (~1.64s < 2.0s) all miss the 60Hz budget before an external 30Hz switch.
    harness.runFrames(80, 20ms, 4ms);
    ASSERT_EQ(harness.swappy().getSwapDuration(), k60HzPeriod);

    harness.triggerPassiveDisplayRateChange(k30HzPeriod);
    harness.runFrames(25, 20ms, 4ms);

    // Total span exceeds 2.0s; 20ms + 1ms margin fits 33.33ms, so pacing stays at 30Hz (interval=1)
    // rather than falsely stepping to 15Hz (interval=2) from pre-switch 60Hz misses.
    EXPECT_EQ(harness.swappy().getRefreshPeriod(), k30HzPeriod);
    EXPECT_EQ(harness.swappy().getSwapDuration(), k30HzPeriod);
}

TEST_F(SwappyCommonHostTest,
       ExternalDisplayDropTo40HzRetainsUnresolvedPipelinedGpuMissesAndStepsDown) {
    Settings::getInstance()->setSwapDuration(SWAPPY_SWAP_60FPS);
    SimulatedSwappyHarness harness(makeSettings(k60HzPeriod), Clock::time_point{1s});

    // 14 cycles of 4 healthy frames (6ms CPU, 6ms GPU) + 1 GPU hitch (6ms CPU, 38ms GPU) span
    // ~1.4s (< 2.0s) with 14 pipelined GPU misses that still exceed a 40Hz (25ms) budget.
    harness.runCombinedHitchCycles(14, 4, 6ms, 6ms, 1, 6ms, 38ms);
    harness.runFrame(6ms, 6ms);
    ASSERT_EQ(harness.swappy().getSwapDuration(), k60HzPeriod);

    harness.triggerPassiveDisplayRateChange(25'000'000ns);
    harness.runFrames(30, 6ms, 6ms);

    // Total span exceeds 2.0s; 38ms GPU + 1ms margin > 25ms, so the 14 unresolved GPU misses
    // (~14% > 10%) survive clearMissesFittingBudget and step swapInterval to 2 (50ms).
    EXPECT_EQ(harness.swappy().getRefreshPeriod(), 25'000'000ns);
    EXPECT_EQ(harness.swappy().getSwapDuration(), 50'000'000ns);
}

} // namespace

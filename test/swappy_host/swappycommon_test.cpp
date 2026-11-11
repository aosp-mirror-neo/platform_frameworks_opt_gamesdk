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

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "RefreshPeriodSet.h"
#include "SdkVersion.h"
#include "Settings.h"
#include "SwappyPlatform.h"
#include "gtest/gtest.h"
#include "swappy/swappy_common.h"

struct ANativeWindow {
    int id = 0;
};

namespace {

using namespace std::chrono_literals;
using swappy::insertRefreshPeriod;
using swappy::RefreshPeriodMap;
using swappy::SdkVersion;
using swappy::Settings;
using swappy::SwappyCommon;
using swappy::SwappyCommonSettings;
using swappy::SwappyPlatform;

constexpr auto k60HzPeriod = 16'666'667ns;
constexpr auto k120HzPeriod = 8'333'333ns;
constexpr auto kFastTickPeriod = 1ms;

SwappyCommonSettings makeSettings(std::chrono::nanoseconds refreshPeriod = k60HzPeriod,
                                  int sdkInt = 33) {
    return SwappyCommonSettings{
            .sdkVersion = SdkVersion{sdkInt, 0},
            .refreshPeriod = refreshPeriod,
            .appVsyncOffset = 1'000'000ns,
            .sfVsyncOffset = 2'000'000ns,
    };
}

SwappyCommon::SwapHandlers readySwapHandlers(std::chrono::nanoseconds gpuTime = 4ms) {
    return SwappyCommon::SwapHandlers{
            .lastFrameIsComplete = [] { return true; },
            .getPrevFrameGpuTime = [gpuTime] { return gpuTime; },
    };
}

struct PlatformSharedState {
    std::mutex mutex;
    bool valid = true;
    bool startsWithExternalChoreographer = false;
    bool choreographerInitialized = true;
    bool displayManagerInitialized = true;
    bool supportsSetFrameRate = true;
    SwappyCommonSettings initialSettings = makeSettings();
    std::shared_ptr<RefreshPeriodMap> supportedPeriods;

    SwappyPlatform::ChoreographerCallback onChoreographer;
    std::vector<ANativeWindow*> acquiredWindows;
    std::vector<ANativeWindow*> releasedWindows;
    std::vector<std::pair<ANativeWindow*, float>> frameRateVotes;
    std::vector<int> preferredModeIds;
    int postFrameCallbacksCount = 0;
    int switchToExternalChoreographerCount = 0;
};

struct PlatformSnapshot {
    std::vector<ANativeWindow*> acquiredWindows;
    std::vector<ANativeWindow*> releasedWindows;
    std::vector<std::pair<ANativeWindow*, float>> frameRateVotes;
    std::vector<int> preferredModeIds;
    int postFrameCallbacksCount = 0;
    int switchToExternalChoreographerCount = 0;
};

PlatformSnapshot snapshotPlatformState(PlatformSharedState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    return PlatformSnapshot{
            .acquiredWindows = state.acquiredWindows,
            .releasedWindows = state.releasedWindows,
            .frameRateVotes = state.frameRateVotes,
            .preferredModeIds = state.preferredModeIds,
            .postFrameCallbacksCount = state.postFrameCallbacksCount,
            .switchToExternalChoreographerCount = state.switchToExternalChoreographerCount,
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

class FakeSwappyPlatform final : public SwappyPlatform {
public:
    explicit FakeSwappyPlatform(std::shared_ptr<PlatformSharedState> state)
          : mState(std::move(state)) {}

    [[nodiscard]] bool isValid() const override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->valid;
    }

    [[nodiscard]] SwappyCommonSettings getInitialSettings() const override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->initialSettings;
    }

    [[nodiscard]] bool startsWithExternalChoreographer() const override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->startsWithExternalChoreographer;
    }

    [[nodiscard]] bool initializeChoreographer(ChoreographerCallback onChoreographer) override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        if (!mState->choreographerInitialized) {
            return false;
        }
        mState->onChoreographer = std::move(onChoreographer);
        return true;
    }

    void postFrameCallbacks() override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        ++mState->postFrameCallbacksCount;
    }

    void switchToExternalChoreographer(ChoreographerCallback onChoreographer) override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        ++mState->switchToExternalChoreographerCount;
        mState->onChoreographer = std::move(onChoreographer);
    }

    [[nodiscard]] bool initializeDisplayManager() override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->displayManagerInitialized;
    }

    void acquireWindow(ANativeWindow* window) override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        mState->acquiredWindows.push_back(window);
    }

    void releaseWindow(ANativeWindow* window) override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        mState->releasedWindows.push_back(window);
    }

    [[nodiscard]] bool supportsSetFrameRate() const override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->supportsSetFrameRate;
    }

    void setFrameRate(ANativeWindow* window, float frameRate) override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        mState->frameRateVotes.emplace_back(window, frameRate);
    }

    [[nodiscard]] std::shared_ptr<RefreshPeriodMap> getSupportedRefreshPeriods() override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        return mState->supportedPeriods;
    }

    void setPreferredDisplayModeId(int modeId) override {
        std::lock_guard<std::mutex> lock(mState->mutex);
        mState->preferredModeIds.push_back(modeId);
    }

private:
    std::shared_ptr<PlatformSharedState> mState;
};

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

} // namespace

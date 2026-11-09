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
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "Clock.h"
#include "RefreshPeriodSet.h"
#include "SdkVersion.h"
#include "Settings.h"
#include "SwappyPlatform.h"
#include "Thread.h"
#include "gtest/gtest.h"
#include "swappy/swappy_common.h"

struct ANativeWindow {
    int id = 0;
};

namespace {

using namespace std::chrono_literals;
using swappy::Clock;
using swappy::getNumCpus;
using swappy::insertRefreshPeriod;
using swappy::PipelineMode;
using swappy::RefreshPeriodMap;
using swappy::SdkVersion;
using swappy::Settings;
using swappy::SwappyCommon;
using swappy::SwappyCommonSettings;
using swappy::SwappyPlatform;

constexpr auto k60HzPeriod = 16'666'667ns;
constexpr auto k30HzPeriod = 33'333'334ns;
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
    bool choreographerCallbackArmed = false;
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
        mState->choreographerCallbackArmed = true;
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

// Deterministic discrete-event clock coordinating SwappyCommon (on the test
// driver thread) and ChoreographerFilter worker threads without sleeps.
class SimulatedClock final : public Clock {
public:
    explicit SimulatedClock(time_point initialTime = time_point{},
                            int32_t filterWorkers = getNumCpus() > 2 ? 2 : 1)
          : mNowNs(initialTime.time_since_epoch().count()), mActiveWorkers(filterWorkers) {}

    [[nodiscard]] time_point now() const override {
        return time_point(std::chrono::nanoseconds(mNowNs.load(std::memory_order_acquire)));
    }

    void sleepUntil(time_point deadline) const override {
        std::unique_lock<std::mutex> lock(mMutex);
        if (mShutdown || now() >= deadline) {
            return;
        }
        SleepWaiter waiter{deadline, false};
        --mActiveWorkers;
        mSleepers.push_back(&waiter);
        mCv.notify_all();
        mCv.wait(lock, [&] { return mShutdown || waiter.woken; });
    }

protected:
    void waitLocked(std::unique_lock<std::mutex>& callerLock, std::condition_variable& condition,
                    const std::function<bool()>& predicate) const override {
        if (std::this_thread::get_id() == mDriverThreadId) {
            while (!predicate()) {
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    if (mShutdown) {
                        return;
                    }
                }
                callerLock.unlock();
                const bool progressed = advanceStepUpTo(time_point::max());
                callerLock.lock();
                if (!progressed) {
                    ADD_FAILURE() << "SimulatedClock stalled waiting on predicate";
                    return;
                }
            }
            return;
        }

        while (!predicate()) {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mShutdown) {
                    return;
                }
                --mActiveWorkers;
                ++mWaitingInCv;
                mCv.notify_all();
            }
            condition.wait(callerLock);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                --mWaitingInCv;
                ++mActiveWorkers;
                mCv.notify_all();
            }
        }
    }

public:
    void setVsyncCallbacks(std::function<std::optional<time_point>(time_point)> nextVsyncAfter,
                           std::function<void(time_point)> onVsync) {
        std::lock_guard<std::mutex> lock(mMutex);
        mNextVsyncAfter = std::move(nextVsyncAfter);
        mOnVsync = std::move(onVsync);
    }

    [[nodiscard]] bool waitForWorkersSettled() const {
        std::unique_lock<std::mutex> lock(mMutex);
        return mCv.wait_for(lock, 5s, [&] { return mShutdown || mActiveWorkers == 0; });
    }

    void advanceBy(std::chrono::nanoseconds delta) const {
        if (delta > 0ns) {
            advanceTo(now() + delta);
        } else {
            EXPECT_TRUE(waitForWorkersSettled());
        }
    }

    [[nodiscard]] bool advanceStepUpTo(time_point target) const {
        if (!waitForWorkersSettled()) {
            ADD_FAILURE() << "Workers failed to settle before step";
            return false;
        }

        time_point nextTime = target;
        bool hasEvent = (target != time_point::max());
        bool fireVsync = false;
        std::function<void(time_point)> onVsync;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            const time_point current = now();
            if (mShutdown || current >= target) {
                return false;
            }
            for (const SleepWaiter* waiter : mSleepers) {
                if (waiter->deadline > current && waiter->deadline < nextTime) {
                    nextTime = waiter->deadline;
                    hasEvent = true;
                }
            }
            if (mNextVsyncAfter) {
                const auto candidate = mNextVsyncAfter(current);
                if (candidate.has_value() && *candidate > current && *candidate <= nextTime) {
                    nextTime = *candidate;
                    hasEvent = true;
                    fireVsync = true;
                    onVsync = mOnVsync;
                }
            }
            if (!hasEvent) {
                return false;
            }

            mNowNs.store(nextTime.time_since_epoch().count(), std::memory_order_release);
            for (auto it = mSleepers.begin(); it != mSleepers.end();) {
                if ((*it)->deadline <= nextTime) {
                    (*it)->woken = true;
                    ++mActiveWorkers;
                    it = mSleepers.erase(it);
                } else {
                    ++it;
                }
            }
            mCv.notify_all();
        }

        if (fireVsync && onVsync) {
            if (!waitForWorkersSettled()) {
                ADD_FAILURE() << "Workers failed to settle before VSync callback";
                return false;
            }
            onVsync(nextTime);
            std::unique_lock<std::mutex> lock(mMutex);
            if (!mCv.wait_for(lock, 5s, [&] {
                    return mShutdown || (mWaitingInCv == 0 && mActiveWorkers == 0);
                })) {
                ADD_FAILURE() << "Workers failed to process VSync callback";
                return false;
            }
        }

        if (!waitForWorkersSettled()) {
            ADD_FAILURE() << "Workers failed to settle after step";
            return false;
        }
        return true;
    }

    void advanceTo(time_point target) const {
        while (advanceStepUpTo(target)) {
        }
        ASSERT_TRUE(waitForWorkersSettled());
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mMutex);
        mShutdown = true;
        for (SleepWaiter* waiter : mSleepers) {
            waiter->woken = true;
        }
        mSleepers.clear();
        mCv.notify_all();
    }

private:
    struct SleepWaiter {
        time_point deadline;
        bool woken = false;
    };

    const std::thread::id mDriverThreadId = std::this_thread::get_id();
    mutable std::atomic<int64_t> mNowNs;
    mutable std::mutex mMutex;
    mutable std::condition_variable mCv;
    mutable int32_t mActiveWorkers;
    mutable int32_t mWaitingInCv = 0;
    mutable std::vector<SleepWaiter*> mSleepers;
    mutable bool mShutdown = false;
    std::function<std::optional<time_point>(time_point)> mNextVsyncAfter;
    std::function<void(time_point)> mOnVsync;
};

class SimulatedSwappyHarness {
public:
    explicit SimulatedSwappyHarness(SwappyCommonSettings settings = makeSettings(),
                                    Clock::time_point initialTime = Clock::time_point{})
          : mState(std::make_shared<PlatformSharedState>()),
            mClock(initialTime),
            mDisplayPeriod(settings.refreshPeriod),
            mAppVsyncOffset(settings.appVsyncOffset),
            mSfToVsyncDelay(settings.refreshPeriod - settings.sfVsyncOffset) {
        mState->initialSettings = settings;
        mClock.setVsyncCallbacks([this](Clock::time_point
                                                current) { return nextVsyncAfter(current); },
                                 [this](Clock::time_point) { deliverVsync(); });

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
        const auto handlers = readySwapHandlers(gpuWork);
        mSwappy->onPreSwap(handlers);
        mClock.advanceBy(swapCpuCost);
        mSwappy->onPostSwap(handlers);
    }

private:
    std::optional<Clock::time_point> nextVsyncAfter(Clock::time_point current) const {
        std::lock_guard<std::mutex> lock(mState->mutex);
        if (!mState->choreographerCallbackArmed || mDisplayPeriod <= 0ns) {
            return std::nullopt;
        }
        const int64_t periodNs = mDisplayPeriod.count();
        const int64_t offsetNs = mAppVsyncOffset.count();
        const int64_t nowNs = current.time_since_epoch().count();
        const int64_t adjusted = nowNs - offsetNs;
        int64_t k = adjusted / periodNs;
        if (adjusted % periodNs < 0) {
            --k;
        }
        int64_t nextNs = (k + 1) * periodNs + offsetNs;
        while (nextNs <= nowNs) {
            nextNs += periodNs;
        }
        return Clock::time_point(std::chrono::nanoseconds(nextNs));
    }

    void deliverVsync() {
        SwappyPlatform::ChoreographerCallback callback;
        {
            std::lock_guard<std::mutex> lock(mState->mutex);
            if (!mState->choreographerCallbackArmed || !mState->onChoreographer) {
                return;
            }
            mState->choreographerCallbackArmed = false;
            callback = mState->onChoreographer;
        }
        callback(mSfToVsyncDelay);
    }

    std::shared_ptr<PlatformSharedState> mState;
    SimulatedClock mClock;
    std::chrono::nanoseconds mDisplayPeriod;
    std::chrono::nanoseconds mAppVsyncOffset;
    std::chrono::nanoseconds mSfToVsyncDelay;
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

} // namespace

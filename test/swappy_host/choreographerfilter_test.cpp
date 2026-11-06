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

#include "ChoreographerFilter.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

#include "Clock.h"
#include "Settings.h"
#include "Thread.h"
#include "gtest/gtest.h"

namespace {

using namespace std::chrono_literals;
using swappy::ChoreographerFilter;
using swappy::Clock;
using swappy::Settings;

constexpr auto kRefreshPeriod = std::chrono::nanoseconds(16'666'667);

class AdvancingClock final : public Clock {
public:
    // Starts at start and moves on by step at each now() call. A zero step
    // freezes the clock.
    explicit AdvancingClock(std::chrono::nanoseconds start = kRefreshPeriod,
                            std::chrono::nanoseconds step = kRefreshPeriod)
          : mNanoseconds(start.count()), mStepNanoseconds(step.count()) {}

    [[nodiscard]] time_point now() const override {
        return time_point(std::chrono::nanoseconds(
                mNanoseconds.fetch_add(mStepNanoseconds, std::memory_order_relaxed)));
    }

    void sleepUntil(time_point) const override {
        mSleepCalls.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
    }

protected:
    void waitLocked(std::unique_lock<std::mutex>& lock, std::condition_variable& condition,
                    const std::function<bool()>& predicate) const override {
        if (predicate()) {
            return;
        }
        mWaitingThreads.fetch_add(1, std::memory_order_relaxed);
        condition.wait(lock, predicate);
        mWaitingThreads.fetch_sub(1, std::memory_order_relaxed);
    }

public:
    [[nodiscard]] int64_t sleepCalls() const {
        return mSleepCalls.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int32_t waitingThreads() const {
        return mWaitingThreads.load(std::memory_order_relaxed);
    }

private:
    mutable std::atomic<int64_t> mNanoseconds;
    const int64_t mStepNanoseconds;
    mutable std::atomic<int64_t> mSleepCalls{0};
    mutable std::atomic<int32_t> mWaitingThreads{0};
};

class ChoreographerFilterTest : public ::testing::Test {
protected:
    void SetUp() override {
        Settings::reset();
    }

    void TearDown() override {
        Settings::reset();
    }
};

// Spins until condition() holds. Gives up after a generous wall-clock limit so
// that a regression fails the test instead of hanging a run without a per-test
// timeout, such as `./gradlew hostUnitTests`.
template <typename Condition>
[[nodiscard]] bool spinUntil(Condition condition) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

TEST_F(ChoreographerFilterTest, IdlesAfterRepeatedUnchangedCallbacksAndWakesOnNewCallback) {
    AdvancingClock clock;
    std::atomic<int64_t> lastObservedDelayNs{-1};
    const int32_t expectedWorkers = swappy::getNumCpus() > 2 ? 2 : 1;

    ChoreographerFilter filter(
            kRefreshPeriod, 0ns,
            [&](std::optional<std::chrono::nanoseconds> sfToVsyncDelay) {
                if (sfToVsyncDelay.has_value()) {
                    lastObservedDelayNs.store(sfToVsyncDelay->count(), std::memory_order_relaxed);
                }
                return 0ns;
            },
            clock);

    // Without any callback, each worker runs 7 wakeups (initial + 6 repeats) and parks on
    // mCondition.
    constexpr int64_t kWakeupsPerCallback = 7;
    ASSERT_TRUE(spinUntil([&] {
        return clock.sleepCalls() >= expectedWorkers * kWakeupsPerCallback &&
                clock.waitingThreads() >= expectedWorkers;
    }));
    EXPECT_EQ(clock.sleepCalls(), expectedWorkers * kWakeupsPerCallback);

    // A single callback wakes all workers; each runs 7 wakeups and re-parks.
    const int64_t sleepsBeforeFirst = clock.sleepCalls();
    filter.onChoreographer(3ms);
    ASSERT_TRUE(spinUntil([&] {
        return clock.sleepCalls() >= sleepsBeforeFirst + expectedWorkers * kWakeupsPerCallback &&
                clock.waitingThreads() >= expectedWorkers;
    }));
    EXPECT_EQ(clock.sleepCalls(), sleepsBeforeFirst + expectedWorkers * kWakeupsPerCallback);
    EXPECT_EQ(lastObservedDelayNs.load(std::memory_order_relaxed),
              std::chrono::nanoseconds(3ms).count());

    // A fresh callback wakes the parked workers again with the updated sfToVsyncDelay.
    const int64_t sleepsBeforeSecond = clock.sleepCalls();
    filter.onChoreographer(5ms);
    ASSERT_TRUE(spinUntil([&] {
        return clock.sleepCalls() >= sleepsBeforeSecond + expectedWorkers * kWakeupsPerCallback &&
                clock.waitingThreads() >= expectedWorkers;
    }));
    EXPECT_EQ(clock.sleepCalls(), sleepsBeforeSecond + expectedWorkers * kWakeupsPerCallback);
    EXPECT_EQ(lastObservedDelayNs.load(std::memory_order_relaxed),
              std::chrono::nanoseconds(5ms).count());
}

TEST(ChoreographerFilterWorkDueTest, RunsWorkAtMostOncePerHalfRefreshPeriod) {
    using swappy::detail::isFilterWorkDue;
    const Clock::time_point lastWorkRun(1s);

    // A second wake-up within half a refresh period skips work, and a wake-up
    // after half a refresh period runs work again.
    EXPECT_FALSE(isFilterWorkDue(lastWorkRun, lastWorkRun, kRefreshPeriod));
    EXPECT_FALSE(isFilterWorkDue(lastWorkRun + 8ms, lastWorkRun, kRefreshPeriod));
    EXPECT_FALSE(isFilterWorkDue(lastWorkRun + kRefreshPeriod / 2, lastWorkRun, kRefreshPeriod));
    EXPECT_TRUE(
            isFilterWorkDue(lastWorkRun + kRefreshPeriod / 2 + 1ns, lastWorkRun, kRefreshPeriod));
    EXPECT_TRUE(isFilterWorkDue(lastWorkRun + 9ms, lastWorkRun, kRefreshPeriod));
}

TEST(ChoreographerFilterWorkDueTest, RunsWorkOnFirstWakeUp) {
    // The first work run is due even at epoch 0.
    EXPECT_TRUE(swappy::detail::isFilterWorkDue(Clock::time_point(), std::nullopt, kRefreshPeriod));
}

TEST_F(ChoreographerFilterTest, RunsWorkAndWakesOnACallbackOnAClockFrozenAtEpochZero) {
    // A simulated clock that stays at time_point{} (epoch 0).
    AdvancingClock clock(0ns, 0ns);
    std::atomic<int32_t> workCalls{0};
    std::atomic<int32_t> workCallsWithDelay{0};
    const int32_t expectedWorkers = swappy::getNumCpus() > 2 ? 2 : 1;

    ChoreographerFilter filter(
            kRefreshPeriod, 0ns,
            [&](std::optional<std::chrono::nanoseconds> sfToVsyncDelay) {
                workCalls.fetch_add(1, std::memory_order_relaxed);
                if (sfToVsyncDelay.has_value()) {
                    workCallsWithDelay.fetch_add(1, std::memory_order_relaxed);
                }
                return 0ns;
            },
            clock);

    // Before the first callback, each worker runs 7 wakeups and parks. Work
    // runs once at epoch 0, without a sfToVsyncDelay.
    constexpr int64_t kWakeupsPerCallback = 7;
    ASSERT_TRUE(spinUntil([&] {
        return clock.sleepCalls() >= expectedWorkers * kWakeupsPerCallback &&
                clock.waitingThreads() >= expectedWorkers;
    }));
    EXPECT_EQ(clock.sleepCalls(), expectedWorkers * kWakeupsPerCallback);
    EXPECT_EQ(workCalls.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(workCallsWithDelay.load(std::memory_order_relaxed), 0);

    // A callback at epoch 0 is a fresh callback, not a repeat of the missing
    // one, so each worker runs all 7 wakeups again before it parks.
    const int64_t sleepsBeforeCallback = clock.sleepCalls();
    filter.onChoreographer(3ms);
    ASSERT_TRUE(spinUntil([&] {
        return clock.sleepCalls() >= sleepsBeforeCallback + expectedWorkers * kWakeupsPerCallback &&
                clock.waitingThreads() >= expectedWorkers;
    }));
    EXPECT_EQ(clock.sleepCalls(), sleepsBeforeCallback + expectedWorkers * kWakeupsPerCallback);
}

TEST_F(ChoreographerFilterTest, SynchronizesShutdownFlag) {
    AdvancingClock clock;

    for (int iteration = 0; iteration < 64; ++iteration) {
        const int64_t baseSleepCalls = clock.sleepCalls();
        {
            ChoreographerFilter filter(
                    kRefreshPeriod, 0ns,
                    [](std::optional<std::chrono::nanoseconds>) { return 0ns; }, clock);
            ASSERT_TRUE(spinUntil([&] { return clock.sleepCalls() != baseSleepCalls; }));
        }
        Settings::reset();
    }

    EXPECT_GT(clock.sleepCalls(), 0);
}

TEST_F(ChoreographerFilterTest, SynchronizesVsyncDelay) {
    AdvancingClock clock;
    std::atomic<int64_t> accumulatedDelayNanoseconds{0};

    {
        ChoreographerFilter filter(
                kRefreshPeriod, 0ns,
                [&](std::optional<std::chrono::nanoseconds> sfToVsyncDelay) {
                    if (sfToVsyncDelay.has_value()) {
                        accumulatedDelayNanoseconds.fetch_add(sfToVsyncDelay->count(),
                                                              std::memory_order_relaxed);
                    }
                    std::this_thread::yield();
                    return 0ns;
                },
                clock);
        ASSERT_TRUE(spinUntil([&] { return clock.sleepCalls() > 0; }));
        // Writes at least 128 delays, and more until a worker has read one, so
        // the check below holds regardless of thread scheduling.
        int64_t delayNanoseconds = 0;
        ASSERT_TRUE(spinUntil([&] {
            if (delayNanoseconds >= 128 &&
                accumulatedDelayNanoseconds.load(std::memory_order_relaxed) > 0) {
                return true;
            }
            filter.onChoreographer(std::chrono::nanoseconds(++delayNanoseconds));
            return false;
        }));
    }

    EXPECT_GT(accumulatedDelayNanoseconds.load(std::memory_order_relaxed), 0);
}

TEST_F(ChoreographerFilterTest, SynchronizesWorkDuration) {
    AdvancingClock clock;
    std::atomic<int> completedWorkCalls{0};

    for (int iteration = 0; iteration < 16; ++iteration) {
        const int64_t targetSleepCalls = clock.sleepCalls() + 12;
        {
            ChoreographerFilter filter(
                    kRefreshPeriod, 0ns,
                    [&](std::optional<std::chrono::nanoseconds>) {
                        const int callIndex =
                                completedWorkCalls.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                        return (callIndex % 2 == 0) ? 1ms : 2ms;
                    },
                    clock);
            ASSERT_TRUE(spinUntil([&] {
                if (clock.sleepCalls() >= targetSleepCalls) {
                    return true;
                }
                filter.onChoreographer(0ns);
                return false;
            }));
        }
        Settings::reset();
    }

    EXPECT_GT(completedWorkCalls.load(std::memory_order_relaxed), 0);
}

} // namespace

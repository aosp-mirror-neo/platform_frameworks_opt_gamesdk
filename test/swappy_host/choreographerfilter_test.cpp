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
    [[nodiscard]] time_point now() const override {
        return time_point(std::chrono::nanoseconds(
                mNanoseconds.fetch_add(kRefreshPeriod.count(), std::memory_order_relaxed)));
    }

    void sleepUntil(time_point) const override {
        mSleepCalls.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
    }

    void wait(std::unique_lock<std::mutex>& lock, std::condition_variable& condition,
              const std::function<bool()>& predicate) const override {
        if (predicate()) {
            return;
        }
        mWaitingThreads.fetch_add(1, std::memory_order_relaxed);
        condition.wait(lock, predicate);
        mWaitingThreads.fetch_sub(1, std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t sleepCalls() const {
        return mSleepCalls.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int32_t waitingThreads() const {
        return mWaitingThreads.load(std::memory_order_relaxed);
    }

private:
    mutable std::atomic<int64_t> mNanoseconds{kRefreshPeriod.count()};
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

} // namespace

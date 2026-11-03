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

#include "Clock.h"

#include <condition_variable>
#include <mutex>

#include "Thread.h"
#include "gtest/gtest.h"

namespace {

using swappy::Clock;
using swappy::SteadyClock;
using swappy::Thread;

TEST(SteadyClockTest, SleepUntilPastOrCurrentDeadlineReturnsWithMonotonicNow) {
    const Clock& clock = SteadyClock::instance();
    const Clock::time_point before = clock.now();

    clock.sleepUntil(Clock::time_point{});
    clock.sleepUntil(before);

    EXPECT_GE(clock.now(), before);
}

TEST(SteadyClockTest, WaitReturnsImmediatelyWhenPredicateAlreadyHolds) {
    const Clock& clock = SteadyClock::instance();
    std::mutex mutex;
    std::condition_variable condition;
    int predicateEvaluations = 0;

    std::unique_lock<std::mutex> lock(mutex);
    clock.wait(lock, condition, [&] {
        ++predicateEvaluations;
        return true;
    });

    EXPECT_TRUE(lock.owns_lock());
    EXPECT_EQ(predicateEvaluations, 1);
}

TEST(SteadyClockTest, WaitBlocksUntilNotifiedAndReturnsWithLockOwned) {
    const Clock& clock = SteadyClock::instance();
    std::mutex mutex;
    std::condition_variable condition;
    bool ready = false;
    int predicateEvaluations = 0;
    int evaluationsBeforeNotify = 0;

    std::unique_lock<std::mutex> lock(mutex);
    Thread notifier([&] {
        std::lock_guard<std::mutex> notifierLock(mutex);
        evaluationsBeforeNotify = predicateEvaluations;
        ready = true;
        condition.notify_one();
    });
    clock.wait(lock, condition, [&] {
        ++predicateEvaluations;
        return ready;
    });
    notifier.join();

    EXPECT_TRUE(lock.owns_lock());
    EXPECT_TRUE(ready);
    EXPECT_GE(evaluationsBeforeNotify, 1);
    EXPECT_GT(predicateEvaluations, evaluationsBeforeNotify);
}

} // namespace

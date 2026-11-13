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

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "Clock.h"
#include "Thread.h"
#include "gtest/gtest.h"

namespace swappy::testing {

// Deterministic discrete-event clock coordinating Swappy driver threads and
// background worker threads without wall-clock sleeps.
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
        using namespace std::chrono_literals;
        std::unique_lock<std::mutex> lock(mMutex);
        return mCv.wait_for(lock, 5s, [&] { return mShutdown || mActiveWorkers == 0; });
    }

    void advanceBy(std::chrono::nanoseconds delta) const {
        using namespace std::chrono_literals;
        if (delta > 0ns) {
            advanceTo(now() + delta);
        } else {
            EXPECT_TRUE(waitForWorkersSettled());
        }
    }

    [[nodiscard]] bool advanceStepUpTo(time_point target) const {
        using namespace std::chrono_literals;
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

} // namespace swappy::testing

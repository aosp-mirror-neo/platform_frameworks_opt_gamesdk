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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "Clock.h"
#include "Thread.h"
#include "gtest/gtest.h"
#include "swappy/swappy_common.h"

namespace swappy::testing {

// Deterministic discrete-event clock coordinating Swappy driver threads and
// background worker threads without wall-clock sleeps. Installs Swappy thread
// hooks automatically for its lifetime so all spawned swappy::Thread workers
// (ChoreographerFilter and SwappyVk fence waiters) share one worker-accounting
// mechanism.
class SimulatedClock final : public Clock {
public:
    explicit SimulatedClock(time_point initialTime = time_point{})
          : mNowNs(initialTime.time_since_epoch().count()) {}

    ~SimulatedClock() {
        shutdown();
    }

    [[nodiscard]] time_point now() const override {
        return time_point(std::chrono::nanoseconds(mNowNs.load(std::memory_order_acquire)));
    }

    void sleepUntil(time_point deadline) const override {
        if (isDriverThread()) {
            if (!isShutdown() && now() < deadline) {
                advanceTo(deadline);
            }
            return;
        }
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
            CvWaiter waiter{callerLock.mutex(), &condition, &predicate, 0, true};
            {
                std::lock_guard<std::mutex> lock(mMutex);
                --mActiveWorkers;
                mCvWaiters.push_back(&waiter);
                mCv.notify_all();
            }
            condition.wait(callerLock);
            waiter.active = false;
            bool needDrain = false;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                const auto it = std::find(mCvWaiters.begin(), mCvWaiters.end(), &waiter);
                if (it != mCvWaiters.end()) {
                    mCvWaiters.erase(it);
                }
                ++mActiveWorkers;
                needDrain = (waiter.activeInspectors > 0);
                mCv.notify_all();
            }
            if (needDrain) {
                callerLock.unlock();
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    mCv.wait(lock, [&] { return waiter.activeInspectors == 0; });
                }
                callerLock.lock();
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
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (true) {
            std::vector<CvWaiter*> waiters;
            {
                std::unique_lock<std::mutex> lock(mMutex);
                if (!mCv.wait_until(lock, deadline,
                                    [&] { return mShutdown || mActiveWorkers == 0; })) {
                    return false;
                }
                if (mShutdown) {
                    return true;
                }
                waiters = mCvWaiters;
                for (CvWaiter* waiter : waiters) {
                    ++waiter->activeInspectors;
                }
            }

            bool anyReady = false;
            for (CvWaiter* waiter : waiters) {
                std::lock_guard<std::mutex> callerGuard(*waiter->callerMutex);
                if (!waiter->active) {
                    anyReady = true;
                } else if ((*waiter->predicate)()) {
                    waiter->condition->notify_all();
                    anyReady = true;
                }
            }

            {
                std::lock_guard<std::mutex> lock(mMutex);
                for (CvWaiter* waiter : waiters) {
                    if (--waiter->activeInspectors == 0) {
                        mCv.notify_all();
                    }
                }
                if (!anyReady && (mShutdown || (mActiveWorkers == 0 && mCvWaiters == waiters))) {
                    return true;
                }
            }

            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::yield();
        }
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
            if (!waitForWorkersSettled()) {
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
        if (mShutdown) {
            return;
        }
        mShutdown = true;
        for (SleepWaiter* waiter : mSleepers) {
            waiter->woken = true;
        }
        mSleepers.clear();
        mCv.notify_all();
    }

    [[nodiscard]] bool isShutdown() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mShutdown;
    }

private:
    struct SleepWaiter {
        time_point deadline;
        bool woken = false;
    };

    struct CvWaiter {
        std::mutex* callerMutex = nullptr;
        std::condition_variable* condition = nullptr;
        const std::function<bool()>* predicate = nullptr;
        uint32_t activeInspectors = 0;
        bool active = true;
    };

    class ThreadHookScope {
    public:
        explicit ThreadHookScope(SimulatedClock& clock) : mClock(clock) {
            bool installGlobalHooks = false;
            {
                std::lock_guard<std::mutex> lock(registry().mutex);
                mPreviousClock = registry().activeClock;
                registry().activeClock = &mClock;
                installGlobalHooks = (mPreviousClock == nullptr && registry().threads.empty());
            }
            if (installGlobalHooks) {
                Swappy_setThreadFunctions(&kThreadFunctions);
            }
        }

        ~ThreadHookScope() {
            mClock.shutdown();
            std::vector<ThreadEntry> leftoverThreads;
            {
                std::lock_guard<std::mutex> lock(registry().mutex);
                if (registry().activeClock == &mClock) {
                    registry().activeClock = mPreviousClock;
                }
                for (auto it = registry().threads.begin(); it != registry().threads.end();) {
                    if (it->second.ownerClock == &mClock) {
                        leftoverThreads.push_back(std::move(it->second));
                        it = registry().threads.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (auto& entry : leftoverThreads) {
                if (entry.worker.joinable()) {
                    entry.worker.join();
                }
            }
            bool clearGlobalHooks = false;
            {
                std::lock_guard<std::mutex> lock(registry().mutex);
                clearGlobalHooks =
                        (registry().activeClock == nullptr && registry().threads.empty());
            }
            if (clearGlobalHooks) {
                Swappy_setThreadFunctions(nullptr);
            }
        }

        ThreadHookScope(const ThreadHookScope&) = delete;
        ThreadHookScope& operator=(const ThreadHookScope&) = delete;

    private:
        struct ThreadEntry {
            const SimulatedClock* ownerClock = nullptr;
            std::thread worker;
            std::shared_ptr<std::atomic<bool>> exited;
        };

        struct Registry {
            std::mutex mutex;
            const SimulatedClock* activeClock = nullptr;
            SwappyThreadId nextId = 1;
            std::map<SwappyThreadId, ThreadEntry> threads;
        };

        static Registry& registry() {
            static Registry instance;
            return instance;
        }

        static int start(SwappyThreadId* threadId, void* (*threadFunc)(void*), void* userData) {
            auto exited = std::make_shared<std::atomic<bool>>(false);
            std::lock_guard<std::mutex> lock(registry().mutex);
            const SimulatedClock* ownerClock = registry().activeClock;
            if (ownerClock != nullptr) {
                ownerClock->onWorkerThreadStarting();
            }
            const SwappyThreadId id = registry().nextId++;
            std::thread worker([threadFunc, userData, exited, ownerClock]() {
                threadFunc(userData);
                exited->store(true, std::memory_order_release);
                std::lock_guard<std::mutex> exitLock(registry().mutex);
                if (ownerClock != nullptr) {
                    ownerClock->onWorkerThreadExiting();
                }
            });
            registry().threads.emplace(id,
                                       ThreadEntry{ownerClock, std::move(worker),
                                                   std::move(exited)});
            *threadId = id;
            return 0;
        }

        static void join(SwappyThreadId threadId) {
            ThreadEntry entry;
            {
                std::lock_guard<std::mutex> lock(registry().mutex);
                const auto it = registry().threads.find(threadId);
                if (it == registry().threads.end()) {
                    return;
                }
                entry = std::move(it->second);
                registry().threads.erase(it);
            }
            const SimulatedClock* clock = entry.ownerClock;
            if (clock != nullptr && clock->isDriverThread()) {
                while (!entry.exited->load(std::memory_order_acquire) && !clock->isShutdown()) {
                    if (!clock->waitForWorkersSettled()) {
                        break;
                    }
                    if (entry.exited->load(std::memory_order_acquire) || clock->isShutdown()) {
                        break;
                    }
                    if (!clock->advanceStepUpTo(Clock::time_point::max())) {
                        break;
                    }
                }
            }
            if (entry.worker.joinable()) {
                entry.worker.join();
            }
        }

        static bool joinable(SwappyThreadId threadId) {
            std::lock_guard<std::mutex> lock(registry().mutex);
            const auto it = registry().threads.find(threadId);
            return it != registry().threads.end() && it->second.worker.joinable();
        }

        static inline const SwappyThreadFunctions kThreadFunctions{
                &ThreadHookScope::start,
                &ThreadHookScope::join,
                &ThreadHookScope::joinable,
        };

        SimulatedClock& mClock;
        const SimulatedClock* mPreviousClock = nullptr;
    };

    [[nodiscard]] bool isDriverThread() const noexcept {
        return std::this_thread::get_id() == mDriverThreadId;
    }

    void onWorkerThreadStarting() const {
        std::lock_guard<std::mutex> lock(mMutex);
        ++mActiveWorkers;
    }

    void onWorkerThreadExiting() const {
        std::lock_guard<std::mutex> lock(mMutex);
        --mActiveWorkers;
        mCv.notify_all();
    }

    const std::thread::id mDriverThreadId = std::this_thread::get_id();
    mutable std::atomic<int64_t> mNowNs;
    mutable std::mutex mMutex;
    mutable std::condition_variable mCv;
    mutable int32_t mActiveWorkers = 0;
    mutable std::vector<CvWaiter*> mCvWaiters;
    mutable std::vector<SleepWaiter*> mSleepers;
    mutable bool mShutdown = false;
    std::function<std::optional<time_point>(time_point)> mNextVsyncAfter;
    std::function<void(time_point)> mOnVsync;
    ThreadHookScope mThreadHooks{*this};
};

} // namespace swappy::testing

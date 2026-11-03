/*
 * Copyright 2018 The Android Open Source Project
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

#define LOG_TAG "ChoreographerFilter"

#include <sched.h>
#include <unistd.h>

#include <deque>
#include <string>

#include "ChoreographerTimer.h"
#include "Settings.h"
#include "SwappyLog.h"
#include "Thread.h"
#include "Trace.h"

namespace swappy {

ChoreographerFilter::ChoreographerFilter(std::chrono::nanoseconds refreshPeriod,
                                         std::chrono::nanoseconds appToSfDelay, Worker doWork,
                                         const Clock& clock)
      : mRefreshPeriod(refreshPeriod), mAppToSfDelay(appToSfDelay), mDoWork(doWork), mClock(clock) {
    Settings::getInstance()->addListener([this]() { onSettingsChanged(); });

    std::lock_guard<std::mutex> lock(mThreadPoolMutex);
    mUseAffinity = Settings::getInstance()->getUseAffinity();
    launchThreadsLocked();
}

ChoreographerFilter::~ChoreographerFilter() {
    std::lock_guard<std::mutex> lock(mThreadPoolMutex);
    terminateThreadsLocked();
}

void ChoreographerFilter::onChoreographer(std::optional<std::chrono::nanoseconds> sfToVsyncDelay) {
    std::lock_guard<std::mutex> lock(mMutex);
    mLastTimestamp = mClock.now();
    mSfToVsyncDelay = sfToVsyncDelay;
    ++mSequenceNumber;
    mCondition.notify_all();
}

void ChoreographerFilter::launchThreadsLocked() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mIsRunning = true;
    }

    const int32_t numThreads = getNumCpus() > 2 ? 2 : 1;
    for (int32_t thread = 0; thread < numThreads; ++thread) {
        mThreadPool.push_back(Thread([this, thread]() { threadMain(mUseAffinity, thread); }));
    }
}

void ChoreographerFilter::terminateThreadsLocked() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mIsRunning = false;
        mCondition.notify_all();
    }

    for (auto& thread : mThreadPool) {
        thread.join();
    }
    mThreadPool.clear();
}

void ChoreographerFilter::onSettingsChanged() {
    const bool useAffinity = Settings::getInstance()->getUseAffinity();
    const Settings::DisplayTimings displayTimings = Settings::getInstance()->getDisplayTimings();
    std::lock_guard<std::mutex> lock(mThreadPoolMutex);
    if (useAffinity == mUseAffinity && mRefreshPeriod == displayTimings.refreshPeriod) {
        return;
    }

    terminateThreadsLocked();
    mUseAffinity = useAffinity;
    mRefreshPeriod = displayTimings.refreshPeriod;
    mAppToSfDelay = displayTimings.sfOffset - displayTimings.appOffset;
    SWAPPY_LOGV("onSettingsChanged(): refreshPeriod=%lld, appOffset=%lld, "
                "sfOffset=%lld",
                (long long)displayTimings.refreshPeriod.count(),
                (long long)displayTimings.appOffset.count(),
                (long long)displayTimings.sfOffset.count());
    launchThreadsLocked();
}

void ChoreographerFilter::threadMain(bool useAffinity, int32_t thread) {
    ChoreographerTimer timer(mRefreshPeriod, mAppToSfDelay, mClock.now());

    {
        int cpu = getNumCpus() - 1 - thread;
        if (cpu >= 0) {
            setAffinity(cpu);
        }
    }

    std::string threadName = "Filter";
    threadName += std::to_string(thread);
    setThreadName(threadName.c_str());

    std::unique_lock<std::mutex> lock(mMutex);
    while (true) {
        auto timestamp = mLastTimestamp;
        auto workDuration = mWorkDuration;
        lock.unlock();

        // If we have received the same timestamp multiple times, it probably
        // means that the app has stopped sending them to us, which could
        // indicate that it's no longer running. If we detect that, we stop
        // until we see a fresh timestamp to avoid spinning forever in the
        // background.
        if (!timer.addTimestamp(timestamp)) {
            lock.lock();
            mClock.wait(lock, mCondition,
                        [=]() { return !mIsRunning || (mLastTimestamp != timestamp); });
            timestamp = mLastTimestamp;
            lock.unlock();
            timer.addTimestamp(timestamp);
        }

        if (!mIsRunning) break;

        mClock.sleepUntil(timer.wakeTime(-workDuration, mClock.now()));
        {
            std::unique_lock<std::mutex> workLock(mWorkMutex);
            const auto now = mClock.now();
            if (now - mLastWorkRun > mRefreshPeriod / 2) {
                // Assume we got here first and there's work to do
                gamesdk::ScopedTrace trace("doWork");
                mWorkDuration = mDoWork(mSfToVsyncDelay);
                mLastWorkRun = now;
            }
        }
        lock.lock();
    }
}

} // namespace swappy

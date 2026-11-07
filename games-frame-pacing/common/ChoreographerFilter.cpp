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
    const bool hasDisplayTimings = displayTimings.refreshPeriod > std::chrono::nanoseconds::zero();
    const std::chrono::nanoseconds appToSfDelay =
            displayTimings.sfOffset - displayTimings.appOffset;

    std::lock_guard<std::mutex> poolLock(mThreadPoolMutex);
    if (useAffinity != mUseAffinity) {
        terminateThreadsLocked();
        mUseAffinity = useAffinity;
        if (hasDisplayTimings) {
            std::lock_guard<std::mutex> lock(mMutex);
            mRefreshPeriod = displayTimings.refreshPeriod;
            mAppToSfDelay = appToSfDelay;
            ++mSettingsGeneration;
        }
        launchThreadsLocked();
        return;
    }

    if (!hasDisplayTimings) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mRefreshPeriod == displayTimings.refreshPeriod && mAppToSfDelay == appToSfDelay) {
            return;
        }
        mRefreshPeriod = displayTimings.refreshPeriod;
        mAppToSfDelay = appToSfDelay;
        ++mSettingsGeneration;
    }

    SWAPPY_LOGV("onSettingsChanged(): refreshPeriod=%lld, appOffset=%lld, "
                "sfOffset=%lld",
                (long long)displayTimings.refreshPeriod.count(),
                (long long)displayTimings.appOffset.count(),
                (long long)displayTimings.sfOffset.count());
}

void ChoreographerFilter::threadMain(bool useAffinity, int32_t thread) {
    if (useAffinity) {
        int cpu = getNumCpus() - 1 - thread;
        if (cpu >= 0) {
            setAffinity(cpu);
        }
    }

    std::string threadName = "Filter";
    threadName += std::to_string(thread);
    setThreadName(threadName.c_str());

    const auto initialNow = mClock.now();
    std::chrono::nanoseconds refreshPeriod;
    std::chrono::nanoseconds appToSfDelay;
    uint64_t settingsGeneration;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        refreshPeriod = mRefreshPeriod;
        appToSfDelay = mAppToSfDelay;
        settingsGeneration = mSettingsGeneration;
    }
    ChoreographerTimer timer(refreshPeriod, appToSfDelay, initialNow);

    auto syncSettingsLocked = [&]() REQUIRES(mMutex) {
        if (settingsGeneration != mSettingsGeneration) {
            refreshPeriod = mRefreshPeriod;
            appToSfDelay = mAppToSfDelay;
            settingsGeneration = mSettingsGeneration;
            timer.setDisplayTimings(refreshPeriod, appToSfDelay);
        }
    };

    while (true) {
        int64_t sequenceNumber;
        std::optional<std::chrono::steady_clock::time_point> timestamp;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!mIsRunning) break;
            syncSettingsLocked();
            sequenceNumber = mSequenceNumber;
            timestamp = mLastTimestamp;
        }
        auto workDuration = mWorkDuration.load();

        // If we have received the same timestamp multiple times, it probably
        // means that the app has stopped sending them to us, which could
        // indicate that it's no longer running. If we detect that, we stop
        // until we see a fresh timestamp to avoid spinning forever in the
        // background.
        if (!timer.addTimestamp(timestamp, sequenceNumber)) {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mClock.wait(mMutex, mCondition, [this, sequenceNumber]() REQUIRES(mMutex) {
                    return !mIsRunning || (mSequenceNumber != sequenceNumber);
                });
                if (!mIsRunning) break;
                syncSettingsLocked();
                sequenceNumber = mSequenceNumber;
                timestamp = mLastTimestamp;
            }
            workDuration = mWorkDuration.load();
            timer.addTimestamp(timestamp, sequenceNumber);
        }

        mClock.sleepUntil(timer.wakeTime(-workDuration, mClock.now()));

        std::optional<std::chrono::nanoseconds> sfToVsyncDelay;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!mIsRunning) break;
            syncSettingsLocked();
            sfToVsyncDelay = mSfToVsyncDelay;
        }

        {
            std::lock_guard<std::mutex> workLock(mWorkMutex);
            const auto now = mClock.now();
            if (detail::isFilterWorkDue(now, mLastWorkRun, refreshPeriod)) {
                // Assume we got here first and there's work to do
                gamesdk::ScopedTrace trace("doWork");
                mWorkDuration.store(mDoWork(sfToVsyncDelay));
                mLastWorkRun = now;
            }
        }
    }
}

} // namespace swappy

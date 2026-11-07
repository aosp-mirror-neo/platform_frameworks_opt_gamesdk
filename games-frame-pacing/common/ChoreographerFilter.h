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

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "Clock.h"
#include "Settings.h"
#include "Thread.h"

namespace swappy {

namespace detail {

// Returns true if a filter worker that wakes at `now` must run the work
// callback. Work runs at most once per half refresh period, so only the first
// worker that wakes for a vsync runs it. lastWorkRun is std::nullopt before
// the first work run.
inline bool isFilterWorkDue(std::chrono::steady_clock::time_point now,
                            std::optional<std::chrono::steady_clock::time_point> lastWorkRun,
                            std::chrono::nanoseconds refreshPeriod) {
    return !lastWorkRun || now - *lastWorkRun > refreshPeriod / 2;
}

} // namespace detail

class ChoreographerFilter {
public:
    using Worker = std::function<std::chrono::nanoseconds(std::optional<std::chrono::nanoseconds>)>;

    explicit ChoreographerFilter(std::chrono::nanoseconds refreshPeriod,
                                 std::chrono::nanoseconds appToSfDelay, Worker doWork,
                                 const Clock& clock = SteadyClock::instance());
    ~ChoreographerFilter();

    void onChoreographer(std::optional<std::chrono::nanoseconds> sfToVsyncDelay);

private:
    void launchThreadsLocked();
    void terminateThreadsLocked();

    void onSettingsChanged();

    void threadMain(bool useAffinity, int32_t thread) EXCLUDES(mMutex, mWorkMutex);

    std::mutex mThreadPoolMutex;
    bool mUseAffinity = true;
    std::vector<Thread> mThreadPool;

    std::mutex mMutex;
    std::condition_variable mCondition;
    bool mIsRunning GUARDED_BY(mMutex) = true;
    int64_t mSequenceNumber GUARDED_BY(mMutex) = 0;
    // Time of the latest callback, or std::nullopt before the first one.
    std::optional<std::chrono::steady_clock::time_point> mLastTimestamp GUARDED_BY(mMutex);
    std::optional<std::chrono::nanoseconds> mSfToVsyncDelay GUARDED_BY(mMutex);
    uint64_t mSettingsGeneration GUARDED_BY(mMutex) = 0;
    std::chrono::nanoseconds mRefreshPeriod GUARDED_BY(mMutex);
    std::chrono::nanoseconds mAppToSfDelay GUARDED_BY(mMutex);

    std::mutex mWorkMutex;
    // Time of the latest work run, or std::nullopt before the first one.
    std::optional<std::chrono::steady_clock::time_point> mLastWorkRun GUARDED_BY(mWorkMutex);
    // Lead duration of the last work run. It is lock-free because workers
    // read it before sleeping without the work mutex.
    std::atomic<std::chrono::nanoseconds> mWorkDuration{};

    const Worker mDoWork;
    const Clock& mClock;
};

} // namespace swappy

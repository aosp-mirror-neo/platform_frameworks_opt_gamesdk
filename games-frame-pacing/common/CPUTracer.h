/*
 * Copyright 2019 The Android Open Source Project
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

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

#include "Clock.h"
#include "Thread.h"

namespace swappy {

class CPUTracer {
public:
    explicit CPUTracer(const Clock& clock = SteadyClock::instance());
    ~CPUTracer();

    CPUTracer(CPUTracer&) = delete;

    void startTrace();
    void endTrace();

private:
    void threadMain();
    void joinThread();

    const Clock& mClock;
    std::mutex mMutex;
    std::condition_variable mCond;
    std::unique_ptr<Thread> mThread;
    bool mRunning GUARDED_BY(mMutex) = true;
    bool mTrace GUARDED_BY(mMutex) = false;
    // Counts the false -> true changes of mTrace, so the worker can detect a
    // new trace when it did not observe the end of the previous one.
    uint64_t mTraceGeneration GUARDED_BY(mMutex) = 0;
};

} // namespace swappy

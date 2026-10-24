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

#include "CPUTracer.h"

#include <memory>

#include "Trace.h"

namespace swappy {

CPUTracer::CPUTracer(const Clock& clock) : mClock(clock) {}

CPUTracer::~CPUTracer() {
    joinThread();
}

void CPUTracer::joinThread() {
    bool join = false;
    if (mThread && mThread->joinable()) {
        std::lock_guard<std::mutex> lock(mMutex);
        mTrace = false;
        mRunning = false;
        mCond.notify_one();
        join = true;
    }
    if (join) {
        mThread->join();
    }
    mThread.reset();
}

void CPUTracer::startTrace() {
    if (TRACE_ENABLED()) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mThread) {
            mRunning = true;
            mThread = std::make_unique<Thread>([this]() { threadMain(); });
        }
        if (!mTrace) {
            ++mTraceGeneration;
        }
        mTrace = true;
        mCond.notify_one();
    } else {
        joinThread();
    }
}

void CPUTracer::endTrace() {
    if (TRACE_ENABLED()) {
        std::lock_guard<std::mutex> lock(mMutex);
        mTrace = false;
        mCond.notify_one();
    } else {
        joinThread();
    }
}

void CPUTracer::threadMain() {
    std::lock_guard<std::mutex> lock(mMutex);
    while (mRunning) {
        if (mTrace) {
            const uint64_t generation = mTraceGeneration;
            TRACE_SCOPE("Swappy: CPU frame time");
            mClock.wait(mMutex, mCond, [this, generation]() REQUIRES(mMutex) {
                return !mRunning || !mTrace || mTraceGeneration != generation;
            });
        } else {
            mClock.wait(mMutex, mCond, [this]() REQUIRES(mMutex) { return !mRunning || mTrace; });
        }
    }
}

} // namespace swappy

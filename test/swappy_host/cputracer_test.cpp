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

#include "CPUTracer.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "Clock.h"
#include "TraceTestHelper.h"
#include "gtest/gtest.h"

namespace {

using gamesdk::test::RecordedTraceCallbacks;
using gamesdk::test::ScopedMockTrace;
using swappy::Clock;
using swappy::CPUTracer;

// Clock that parks each wait() of a worker thread until the test lets the
// worker evaluate its predicate again. The test thus controls which state
// changes the worker observes.
class GatedClock final : public Clock {
public:
    [[nodiscard]] time_point now() const override {
        return time_point{};
    }

    void sleepUntil(time_point) const override {}

    // Blocks until the worker parked parkCount times in total. The worker
    // does not hold the mutex of its wait() while it is parked.
    void awaitParked(int parkCount) const {
        std::unique_lock<std::mutex> gate(mGateMutex);
        mGateCond.wait(gate, [&] { return mParkCount >= parkCount; });
    }

    // Lets the parked worker evaluate its predicate one time.
    void releaseOnce() const {
        std::lock_guard<std::mutex> gate(mGateMutex);
        ++mPermits;
        mGateCond.notify_all();
    }

    // Stops the parking. Subsequent waits block on their condition variable.
    void open() const {
        std::lock_guard<std::mutex> gate(mGateMutex);
        mOpen = true;
        mGateCond.notify_all();
    }

protected:
    void waitLocked(std::unique_lock<std::mutex>& lock, std::condition_variable& condition,
                    const std::function<bool()>& predicate) const override {
        while (!predicate()) {
            std::unique_lock<std::mutex> gate(mGateMutex);
            if (mOpen) {
                gate.unlock();
                condition.wait(lock, predicate);
                return;
            }
            ++mParkCount;
            mGateCond.notify_all();
            lock.unlock();
            mGateCond.wait(gate, [&] { return mOpen || mPermits > 0; });
            if (mPermits > 0) {
                --mPermits;
            }
            gate.unlock();
            lock.lock();
        }
    }

private:
    mutable std::mutex mGateMutex;
    mutable std::condition_variable mGateCond;
    mutable int mParkCount = 0;
    mutable int mPermits = 0;
    mutable bool mOpen = false;
};

TEST(CPUTracerHostTest, StartAfterEndThatWorkerDidNotObserveClosesSliceAndOpensNewOne) {
    ScopedMockTrace mockTrace(true);
    GatedClock clock;
    CPUTracer tracer(clock);

    tracer.startTrace();
    clock.awaitParked(1);
    const std::vector<std::string> firstTraceEvents = RecordedTraceCallbacks::eventsSnapshot();

    // The worker is parked, so it evaluates its predicate only after the new
    // trace started.
    tracer.endTrace();
    tracer.startTrace();
    clock.releaseOnce();
    clock.awaitParked(2);
    const std::vector<std::string> secondTraceEvents = RecordedTraceCallbacks::eventsSnapshot();

    tracer.endTrace();
    clock.releaseOnce();
    clock.awaitParked(3);
    const std::vector<std::string> endedTraceEvents = RecordedTraceCallbacks::eventsSnapshot();
    clock.open();

    EXPECT_EQ(firstTraceEvents, (std::vector<std::string>{"B:Swappy: CPU frame time"}));
    EXPECT_EQ(secondTraceEvents,
              (std::vector<std::string>{"B:Swappy: CPU frame time", "E",
                                        "B:Swappy: CPU frame time"}));
    EXPECT_EQ(endedTraceEvents,
              (std::vector<std::string>{"B:Swappy: CPU frame time", "E", "B:Swappy: CPU frame time",
                                        "E"}));
}

TEST(CPUTracerHostTest, RepeatedStartWhileActiveKeepsSliceOpen) {
    ScopedMockTrace mockTrace(true);
    GatedClock clock;
    CPUTracer tracer(clock);

    tracer.startTrace();
    clock.awaitParked(1);

    tracer.startTrace();
    clock.releaseOnce();
    clock.awaitParked(2);
    const std::vector<std::string> events = RecordedTraceCallbacks::eventsSnapshot();
    clock.open();

    EXPECT_EQ(events, (std::vector<std::string>{"B:Swappy: CPU frame time"}));
}

} // namespace

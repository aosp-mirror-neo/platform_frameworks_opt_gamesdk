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
#include <chrono>
#include <deque>

#include "Clock.h"
#include "PacingImplementation.h"

namespace swappy::testing {

// Models serialized FIFO GPU queue execution and fence wait durations matching
// SwappyVkBase::injectFence and waitForFenceThreadMain.
class SimulatedGpuQueue {
public:
    void submit(Clock::time_point submitTime, std::chrono::nanoseconds gpuWork) {
        using namespace std::chrono_literals;
        const Clock::time_point waitStartTime =
                mHasSubmitted ? std::max(submitTime, mLastScheduledCompletion) : submitTime;
        const Clock::time_point completionTime = waitStartTime + std::max(0ns, gpuWork);
        mInFlight.push_back(InFlightFence{waitStartTime, completionTime});
        mLastScheduledCompletion = completionTime;
        mHasSubmitted = true;
    }

    [[nodiscard]] bool lastFrameIsComplete(Clock::time_point now, PipelineMode pipelineMode) {
        reclaimCompleted(now);
        if (pipelineMode == PipelineMode::On) {
            return mInFlight.size() < 2;
        }
        return mInFlight.empty();
    }

    [[nodiscard]] std::chrono::nanoseconds lastFenceTime(Clock::time_point now) {
        reclaimCompleted(now);
        return mLastFenceTime;
    }

private:
    struct InFlightFence {
        Clock::time_point waitStartTime{};
        Clock::time_point completionTime{};
    };

    void reclaimCompleted(Clock::time_point now) {
        while (!mInFlight.empty() && mInFlight.front().completionTime <= now) {
            const InFlightFence completed = mInFlight.front();
            mInFlight.pop_front();
            mLastFenceTime = completed.completionTime - completed.waitStartTime;
        }
    }

    std::deque<InFlightFence> mInFlight;
    Clock::time_point mLastScheduledCompletion{};
    std::chrono::nanoseconds mLastFenceTime{0};
    bool mHasSubmitted = false;
};

} // namespace swappy::testing

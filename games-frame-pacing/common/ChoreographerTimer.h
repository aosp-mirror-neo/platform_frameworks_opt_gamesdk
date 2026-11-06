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

#include <chrono>
#include <cstdint>

namespace swappy {

// Estimates display vsync times from Choreographer callback timestamps: when a
// filter loop should wake up next, and whether callbacks have stopped arriving.
// The caller supplies all timestamps, so the timer runs the same way on the
// steady clock and on a test clock.
class ChoreographerTimer {
public:
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::nanoseconds;

    // Uses now as the initial vsync base until callbacks arrive.
    ChoreographerTimer(duration refreshPeriod, duration appToSfDelay, time_point now)
          : mRefreshPeriod(refreshPeriod),
            mAppToSfDelay(appToSfDelay),
            mBaseTime(now),
            mLastTimestamp(now) {}

    // Returns false if we have detected that we have received the same
    // timestamp multiple times so that the caller can wait for fresh timestamps
    bool addTimestamp(time_point point, int64_t sequenceNumber = 0) {
        // Keep track of the previous timestamp and how many times we've seen it
        // to determine if we've stopped receiving Choreographer callbacks,
        // which would indicate that we should probably stop until we see them
        // again (e.g., if the app has been moved to the background)
        if (point == mLastTimestamp && sequenceNumber == mLastSequenceNumber) {
            if (mRepeatCount++ > 5) {
                return false;
            }
        } else {
            mRepeatCount = 0;
        }
        mLastTimestamp = point;
        mLastSequenceNumber = sequenceNumber;

        point += mAppToSfDelay;

        bool moreThanOneRefreshPeriodElapsed = mBaseTime + mRefreshPeriod * 1.5 < point;
        if (moreThanOneRefreshPeriodElapsed) {
            do {
                mBaseTime += mRefreshPeriod;
            } while (mBaseTime + mRefreshPeriod * 1.5 < point);
            mBaseTime += mRefreshPeriod;
            // Long waits pollute the filter so don't adjust refreshPeriod.
            return true;
        }

        std::chrono::nanoseconds delta = (point - (mBaseTime + mRefreshPeriod));
        if (delta < -mRefreshPeriod / 2) {
            // Also ignore short intervals
            return true;
        }

        // Exponential smoothing factor = 0.04 avoids roughness.
        mRefreshPeriod += delta / 25;
        mBaseTime += mRefreshPeriod;

        return true;
    }

    // Returns when to wake up: offset from the next vsync, moved on by whole
    // refresh periods until it is no earlier than now. Offsets exceeding half a
    // refresh period in either direction count as zero.
    [[nodiscard]] time_point wakeTime(duration offset, time_point now) const {
        if (offset < -(mRefreshPeriod / 2) || offset > mRefreshPeriod / 2) {
            offset = duration::zero();
        }

        auto targetTime = mBaseTime + mRefreshPeriod + offset;
        while (targetTime < now) {
            targetTime += mRefreshPeriod;
        }
        return targetTime;
    }

private:
    duration mRefreshPeriod;
    const duration mAppToSfDelay;
    time_point mBaseTime;

    time_point mLastTimestamp;
    int64_t mLastSequenceNumber = 0;
    int32_t mRepeatCount = 0;
};

} // namespace swappy

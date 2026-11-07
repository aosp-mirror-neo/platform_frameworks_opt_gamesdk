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
#include <optional>

namespace swappy {

// Estimates display vsync times from Choreographer callback timestamps: when a
// filter loop should wake up next, and whether callbacks have stopped arriving.
// The caller supplies all timestamps, so the timer runs the same way on the
// steady clock and on a test clock, including one that starts at time_point{}
// (epoch 0).
class ChoreographerTimer {
public:
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::nanoseconds;

    // Uses now as the initial vsync base until callbacks arrive.
    ChoreographerTimer(duration refreshPeriod, duration appToSfDelay, time_point now)
          : mRefreshPeriod(refreshPeriod), mAppToSfDelay(appToSfDelay), mBaseTime(now) {}

    void setDisplayTimings(duration refreshPeriod, duration appToSfDelay) {
        if (refreshPeriod <= duration::zero()) {
            return;
        }
        if (mRefreshPeriod != refreshPeriod || mAppToSfDelay != appToSfDelay) {
            mRefreshPeriod = refreshPeriod;
            mAppToSfDelay = appToSfDelay;
            mRebaseOnNextTimestamp = true;
        }
    }

    // Records the latest callback timestamp, or std::nullopt when no callback
    // has arrived yet, with the callback sequence number. Returns false after
    // six repeats of the same timestamp and sequence number so the caller can
    // wait for a fresh callback. A new sequence number at the same timestamp,
    // as on a coarse or frozen clock, counts as a fresh callback.
    bool addTimestamp(std::optional<time_point> timestamp, int64_t sequenceNumber = 0) {
        // Keep track of the previous timestamp and how many times we've seen it
        // to determine if we've stopped receiving Choreographer callbacks,
        // which would indicate that we should probably stop until we see them
        // again (e.g., if the app has been moved to the background)
        if (mLastTimestamp == timestamp && mLastSequenceNumber == sequenceNumber) {
            if (mSameTimestampCount > 6) {
                return false;
            }
            ++mSameTimestampCount;
            return true;
        }
        mLastTimestamp = timestamp;
        mLastSequenceNumber = sequenceNumber;
        mSameTimestampCount = 1;
        if (!timestamp.has_value()) {
            return true;
        }

        if (mRefreshPeriod <= duration::zero()) {
            mBaseTime = *timestamp;
            return true;
        }

        const time_point point = *timestamp + mAppToSfDelay;
        if (mRebaseOnNextTimestamp) {
            mBaseTime = point;
            mRebaseOnNextTimestamp = false;
            return true;
        }

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
    // refresh periods until it lies strictly after now, so a loop that wakes
    // exactly at its target, as on a simulated clock, does not wake again at
    // the same instant. Offsets exceeding half a refresh period in either
    // direction count as zero.
    [[nodiscard]] time_point wakeTime(duration offset, time_point now) const {
        if (mRefreshPeriod <= duration::zero()) {
            return now;
        }
        if (offset < -(mRefreshPeriod / 2) || offset > mRefreshPeriod / 2) {
            offset = duration::zero();
        }

        auto targetTime = mBaseTime + mRefreshPeriod + offset;
        while (targetTime <= now) {
            targetTime += mRefreshPeriod;
        }
        return targetTime;
    }

private:
    duration mRefreshPeriod;
    duration mAppToSfDelay;
    time_point mBaseTime;

    std::optional<time_point> mLastTimestamp;
    int64_t mLastSequenceNumber = 0;
    int32_t mSameTimestampCount = 0;
    bool mRebaseOnNextTimestamp = false;
};

} // namespace swappy

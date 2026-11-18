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
#include <cmath>
#include <deque>
#include <utility>

// The frame-time accumulator behind Swappy's swap interval decision.
//
// Kept free of platform dependencies (no JNI, ChoreographerThread, or
// SwappyDisplayManager headers) so host tests can exercise the two-second
// sample window and the clear-on-change behaviour directly: together they are
// what make a bad swap interval persist rather than correct itself on the
// next frame.

namespace swappy {

// At namespace scope rather than nested in SwappyCommon, because
// FrameDuration::getTime() takes it.
enum class PipelineMode { Off, On };

// Added to every frame time as a safety margin against the measured cost being
// an underestimate. `inline` because FrameDuration::getTime() below is an
// inline function that reads it, so all translation units must agree on one
// entity rather than each getting an internal-linkage copy.
inline constexpr std::chrono::nanoseconds FRAME_MARGIN = std::chrono::milliseconds(1);

class FrameDuration {
public:
    FrameDuration() = default;

    FrameDuration(std::chrono::nanoseconds cpuTime, std::chrono::nanoseconds gpuTime,
                  bool frameMissedDeadline)
          : mCpuTime(cpuTime), mGpuTime(gpuTime), mFrameMissedDeadline(frameMissedDeadline) {
        mCpuTime = std::min(mCpuTime, MAX_DURATION);
        mGpuTime = std::min(mGpuTime, MAX_DURATION);
    }

    std::chrono::nanoseconds getCpuTime() const {
        return mCpuTime;
    }
    std::chrono::nanoseconds getGpuTime() const {
        return mGpuTime;
    }

    bool frameMiss() const {
        return mFrameMissedDeadline;
    }

    std::chrono::nanoseconds getTime(PipelineMode pipeline) const {
        if (mCpuTime == std::chrono::nanoseconds(0) && mGpuTime == std::chrono::nanoseconds(0)) {
            return std::chrono::nanoseconds(0);
        }

        if (pipeline == PipelineMode::On) {
            return std::max(mCpuTime, mGpuTime) + FRAME_MARGIN;
        }

        return mCpuTime + mGpuTime + FRAME_MARGIN;
    }

    FrameDuration& operator+=(const FrameDuration& other) {
        mCpuTime += other.mCpuTime;
        mGpuTime += other.mGpuTime;
        return *this;
    }

    FrameDuration& operator-=(const FrameDuration& other) {
        mCpuTime -= other.mCpuTime;
        mGpuTime -= other.mGpuTime;
        return *this;
    }

    friend FrameDuration operator/(FrameDuration lhs, int rhs) {
        lhs.mCpuTime /= rhs;
        lhs.mGpuTime /= rhs;
        return lhs;
    }

private:
    std::chrono::nanoseconds mCpuTime = std::chrono::nanoseconds(0);
    std::chrono::nanoseconds mGpuTime = std::chrono::nanoseconds(0);
    bool mFrameMissedDeadline = false;

    static constexpr std::chrono::nanoseconds MAX_DURATION = std::chrono::milliseconds(100);
};

class FrameDurations {
public:
    using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;

    // The caller supplies the sample timestamp rather than add() reading
    // steady_clock::now() itself, so host tests can drive a multi-minute
    // timeline in microseconds. Timestamps must be non-decreasing across
    // calls: the eviction loop below and hasEnoughSamples() both measure
    // spans between stored samples and will under-report the window if time
    // goes backwards. SwappyCommon reads the clock under mMutex, which gives
    // this ordering for free on the production path.
    void add(FrameDuration frameDuration, TimePoint now) {
        mFrames.push_back({now, frameDuration});
        mFrameDurationsSum += frameDuration;
        if (frameDuration.frameMiss()) {
            mMissedFrameCount++;
        }

        // Note the `begin() + 1` and the `size() >= 2` guard: one sample older
        // than the window is deliberately retained, so that hasEnoughSamples()
        // can still measure a span of at least FRAME_DURATION_SAMPLE_SECONDS.
        while (mFrames.size() >= 2 &&
               now - (mFrames.begin() + 1)->first > FRAME_DURATION_SAMPLE_SECONDS) {
            mFrameDurationsSum -= mFrames.front().second;
            if (mFrames.front().second.frameMiss()) {
                mMissedFrameCount--;
            }
            mFrames.pop_front();
        }
    }

    bool hasEnoughSamples() const {
        return (!mFrames.empty()) &&
                (mFrames.back().first - mFrames.front().first > FRAME_DURATION_SAMPLE_SECONDS);
    }

    // Returns a zero-initialised duration when the window is too short. Callers
    // must test hasEnoughSamples() first, or they will read a frame time of
    // zero as "everything fits" and upshift.
    FrameDuration getAverageFrameTime() const {
        if (hasEnoughSamples()) {
            return mFrameDurationsSum / mFrames.size();
        }

        return {};
    }

    // Divides by the sample count without guarding against an empty window.
    // Safe only because every caller tests hasEnoughSamples() first.
    int getMissedFramePercent() const {
        return std::round(mMissedFrameCount * 100.0f / mFrames.size());
    }

    // Marks the newest sample as missed if it is on time. In pipelined mode,
    // the next frame's fence wait detects a GPU overrun and charges this sample
    // once. Does nothing when the window is empty after clear().
    bool markLastFrameMissed() {
        if (mFrames.empty() || mFrames.back().second.frameMiss()) {
            return false;
        }
        FrameDuration& last = mFrames.back().second;
        last = FrameDuration(last.getCpuTime(), last.getGpuTime(), true);
        mMissedFrameCount++;
        return true;
    }

    void clear() {
        mFrames.clear();
        mFrameDurationsSum = {};
        mMissedFrameCount = 0;
    }

private:
    static constexpr std::chrono::nanoseconds FRAME_DURATION_SAMPLE_SECONDS =
            std::chrono::seconds(2);

    std::deque<std::pair<TimePoint, FrameDuration>> mFrames;
    FrameDuration mFrameDurationsSum = {};
    int mMissedFrameCount = 0;
};

} // namespace swappy

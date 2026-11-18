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
#include <cstddef>
#include <cstdint>
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
          : mCpuTime(std::min(cpuTime, MAX_DURATION)),
            mGpuTime(gpuTime >= std::chrono::nanoseconds(0) ? std::min(gpuTime, MAX_DURATION)
                                                            : std::chrono::nanoseconds(0)),
            mHasGpuTime(gpuTime >= std::chrono::nanoseconds(0)),
            mFrameMissedDeadline(frameMissedDeadline) {}

    // Creates a pipelined sample whose CPU duration is known now and whose own
    // GPU duration will be attached on the next frame's fence wait via
    // FrameDurations::completeLastPipelinedFrame().
    [[nodiscard]] static FrameDuration withPendingGpu(std::chrono::nanoseconds cpuTime,
                                                      bool cpuMissedDeadline) {
        return FrameDuration(cpuTime, std::chrono::nanoseconds(-1), cpuMissedDeadline);
    }

    std::chrono::nanoseconds getCpuTime() const {
        return mCpuTime;
    }
    std::chrono::nanoseconds getGpuTime() const {
        return mGpuTime;
    }
    bool hasGpuTime() const {
        return mHasGpuTime;
    }

    bool frameMiss() const {
        return mFrameMissedDeadline;
    }

    void completeGpu(std::chrono::nanoseconds gpuTime) {
        if (gpuTime >= std::chrono::nanoseconds(0)) {
            mGpuTime = std::min(gpuTime, MAX_DURATION);
            mHasGpuTime = true;
        }
    }

    void markMiss() {
        mFrameMissedDeadline = true;
    }

    void clearMiss() {
        mFrameMissedDeadline = false;
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

private:
    std::chrono::nanoseconds mCpuTime = std::chrono::nanoseconds(0);
    std::chrono::nanoseconds mGpuTime = std::chrono::nanoseconds(0);
    bool mHasGpuTime = false;
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
        mCpuTimeSum += frameDuration.getCpuTime();
        if (frameDuration.hasGpuTime()) {
            mGpuTimeSum += frameDuration.getGpuTime();
            ++mGpuSampleCount;
        }
        if (frameDuration.frameMiss()) {
            ++mMissedFrameCount;
        }

        // Note the `begin() + 1` and the `size() >= 2` guard: one sample older
        // than the window is deliberately retained, so that hasEnoughSamples()
        // can still measure a span of at least FRAME_DURATION_SAMPLE_SECONDS.
        while (mFrames.size() >= 2 &&
               now - (mFrames.begin() + 1)->first > FRAME_DURATION_SAMPLE_SECONDS) {
            const FrameDuration& evicted = mFrames.front().second;
            mCpuTimeSum -= evicted.getCpuTime();
            if (evicted.hasGpuTime()) {
                mGpuTimeSum -= evicted.getGpuTime();
                --mGpuSampleCount;
            }
            if (evicted.frameMiss()) {
                --mMissedFrameCount;
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
    // zero as "everything fits" and upshift. Completed GPU samples are averaged
    // over mGpuSampleCount so a trailing pending pipelined sample never biases
    // the GPU mean downward.
    FrameDuration getAverageFrameTime() const {
        if (!hasEnoughSamples()) {
            return {};
        }
        const auto avgCpu = mCpuTimeSum / static_cast<int64_t>(mFrames.size());
        const auto avgGpu = mGpuSampleCount > 0
                ? (mGpuTimeSum / static_cast<int64_t>(mGpuSampleCount))
                : std::chrono::nanoseconds(0);
        return FrameDuration(avgCpu, avgGpu, false);
    }

    // Divides by the sample count without guarding against an empty window.
    // Safe only because every caller tests hasEnoughSamples() first.
    int getMissedFramePercent() const {
        return std::round(mMissedFrameCount * 100.0f / mFrames.size());
    }

    // Attach the newest pending pipelined sample's GPU duration (when
    // non-negative) and charge any GPU deadline miss. Ignore empty windows and
    // samples that already recorded GPU time in PipelineMode::Off.
    bool completeLastPipelinedFrame(std::chrono::nanoseconds gpuTime, bool gpuMissedDeadline) {
        if (mFrames.empty() || mFrames.back().second.hasGpuTime()) {
            return false;
        }
        FrameDuration& last = mFrames.back().second;
        if (gpuTime >= std::chrono::nanoseconds(0)) {
            last.completeGpu(gpuTime);
            mGpuTimeSum += last.getGpuTime();
            ++mGpuSampleCount;
        }
        return gpuMissedDeadline && markLastFrameMissed();
    }

    // Marks the newest sample as missed if it is on time. Does nothing when the
    // window is empty after clear().
    bool markLastFrameMissed() {
        if (mFrames.empty() || mFrames.back().second.frameMiss()) {
            return false;
        }
        mFrames.back().second.markMiss();
        ++mMissedFrameCount;
        return true;
    }

    // Clears deadline-miss flags whose work duration fits `budget` under
    // `pipeline`, while retaining sample timestamps and work durations.
    void clearMissesFittingBudget(std::chrono::nanoseconds budget, PipelineMode pipeline) {
        for (auto& entry : mFrames) {
            if (entry.second.frameMiss() && entry.second.getTime(pipeline) <= budget) {
                entry.second.clearMiss();
                --mMissedFrameCount;
            }
        }
    }

    void clear() {
        mFrames.clear();
        mCpuTimeSum = std::chrono::nanoseconds(0);
        mGpuTimeSum = std::chrono::nanoseconds(0);
        mGpuSampleCount = 0;
        mMissedFrameCount = 0;
    }

private:
    static constexpr std::chrono::nanoseconds FRAME_DURATION_SAMPLE_SECONDS =
            std::chrono::seconds(2);

    std::deque<std::pair<TimePoint, FrameDuration>> mFrames;
    std::chrono::nanoseconds mCpuTimeSum = std::chrono::nanoseconds(0);
    std::chrono::nanoseconds mGpuTimeSum = std::chrono::nanoseconds(0);
    size_t mGpuSampleCount = 0;
    int mMissedFrameCount = 0;
};

} // namespace swappy

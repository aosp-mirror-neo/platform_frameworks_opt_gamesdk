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

// Characterisation tests for the frame-time accumulator.
//
// These describe what the shipping code does today, not what it ideally would.
// They exist so that the extraction of the swap interval ladder cannot silently
// change the inputs that decision is made from. Everything asserted here was
// read off the implementation, not designed.
//
// That this file compiles at all is part of the point: the accumulator was
// nested inside SwappyCommon, which includes jni.h, so none of it was reachable
// from a host test before.

#include "FrameDurations.h"

#include <gtest/gtest.h>

using namespace swappy;
using namespace std::chrono_literals;

using std::chrono::nanoseconds;
using TimePoint = FrameDurations::TimePoint;

namespace {

constexpr nanoseconds kFrame60Hz = 16666667ns;

// Feeds `count` identical frames, one every `spacing`, starting at `start`.
// Returns the timestamp just past the last frame added.
TimePoint feed(FrameDurations& durations, TimePoint start, int count, nanoseconds spacing,
               nanoseconds cpu, nanoseconds gpu = 0ns, bool miss = false) {
    TimePoint now = start;
    for (int i = 0; i < count; ++i) {
        durations.add(FrameDuration(cpu, gpu, miss), now);
        now += spacing;
    }
    return now;
}

} // namespace

// ---------------------------------------------------------------------------
// FrameDuration
// ---------------------------------------------------------------------------

// The shipping margin, pinned separately so that retuning it has to come
// through this file rather than silently flowing into every expectation below.
TEST(FrameDurationTest, FrameMarginIsOneMillisecond) {
    EXPECT_EQ(FRAME_MARGIN, 1ms);
}

// Pipelined, CPU and GPU overlap so the frame costs the larger of the two.
// Unpipelined they run back to back and the costs add. Both orderings are
// asserted so that the max() cannot collapse to whichever side is tested.
TEST(FrameDurationTest, PipelineModeDecidesWhetherCpuAndGpuOverlap) {
    const FrameDuration cpuBound(10ms, 4ms, false);
    EXPECT_EQ(cpuBound.getTime(PipelineMode::On), 10ms + FRAME_MARGIN);
    EXPECT_EQ(cpuBound.getTime(PipelineMode::Off), 14ms + FRAME_MARGIN);

    const FrameDuration gpuBound(4ms, 10ms, false);
    EXPECT_EQ(gpuBound.getTime(PipelineMode::On), 10ms + FRAME_MARGIN);
    EXPECT_EQ(gpuBound.getTime(PipelineMode::Off), 14ms + FRAME_MARGIN);
}

// A wholly empty duration reports zero rather than FRAME_MARGIN, which is what
// lets "no samples yet" be distinguished from "an extremely cheap frame". Only
// wholly empty: one measured leg is enough to make the frame cost something,
// and feed() leaves gpu at 0ns by default.
TEST(FrameDurationTest, AnEmptyDurationCostsNothingRatherThanTheMargin) {
    const FrameDuration empty;
    EXPECT_EQ(empty.getTime(PipelineMode::On), 0ns);
    EXPECT_EQ(empty.getTime(PipelineMode::Off), 0ns);

    const FrameDuration cpuOnly(10ms, 0ns, false);
    EXPECT_EQ(cpuOnly.getTime(PipelineMode::On), 10ms + FRAME_MARGIN);
    EXPECT_EQ(cpuOnly.getTime(PipelineMode::Off), 10ms + FRAME_MARGIN);

    const FrameDuration gpuOnly(0ns, 4ms, false);
    EXPECT_EQ(gpuOnly.getTime(PipelineMode::On), 4ms + FRAME_MARGIN);
    EXPECT_EQ(gpuOnly.getTime(PipelineMode::Off), 4ms + FRAME_MARGIN);
}

// Both inputs are clamped at 100 ms, so a single pathological frame cannot
// drag the running average far enough to force a swap interval change.
TEST(FrameDurationTest, OutlierFramesAreClamped) {
    const FrameDuration huge(5s, 5s, false);

    EXPECT_EQ(huge.getCpuTime(), 100ms);
    EXPECT_EQ(huge.getGpuTime(), 100ms);
}

// ---------------------------------------------------------------------------
// FrameDurations
// ---------------------------------------------------------------------------

TEST(FrameDurationsTest, AnEmptyWindowHasNoSamples) {
    FrameDurations durations;

    EXPECT_FALSE(durations.hasEnoughSamples());
}

// The decision is gated on more than two seconds of history.
TEST(FrameDurationsTest, NeedsMoreThanTwoSecondsOfHistory) {
    FrameDurations durations;
    TimePoint now{};

    // 1.5 s of frames is not enough.
    now = feed(durations, now, 90, kFrame60Hz, 10ms);
    EXPECT_FALSE(durations.hasEnoughSamples());

    // Past two seconds it is.
    feed(durations, now, 60, kFrame60Hz, 10ms);
    EXPECT_TRUE(durations.hasEnoughSamples());
}

// The comparison is strict: a span of exactly two seconds still is not enough.
// Two samples that straddle the boundary are the whole test, because neither
// add() is far enough apart to trigger eviction.
TEST(FrameDurationsTest, TwoSecondsExactlyIsNotEnoughHistory) {
    FrameDurations durations;
    const TimePoint start{};

    durations.add(FrameDuration(10ms, 0ns, false), start);
    durations.add(FrameDuration(10ms, 0ns, false), start + 2s);
    EXPECT_FALSE(durations.hasEnoughSamples()) << "a span of exactly 2 s must not qualify";

    durations.add(FrameDuration(10ms, 0ns, false), start + 2s + 1ns);
    EXPECT_TRUE(durations.hasEnoughSamples());
}

// The eviction loop keeps one sample older than the window rather than
// dropping everything past it. This looks like an off-by-one and is not: if
// eviction trimmed the deque to exactly the window, then back - front would be
// at most the window length, hasEnoughSamples() would be permanently false in
// steady state, and updateSwapInterval() would return early on every frame.
//
// This test is the reason that loop must be left alone.
TEST(FrameDurationsTest, EvictionRetainsEnoughHistoryToStaySatisfied) {
    FrameDurations durations;
    TimePoint now{};

    // Well past the point where eviction is active.
    now = feed(durations, now, 600, kFrame60Hz, 30ms);

    EXPECT_TRUE(durations.hasEnoughSamples())
            << "eviction trimmed the window so tightly that the ladder would never run";

    // Bounded, not merely growing: the 30 ms history ages out rather than
    // diluting the 10 ms window. hasEnoughSamples() alone cannot show this --
    // it is a lower bound, and a window that never evicts satisfies it too.
    feed(durations, now, 600, kFrame60Hz, 10ms);
    EXPECT_TRUE(durations.hasEnoughSamples());
    EXPECT_EQ(durations.getAverageFrameTime().getCpuTime(), 10ms);
}

// Until the window is full the average is a default-constructed duration, which
// reports zero. A caller that skipped hasEnoughSamples() would read that as a
// free frame and upshift.
TEST(FrameDurationsTest, AverageIsZeroUntilTheWindowIsFull) {
    FrameDurations durations;
    TimePoint now{};

    feed(durations, now, 30, kFrame60Hz, 10ms);

    ASSERT_FALSE(durations.hasEnoughSamples());
    EXPECT_EQ(durations.getAverageFrameTime().getTime(PipelineMode::On), 0ns)
            << "a short window must not look like a cheap frame";
}

TEST(FrameDurationsTest, AverageReflectsTheFramesInTheWindow) {
    FrameDurations durations;
    TimePoint now{};

    // The first batch must be fully subtracted from the running sum as it ages
    // out; identical batches would hide both a missing eviction and a
    // subtraction of the wrong sample. The arithmetic is exact integer
    // nanoseconds, so no tolerance is warranted.
    now = feed(durations, now, 200, kFrame60Hz, 30ms, 12ms);
    feed(durations, now, 200, kFrame60Hz, 10ms, 4ms);

    ASSERT_TRUE(durations.hasEnoughSamples());
    const auto average = durations.getAverageFrameTime();
    EXPECT_EQ(average.getCpuTime(), 10ms);
    EXPECT_EQ(average.getGpuTime(), 4ms);
}

TEST(FrameDurationsTest, MissedFramePercentTracksTheWindow) {
    FrameDurations durations;
    TimePoint now{};

    now = feed(durations, now, 200, kFrame60Hz, 10ms, 0ns, /*miss=*/true);
    EXPECT_EQ(durations.getMissedFramePercent(), 100);

    // Halfway through ageing out: 61 missed and 60 on-time in the 121-sample
    // steady-state window. An integer-division form would report 0 here and
    // still pass the 100% and 0% cases below.
    now = feed(durations, now, 60, kFrame60Hz, 10ms, 0ns, /*miss=*/false);
    EXPECT_EQ(durations.getMissedFramePercent(), 50);

    // Once all the misses age out the percentage follows them down.
    feed(durations, now, 140, kFrame60Hz, 10ms, 0ns, /*miss=*/false);
    EXPECT_EQ(durations.getMissedFramePercent(), 0)
            << "misses outlived the window they were counted in";
}

// Two misses out of three is 66.67%, which the implementation rounds to
// nearest rather than truncating. Samples are added directly so the window
// size is exactly three and the fraction is unambiguous.
TEST(FrameDurationsTest, MissedFramePercentRoundsToNearest) {
    FrameDurations durations;
    const TimePoint start{};

    durations.add(FrameDuration(10ms, 0ns, /*miss=*/true), start);
    durations.add(FrameDuration(10ms, 0ns, /*miss=*/true), start + 1500ms);
    durations.add(FrameDuration(10ms, 0ns, /*miss=*/false), start + 2001ms);

    ASSERT_TRUE(durations.hasEnoughSamples());
    EXPECT_EQ(durations.getMissedFramePercent(), 67);
}

// clear() is what the ladder calls after changing the swap interval, so that
// frames measured under the old cadence cannot influence the next decision.
TEST(FrameDurationsTest, ClearDiscardsTheWindow) {
    FrameDurations durations;
    TimePoint now{};

    now = feed(durations, now, 200, kFrame60Hz, 10ms, 4ms, /*miss=*/true);
    ASSERT_TRUE(durations.hasEnoughSamples());

    durations.clear();

    EXPECT_FALSE(durations.hasEnoughSamples());
    EXPECT_EQ(durations.getAverageFrameTime().getTime(PipelineMode::On), 0ns);

    // The running sum and the missed-frame count belong to the window and go
    // with it. Refill before asserting: while the window is short,
    // getAverageFrameTime() returns {} whatever the sum holds, so a leaked sum
    // is only visible once hasEnoughSamples() is true again.
    feed(durations, now, 200, kFrame60Hz, 20ms, 0ns, /*miss=*/false);

    ASSERT_TRUE(durations.hasEnoughSamples());
    EXPECT_EQ(durations.getAverageFrameTime().getCpuTime(), 20ms)
            << "frames from before clear() leaked into the new window";
    EXPECT_EQ(durations.getAverageFrameTime().getGpuTime(), 0ns);
    EXPECT_EQ(durations.getMissedFramePercent(), 0)
            << "the missed-frame count outlived the window it was counted in";
}

// In pipelined mode the next frame's fence wait detects a GPU overrun, so
// markLastFrameMissed() charges the newest sample after the fact.
TEST(FrameDurationsTest, MarkLastFrameMissedChargesTheNewestSampleOnce) {
    FrameDurations durations;
    const TimePoint start{};

    EXPECT_FALSE(durations.markLastFrameMissed()); // Empty window: nothing to charge.

    durations.add(FrameDuration(10ms, 0ns, /*miss=*/false), start);
    durations.add(FrameDuration(10ms, 0ns, /*miss=*/false), start + 1500ms);
    durations.add(FrameDuration(10ms, 0ns, /*miss=*/false), start + 2001ms);
    ASSERT_TRUE(durations.hasEnoughSamples());
    EXPECT_EQ(durations.getMissedFramePercent(), 0);

    EXPECT_TRUE(durations.markLastFrameMissed());
    EXPECT_EQ(durations.getMissedFramePercent(), 33);
    EXPECT_FALSE(durations.markLastFrameMissed()); // Already missed: counted once.
    EXPECT_EQ(durations.getMissedFramePercent(), 33);

    // The charge ages out with its sample.
    feed(durations, start + 2001ms + kFrame60Hz, 200, kFrame60Hz, 10ms);
    EXPECT_EQ(durations.getMissedFramePercent(), 0);
}

TEST(FrameDurationsTest, CompleteLastPipelinedFrameComputesExactMeanWithoutTrailingPendingBias) {
    FrameDurations durations;
    const TimePoint start{};

    // Two completed 29ms GPU samples plus one trailing pending sample span more than 2s.
    durations.add(FrameDuration::withPendingGpu(8ms, /*cpuMissedDeadline=*/false), start);
    EXPECT_FALSE(durations.completeLastPipelinedFrame(29ms, /*gpuMissedDeadline=*/false));
    durations.add(FrameDuration::withPendingGpu(8ms, /*cpuMissedDeadline=*/false), start + 1000ms);
    EXPECT_FALSE(durations.completeLastPipelinedFrame(29ms, /*gpuMissedDeadline=*/false));
    durations.add(FrameDuration::withPendingGpu(8ms, /*cpuMissedDeadline=*/false), start + 2001ms);

    ASSERT_TRUE(durations.hasEnoughSamples());
    const FrameDuration average = durations.getAverageFrameTime();
    EXPECT_EQ(average.getCpuTime(), 8ms);
    EXPECT_EQ(average.getGpuTime(), 29ms);
    EXPECT_EQ(average.getTime(PipelineMode::On), 30ms);
}

TEST(FrameDurationsTest, CompleteLastPipelinedFrameIgnoresCompletedUnpipelinedSample) {
    FrameDurations durations;
    const TimePoint start{};

    durations.add(FrameDuration(8ms, 14ms, /*frameMissedDeadline=*/false), start);
    durations.add(FrameDuration(8ms, 14ms, /*frameMissedDeadline=*/false), start + 2001ms);
    ASSERT_TRUE(durations.hasEnoughSamples());

    EXPECT_FALSE(durations.completeLastPipelinedFrame(2ms, /*gpuMissedDeadline=*/true));
    EXPECT_EQ(durations.getAverageFrameTime().getGpuTime(), 14ms);
    EXPECT_EQ(durations.getMissedFramePercent(), 0);
}

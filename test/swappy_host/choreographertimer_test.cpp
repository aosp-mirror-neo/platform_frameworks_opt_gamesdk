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

#include "ChoreographerTimer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

using namespace swappy;
using namespace std::chrono_literals;

using std::chrono::nanoseconds;

namespace {

constexpr nanoseconds k60Hz = 16666667ns;

ChoreographerTimer::time_point at(nanoseconds sinceEpoch) {
    return ChoreographerTimer::time_point(sinceEpoch);
}

int64_t nanosSinceEpoch(ChoreographerTimer::time_point time) {
    return time.time_since_epoch().count();
}

int repeatsUntilStopped(ChoreographerTimer& timer,
                        std::optional<ChoreographerTimer::time_point> timestamp,
                        int64_t sequenceNumber = 0) {
    constexpr int kMaxRepeats = 100;
    int repeats = 0;
    while (repeats < kMaxRepeats && timer.addTimestamp(timestamp, sequenceNumber)) {
        ++repeats;
    }
    return repeats;
}

TEST(ChoreographerTimerTest, WakesUpARefreshPeriodAfterACallbackOnTime) {
    ChoreographerTimer timer(k60Hz, 0ns, at(1s));

    EXPECT_TRUE(timer.addTimestamp(at(1'016'666'667ns)));

    EXPECT_EQ(nanosSinceEpoch(timer.wakeTime(0ns, at(1'016'666'667ns))), 1'033'333'334);
}

TEST(ChoreographerTimerTest, WakesUpOffsetFromTheVsync) {
    ChoreographerTimer timer(k60Hz, 0ns, at(1s));
    timer.addTimestamp(at(1'016'666'667ns));

    EXPECT_EQ(nanosSinceEpoch(timer.wakeTime(-2ms, at(1'016'666'667ns))), 1'031'333'334);
    EXPECT_EQ(nanosSinceEpoch(timer.wakeTime(2ms, at(1'016'666'667ns))), 1'035'333'334);
}

TEST(ChoreographerTimerTest, IgnoresOffsetsOfMoreThanHalfARefreshPeriod) {
    ChoreographerTimer timer(k60Hz, 0ns, at(1s));
    timer.addTimestamp(at(1'016'666'667ns));

    EXPECT_EQ(nanosSinceEpoch(timer.wakeTime(-9ms, at(1'016'666'667ns))), 1'033'333'334);
    EXPECT_EQ(nanosSinceEpoch(timer.wakeTime(9ms, at(1'016'666'667ns))), 1'033'333'334);
}

TEST(ChoreographerTimerTest, ShiftsCallbacksToSurfaceFlingersVsync) {
    // Callbacks arrive 4ms before SurfaceFlinger wakes up for the vsync.
    ChoreographerTimer timer(k60Hz, 4ms, at(1s));

    timer.addTimestamp(at(1'012'666'667ns));

    EXPECT_EQ(nanosSinceEpoch(timer.wakeTime(0ns, at(1'012'666'667ns))), 1'033'333'334);
}

TEST(ChoreographerTimerTest, MovesOneTwentyFifthOfTheWayToALateCallback) {
    ChoreographerTimer timer(k60Hz, 0ns, at(1s));

    // 4ms late: the refresh period grows by 160us, and the vsync moves by that
    // much, to 1s + 16826667ns.
    timer.addTimestamp(at(1'020'666'667ns));

    EXPECT_EQ(nanosSinceEpoch(timer.wakeTime(0ns, at(1'020'666'667ns))), 1'033'653'334);
}

TEST(ChoreographerTimerTest, IgnoresCallbacksMoreThanHalfARefreshPeriodEarly) {
    ChoreographerTimer timer(k60Hz, 0ns, at(1s));
    timer.addTimestamp(at(1'016'666'667ns));

    // 13.3ms before the vsync expected at 1s + 33333334ns.
    EXPECT_TRUE(timer.addTimestamp(at(1'020'000'000ns)));

    EXPECT_EQ(nanosSinceEpoch(timer.wakeTime(0ns, at(1'020'000'000ns))), 1'033'333'334);
}

TEST(ChoreographerTimerTest, SnapsToTheNearestVsyncAfterAGapWithoutAdjustingThePeriod) {
    ChoreographerTimer timer(k60Hz, 0ns, at(1s));

    // 3.4 refresh periods after start: the latest vsync is the third, at
    // 1s + 50000001ns.
    timer.addTimestamp(at(1'056'666'668ns));
    EXPECT_EQ(nanosSinceEpoch(timer.wakeTime(0ns, at(1'056'666'668ns))), 1'066'666'668);

    // A callback on the fourth vsync then finds the period unchanged.
    timer.addTimestamp(at(1'066'666'668ns));
    EXPECT_EQ(nanosSinceEpoch(timer.wakeTime(0ns, at(1'066'666'668ns))), 1'083'333'335);
}

TEST(ChoreographerTimerTest, WakesUpAtTheFirstVsyncStrictlyAfterNow) {
    ChoreographerTimer timer(k60Hz, 0ns, at(1s));

    EXPECT_EQ(nanosSinceEpoch(timer.wakeTime(0ns, at(1'040'000'000ns))), 1'050'000'001);
    // When now equals a vsync without a new callback, wakeTime advances to the
    // next refresh boundary instead of returning now.
    EXPECT_EQ(nanosSinceEpoch(timer.wakeTime(0ns, at(1'016'666'667ns))), 1'033'333'334);
}

TEST(ChoreographerTimerTest, ReportsThatCallbacksStoppedAfterSixRepeats) {
    ChoreographerTimer timer(k60Hz, 0ns, at(1s));
    timer.addTimestamp(at(1'016'666'667ns));

    EXPECT_EQ(repeatsUntilStopped(timer, at(1'016'666'667ns)), 6);
    // A new callback resets the repeat count.
    EXPECT_TRUE(timer.addTimestamp(at(1'033'333'334ns)));
}

TEST(ChoreographerTimerTest, ResetsRepeatsWhenSequenceNumberAdvancesAtSameTimestamp) {
    ChoreographerTimer timer(k60Hz, 0ns, at(1s));
    timer.addTimestamp(at(1'016'666'667ns), 1);
    ASSERT_EQ(repeatsUntilStopped(timer, at(1'016'666'667ns), 1), 6);

    EXPECT_TRUE(timer.addTimestamp(at(1'016'666'667ns), 2));
    EXPECT_EQ(repeatsUntilStopped(timer, at(1'016'666'667ns), 2), 6);
}

TEST(ChoreographerTimerTest, DistinguishesMissingCallbacksFromCallbackAtEpochZero) {
    ChoreographerTimer timer(k60Hz, 10ms, at(0ns));

    // Initial check + six repeats without any callback keep mBaseTime at 0ns
    // without letting mAppToSfDelay shift the period, then stop.
    EXPECT_TRUE(timer.addTimestamp(std::nullopt));
    EXPECT_EQ(nanosSinceEpoch(timer.wakeTime(0ns, at(0ns))), 16'666'667);
    EXPECT_EQ(repeatsUntilStopped(timer, std::nullopt), 6);

    // A real callback arriving at time_point{} (0ns) counts as a fresh
    // callback rather than a repeat of an empty timestamp.
    EXPECT_TRUE(timer.addTimestamp(at(0ns)));
    EXPECT_EQ(repeatsUntilStopped(timer, at(0ns)), 6);

    // An immediate callback at 0ns on a fresh 0ns timer also counts as fresh.
    ChoreographerTimer immediateTimer(k60Hz, 10ms, at(0ns));
    EXPECT_TRUE(immediateTimer.addTimestamp(at(0ns)));
    EXPECT_EQ(repeatsUntilStopped(immediateTimer, at(0ns)), 6);
}

} // namespace

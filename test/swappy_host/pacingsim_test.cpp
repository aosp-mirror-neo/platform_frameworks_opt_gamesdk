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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include "ClassicPacingImpl.h"
#include "DisplayRateController.h"
#include "FrameDurations.h"
#include "SimulatedDisplayPanel.h"
#include "SimulatedGpuQueue.h"
#include "SwapIntervalLadder.h"
#include "gtest/gtest.h"
#include "swappy/swappy_common.h"

namespace {

using namespace std::chrono_literals;
using swappy::ClassicPacingImplementation;
using swappy::DisplayRateController;
using swappy::FrameDuration;
using swappy::FrameDurations;
using swappy::PacingResult;
using swappy::PipelineMode;
using swappy::TimingChange;
using swappy::testing::DisplayPanelModel;
using swappy::testing::SimulatedDisplayPanel;
using swappy::testing::SimulatedGpuQueue;

constexpr auto k120HzPeriod = 8'333'333ns;
constexpr auto k60HzPeriod = 16'666'667ns;
constexpr auto k48HzPeriod = 20'833'333ns;
constexpr auto k30HzPeriod = 33'333'333ns;
constexpr auto kAutoSwapIntervalThreshold = 50ms;

struct FrameWorkSample {
    std::chrono::nanoseconds peakCpuWork{0};
    std::chrono::nanoseconds peakGpuWork{0};
};

// Models CPU/GPU utilization-targeted DVFS scaling (such as schedutil / simple_ondemand)
// where lower frame duty cycles at 30 Hz reduce operating frequency and stretch measured
// frame work relative to peak-clock capability.
class DutyCycleGovernorModel {
public:
    [[nodiscard]] static DutyCycleGovernorModel disabled() noexcept {
        return DutyCycleGovernorModel{false, 1.0, 0.70};
    }

    [[nodiscard]] static DutyCycleGovernorModel enabled(double minFreqScale = 0.55,
                                                        double targetUtilization = 0.70) noexcept {
        return DutyCycleGovernorModel{true, std::clamp(minFreqScale, 0.1, 1.0),
                                      std::clamp(targetUtilization, 0.1, 1.0)};
    }

    [[nodiscard]] std::chrono::nanoseconds scaleWork(
            std::chrono::nanoseconds peakWork) const noexcept {
        if (!mEnabled || peakWork <= 0ns) {
            return peakWork;
        }
        const double scaledNs = static_cast<double>(peakWork.count()) / mCurrentFreqScale;
        return std::chrono::nanoseconds(static_cast<int64_t>(scaledNs));
    }

    void onFrameCompleted(std::chrono::nanoseconds peakBusyWork,
                          std::chrono::nanoseconds frameElapsed) noexcept {
        if (!mEnabled || frameElapsed <= 0ns) {
            return;
        }
        const double denom = mTargetUtilization * static_cast<double>(frameElapsed.count());
        const double rawScale =
                denom > 0.0 ? (static_cast<double>(peakBusyWork.count()) / denom) : 1.0;
        const double targetScale = std::clamp(rawScale, mMinFreqScale, 1.0);
        mCurrentFreqScale = 0.5 * mCurrentFreqScale + 0.5 * targetScale;
    }

private:
    DutyCycleGovernorModel(bool enabled, double minFreqScale, double targetUtilization) noexcept
          : mEnabled(enabled), mMinFreqScale(minFreqScale), mTargetUtilization(targetUtilization) {}

    bool mEnabled = false;
    double mMinFreqScale = 1.0;
    double mTargetUtilization = 0.70;
    double mCurrentFreqScale = 1.0;
};

struct SimulatorConfig {
    std::chrono::nanoseconds initialRefreshPeriod = k60HzPeriod;
    std::chrono::nanoseconds swapDuration = k60HzPeriod;
    DisplayPanelModel panelModel = DisplayPanelModel::openLoop();
    DutyCycleGovernorModel governor = DutyCycleGovernorModel::disabled();
};

// Single-threaded closed-loop simulator driving Swappy's production FrameDurations,
// ClassicPacingImplementation, and DisplayRateController across multi-minute runs
// without OS threads or condition-variable sleeps.
class PacingSimulator {
public:
    explicit PacingSimulator(SimulatorConfig config)
          : mConfig(std::move(config)),
            mRefreshPeriod(mConfig.initialRefreshPeriod),
            mPanel(mConfig.initialRefreshPeriod, mConfig.panelModel),
            mGovernor(mConfig.governor) {}

    void runUniformFrames(int frameCount, std::chrono::nanoseconds peakCpuWork,
                          std::chrono::nanoseconds peakGpuWork) {
        const FrameWorkSample sample{peakCpuWork, peakGpuWork};
        for (int i = 0; i < frameCount; ++i) {
            stepFrame(sample);
        }
    }

    void runRepeatingPattern(int totalFrames,
                             std::initializer_list<FrameWorkSample> patternSamples) {
        const std::vector<FrameWorkSample> pattern(patternSamples);
        if (pattern.empty()) {
            return;
        }
        for (int i = 0; i < totalFrames; ++i) {
            stepFrame(pattern[static_cast<size_t>(i) % pattern.size()]);
        }
    }

    void resetTransitionCounters() noexcept {
        mIntervalTransitions = 0;
        mPanelTransitions = 0;
        mMissedFrames = 0;
        mMaxEffectiveFramePeriod = effectiveFramePeriod();
    }

    [[nodiscard]] int32_t autoSwapInterval() const noexcept {
        return mAutoSwapInterval;
    }

    [[nodiscard]] std::chrono::nanoseconds refreshPeriod() const noexcept {
        return mRefreshPeriod;
    }

    [[nodiscard]] std::chrono::nanoseconds maxEffectiveFramePeriod() const noexcept {
        return mMaxEffectiveFramePeriod;
    }

    [[nodiscard]] int intervalTransitions() const noexcept {
        return mIntervalTransitions;
    }

    [[nodiscard]] int panelTransitions() const noexcept {
        return mPanelTransitions;
    }

    [[nodiscard]] int missedFrames() const noexcept {
        return mMissedFrames;
    }

    [[nodiscard]] std::chrono::nanoseconds simulatedDuration() const noexcept {
        return mNow - FrameDurations::TimePoint{};
    }

private:
    [[nodiscard]] std::chrono::nanoseconds effectiveFramePeriod() const noexcept {
        return mRefreshPeriod * mAutoSwapInterval;
    }

    void stepFrame(const FrameWorkSample& sample) {
        const FrameDurations::TimePoint frameBegin = mNow;
        const auto effectiveCpu = mGovernor.scaleWork(sample.peakCpuWork);
        const auto effectiveGpu = mGovernor.scaleWork(sample.peakGpuWork);

        advanceClockTo(mNow + effectiveCpu);
        mGpuQueue.submit(mNow, effectiveGpu);

        waitForNextFrame();
        updateSwapInterval();
        updateDisplayTimings();

        const auto frameElapsed = mNow - frameBegin;
        const auto peakBusy = mPipelineMode == PipelineMode::On
                ? std::max(sample.peakCpuWork, sample.peakGpuWork)
                : (sample.peakCpuWork + sample.peakGpuWork);
        mGovernor.onFrameCompleted(peakBusy, frameElapsed);
        mMaxEffectiveFramePeriod = std::max(mMaxEffectiveFramePeriod, effectiveFramePeriod());

        startFrame();
    }

    void waitForNextFrame() {
        const auto cpuTime = mStartFrameTime.has_value() ? (mNow - *mStartFrameTime) : 0ns;
        if (mRefreshPeriod * mAutoSwapInterval <= kAutoSwapIntervalThreshold) {
            while (mCurrentVsync < mTargetVsync) {
                advanceOneVsync();
            }
            while (!mGpuQueue.lastFrameIsComplete(mNow, mPipelineMode)) {
                advanceOneVsync();
            }
        }

        const auto gpuTime = mGpuQueue.lastFrameIsComplete(mNow, mPipelineMode)
                ? mGpuQueue.lastFenceTime(mNow)
                : -1ns;
        const bool frameMissed = mCurrentVsync > mTargetVsync;
        if (frameMissed) {
            ++mMissedFrames;
        }
        mDurations.add(FrameDuration{cpuTime, gpuTime, frameMissed}, mNow);
    }

    void updateSwapInterval() {
        const int32_t previousInterval = mAutoSwapInterval;
        const PacingResult result =
                mPacing.updateSwapInterval(mAutoSwapInterval, mPipelineMode, mDurations,
                                           mRefreshPeriod, mSwapDuration,
                                           kAutoSwapIntervalThreshold,
                                           /*pipelineModeAutoMode=*/true);
        mAutoSwapInterval = result.autoSwapInterval;
        mPipelineMode = result.pipelineMode;
        if (result.configChanged && mAutoSwapInterval != previousInterval) {
            ++mIntervalTransitions;
        }
        if (result.preferredRefreshPeriodHint > 0ns) {
            issueRefreshRateVote(result.preferredRefreshPeriodHint);
        }
    }

    void issueRefreshRateVote(std::chrono::nanoseconds targetPeriod) {
        if (const auto votedHz = mRateController.voteFrameRateHz(targetPeriod, mSwapDuration)) {
            mPanel.onFrameRateVote(*votedHz);
        }
    }

    void advanceClockTo(FrameDurations::TimePoint target) {
        while (mNextVsyncTime <= target) {
            mNow = mNextVsyncTime;
            onVsyncTick();
        }
        mNow = target;
    }

    void advanceOneVsync() {
        mNow = mNextVsyncTime;
        onVsyncTick();
    }

    void onVsyncTick() {
        ++mCurrentVsync;
        (void)mPanel.advanceVsync();
        mNextVsyncTime += mPanel.activePeriod();
    }

    void updateDisplayTimings() {
        const auto newPeriod = mPanel.activePeriod();
        const bool swapIntervalChangedBySettings = mSwapDuration != mConfig.swapDuration;
        if (!mTimingSettingsNeedUpdate && newPeriod == mRefreshPeriod &&
            !swapIntervalChangedBySettings) {
            return;
        }
        mTimingSettingsNeedUpdate = false;

        const int32_t previousInterval = mAutoSwapInterval;
        const bool panelPeriodChanged = newPeriod != mRefreshPeriod;
        const TimingChange change{
                .oldRefreshPeriod = mRefreshPeriod,
                .newRefreshPeriod = newPeriod,
                .swapDuration = mSwapDuration,
        };
        mRefreshPeriod = newPeriod;
        if (panelPeriodChanged) {
            ++mPanelTransitions;
        }

        const PacingResult timingResult =
                mPacing.onDisplayTimingsChanged(mAutoSwapInterval, mPipelineMode, mDurations,
                                                change);
        mAutoSwapInterval = timingResult.autoSwapInterval;
        mPipelineMode = timingResult.pipelineMode;

        const bool swapIntervalValid = mRefreshPeriod * mAutoSwapInterval >= mConfig.swapDuration;
        mSwapDuration = mConfig.swapDuration;
        if (swapIntervalChangedBySettings || !swapIntervalValid) {
            mAutoSwapInterval = swappy::calculateSwapInterval(mSwapDuration, mRefreshPeriod);
            mPipelineMode = PipelineMode::On;
            issueRefreshRateVote(mSwapDuration);
        }
        if (!mRateController.hasVoted()) {
            issueRefreshRateVote(mSwapDuration);
        }
        if (mAutoSwapInterval != previousInterval) {
            ++mIntervalTransitions;
        }
        mDurations.clear();
    }

    void startFrame() {
        mTargetVsync = mCurrentVsync + mAutoSwapInterval;
        mStartFrameTime = mNow;
    }

    SimulatorConfig mConfig;
    std::chrono::nanoseconds mRefreshPeriod;
    std::chrono::nanoseconds mSwapDuration{0};
    SimulatedDisplayPanel mPanel;
    DutyCycleGovernorModel mGovernor;

    FrameDurations mDurations;
    ClassicPacingImplementation mPacing;
    DisplayRateController mRateController;
    SimulatedGpuQueue mGpuQueue;

    FrameDurations::TimePoint mNow{};
    FrameDurations::TimePoint mNextVsyncTime{mConfig.initialRefreshPeriod};
    std::optional<FrameDurations::TimePoint> mStartFrameTime;
    int64_t mCurrentVsync = 0;
    int64_t mTargetVsync = 0;
    int32_t mAutoSwapInterval = 1;
    PipelineMode mPipelineMode = PipelineMode::On;
    bool mTimingSettingsNeedUpdate = true;

    int mIntervalTransitions = 0;
    int mPanelTransitions = 0;
    int mMissedFrames = 0;
    std::chrono::nanoseconds mMaxEffectiveFramePeriod{0};
};

TEST(PacingSimTest, BimodalTailMissesWithLowMeanOscillateEveryTwoSecondWindow) {
    PacingSimulator sim(SimulatorConfig{
            .initialRefreshPeriod = k60HzPeriod,
            .swapDuration = k60HzPeriod,
    });

    // Three 2-frame GPU tail bursts per 20 frames: at 60Hz each pair slips 1 frame (15% > 10%
    // threshold) and steps down to 30Hz, while at 30Hz the second spike stays in flight past
    // 66.67ms so FrameDurations records three 29ms GPU samples per 20 frames (9.6ms pipelined
    // mean < 15.67ms lower bound, 0% misses) and steps back up to 60Hz on the next 2s window.
    sim.runRepeatingPattern(900,
                            {
                                    {5ms, 5ms},  {5ms, 5ms},  {5ms, 5ms},  {5ms, 5ms}, {5ms, 29ms},
                                    {5ms, 29ms}, {5ms, 5ms},  {5ms, 5ms},  {5ms, 5ms}, {5ms, 5ms},
                                    {5ms, 29ms}, {5ms, 29ms}, {5ms, 5ms},  {5ms, 5ms}, {5ms, 5ms},
                                    {5ms, 5ms},  {5ms, 29ms}, {5ms, 29ms}, {5ms, 5ms}, {5ms, 5ms},
                            });

    EXPECT_GE(sim.intervalTransitions(), 6);
    EXPECT_GT(sim.missedFrames(), 30);
    EXPECT_EQ(sim.maxEffectiveFramePeriod(), k60HzPeriod * 2);
}

TEST(PacingSimTest, EngineThirtyFpsSwapDurationFloorClampsArrPanelAcrossTenMinuteRun) {
    PacingSimulator sim(SimulatorConfig{
            .initialRefreshPeriod = k120HzPeriod,
            .swapDuration = std::chrono::nanoseconds(SWAPPY_SWAP_30FPS),
            .panelModel = DisplayPanelModel::continuousArr(24, 120, /*applyLatencyVsyncs=*/2),
    });

    // Warm up past the initial 120Hz -> 30Hz ARR panel switch, then run 18,000 frames
    // (10 simulated minutes of a 30 FPS engine cap with 7ms CPU / 7ms GPU work).
    sim.runUniformFrames(10, 7ms, 7ms);
    EXPECT_EQ(sim.refreshPeriod(), k30HzPeriod);
    EXPECT_EQ(sim.autoSwapInterval(), 1);

    sim.resetTransitionCounters();
    sim.runUniformFrames(18'000, 7ms, 7ms);

    EXPECT_EQ(sim.refreshPeriod(), k30HzPeriod);
    EXPECT_EQ(sim.autoSwapInterval(), 1);
    EXPECT_EQ(sim.panelTransitions(), 0);
    EXPECT_EQ(sim.intervalTransitions(), 0);
    EXPECT_EQ(sim.missedFrames(), 0);
    EXPECT_GE(sim.simulatedDuration(), 600s);
}

TEST(PacingSimTest, SingleIsolatedShaderHitchStaysAt60HzOnTeDivisorPanel) {
    PacingSimulator sim(SimulatorConfig{
            .initialRefreshPeriod = k60HzPeriod,
            .swapDuration = k60HzPeriod,
            .panelModel = DisplayPanelModel::teDivisor240Hz(/*applyLatencyVsyncs=*/2),
    });

    sim.runUniformFrames(200, 6ms, 6ms);
    sim.resetTransitionCounters();

    // A single 120ms shader hitch (clamped to 100ms by FrameDuration::MAX_DURATION) causes
    // < 10% missed frames in the 2s window and leaves the 60Hz pacing state unchanged.
    sim.runUniformFrames(1, 120ms, 20ms);
    sim.runUniformFrames(200, 6ms, 6ms);

    EXPECT_EQ(sim.refreshPeriod(), k60HzPeriod);
    EXPECT_EQ(sim.autoSwapInterval(), 1);
    EXPECT_EQ(sim.panelTransitions(), 0);
    EXPECT_EQ(sim.intervalTransitions(), 0);
}

TEST(PacingSimTest, SustainedShaderCompileBurstDownshiftsTo48HzAndRecoversTo60Hz) {
    PacingSimulator sim(SimulatorConfig{
            .initialRefreshPeriod = k60HzPeriod,
            .swapDuration = k60HzPeriod,
            .panelModel = DisplayPanelModel::teDivisor240Hz(/*applyLatencyVsyncs=*/2),
    });

    sim.runUniformFrames(200, 6ms, 6ms);
    sim.resetTransitionCounters();

    // A sustained 150-frame (19ms CPU / 19ms GPU => 20ms with FRAME_MARGIN) shader burst
    // exceeds the 10% miss threshold at 60Hz, votes ~56Hz, and snaps the 240Hz TE-divisor
    // panel to 48Hz (20.83ms) at swapInterval=1 before recovering to 60Hz across a 5-minute run.
    sim.runUniformFrames(150, 19ms, 19ms);
    EXPECT_EQ(sim.refreshPeriod(), k48HzPeriod);
    EXPECT_EQ(sim.autoSwapInterval(), 1);
    EXPECT_GE(sim.panelTransitions(), 1);

    sim.runUniformFrames(18'000, 6ms, 6ms);
    EXPECT_EQ(sim.refreshPeriod(), k60HzPeriod);
    EXPECT_EQ(sim.autoSwapInterval(), 1);
    EXPECT_GE(sim.simulatedDuration(), 300s);
}

TEST(PacingSimTest, DutyCycleGovernorDownclockReproducesThirtyFpsLockIn) {
    PacingSimulator unscaledSim(SimulatorConfig{
            .initialRefreshPeriod = k60HzPeriod,
            .swapDuration = k60HzPeriod,
            .governor = DutyCycleGovernorModel::disabled(),
    });
    PacingSimulator dvfsSim(SimulatorConfig{
            .initialRefreshPeriod = k60HzPeriod,
            .swapDuration = k60HzPeriod,
            .governor = DutyCycleGovernorModel::enabled(/*minFreqScale=*/0.55,
                                                        /*targetUtilization=*/0.70),
    });

    // Heavy initial spike steps both runs from 60Hz down to 30Hz (autoSwapInterval == 2).
    unscaledSim.runUniformFrames(150, 12ms, 24ms);
    dvfsSim.runUniformFrames(150, 12ms, 24ms);
    ASSERT_EQ(unscaledSim.autoSwapInterval(), 2);
    ASSERT_EQ(dvfsSim.autoSwapInterval(), 2);

    // Workload drops to 10ms peak-clock CPU/GPU work: unscaled clocks recover to 60Hz, whereas
    // duty-cycle downclocking at 30Hz stretches 10ms peak work above the 15.67ms upshift bound.
    unscaledSim.runUniformFrames(600, 10ms, 10ms);
    dvfsSim.runUniformFrames(600, 10ms, 10ms);

    EXPECT_EQ(unscaledSim.autoSwapInterval(), 1);
    EXPECT_EQ(dvfsSim.autoSwapInterval(), 2);
}

} // namespace

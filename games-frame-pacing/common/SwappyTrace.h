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

#include "Trace.h"

namespace swappy {

// Graphics backend that emits a Swappy.Target or Swappy.Actual trace event.
enum class TraceBackend { GL, Vulkan };

} // namespace swappy

#if AGDK_TRACE == 0

#define SWAPPY_TRACE_SETTINGS(swappy_p_session, swappy_p_snapshotExpr) ((void)0)
#define SWAPPY_TRACE_WORK_TIMES(swappy_p_cpuNs, swappy_p_gpuNs) ((void)0)
#define SWAPPY_TRACE_WAKE_ERROR(swappy_p_wakeErrorNs) ((void)0)
#define SWAPPY_TRACE_PRESENT_TARGET(swappy_p_frameId, swappy_p_targetTp, swappy_p_swapInterval, \
                                    swappy_p_backend)                                           \
    ((void)0)
#define SWAPPY_TRACE_PRESENT_ACTUAL(swappy_p_frameId, swappy_p_actualNs, swappy_p_desiredNs, \
                                    swappy_p_marginNs, swappy_p_backend)                     \
    ((void)0)

#else

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>

#include "FrameDurations.h"
#include "SwapIntervalLadder.h"

namespace swappy {

struct TraceSettingsSnapshot {
    int64_t swapDurationNs = 0;
    int64_t refreshPeriodNs = 0;
    int64_t targetPeriodNs = 0;
    int64_t autoSwapIntervalThresholdNs = 0;
    int64_t fenceTimeoutNs = 0;
    int32_t autoSwapInterval = 1;
    int32_t bufferStuffingFixWait = 0;
    PipelineMode pipelineMode = PipelineMode::On;
    bool autoSwapIntervalEnabled = true;
    bool autoPipelineMode = true;
    bool framePacingEnabled = true;
    bool blockingWaitEnabled = true;
    bool useAffinity = true;
};

// Tracks per-instance trace attachment and settings changes so each Perfetto
// trace session starts with a Swappy.Settings snapshot and baseline counters.
class TraceSession {
public:
    void onUntracedFrame() noexcept {
        mLastSettingsKey.reset();
    }

    void onTracedFrame(const TraceSettingsSnapshot& snap) {
        const int pipelineConfig =
                snap.autoPipelineMode ? 0 : (snap.pipelineMode == PipelineMode::On ? 1 : 2);
        const SettingsKey key = settingsKey(snap, pipelineConfig);
        const bool attaching = !mLastSettingsKey.has_value();

        if (attaching || key != *mLastSettingsKey) {
            const char* pipelineStr = "auto";
            if (pipelineConfig == 1) {
                pipelineStr = "on";
            } else if (pipelineConfig == 2) {
                pipelineStr = "off";
            }

            TRACE_INSTANT("Swappy.Settings impl=Classic(drop=%d%%,pipeOff=%d%%) swap=%.2fms "
                          "refresh=%.2fms autoSI=%d autoPM=%d pipeline=%s maxAuto=%.0fms "
                          "pacing=%d block=%d fence=%.0fms stuffFix=%d affinity=%d",
                          FRAME_DROP_THRESHOLD, NON_PIPELINE_PERCENT,
                          static_cast<double>(snap.swapDurationNs) / 1e6,
                          static_cast<double>(snap.refreshPeriodNs) / 1e6,
                          static_cast<int>(snap.autoSwapIntervalEnabled),
                          static_cast<int>(snap.autoPipelineMode), pipelineStr,
                          static_cast<double>(snap.autoSwapIntervalThresholdNs) / 1e6,
                          static_cast<int>(snap.framePacingEnabled),
                          static_cast<int>(snap.blockingWaitEnabled),
                          static_cast<double>(snap.fenceTimeoutNs) / 1e6,
                          snap.bufferStuffingFixWait, static_cast<int>(snap.useAffinity));
            TRACE_INT("mPipelineModeAutoMode", static_cast<int64_t>(snap.autoPipelineMode));
            TRACE_INT("mPipelineMode", static_cast<int64_t>(snap.pipelineMode));
            TRACE_INT("Swappy.TargetPeriodNs", snap.targetPeriodNs);
            TRACE_INT("mAutoSwapInterval", snap.autoSwapInterval);
            TRACE_INT("mCommonSettings.refreshPeriod", snap.refreshPeriodNs);
            TRACE_INT("mSwapDuration", snap.swapDurationNs);
        }

        mLastSettingsKey = key;
    }

private:
    // Swappy.Settings fields, grouped by size to limit padding. Uses
    // pipelineConfig, not pipelineMode, so auto-mode pipeline flips do not re-emit.
    using SettingsKey = std::tuple<int64_t, int64_t, int64_t, int64_t, int, int32_t, bool, bool,
                                   bool, bool, bool>;

    static SettingsKey settingsKey(const TraceSettingsSnapshot& snap, int pipelineConfig) noexcept {
        return std::make_tuple(snap.swapDurationNs, snap.refreshPeriodNs,
                               snap.autoSwapIntervalThresholdNs, snap.fenceTimeoutNs,
                               pipelineConfig, snap.bufferStuffingFixWait,
                               snap.autoSwapIntervalEnabled, snap.autoPipelineMode,
                               snap.framePacingEnabled, snap.blockingWaitEnabled, snap.useAffinity);
    }

    // Last emitted key. Empty while no trace session is attached.
    std::optional<SettingsKey> mLastSettingsKey;
};

constexpr const char* traceBackendStr(TraceBackend backend) noexcept {
    return backend == TraceBackend::GL ? "gl" : "vk";
}

inline void traceWorkTimes(std::chrono::nanoseconds cpuTime, std::chrono::nanoseconds gpuTime) {
    TRACE_INT("Swappy.CpuTimeUs",
              std::chrono::duration_cast<std::chrono::microseconds>(cpuTime).count());
    if (gpuTime > std::chrono::nanoseconds(0)) {
        TRACE_INT("Swappy.GpuTimeUs",
                  std::chrono::duration_cast<std::chrono::microseconds>(gpuTime).count());
    }
}

inline void traceWakeError(std::chrono::nanoseconds wakeError) {
    TRACE_INT("Swappy.WakeErrorUs",
              std::chrono::duration_cast<std::chrono::microseconds>(wakeError).count());
}

// Returns how late the waiter woke relative to the wake deadline of tick
// `targetFrame`. `latestTickDeadline` is the wake deadline of tick
// `currentFrame`. Each tick after `targetFrame` moves that deadline one
// refresh period later, so step back by that many periods.
inline std::chrono::nanoseconds wakeErrorForTarget(
        std::chrono::steady_clock::time_point now,
        std::chrono::steady_clock::time_point latestTickDeadline, int32_t currentFrame,
        int32_t targetFrame, std::chrono::nanoseconds refreshPeriod) {
    const int32_t ticksPastTarget = currentFrame - targetFrame;
    return now - (latestTickDeadline - ticksPastTarget * refreshPeriod);
}

// Remembers the present time that Swappy requested for recent frames. GL
// reads it back when the frame timestamps arrive, because EGL reports a
// "requested" time also for frames where Swappy set none. A surface on a new
// window starts its frame ids again, so all slots are reset when the surface
// changes or the frame id goes backwards.
class TracePresentTargets {
public:
    // Frame ids are sequential, so each of the last kCapacity frames has its
    // own slot.
    static constexpr size_t kCapacity = 16;

    void observeNextFrame(uint64_t frameId, const void* surface = nullptr) noexcept {
        if (mHasLastFrame && (surface != mLastSurface || frameId < mLastFrameId)) {
            mSlots = {};
        }
        // The frame has no target until record() sets one.
        mSlots[frameId % kCapacity] = {};
        mHasLastFrame = true;
        mLastSurface = surface;
        mLastFrameId = frameId;
    }

    void record(uint64_t frameId, int64_t targetNs, const void* surface = nullptr) noexcept {
        observeNextFrame(frameId, surface);
        mSlots[frameId % kCapacity] = {surface, frameId, targetNs};
    }

    // Returns 0 when Swappy set no present time for frameId on surface.
    int64_t targetFor(uint64_t frameId, const void* surface = nullptr) const noexcept {
        const Slot& slot = mSlots[frameId % kCapacity];
        return slot.surface == surface && slot.frameId == frameId ? slot.targetNs : 0;
    }

private:
    struct Slot {
        const void* surface = nullptr;
        uint64_t frameId = UINT64_MAX;
        int64_t targetNs = 0;
    };
    std::array<Slot, kCapacity> mSlots{};
    bool mHasLastFrame = false;
    const void* mLastSurface = nullptr;
    uint64_t mLastFrameId = 0;
};

inline void tracePresentTarget(uint64_t frameId, std::chrono::steady_clock::time_point targetTp,
                               int32_t swapInterval, TraceBackend backend) {
    const auto currentNow = std::chrono::steady_clock::now();
    TRACE_INT("Swappy.TargetLeadUs",
              std::chrono::duration_cast<std::chrono::microseconds>(targetTp - currentNow).count());
    TRACE_INSTANT("Swappy.Target f=%" PRIu64 " t=%" PRId64 " n=%" PRId64 " si=%d be=%s", frameId,
                  static_cast<int64_t>(targetTp.time_since_epoch().count()),
                  static_cast<int64_t>(currentNow.time_since_epoch().count()), swapInterval,
                  traceBackendStr(backend));
}

inline void tracePresentActual(uint64_t frameId, int64_t actualNs, int64_t desiredNs,
                               int64_t marginNs, TraceBackend backend) {
    if (actualNs <= 0) {
        return;
    }
    const auto currentNow = std::chrono::steady_clock::now();
    if (desiredNs > 0) {
        TRACE_INT("Swappy.PresentErrorUs", (actualNs - desiredNs) / 1000);
    }
    if (marginNs >= 0) {
        TRACE_INT("Swappy.PresentMarginUs", marginNs / 1000);
    }
    TRACE_INSTANT("Swappy.Actual f=%" PRIu64 " t=%" PRId64 " d=%" PRId64 " n=%" PRId64 " be=%s",
                  frameId, actualNs, desiredNs,
                  static_cast<int64_t>(currentNow.time_since_epoch().count()),
                  traceBackendStr(backend));
}

} // namespace swappy

// Evaluates swappy_p_snapshotExpr, which gives a TraceSettingsSnapshot, only
// while tracing is enabled.
#define SWAPPY_TRACE_SETTINGS(swappy_p_session, swappy_p_snapshotExpr) \
    do {                                                               \
        if (!TRACE_ENABLED()) {                                        \
            (swappy_p_session).onUntracedFrame();                      \
        } else {                                                       \
            (swappy_p_session).onTracedFrame((swappy_p_snapshotExpr)); \
        }                                                              \
    } while (0)

#define SWAPPY_TRACE_WORK_TIMES(swappy_p_cpuNs, swappy_p_gpuNs)           \
    do {                                                                  \
        if (TRACE_ENABLED()) {                                            \
            ::swappy::traceWorkTimes((swappy_p_cpuNs), (swappy_p_gpuNs)); \
        }                                                                 \
    } while (0)

#define SWAPPY_TRACE_WAKE_ERROR(swappy_p_wakeErrorNs)         \
    do {                                                      \
        if (TRACE_ENABLED()) {                                \
            ::swappy::traceWakeError((swappy_p_wakeErrorNs)); \
        }                                                     \
    } while (0)

#define SWAPPY_TRACE_PRESENT_TARGET(swappy_p_frameId, swappy_p_targetTp, swappy_p_swapInterval, \
                                    swappy_p_backend)                                           \
    do {                                                                                        \
        if (TRACE_ENABLED()) {                                                                  \
            ::swappy::tracePresentTarget((swappy_p_frameId), (swappy_p_targetTp),               \
                                         (swappy_p_swapInterval), (swappy_p_backend));          \
        }                                                                                       \
    } while (0)

#define SWAPPY_TRACE_PRESENT_ACTUAL(swappy_p_frameId, swappy_p_actualNs, swappy_p_desiredNs, \
                                    swappy_p_marginNs, swappy_p_backend)                     \
    do {                                                                                     \
        if (TRACE_ENABLED()) {                                                               \
            ::swappy::tracePresentActual((swappy_p_frameId), (swappy_p_actualNs),            \
                                         (swappy_p_desiredNs), (swappy_p_marginNs),          \
                                         (swappy_p_backend));                                \
        }                                                                                    \
    } while (0)

#endif

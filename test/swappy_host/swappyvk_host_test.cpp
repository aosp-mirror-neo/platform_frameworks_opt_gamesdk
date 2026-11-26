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
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Clock.h"
#include "FakeSwappyPlatform.h"
#include "Settings.h"
#include "SimulatedClock.h"
#include "SwappyVk.h"
#include "TraceTestHelper.h"
#include "gtest/gtest.h"
#include "swappy/swappyVk.h"

namespace swappy {

class SwappyVkTestPeer {
public:
    class ScopedHooks {
    public:
        ScopedHooks(SwappyVk::PlatformFactory factory, const Clock& clock,
                    const SwappyVkFunctionProvider* provider) {
            SwappyVk& instance = SwappyVk::getInstance();
            instance.mPlatformFactory = std::move(factory);
            instance.mClock = &clock;
            SwappyVk_setFunctionProvider(provider);
        }

        ~ScopedHooks() {
            SwappyVk::getInstance().clearState();
        }

        ScopedHooks(const ScopedHooks&) = delete;
        ScopedHooks& operator=(const ScopedHooks&) = delete;
    };

    static void clearAll() {
        SwappyVk::getInstance().clearState();
    }
};

} // namespace swappy

namespace {

using namespace std::chrono_literals;
using swappy::Clock;
using swappy::Settings;
using swappy::SwappyCommonSettings;
using swappy::SwappyVkTestPeer;
using swappy::testing::FakeSwappyPlatform;
using swappy::testing::makeTestSwappySettings;
using swappy::testing::PlatformSharedState;
using swappy::testing::PlatformSnapshot;
using swappy::testing::SimulatedClock;
using swappy::testing::snapshotPlatformState;
using swappy::testing::VsyncPanelTimeline;

constexpr auto k60HzPeriod = 16'666'667ns;
constexpr auto k30HzPeriod = 33'333'334ns;

template <typename HandleT>
HandleT makeVkHandle(uintptr_t rawId) {
    if constexpr (std::is_pointer_v<HandleT>) {
        return reinterpret_cast<HandleT>(rawId);
    } else {
        return static_cast<HandleT>(rawId);
    }
}

template <typename HandleT>
uintptr_t vkHandleId(HandleT handle) {
    if constexpr (std::is_pointer_v<HandleT>) {
        return reinterpret_cast<uintptr_t>(handle);
    } else {
        return static_cast<uintptr_t>(handle);
    }
}

SwappyCommonSettings makeVkSettings(std::chrono::nanoseconds refreshPeriod = k60HzPeriod,
                                    int sdkInt = 34) {
    return makeTestSwappySettings(refreshPeriod, sdkInt);
}

struct RecordedSubmit {
    std::vector<VkSemaphore> waitSemaphores;
    std::vector<VkPipelineStageFlags> waitDstStageMasks;
    uint32_t commandBufferCount = 0;
    std::vector<VkSemaphore> signalSemaphores;
    VkFence fence = VK_NULL_HANDLE;
    bool hadNullWaitSemaphores = false;
    bool hadNullWaitDstStageMask = false;
};

struct RecordedPresent {
    uint32_t imageIndex = 0;
    bool hasDisplayTiming = false;
    uint32_t presentId = 0;
    uint64_t desiredPresentTimeNs = 0;
    uint64_t actualPresentTimeNs = 0;
    VkSemaphore waitedSemaphore = VK_NULL_HANDLE;
    std::vector<VkSemaphore> waitedSemaphores;
};

class ScopedVkTracer {
public:
    explicit ScopedVkTracer(const SwappyTracer* tracer) : mTracer(tracer) {
        SwappyVk_injectTracer(mTracer);
    }

    ~ScopedVkTracer() {
        SwappyVk_uninjectTracer(mTracer);
    }

    ScopedVkTracer(const ScopedVkTracer&) = delete;
    ScopedVkTracer& operator=(const ScopedVkTracer&) = delete;

private:
    const SwappyTracer* mTracer;
};

// In-memory Vulkan driver fake implementing the entry points consumed by
// SwappyVk, SwappyVkBase, SwappyVkGoogleDisplayTiming, and SwappyVkFallback.
class FakeVulkanDriver {
public:
    explicit FakeVulkanDriver(const SimulatedClock& clock,
                              std::chrono::nanoseconds refreshPeriod = k60HzPeriod)
          : mClock(clock), mRefreshPeriod(refreshPeriod) {
        sActiveDriver = this;
    }

    ~FakeVulkanDriver() {
        if (sActiveDriver == this) {
            SwappyVkTestPeer::clearAll();
            sActiveDriver = nullptr;
        }
    }

    FakeVulkanDriver(const FakeVulkanDriver&) = delete;
    FakeVulkanDriver& operator=(const FakeVulkanDriver&) = delete;

    [[nodiscard]] const SwappyVkFunctionProvider* provider() const noexcept {
        return &kFunctionProvider;
    }

    void setNextGpuWork(std::chrono::nanoseconds gpuWork) {
        std::lock_guard<std::mutex> lock(mMutex);
        mNextGpuWork = gpuWork;
    }

    void setExtraPresentDelay(std::chrono::nanoseconds extraDelay) {
        std::lock_guard<std::mutex> lock(mMutex);
        mExtraPresentDelay = extraDelay;
    }

    [[nodiscard]] std::vector<RecordedSubmit> recordedSubmits() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mRecordedSubmits;
    }

    [[nodiscard]] std::vector<RecordedPresent> recordedPresents() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mRecordedPresents;
    }

    [[nodiscard]] Clock::time_point imageReleaseTime(uint32_t imageIndex) const {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto it = mImageReleaseTimes.find(imageIndex);
        return it != mImageReleaseTimes.end() ? it->second : Clock::time_point{};
    }

private:
    struct FenceState {
        bool signaled = false;
        Clock::time_point completionTime{};
    };

    static inline FakeVulkanDriver* sActiveDriver = nullptr;

    uintptr_t allocateHandleLocked() REQUIRES(mMutex) {
        return mNextHandleId++;
    }

    VkResult createFence(const VkFenceCreateInfo* createInfo, VkFence* outFence) {
        std::lock_guard<std::mutex> lock(mMutex);
        const uintptr_t id = allocateHandleLocked();
        const bool signaled = (createInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
        mFences[id] = FenceState{signaled, mClock.now()};
        *outFence = makeVkHandle<VkFence>(id);
        return VK_SUCCESS;
    }

    void destroyFence(VkFence fence) {
        std::lock_guard<std::mutex> lock(mMutex);
        mFences.erase(vkHandleId(fence));
    }

    VkResult resetFences(uint32_t fenceCount, const VkFence* fences) {
        std::lock_guard<std::mutex> lock(mMutex);
        for (uint32_t i = 0; i < fenceCount; ++i) {
            auto& state = mFences[vkHandleId(fences[i])];
            state.signaled = false;
            state.completionTime = Clock::time_point::max();
        }
        return VK_SUCCESS;
    }

    VkResult getFenceStatus(VkFence fence) {
        std::lock_guard<std::mutex> lock(mMutex);
        auto& state = mFences[vkHandleId(fence)];
        if (mClock.isShutdown() || mClock.now() >= state.completionTime) {
            state.signaled = true;
        }
        return state.signaled ? VK_SUCCESS : VK_NOT_READY;
    }

    VkResult waitForFences(uint32_t fenceCount, const VkFence* fences, uint64_t timeoutNs) {
        Clock::time_point deadline = mClock.now();
        for (uint32_t i = 0; i < fenceCount; ++i) {
            std::lock_guard<std::mutex> lock(mMutex);
            auto& state = mFences[vkHandleId(fences[i])];
            if (!state.signaled && state.completionTime > deadline) {
                deadline = state.completionTime;
            }
        }

        const Clock::time_point start = mClock.now();
        Clock::time_point sleepTarget = deadline;
        if (timeoutNs != UINT64_MAX) {
            const uint64_t cappedTimeoutNs = std::min<uint64_t>(timeoutNs, 1'000'000'000'000ULL);
            const Clock::time_point timeoutPoint =
                    start + std::chrono::nanoseconds(static_cast<int64_t>(cappedTimeoutNs));
            sleepTarget = std::min(deadline, timeoutPoint);
        }
        if (sleepTarget > start) {
            mClock.sleepUntil(sleepTarget);
        }

        const Clock::time_point after = mClock.now();
        bool allSignaled = true;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (uint32_t i = 0; i < fenceCount; ++i) {
                auto& state = mFences[vkHandleId(fences[i])];
                if (mClock.isShutdown() || after >= state.completionTime) {
                    state.signaled = true;
                }
                if (!state.signaled) {
                    allSignaled = false;
                }
            }
        }
        return allSignaled ? VK_SUCCESS : VK_TIMEOUT;
    }

    VkResult queueSubmit(uint32_t submitCount, const VkSubmitInfo* submits, VkFence fence) {
        if (submitCount > 0) {
            EXPECT_NE(submits, nullptr);
        }
        std::lock_guard<std::mutex> lock(mMutex);
        if (submits != nullptr) {
            for (uint32_t i = 0; i < submitCount; ++i) {
                const VkSubmitInfo& submit = submits[i];
                EXPECT_EQ(submit.sType, VK_STRUCTURE_TYPE_SUBMIT_INFO);
                RecordedSubmit recorded{};
                recorded.commandBufferCount = submit.commandBufferCount;
                recorded.fence = fence;
                recorded.hadNullWaitSemaphores = (submit.pWaitSemaphores == nullptr);
                recorded.hadNullWaitDstStageMask = (submit.pWaitDstStageMask == nullptr);
                if (submit.waitSemaphoreCount > 0) {
                    EXPECT_NE(submit.pWaitSemaphores, nullptr);
                    EXPECT_NE(submit.pWaitDstStageMask, nullptr);
                    if (submit.pWaitSemaphores != nullptr) {
                        recorded.waitSemaphores.assign(submit.pWaitSemaphores,
                                                       submit.pWaitSemaphores +
                                                               submit.waitSemaphoreCount);
                    }
                    if (submit.pWaitDstStageMask != nullptr) {
                        recorded.waitDstStageMasks.assign(submit.pWaitDstStageMask,
                                                          submit.pWaitDstStageMask +
                                                                  submit.waitSemaphoreCount);
                        for (VkPipelineStageFlags stageMask : recorded.waitDstStageMasks) {
                            EXPECT_NE(stageMask, 0u);
                        }
                    }
                }
                if (submit.signalSemaphoreCount > 0) {
                    EXPECT_NE(submit.pSignalSemaphores, nullptr);
                    if (submit.pSignalSemaphores != nullptr) {
                        recorded.signalSemaphores.assign(submit.pSignalSemaphores,
                                                         submit.pSignalSemaphores +
                                                                 submit.signalSemaphoreCount);
                    }
                }
                mRecordedSubmits.push_back(std::move(recorded));
            }
        }

        const Clock::time_point now = mClock.now();
        const Clock::time_point start =
                mHasQueueSubmission ? std::max(now, mLastQueueCompletion) : now;
        const Clock::time_point completion = start + std::max(0ns, mNextGpuWork);
        mLastQueueCompletion = completion;
        mHasQueueSubmission = true;
        if (fence != VK_NULL_HANDLE) {
            auto& state = mFences[vkHandleId(fence)];
            state.signaled = (completion <= now);
            state.completionTime = completion;
        }
        return VK_SUCCESS;
    }

    VkResult queuePresent(const VkPresentInfoKHR* presentInfo) {
        std::lock_guard<std::mutex> lock(mMutex);
        RecordedPresent record{};
        record.imageIndex = presentInfo->pImageIndices[0];
        if (presentInfo->waitSemaphoreCount > 0) {
            EXPECT_NE(presentInfo->pWaitSemaphores, nullptr);
            if (presentInfo->pWaitSemaphores != nullptr) {
                record.waitedSemaphores.assign(presentInfo->pWaitSemaphores,
                                               presentInfo->pWaitSemaphores +
                                                       presentInfo->waitSemaphoreCount);
                record.waitedSemaphore = record.waitedSemaphores.front();
            }
        }

        const Clock::time_point gpuDone = mHasQueueSubmission ? mLastQueueCompletion : mClock.now();
        Clock::time_point minPresent = gpuDone;
        Clock::time_point actualPresent = gpuDone + mExtraPresentDelay;

        const auto* nextStruct = static_cast<const VkBaseInStructure*>(presentInfo->pNext);
        while (nextStruct != nullptr) {
            if (nextStruct->sType == VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE) {
                const auto* timesInfo =
                        reinterpret_cast<const VkPresentTimesInfoGOOGLE*>(nextStruct);
                record.hasDisplayTiming = true;
                record.presentId = timesInfo->pTimes[0].presentID;
                record.desiredPresentTimeNs = timesInfo->pTimes[0].desiredPresentTime;
                const Clock::time_point desiredPoint{std::chrono::nanoseconds(
                        static_cast<int64_t>(record.desiredPresentTimeNs))};
                if (record.desiredPresentTimeNs > 0 && desiredPoint > minPresent) {
                    minPresent = desiredPoint;
                }
                const int64_t periodNs = mRefreshPeriod.count();
                const int64_t minNs = minPresent.time_since_epoch().count();
                const int64_t snappedNs =
                        periodNs > 0 ? (((minNs + periodNs - 1) / periodNs) * periodNs) : minNs;
                actualPresent =
                        Clock::time_point{std::chrono::nanoseconds(snappedNs)} + mExtraPresentDelay;
                const uint64_t gpuDoneNs =
                        static_cast<uint64_t>(gpuDone.time_since_epoch().count());
                const uint64_t actualNs =
                        static_cast<uint64_t>(actualPresent.time_since_epoch().count());
                const uint64_t marginNs = actualNs > gpuDoneNs ? (actualNs - gpuDoneNs) : 0;
                mPendingTimingHistory.push_back(VkPastPresentationTimingGOOGLE{
                        .presentID = record.presentId,
                        .desiredPresentTime = record.desiredPresentTimeNs,
                        .actualPresentTime = actualNs,
                        .earliestPresentTime = actualNs,
                        .presentMargin = marginNs,
                });
                break;
            }
            nextStruct = nextStruct->pNext;
        }

        record.actualPresentTimeNs =
                static_cast<uint64_t>(actualPresent.time_since_epoch().count());
        mImageReleaseTimes[record.imageIndex] = actualPresent;
        mRecordedPresents.push_back(record);
        return VK_SUCCESS;
    }

    VkResult getRefreshCycleDuration(VkRefreshCycleDurationGOOGLE* outDuration) {
        std::lock_guard<std::mutex> lock(mMutex);
        outDuration->refreshDuration = static_cast<uint64_t>(mRefreshPeriod.count());
        return VK_SUCCESS;
    }

    VkResult getPastPresentationTimings(uint32_t* count, VkPastPresentationTimingGOOGLE* timings) {
        std::lock_guard<std::mutex> lock(mMutex);
        // Mirror Android libvulkan swapchain.cpp MIN_NUM_FRAMES_AGO = 5: the newest
        // 4 presented frames remain in flight and are not yet returned.
        constexpr size_t kInFlightHistoryHoldback = 4;
        const size_t available = mPendingTimingHistory.size() > kInFlightHistoryHoldback
                ? (mPendingTimingHistory.size() - kInFlightHistoryHoldback)
                : 0;
        if (timings == nullptr) {
            *count = static_cast<uint32_t>(available);
            return VK_SUCCESS;
        }
        const size_t toCopy = std::min<size_t>(*count, available);
        for (size_t i = 0; i < toCopy; ++i) {
            timings[i] = mPendingTimingHistory.front();
            mPendingTimingHistory.pop_front();
        }
        const bool moreAvailable = (mPendingTimingHistory.size() > kInFlightHistoryHoldback);
        *count = static_cast<uint32_t>(toCopy);
        return moreAvailable ? VK_INCOMPLETE : VK_SUCCESS;
    }

    static bool providerInit() {
        return true;
    }

    static void providerClose() {}

    static VKAPI_ATTR VkResult VKAPI_CALL
    vkCreateCommandPoolHook(VkDevice /*device*/, const VkCommandPoolCreateInfo* /*createInfo*/,
                            const VkAllocationCallbacks* /*allocator*/, VkCommandPool* outPool) {
        std::lock_guard<std::mutex> lock(sActiveDriver->mMutex);
        *outPool = makeVkHandle<VkCommandPool>(sActiveDriver->allocateHandleLocked());
        return VK_SUCCESS;
    }

    static VKAPI_ATTR void VKAPI_CALL
    vkDestroyCommandPoolHook(VkDevice /*device*/, VkCommandPool /*pool*/,
                             const VkAllocationCallbacks* /*allocator*/) {}

    static VKAPI_ATTR VkResult VKAPI_CALL
    vkCreateFenceHook(VkDevice /*device*/, const VkFenceCreateInfo* createInfo,
                      const VkAllocationCallbacks* /*allocator*/, VkFence* outFence) {
        return sActiveDriver->createFence(createInfo, outFence);
    }

    static VKAPI_ATTR void VKAPI_CALL vkDestroyFenceHook(
            VkDevice /*device*/, VkFence fence, const VkAllocationCallbacks* /*allocator*/) {
        sActiveDriver->destroyFence(fence);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFencesHook(VkDevice /*device*/,
                                                              uint32_t fenceCount,
                                                              const VkFence* fences,
                                                              VkBool32 /*waitAll*/,
                                                              uint64_t timeout) {
        return sActiveDriver->waitForFences(fenceCount, fences, timeout);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatusHook(VkDevice /*device*/, VkFence fence) {
        return sActiveDriver->getFenceStatus(fence);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL vkResetFencesHook(VkDevice /*device*/,
                                                            uint32_t fenceCount,
                                                            const VkFence* fences) {
        return sActiveDriver->resetFences(fenceCount, fences);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL
    vkCreateSemaphoreHook(VkDevice /*device*/, const VkSemaphoreCreateInfo* /*createInfo*/,
                          const VkAllocationCallbacks* /*allocator*/, VkSemaphore* outSemaphore) {
        std::lock_guard<std::mutex> lock(sActiveDriver->mMutex);
        *outSemaphore = makeVkHandle<VkSemaphore>(sActiveDriver->allocateHandleLocked());
        return VK_SUCCESS;
    }

    static VKAPI_ATTR void VKAPI_CALL
    vkDestroySemaphoreHook(VkDevice /*device*/, VkSemaphore /*semaphore*/,
                           const VkAllocationCallbacks* /*allocator*/) {}

    static VKAPI_ATTR VkResult VKAPI_CALL
    vkCreateEventHook(VkDevice /*device*/, const VkEventCreateInfo* /*createInfo*/,
                      const VkAllocationCallbacks* /*allocator*/, VkEvent* outEvent) {
        std::lock_guard<std::mutex> lock(sActiveDriver->mMutex);
        *outEvent = makeVkHandle<VkEvent>(sActiveDriver->allocateHandleLocked());
        return VK_SUCCESS;
    }

    static VKAPI_ATTR void VKAPI_CALL vkDestroyEventHook(
            VkDevice /*device*/, VkEvent /*event*/, const VkAllocationCallbacks* /*allocator*/) {}

    static VKAPI_ATTR void VKAPI_CALL vkCmdSetEventHook(VkCommandBuffer /*commandBuffer*/,
                                                        VkEvent /*event*/,
                                                        VkPipelineStageFlags /*stageMask*/) {}

    static VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffersHook(
            VkDevice /*device*/, const VkCommandBufferAllocateInfo* allocateInfo,
            VkCommandBuffer* outBuffers) {
        std::lock_guard<std::mutex> lock(sActiveDriver->mMutex);
        for (uint32_t i = 0; i < allocateInfo->commandBufferCount; ++i) {
            outBuffers[i] = makeVkHandle<VkCommandBuffer>(sActiveDriver->allocateHandleLocked());
        }
        return VK_SUCCESS;
    }

    static VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffersHook(VkDevice /*device*/,
                                                               VkCommandPool /*pool*/,
                                                               uint32_t /*count*/,
                                                               const VkCommandBuffer* /*buffers*/) {
    }

    static VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBufferHook(
            VkCommandBuffer /*buffer*/, const VkCommandBufferBeginInfo* /*beginInfo*/) {
        return VK_SUCCESS;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBufferHook(VkCommandBuffer /*buffer*/) {
        return VK_SUCCESS;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmitHook(VkQueue /*queue*/, uint32_t submitCount,
                                                            const VkSubmitInfo* submits,
                                                            VkFence fence) {
        return sActiveDriver->queueSubmit(submitCount, submits, fence);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL
    vkQueuePresentKHRHook(VkQueue /*queue*/, const VkPresentInfoKHR* presentInfo) {
        return sActiveDriver->queuePresent(presentInfo);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL
    vkGetRefreshCycleDurationGOOGLEHook(VkDevice /*device*/, VkSwapchainKHR /*swapchain*/,
                                        VkRefreshCycleDurationGOOGLE* outDuration) {
        return sActiveDriver->getRefreshCycleDuration(outDuration);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL vkGetPastPresentationTimingGOOGLEHook(
            VkDevice /*device*/, VkSwapchainKHR /*swapchain*/, uint32_t* count,
            VkPastPresentationTimingGOOGLE* timings) {
        return sActiveDriver->getPastPresentationTimings(count, timings);
    }

    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddrHook(VkDevice /*device*/,
                                                                            const char* name) {
        if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(&vkQueuePresentKHRHook);
        }
        if (std::strcmp(name, "vkGetRefreshCycleDurationGOOGLE") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(&vkGetRefreshCycleDurationGOOGLEHook);
        }
        if (std::strcmp(name, "vkGetPastPresentationTimingGOOGLE") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPastPresentationTimingGOOGLEHook);
        }
        return nullptr;
    }

    static void* providerGetProcAddr(const char* name) {
        if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
            return reinterpret_cast<void*>(&vkGetDeviceProcAddrHook);
        }
        if (std::strcmp(name, "vkCreateCommandPool") == 0) {
            return reinterpret_cast<void*>(&vkCreateCommandPoolHook);
        }
        if (std::strcmp(name, "vkDestroyCommandPool") == 0) {
            return reinterpret_cast<void*>(&vkDestroyCommandPoolHook);
        }
        if (std::strcmp(name, "vkCreateFence") == 0) {
            return reinterpret_cast<void*>(&vkCreateFenceHook);
        }
        if (std::strcmp(name, "vkDestroyFence") == 0) {
            return reinterpret_cast<void*>(&vkDestroyFenceHook);
        }
        if (std::strcmp(name, "vkWaitForFences") == 0) {
            return reinterpret_cast<void*>(&vkWaitForFencesHook);
        }
        if (std::strcmp(name, "vkGetFenceStatus") == 0) {
            return reinterpret_cast<void*>(&vkGetFenceStatusHook);
        }
        if (std::strcmp(name, "vkResetFences") == 0) {
            return reinterpret_cast<void*>(&vkResetFencesHook);
        }
        if (std::strcmp(name, "vkCreateSemaphore") == 0) {
            return reinterpret_cast<void*>(&vkCreateSemaphoreHook);
        }
        if (std::strcmp(name, "vkDestroySemaphore") == 0) {
            return reinterpret_cast<void*>(&vkDestroySemaphoreHook);
        }
        if (std::strcmp(name, "vkCreateEvent") == 0) {
            return reinterpret_cast<void*>(&vkCreateEventHook);
        }
        if (std::strcmp(name, "vkDestroyEvent") == 0) {
            return reinterpret_cast<void*>(&vkDestroyEventHook);
        }
        if (std::strcmp(name, "vkCmdSetEvent") == 0) {
            return reinterpret_cast<void*>(&vkCmdSetEventHook);
        }
        if (std::strcmp(name, "vkAllocateCommandBuffers") == 0) {
            return reinterpret_cast<void*>(&vkAllocateCommandBuffersHook);
        }
        if (std::strcmp(name, "vkFreeCommandBuffers") == 0) {
            return reinterpret_cast<void*>(&vkFreeCommandBuffersHook);
        }
        if (std::strcmp(name, "vkBeginCommandBuffer") == 0) {
            return reinterpret_cast<void*>(&vkBeginCommandBufferHook);
        }
        if (std::strcmp(name, "vkEndCommandBuffer") == 0) {
            return reinterpret_cast<void*>(&vkEndCommandBufferHook);
        }
        if (std::strcmp(name, "vkQueueSubmit") == 0) {
            return reinterpret_cast<void*>(&vkQueueSubmitHook);
        }
        return nullptr;
    }

    static inline const SwappyVkFunctionProvider kFunctionProvider{
            &FakeVulkanDriver::providerInit,
            &FakeVulkanDriver::providerGetProcAddr,
            &FakeVulkanDriver::providerClose,
    };

    const SimulatedClock& mClock;
    mutable std::mutex mMutex;
    std::chrono::nanoseconds mRefreshPeriod GUARDED_BY(mMutex);
    uintptr_t mNextHandleId GUARDED_BY(mMutex) = 1000;
    std::map<uintptr_t, FenceState> mFences GUARDED_BY(mMutex);
    Clock::time_point mLastQueueCompletion GUARDED_BY(mMutex){};
    bool mHasQueueSubmission GUARDED_BY(mMutex) = false;
    std::chrono::nanoseconds mNextGpuWork GUARDED_BY(mMutex){2ms};
    std::chrono::nanoseconds mExtraPresentDelay GUARDED_BY(mMutex){0ns};
    std::vector<RecordedSubmit> mRecordedSubmits GUARDED_BY(mMutex);
    std::vector<RecordedPresent> mRecordedPresents GUARDED_BY(mMutex);
    std::deque<VkPastPresentationTimingGOOGLE> mPendingTimingHistory GUARDED_BY(mMutex);
    std::map<uint32_t, Clock::time_point> mImageReleaseTimes GUARDED_BY(mMutex);
};

class SimulatedSwappyVkHarness {
public:
    explicit SimulatedSwappyVkHarness(bool advertiseGoogleDisplayTiming = true,
                                      SwappyCommonSettings settings = makeVkSettings(),
                                      Clock::time_point initialTime = Clock::time_point{1s})
          : mState(std::make_shared<PlatformSharedState>()),
            mClock(initialTime),
            mDriver(mClock, settings.refreshPeriod),
            mTimeline(mState, settings, &mClock),
            mTestHooks(
                    [state = mState](JNIEnv* /*env*/, jobject /*activity*/) {
                        return std::make_unique<FakeSwappyPlatform>(state);
                    },
                    mClock, mDriver.provider()) {
        mClock.setVsyncCallbacks(
                [this](Clock::time_point current) { return mTimeline.nextVsyncAfter(current); },
                [this](Clock::time_point vsyncTime) { mTimeline.deliverVsync(vsyncTime); });

        if (advertiseGoogleDisplayTiming) {
            VkExtensionProperties ext{};
            std::strncpy(ext.extensionName, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
                         VK_MAX_EXTENSION_NAME_SIZE - 1);
            ext.specVersion = 1;
            uint32_t requiredCount = 0;
            SwappyVk_determineDeviceExtensions(mPhysicalDevice, 1, &ext, &requiredCount, nullptr);
            EXPECT_EQ(requiredCount, 1u);
            std::array<char, VK_MAX_EXTENSION_NAME_SIZE> extNameBuf{};
            char* requiredNames[1] = {extNameBuf.data()};
            SwappyVk_determineDeviceExtensions(mPhysicalDevice, 1, &ext, &requiredCount,
                                               requiredNames);
        } else {
            uint32_t requiredCount = 0;
            SwappyVk_determineDeviceExtensions(mPhysicalDevice, 0, nullptr, &requiredCount,
                                               nullptr);
            EXPECT_EQ(requiredCount, 0u);
        }

        SwappyVk_setQueueFamilyIndex(mDevice, mQueue, 0);
        uint64_t refreshDurationNs = 0;
        EXPECT_TRUE(SwappyVk_initAndGetRefreshCycleDuration(nullptr, nullptr, mPhysicalDevice,
                                                            mDevice, mSwapchain,
                                                            &refreshDurationNs));
        EXPECT_EQ(refreshDurationNs, static_cast<uint64_t>(settings.refreshPeriod.count()));
        SwappyVk_setWindow(mDevice, mSwapchain, &mWindow);
        SwappyVk_setSwapIntervalNS(mDevice, mSwapchain, SWAPPY_SWAP_60FPS);
        EXPECT_TRUE(mClock.waitForWorkersSettled());
    }

    ~SimulatedSwappyVkHarness() {
        SwappyVk_destroySwapchain(mDevice, mSwapchain);
        SwappyVk_destroyDevice(mDevice);
        SwappyVkTestPeer::clearAll();
        mClock.shutdown();
    }

    [[nodiscard]] VkDevice device() const noexcept {
        return mDevice;
    }

    [[nodiscard]] VkSwapchainKHR swapchain() const noexcept {
        return mSwapchain;
    }

    [[nodiscard]] VkSemaphore appSemaphore() const noexcept {
        return mAppRenderSemaphore;
    }

    SimulatedClock& clock() noexcept {
        return mClock;
    }

    FakeVulkanDriver& driver() noexcept {
        return mDriver;
    }

    VkResult presentFrameWithRawSemaphores(uint32_t imageIndex, uint32_t waitSemaphoreCount,
                                           const VkSemaphore* pWaitSemaphores,
                                           std::chrono::nanoseconds cpuWork,
                                           std::chrono::nanoseconds gpuWork,
                                           bool recordStart = false) {
        if (recordStart) {
            SwappyVk_recordFrameStart(mQueue, mSwapchain, imageIndex);
        }
        mClock.advanceBy(cpuWork);
        mDriver.setNextGpuWork(gpuWork);

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = waitSemaphoreCount;
        presentInfo.pWaitSemaphores = pWaitSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &mSwapchain;
        presentInfo.pImageIndices = &imageIndex;
        return SwappyVk_queuePresent(mQueue, &presentInfo);
    }

    VkResult presentFrameWithSemaphores(uint32_t imageIndex,
                                        const std::vector<VkSemaphore>& waitSemaphores,
                                        std::chrono::nanoseconds cpuWork,
                                        std::chrono::nanoseconds gpuWork,
                                        bool recordStart = false) {
        return presentFrameWithRawSemaphores(imageIndex,
                                             static_cast<uint32_t>(waitSemaphores.size()),
                                             waitSemaphores.empty() ? nullptr
                                                                    : waitSemaphores.data(),
                                             cpuWork, gpuWork, recordStart);
    }

    VkResult presentFrame(uint32_t imageIndex, std::chrono::nanoseconds cpuWork,
                          std::chrono::nanoseconds gpuWork, bool recordStart = false) {
        return presentFrameWithRawSemaphores(imageIndex, 1, &mAppRenderSemaphore, cpuWork, gpuWork,
                                             recordStart);
    }

    void presentFrames(int frameCount, std::chrono::nanoseconds cpuWork,
                       std::chrono::nanoseconds gpuWork, uint32_t swapchainImages = 3,
                       bool recordStart = false) {
        for (int i = 0; i < frameCount; ++i) {
            const uint32_t imageIndex = static_cast<uint32_t>(i) % swapchainImages;
            EXPECT_EQ(presentFrame(imageIndex, cpuWork, gpuWork, recordStart), VK_SUCCESS);
        }
    }

    // Simulates a 3-image swapchain where vkAcquireNextImageKHR stalls until SurfaceFlinger
    // releases the target swapchain buffer at actualPresentTime.
    VkResult presentFrameWithAcquireBackpressure(uint32_t imageIndex,
                                                 std::chrono::nanoseconds cpuWork,
                                                 std::chrono::nanoseconds gpuWork) {
        const Clock::time_point releaseTime = mDriver.imageReleaseTime(imageIndex);
        if (releaseTime > mClock.now()) {
            mClock.advanceTo(releaseTime);
        }
        return presentFrame(imageIndex, cpuWork, gpuWork);
    }

private:
    VkPhysicalDevice mPhysicalDevice = makeVkHandle<VkPhysicalDevice>(1);
    VkDevice mDevice = makeVkHandle<VkDevice>(2);
    VkQueue mQueue = makeVkHandle<VkQueue>(3);
    VkSwapchainKHR mSwapchain = makeVkHandle<VkSwapchainKHR>(4);
    VkSemaphore mAppRenderSemaphore = makeVkHandle<VkSemaphore>(5);
    ANativeWindow mWindow{10};

    std::shared_ptr<PlatformSharedState> mState;
    SimulatedClock mClock;
    FakeVulkanDriver mDriver;
    VsyncPanelTimeline mTimeline;
    SwappyVkTestPeer::ScopedHooks mTestHooks;
};

class SwappyVkHostTest : public ::testing::Test {
protected:
    void SetUp() override {
        Settings::reset();
        SwappyVkTestPeer::clearAll();
    }

    void TearDown() override {
        SwappyVkTestPeer::clearAll();
        Settings::reset();
    }
};

TEST_F(SwappyVkHostTest, GoogleDisplayTimingSetsDesiredPresentTimeAndSwappySemaphore) {
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/true);

    harness.presentFrames(5, 4ms, 4ms);

    const auto pacedPresents = harness.driver().recordedPresents();
    ASSERT_EQ(pacedPresents.size(), 5u);
    EXPECT_TRUE(pacedPresents.back().hasDisplayTiming);
    EXPECT_GT(pacedPresents.back().desiredPresentTimeNs, 0u);
    EXPECT_NE(pacedPresents.back().waitedSemaphore, harness.appSemaphore());
}

TEST_F(SwappyVkHostTest, GoogleDisplayTimingDropsDesiredPresentTimeToZeroAboveMaxThreshold) {
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/true);

    // Cap max auto-swap duration at 30ms so stepping down to 30Hz (33.33ms) exceeds the
    // threshold and presents unpaced with desiredPresentTime == 0.
    SwappyVk_setMaxAutoSwapIntervalNS(30'000'000);
    SwappyVk_setFenceTimeoutNS(200'000'000);
    harness.presentFrames(150, 4ms, 22ms);

    EXPECT_NEAR(SwappyVk_getSwapIntervalNS(harness.swapchain()),
                static_cast<uint64_t>(k30HzPeriod.count()), 100);
    const auto unpacedPresents = harness.driver().recordedPresents();
    ASSERT_FALSE(unpacedPresents.empty());
    EXPECT_TRUE(unpacedPresents.back().hasDisplayTiming);
    EXPECT_EQ(unpacedPresents.back().desiredPresentTimeNs, 0u);
}

TEST_F(SwappyVkHostTest, PendingImageFencePresentsUnpacedWithAppSemaphore) {
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/true);
    SwappyVk_setFenceTimeoutNS(500'000'000);

    // Lower max auto-swap duration to 0ns so Swappy skips VSYNC/blocking waits immediately,
    // allowing image 0 to be re-presented while its 120ms submission fence is still unsignaled
    // and the second pool fence remains free.
    SwappyVk_setMaxAutoSwapIntervalNS(0);
    ASSERT_EQ(harness.presentFrame(0, 2ms, 120ms), VK_SUCCESS);
    ASSERT_EQ(harness.presentFrame(0, 2ms, 2ms), VK_SUCCESS);

    const auto unpacedPresents = harness.driver().recordedPresents();
    ASSERT_EQ(unpacedPresents.size(), 2u);
    EXPECT_NE(unpacedPresents[0].waitedSemaphore, harness.appSemaphore());
    // Re-presenting image 0 while its pendingFence is unsignaled bypasses Swappy's per-image
    // semaphore and forwards the app's own render-complete semaphore directly.
    EXPECT_EQ(unpacedPresents[1].waitedSemaphore, harness.appSemaphore());
}

TEST_F(SwappyVkHostTest, MultipleWaitSemaphoresPopulateAllWaitDstStageMasksInGoogleDisplayTiming) {
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/true);

    // 3 wait semaphores exercises the inline stack stage-mask buffer (<= 8).
    const std::vector<VkSemaphore> threeSemaphores = {
            makeVkHandle<VkSemaphore>(50),
            makeVkHandle<VkSemaphore>(51),
            makeVkHandle<VkSemaphore>(52),
    };
    ASSERT_EQ(harness.presentFrameWithSemaphores(0, threeSemaphores, 3ms, 3ms), VK_SUCCESS);

    // 10 wait semaphores exercises the dynamic stage-mask fallback (> 8).
    std::vector<VkSemaphore> tenSemaphores;
    for (uintptr_t i = 0; i < 10; ++i) {
        tenSemaphores.push_back(makeVkHandle<VkSemaphore>(60 + i));
    }
    ASSERT_EQ(harness.presentFrameWithSemaphores(1, tenSemaphores, 3ms, 3ms), VK_SUCCESS);

    const auto submits = harness.driver().recordedSubmits();
    ASSERT_EQ(submits.size(), 2u);
    EXPECT_EQ(submits[0].waitSemaphores, threeSemaphores);
    EXPECT_EQ(submits[0].waitDstStageMasks,
              std::vector<VkPipelineStageFlags>(3, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));
    ASSERT_EQ(submits[0].signalSemaphores.size(), 1u);

    EXPECT_EQ(submits[1].waitSemaphores, tenSemaphores);
    EXPECT_EQ(submits[1].waitDstStageMasks,
              std::vector<VkPipelineStageFlags>(10, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));
    ASSERT_EQ(submits[1].signalSemaphores.size(), 1u);

    const auto presents = harness.driver().recordedPresents();
    ASSERT_EQ(presents.size(), 2u);
    EXPECT_EQ(presents[0].waitedSemaphores,
              std::vector<VkSemaphore>{submits[0].signalSemaphores[0]});
    EXPECT_EQ(presents[1].waitedSemaphores,
              std::vector<VkSemaphore>{submits[1].signalSemaphores[0]});
}

TEST_F(SwappyVkHostTest, MultipleWaitSemaphoresPopulateAllWaitDstStageMasksInFallbackBackend) {
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/false);

    const std::vector<VkSemaphore> threeSemaphores = {
            makeVkHandle<VkSemaphore>(70),
            makeVkHandle<VkSemaphore>(71),
            makeVkHandle<VkSemaphore>(72),
    };
    std::vector<VkSemaphore> tenSemaphores;
    for (uintptr_t i = 0; i < 10; ++i) {
        tenSemaphores.push_back(makeVkHandle<VkSemaphore>(80 + i));
    }

    ASSERT_EQ(harness.presentFrameWithSemaphores(0, threeSemaphores, 3ms, 3ms), VK_SUCCESS);
    ASSERT_EQ(harness.presentFrameWithSemaphores(1, tenSemaphores, 3ms, 3ms), VK_SUCCESS);

    const auto submits = harness.driver().recordedSubmits();
    ASSERT_EQ(submits.size(), 2u);
    EXPECT_EQ(submits[0].waitSemaphores, threeSemaphores);
    EXPECT_EQ(submits[0].waitDstStageMasks,
              std::vector<VkPipelineStageFlags>(3, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));
    EXPECT_EQ(submits[1].waitSemaphores, tenSemaphores);
    EXPECT_EQ(submits[1].waitDstStageMasks,
              std::vector<VkPipelineStageFlags>(10, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));

    const auto presents = harness.driver().recordedPresents();
    ASSERT_EQ(presents.size(), 2u);
    EXPECT_FALSE(presents[0].hasDisplayTiming);
    EXPECT_FALSE(presents[1].hasDisplayTiming);
    EXPECT_EQ(presents[0].waitedSemaphores,
              std::vector<VkSemaphore>{submits[0].signalSemaphores[0]});
    EXPECT_EQ(presents[1].waitedSemaphores,
              std::vector<VkSemaphore>{submits[1].signalSemaphores[0]});
}

TEST_F(SwappyVkHostTest, PendingImageFenceForwardsAllMultipleAppWaitSemaphoresUnpaced) {
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/true);
    SwappyVk_setFenceTimeoutNS(500'000'000);
    SwappyVk_setMaxAutoSwapIntervalNS(0);

    const std::vector<VkSemaphore> firstSemaphores = {
            makeVkHandle<VkSemaphore>(90),
            makeVkHandle<VkSemaphore>(91),
            makeVkHandle<VkSemaphore>(92),
    };
    const std::vector<VkSemaphore> unpacedSemaphores = {
            makeVkHandle<VkSemaphore>(93),
            makeVkHandle<VkSemaphore>(94),
            makeVkHandle<VkSemaphore>(95),
    };

    ASSERT_EQ(harness.presentFrameWithSemaphores(0, firstSemaphores, 2ms, 120ms), VK_SUCCESS);
    ASSERT_EQ(harness.presentFrameWithSemaphores(0, unpacedSemaphores, 2ms, 2ms), VK_SUCCESS);

    const auto submits = harness.driver().recordedSubmits();
    ASSERT_EQ(submits.size(), 1u);
    EXPECT_EQ(submits[0].waitSemaphores, firstSemaphores);
    EXPECT_EQ(submits[0].waitDstStageMasks,
              std::vector<VkPipelineStageFlags>(3, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));

    const auto presents = harness.driver().recordedPresents();
    ASSERT_EQ(presents.size(), 2u);
    EXPECT_EQ(presents[0].waitedSemaphores,
              std::vector<VkSemaphore>{submits[0].signalSemaphores[0]});
    EXPECT_EQ(presents[1].waitedSemaphores, unpacedSemaphores);
}

TEST_F(SwappyVkHostTest, ZeroWaitSemaphoresSubmitsNullWaitPointersAndSignalsSwappySemaphore) {
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/true);

    const VkSemaphore unusedSemaphore = makeVkHandle<VkSemaphore>(99);
    ASSERT_EQ(harness.presentFrameWithRawSemaphores(0, 0, nullptr, 3ms, 3ms), VK_SUCCESS);
    ASSERT_EQ(harness.presentFrameWithRawSemaphores(1, 0, &unusedSemaphore, 3ms, 3ms), VK_SUCCESS);

    const auto submits = harness.driver().recordedSubmits();
    ASSERT_EQ(submits.size(), 2u);
    for (const auto& submit : submits) {
        EXPECT_TRUE(submit.waitSemaphores.empty());
        EXPECT_TRUE(submit.waitDstStageMasks.empty());
        EXPECT_TRUE(submit.hadNullWaitSemaphores);
        EXPECT_TRUE(submit.hadNullWaitDstStageMask);
        ASSERT_EQ(submit.signalSemaphores.size(), 1u);
    }

    const auto presents = harness.driver().recordedPresents();
    ASSERT_EQ(presents.size(), 2u);
    EXPECT_EQ(presents[0].waitedSemaphores,
              std::vector<VkSemaphore>{submits[0].signalSemaphores[0]});
    EXPECT_EQ(presents[1].waitedSemaphores,
              std::vector<VkSemaphore>{submits[1].signalSemaphores[0]});
}

TEST_F(SwappyVkHostTest, SwapchainAcquireBackpressureInflatesMeasuredCpuTime) {
    int64_t lastCpuTimeNs = 0;
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/true);

    SwappyTracer tracer{};
    tracer.userData = &lastCpuTimeNs;
    tracer.postWait = [](void* data, int64_t cpuTimeNs, int64_t /*gpuTimeNs*/) {
        *static_cast<int64_t*>(data) = cpuTimeNs;
    };
    ScopedVkTracer scopedTracer(&tracer);

    harness.presentFrames(4, 3ms, 3ms);

    // Model a 3-image swapchain stall where SurfaceFlinger holds image 2 an extra 25ms
    // before releasing it to vkAcquireNextImageKHR.
    harness.driver().setExtraPresentDelay(25ms);
    ASSERT_EQ(harness.presentFrameWithAcquireBackpressure(2, 3ms, 3ms), VK_SUCCESS);
    ASSERT_EQ(harness.presentFrameWithAcquireBackpressure(0, 3ms, 3ms), VK_SUCCESS);
    ASSERT_EQ(harness.presentFrameWithAcquireBackpressure(1, 3ms, 3ms), VK_SUCCESS);
    // Re-acquiring image 2 blocks on its delayed actualPresentTime before queuePresent,
    // inflating Swappy's measured CPU duration well above the 3ms recording work.
    ASSERT_EQ(harness.presentFrameWithAcquireBackpressure(2, 3ms, 3ms), VK_SUCCESS);
    EXPECT_GT(lastCpuTimeNs, std::chrono::nanoseconds(15ms).count());
}

TEST_F(SwappyVkHostTest, PastPresentationTimingFiveFrameLagPopulatesSwappyStatsHistograms) {
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/true);
    SwappyVk_enableStats(harness.swapchain(), true);

    // Present paced frames with SwappyVk_recordFrameStart; the fake loader holds back
    // the 4 newest frames (MIN_FRAME_LAG = 5) and returns completed timings for older frames.
    harness.presentFrames(6, 3ms, 3ms, /*swapchainImages=*/3, /*recordStart=*/true);
    harness.driver().setExtraPresentDelay(k60HzPeriod);
    ASSERT_EQ(harness.presentFrame(0, 3ms, 3ms, /*recordStart=*/true), VK_SUCCESS);
    harness.driver().setExtraPresentDelay(0ns);
    harness.presentFrames(6, 3ms, 3ms, /*swapchainImages=*/3, /*recordStart=*/true);

    SwappyStats stats{};
    SwappyVk_getStats(harness.swapchain(), &stats);
    EXPECT_GE(stats.totalFrames, 6u);
    EXPECT_GE(stats.lateFrames[1], 1u);
    EXPECT_GE(stats.offsetFromPreviousFrame[1], 4u);

    SwappyVk_clearStats(harness.swapchain());
    SwappyVk_getStats(harness.swapchain(), &stats);
    EXPECT_EQ(stats.totalFrames, 0u);
}

#if AGDK_TRACE != 0

using gamesdk::test::RecordedTraceCallbacks;
using gamesdk::test::ScopedMockTrace;

// Returns the recorded slice names that start with prefix, without the " n="
// field. That field holds a std::chrono::steady_clock reading, which the
// simulated clock does not control.
std::vector<std::string> recordedSlicesWithoutNowField(const std::string& prefix) {
    std::vector<std::string> slices;
    for (const std::string& event : RecordedTraceCallbacks::eventsSnapshot()) {
        if (event.rfind(prefix, 0) != 0) {
            continue;
        }
        const size_t fieldBegin = event.find(" n=");
        const size_t fieldEnd = event.find(' ', fieldBegin + 1);
        slices.push_back(event.substr(0, fieldBegin) + event.substr(fieldEnd));
    }
    return slices;
}

TEST_F(SwappyVkHostTest, GoogleDisplayTimingEmitsFrameTargetAndActualTraceEvents) {
    ScopedMockTrace mockTrace(true);
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/true);

    // The fake loader holds back the 4 newest frames (MIN_FRAME_LAG = 5), so
    // 7 frames give the past presentation timings of frames 0 and 1.
    harness.presentFrames(7, 3ms, 3ms, /*swapchainImages=*/3, /*recordStart=*/true);

    const std::vector<std::string> events = RecordedTraceCallbacks::eventsSnapshot();
    EXPECT_EQ(std::count(events.begin(), events.end(), "B:Swappy::Frame"), 7);
    EXPECT_EQ(recordedSlicesWithoutNowField("B:Swappy.Target "),
              (std::vector<std::string>{
                      "B:Swappy.Target f=0 t=1000000000 si=1 be=vk",
                      "B:Swappy.Target f=1 t=1033333334 si=1 be=vk",
                      "B:Swappy.Target f=2 t=1051000001 si=1 be=vk",
                      "B:Swappy.Target f=3 t=1073000002 si=1 be=vk",
                      "B:Swappy.Target f=4 t=1089906669 si=1 be=vk",
                      "B:Swappy.Target f=5 t=1106874136 si=1 be=vk",
                      "B:Swappy.Target f=6 t=1123887939 si=1 be=vk",
              }));
    // The lead is relative to std::chrono::steady_clock, so only the count is
    // deterministic.
    EXPECT_EQ(RecordedTraceCallbacks::counterValues("Swappy.TargetLeadUs").size(), 7u);
    EXPECT_EQ(recordedSlicesWithoutNowField("B:Swappy.Actual "),
              (std::vector<std::string>{
                      "B:Swappy.Actual f=0 t=1016666687 d=1000000000 be=vk",
                      "B:Swappy.Actual f=1 t=1033333354 d=1033333334 be=vk",
              }));
    EXPECT_EQ(RecordedTraceCallbacks::counterValues("Swappy.PresentErrorUs"),
              (std::vector<int64_t>{16666, 0}));
    EXPECT_EQ(RecordedTraceCallbacks::counterValues("Swappy.PresentMarginUs"),
              (std::vector<int64_t>{10666, 24333}));
}

#endif // AGDK_TRACE != 0

TEST_F(SwappyVkHostTest, FallbackBackendPacesWithoutDisplayTimingExtensionAndStepsDownOnGpuLoad) {
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/false);
    SwappyVk_setFenceTimeoutNS(200'000'000);

    harness.presentFrames(5, 3ms, 3ms);
    const auto initialPresents = harness.driver().recordedPresents();
    ASSERT_EQ(initialPresents.size(), 5u);
    EXPECT_FALSE(initialPresents.back().hasDisplayTiming);
    EXPECT_EQ(SwappyVk_getSwapIntervalNS(harness.swapchain()),
              static_cast<uint64_t>(k60HzPeriod.count()));

    harness.presentFrames(150, 4ms, 22ms);
    EXPECT_NEAR(SwappyVk_getSwapIntervalNS(harness.swapchain()),
                static_cast<uint64_t>(k30HzPeriod.count()), 100);
}

TEST_F(SwappyVkHostTest, UnsetPlatformFactoryFailsInitAndReportsDisabledOnHost) {
    SimulatedClock clock(Clock::time_point{1s});
    FakeVulkanDriver driver(clock);
    SwappyVkTestPeer::ScopedHooks hooks(nullptr, clock, driver.provider());

    const VkPhysicalDevice physicalDevice = makeVkHandle<VkPhysicalDevice>(11);
    const VkDevice device = makeVkHandle<VkDevice>(12);
    const VkQueue queue = makeVkHandle<VkQueue>(13);
    const VkSwapchainKHR swapchain = makeVkHandle<VkSwapchainKHR>(14);
    uint32_t requiredCount = 0;
    SwappyVk_determineDeviceExtensions(physicalDevice, 0, nullptr, &requiredCount, nullptr);
    SwappyVk_setQueueFamilyIndex(device, queue, 0);

    uint64_t refreshDurationNs = 0;
    EXPECT_FALSE(SwappyVk_initAndGetRefreshCycleDuration(nullptr, nullptr, physicalDevice, device,
                                                         swapchain, &refreshDurationNs));

    bool isEnabled = true;
    EXPECT_TRUE(SwappyVk_isEnabled(swapchain, &isEnabled));
    EXPECT_FALSE(isEnabled);

    uint32_t imageIndex = 0;
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;
    EXPECT_EQ(SwappyVk_queuePresent(queue, &presentInfo), VK_ERROR_INITIALIZATION_FAILED);
}

TEST_F(SwappyVkHostTest, ClearAllResetsCachedVulkanDispatchPointersAndPlatformFactory) {
    {
        SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/true);
        harness.presentFrames(2, 3ms, 3ms);
        EXPECT_NE(swappy::vkCreateCommandPool, nullptr);
        EXPECT_NE(swappy::vkQueueSubmit, nullptr);
    }

    EXPECT_EQ(swappy::vkCreateCommandPool, nullptr);
    EXPECT_EQ(swappy::vkQueueSubmit, nullptr);

    SimulatedClock clock(Clock::time_point{1s});
    FakeVulkanDriver driver(clock);
    SwappyVk_setFunctionProvider(driver.provider());
    const VkPhysicalDevice physicalDevice = makeVkHandle<VkPhysicalDevice>(21);
    const VkDevice device = makeVkHandle<VkDevice>(22);
    const VkQueue queue = makeVkHandle<VkQueue>(23);
    const VkSwapchainKHR swapchain = makeVkHandle<VkSwapchainKHR>(24);
    uint32_t requiredCount = 0;
    SwappyVk_determineDeviceExtensions(physicalDevice, 0, nullptr, &requiredCount, nullptr);
    SwappyVk_setQueueFamilyIndex(device, queue, 0);
    uint64_t refreshDurationNs = 0;
    EXPECT_FALSE(SwappyVk_initAndGetRefreshCycleDuration(nullptr, nullptr, physicalDevice, device,
                                                         swapchain, &refreshDurationNs));
}

TEST_F(SwappyVkHostTest, DestroySwapchainWhileFenceInFlightDrainsWorkerWithoutDeadlock) {
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/true);
    SwappyVk_setFenceTimeoutNS(500'000'000);
    SwappyVk_setMaxAutoSwapIntervalNS(0);

    ASSERT_EQ(harness.presentFrame(0, 2ms, 120ms), VK_SUCCESS);
    // Settle workers so waitForFenceThreadMain is parked inside vkWaitForFences before teardown.
    harness.clock().advanceBy(0ns);
    EXPECT_LT(harness.clock().now(), Clock::time_point{1s + 50ms});

    // Destroying the swapchain mid-test while waitForFenceThreadMain is blocked in
    // vkWaitForFences advances the simulated clock to retire the fence and join cleanly.
    SwappyVk_destroySwapchain(harness.device(), harness.swapchain());
    EXPECT_GE(harness.clock().now(), Clock::time_point{1s + 122ms});
}

TEST_F(SwappyVkHostTest, HarnessTeardownWhileFenceInFlightDrainsCleanly) {
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/true);
    SwappyVk_setFenceTimeoutNS(500'000'000);
    SwappyVk_setMaxAutoSwapIntervalNS(0);

    ASSERT_EQ(harness.presentFrame(0, 2ms, 120ms), VK_SUCCESS);
    harness.clock().advanceBy(0ns);
}

TEST_F(SwappyVkHostTest, NestedSimulatedClockPreservesOuterThreadHooksAndWorkerAccounting) {
    SimulatedSwappyVkHarness harness(/*advertiseGoogleDisplayTiming=*/true);
    harness.presentFrames(2, 3ms, 3ms);

    {
        SimulatedClock nestedClock(Clock::time_point{5s});
        nestedClock.advanceBy(10ms);
        EXPECT_EQ(nestedClock.now(), Clock::time_point{5s + 10ms});
    }

    harness.presentFrames(3, 3ms, 3ms);
    EXPECT_EQ(harness.driver().recordedPresents().size(), 5u);
}

TEST_F(SwappyVkHostTest, DestroySwapchainReleasesWindowAcrossSwapchainRecreations) {
    auto state = std::make_shared<PlatformSharedState>();
    SimulatedClock clock(Clock::time_point{1s});
    FakeVulkanDriver driver(clock);
    SwappyVkTestPeer::ScopedHooks hooks(
            [state](JNIEnv* /*env*/, jobject /*activity*/) {
                return std::make_unique<FakeSwappyPlatform>(state);
            },
            clock, driver.provider());

    const VkPhysicalDevice physicalDevice = makeVkHandle<VkPhysicalDevice>(31);
    const VkDevice device = makeVkHandle<VkDevice>(32);
    const VkQueue queue = makeVkHandle<VkQueue>(33);
    const VkSwapchainKHR swapchainA = makeVkHandle<VkSwapchainKHR>(34);
    const VkSwapchainKHR swapchainB = makeVkHandle<VkSwapchainKHR>(35);
    ANativeWindow windowA{101};
    ANativeWindow windowB{102};

    uint32_t requiredCount = 0;
    SwappyVk_determineDeviceExtensions(physicalDevice, 0, nullptr, &requiredCount, nullptr);
    SwappyVk_setQueueFamilyIndex(device, queue, 0);

    uint64_t refreshDurationNs = 0;
    ASSERT_TRUE(SwappyVk_initAndGetRefreshCycleDuration(nullptr, nullptr, physicalDevice, device,
                                                        swapchainA, &refreshDurationNs));
    SwappyVk_setWindow(device, swapchainA, &windowA);
    ASSERT_TRUE(clock.waitForWorkersSettled());
    SwappyVk_destroySwapchain(device, swapchainA);

    ASSERT_TRUE(SwappyVk_initAndGetRefreshCycleDuration(nullptr, nullptr, physicalDevice, device,
                                                        swapchainB, &refreshDurationNs));
    SwappyVk_setWindow(device, swapchainB, &windowB);
    ASSERT_TRUE(clock.waitForWorkersSettled());
    SwappyVk_destroySwapchain(device, swapchainB);

    const PlatformSnapshot snapshot = snapshotPlatformState(*state);
    ASSERT_EQ(snapshot.acquiredWindows.size(), 2u);
    EXPECT_EQ(snapshot.acquiredWindows[0], &windowA);
    EXPECT_EQ(snapshot.acquiredWindows[1], &windowB);
    ASSERT_EQ(snapshot.releasedWindows.size(), 2u);
    EXPECT_EQ(snapshot.releasedWindows[0], &windowA);
    EXPECT_EQ(snapshot.releasedWindows[1], &windowB);

    SwappyVk_destroyDevice(device);
}

} // namespace

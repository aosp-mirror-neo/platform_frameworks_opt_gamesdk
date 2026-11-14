/*
 * Copyright 2019 The Android Open Source Project
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

/***************************************************************************************************
 *
 * Per-Device abstract base class.
 *
 ***************************************************************************************************/

/**
 * Abstract base class that calls the Vulkan API.
 *
 * It is expected that one concrete class will be instantiated per VkDevice, and
 * that all VkSwapchainKHR's for a given VkDevice will share the same instance.
 *
 * Base class members are used by the derived classes to unify the behavior
 * across implementations:
 *  @mThread - Thread used for getting Choreographer events.
 *  @mTreadRunning - Used to signal the tread to exit
 *  @mNextPresentID - unique ID for frame presentation.
 *  @mNextDesiredPresentTime - Holds the time in nanoseconds for the next frame
 * to be presented.
 *  @mNextPresentIDToCheck - Used to determine whether presentation time needs
 * to be adjusted.
 *  @mFrameID - Keeps track of how many Choreographer callbacks received.
 *  @mLastframeTimeNanos - Holds the last frame time reported by Choreographer.
 *  @mSumRefreshTime - Used together with @mSamples to calculate refresh rate
 * based on Choreographer.
 */

#pragma once

#include <dlfcn.h>
#include <inttypes.h>
#include <pthread.h>
#include <swappy/swappyVk.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>

#include "ChoreographerShim.h"
#include "Settings.h"
#include "SwappyCommon.h"
#include "Trace.h"

namespace swappy {

constexpr uint32_t kThousand = 1000;
constexpr uint32_t kMillion = 1000000;
constexpr uint32_t kBillion = 1000000000;
constexpr uint32_t k16_6msec = 16666666;

constexpr uint32_t kTooCloseToVsyncBoundary = 3000000;
constexpr uint32_t kTooFarAwayFromVsyncBoundary = 7000000;
constexpr uint32_t kNudgeWithinVsyncBoundaries = 2000000;

// AChoreographer is supported from API 24. To allow compilation for minSDK < 24
// and still use AChoreographer for SDK >= 24 we need runtime support to call
// AChoreographer APIs.

using PFN_AChoreographer_getInstance = AChoreographer* (*)();

using PFN_AChoreographer_postFrameCallback = void (*)(AChoreographer* choreographer,
                                                      AChoreographer_frameCallback callback,
                                                      void* data);

using PFN_AChoreographer_postFrameCallbackDelayed = void (*)(AChoreographer* choreographer,
                                                             AChoreographer_frameCallback callback,
                                                             void* data, long delayMillis);

extern PFN_vkCreateCommandPool vkCreateCommandPool;
extern PFN_vkDestroyCommandPool vkDestroyCommandPool;
extern PFN_vkCreateFence vkCreateFence;
extern PFN_vkDestroyFence vkDestroyFence;
extern PFN_vkWaitForFences vkWaitForFences;
extern PFN_vkGetFenceStatus vkGetFenceStatus;
extern PFN_vkResetFences vkResetFences;
extern PFN_vkCreateSemaphore vkCreateSemaphore;
extern PFN_vkDestroySemaphore vkDestroySemaphore;
extern PFN_vkCreateEvent vkCreateEvent;
extern PFN_vkDestroyEvent vkDestroyEvent;
extern PFN_vkCmdSetEvent vkCmdSetEvent;
extern PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
extern PFN_vkFreeCommandBuffers vkFreeCommandBuffers;
extern PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
extern PFN_vkEndCommandBuffer vkEndCommandBuffer;
extern PFN_vkQueueSubmit vkQueueSubmit;

void LoadVulkanFunctions(const SwappyVkFunctionProvider* pFunctionProvider);
bool AreVulkanFunctionsLoaded();
void ResetVulkanFunctions();

class SwappyVkBase {
public:
    SwappyVkBase(std::unique_ptr<SwappyPlatform> platform, VkPhysicalDevice physicalDevice,
                 VkDevice device, PFN_vkQueuePresentKHR pfnQueuePresentKHR);

    virtual ~SwappyVkBase();

    virtual bool doGetRefreshCycleDuration(VkSwapchainKHR swapchain,
                                           uint64_t* pRefreshDuration) = 0;
    uint64_t reportRefreshCycleDuration() const;

    virtual VkResult doQueuePresent(VkQueue queue, uint32_t queueFamilyIndex,
                                    const VkPresentInfoKHR* pPresentInfo) = 0;

    void doSetWindow(ANativeWindow* window);
    void doSetSwapInterval(VkSwapchainKHR swapchain, uint64_t swapNs);

    VkResult injectFence(VkQueue queue, const VkPresentInfoKHR* pPresentInfo,
                         VkSemaphore* pSemaphore);

    [[nodiscard]] bool isValid() {
        return mCommonBase.isValid() && mpfnQueuePresentKHR != nullptr;
    }

    void setAutoSwapInterval(bool enabled);
    void setAutoPipelineMode(bool enabled);

    void setMaxAutoSwapDuration(std::chrono::nanoseconds swapMaxNS);

    void setFenceTimeout(std::chrono::nanoseconds duration);
    std::chrono::nanoseconds getFenceTimeout() const;
    std::chrono::nanoseconds getSwapInterval();

    void addTracer(const SwappyTracer* tracer);
    void removeTracer(const SwappyTracer* tracer);

    VkDevice getDevice() const {
        return mDevice;
    }

    int getSupportedRefreshPeriodsNS(uint64_t* out_refreshrates, int allocated_entries);

    virtual void enableStats(bool enabled) = 0;
    virtual void getStats(SwappyStats* swappyStats) = 0;
    virtual void recordFrameStart(VkQueue queue, uint32_t image) = 0;
    virtual void clearStats() = 0;

    void resetFramePacing();
    void enableFramePacing(bool enable);
    void enableBlockingWait(bool enable);

protected:
    struct VkSync {
        VkFence fence;
        VkCommandBuffer command;
        VkEvent event;
    };

    struct SwapchainImageSemaphore {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        // Fence of the last Swappy submission that signaled |semaphore|. A re-acquired image can
        // come back before that submission retires (e.g. Android returns a dropped MAILBOX buffer
        // with its pending acquire fence), so |semaphore| is not signaled again, and the frame is
        // presented unpaced, until this fence signals. Cleared once the fence is seen signaled.
        VkFence pendingFence = VK_NULL_HANDLE;
    };

    struct ThreadContext {
        ThreadContext(VkQueue queue) : queue(queue) {}

        Thread thread;
        bool running GUARDED_BY(lock) = true;
        bool hasPendingWork GUARDED_BY(lock);
        std::mutex lock;
        std::condition_variable_any condition;
        VkQueue queue GUARDED_BY(lock);
    };

    SwappyCommon mCommonBase;
    VkPhysicalDevice mPhysicalDevice;
    VkDevice mDevice;

    uint32_t mNextPresentID = 0;
    uint32_t mNextPresentIDToCheck = 2;

    PFN_vkQueuePresentKHR mpfnQueuePresentKHR = nullptr;
    // Holds VKSync objects ready to be used
    std::map<VkQueue, std::list<VkSync>> mFreeSyncPool;

    // Holds VKSync objects queued and but signaled yet
    std::map<VkQueue, std::list<VkSync>> mWaitingSyncs;

    // Holds VKSync objects that were signaled
    std::map<VkQueue, std::list<VkSync>> mSignaledSyncs;

    // Holds present wait semaphores indexed by swapchain image index so a semaphore
    // is only reused after the same swapchain image is re-acquired.
    std::map<uint32_t, SwapchainImageSemaphore> mSwapchainImageSemaphores;

    std::map<VkQueue, VkCommandPool> mCommandPool;
    std::map<VkQueue, std::unique_ptr<ThreadContext>> mThreads;

    static constexpr int MAX_PENDING_FENCES = 2;

    std::atomic<std::chrono::nanoseconds> mLastFenceTime = {};

    VkResult initializeVkSyncObjects(VkQueue queue, uint32_t queueFamilyIndex);
    void destroyVkSyncObjects();
    void reclaimSignaledFences(VkQueue queue);
    bool lastFrameIsCompleted(VkQueue queue);
    std::chrono::nanoseconds getLastFenceTime(VkQueue queue);
    void waitForFenceThreadMain(ThreadContext& thread);
};

} // namespace swappy

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

#pragma once

#include <functional>
#include <memory>

#include "SwappyVkBase.h"
#include "SwappyVkFallback.h"
#include "SwappyVkGoogleDisplayTiming.h"

namespace swappy {

// The API functions call methods of the singleton SwappyVk class.
// Those methods call virtual methods of the abstract SwappyVkBase class,
// which is actually implemented by one of the derived/concrete classes:
//
// - SwappyVkGoogleDisplayTiming
// - SwappyVkFallback

/***************************************************************************************************
 *
 * Singleton class that provides the high-level implementation of the Swappy
 *entrypoints.
 *
 ***************************************************************************************************/
/**
 * Singleton class that provides the high-level implementation of the Swappy
 * entrypoints.
 *
 * This class determines which low-level implementation to use for each physical
 * device, and then calls that class's do-method for the entrypoint.
 */
class SwappyVk {
public:
    static SwappyVk& getInstance() {
        static SwappyVk instance;
        return instance;
    }

    ~SwappyVk() {
        perSwapchainImplementation.clear();
        if (pFunctionProvider != nullptr && pFunctionProvider->close != nullptr) {
            pFunctionProvider->close();
        }
    }

    void swappyVkDetermineDeviceExtensions(VkPhysicalDevice physicalDevice,
                                           uint32_t availableExtensionCount,
                                           VkExtensionProperties* pAvailableExtensions,
                                           uint32_t* pRequiredExtensionCount,
                                           char** pRequiredExtensions);
    void SetQueueFamilyIndex(VkDevice device, VkQueue queue, uint32_t queueFamilyIndex);
    bool GetRefreshCycleDuration(JNIEnv* env, jobject jactivity, VkPhysicalDevice physicalDevice,
                                 VkDevice device, VkSwapchainKHR swapchain,
                                 uint64_t* pRefreshDuration);
    void SetWindow(VkDevice device, VkSwapchainKHR swapchain, ANativeWindow* window);
    void SetSwapDuration(VkDevice device, VkSwapchainKHR swapchain, uint64_t swapNs);
    VkResult QueuePresent(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
    void DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain);
    void DestroyDevice(VkDevice device);

    void SetAutoSwapInterval(bool enabled);
    void SetAutoPipelineMode(bool enabled);
    void SetMaxAutoSwapDuration(std::chrono::nanoseconds maxDuration);
    void SetFenceTimeout(std::chrono::nanoseconds duration);
    std::chrono::nanoseconds GetFenceTimeout() const;
    std::chrono::nanoseconds GetSwapInterval(VkSwapchainKHR swapchain);
    int GetSupportedRefreshPeriodsNS(uint64_t* out_refreshrates, int allocated_entries,
                                     VkSwapchainKHR swapchain);

    void addTracer(const SwappyTracer* t);
    void removeTracer(const SwappyTracer* t);

    void SetFunctionProvider(const SwappyVkFunctionProvider* pFunctionProvider);
    bool InitFunctions();

    bool IsEnabled(VkSwapchainKHR swapchain, bool* isEnabled);

    void clearState();

    // Frame statistics.
    void enableStats(VkSwapchainKHR swapchain, bool enabled);
    void getStats(VkSwapchainKHR swapchain, SwappyStats* swappyStats);
    void recordFrameStart(VkQueue queue, VkSwapchainKHR swapchain, uint32_t image);
    void clearStats(VkSwapchainKHR swapchain);

    void resetFramePacing(VkSwapchainKHR swapchain);
    void enableFramePacing(VkSwapchainKHR swapchain, bool enable);
    void enableBlockingWait(VkSwapchainKHR swapchain, bool enable);

private:
    struct SwapchainEntry {
        VkDevice device = VK_NULL_HANDLE;
        // Non-null if and only if Swappy is enabled and swapchain initialization succeeded.
        std::unique_ptr<SwappyVkBase> impl;
    };

    // Unlike perSwapchainImplementation[swapchain], these helpers do not insert an
    // empty entry for a swapchain that was never initialized.
    const SwapchainEntry* findSwapchainEntry(VkSwapchainKHR swapchain) const;
    SwappyVkBase* findImplementation(VkSwapchainKHR swapchain) const;
    std::unique_ptr<SwappyVkBase> createImplementation(JNIEnv* env, jobject jactivity,
                                                       VkPhysicalDevice physicalDevice,
                                                       VkDevice device, VkSwapchainKHR swapchain,
                                                       uint64_t* pRefreshDuration);

    std::map<VkPhysicalDevice, bool> doesPhysicalDeviceHaveGoogleDisplayTiming;
    std::map<VkSwapchainKHR, SwapchainEntry> perSwapchainImplementation;

    struct QueueFamilyIndex {
        VkDevice device;
        uint32_t queueFamilyIndex;
    };
    std::map<VkQueue, QueueFamilyIndex> perQueueFamilyIndex;

    const SwappyVkFunctionProvider* pFunctionProvider = nullptr;

private:
    friend class SwappyVkTestPeer;
    using PlatformFactory = std::function<std::unique_ptr<SwappyPlatform>(JNIEnv*, jobject)>;

    PlatformFactory mPlatformFactory;
    const Clock* mClock = &SteadyClock::instance();

    SwappyVk() {} // Need to implement this constructor

    // Forbid copies.
    SwappyVk(SwappyVk const&) = delete;
    void operator=(SwappyVk const&) = delete;

    std::mutex tracer_list_lock;
    std::list<SwappyTracer> tracer_list GUARDED_BY(tracer_list_lock);
};

} // namespace swappy

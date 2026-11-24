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

#include "SwappyVk.h"

#include <utility>

#ifdef __ANDROID__
#include "AndroidSwappyPlatform.h"
#include "system_utils.h"
#endif

#define LOG_TAG "SwappyVk"
#include "SwappyLog.h"
#include "Trace.h"

/* For tracking tracers internally within vulkan instance, we cannot store the
 * pointers as there is no requirement for the life of the object once the
 * SwappyVk_injectTracer(*t) call is returned. So we store the struct objects by
 * value. In turn, this implies that we have to compare the whole struct for
 * managing it. So we define a local "==" operator to this file in order to
 * handle the C struct API in the std::list.
 *
 * In addition, there is a risk that updates to the SwappyTracer struct could go
 * unnoticed silently. So we have a copy here and we check at compile time that
 * the sizes are the same, it is a weak check, but it is better than nothing.
 */

typedef struct SwappyTracerLocalStruct {
    SwappyPreWaitCallback preWait;
    SwappyPostWaitCallback postWait;
    SwappyPreSwapBuffersCallback preSwapBuffers;
    SwappyPostSwapBuffersCallback postSwapBuffers;
    SwappyStartFrameCallback startFrame;
    void* userData;
    SwappySwapIntervalChangedCallback swapIntervalChanged;
} SwappyTracerLocalStruct;

static bool operator==(const SwappyTracer& t1, const SwappyTracer& t2) {
    static_assert(sizeof(SwappyTracer) == sizeof(SwappyTracerLocalStruct),
                  "SwappyTracer struct appears to have changed, please "
                  "consider updating locally.");
    return (t1.preWait == t2.preWait) && (t1.postWait == t2.postWait) &&
            (t1.preSwapBuffers == t2.preSwapBuffers) &&
            (t1.postSwapBuffers == t2.postSwapBuffers) && (t1.startFrame == t2.startFrame) &&
            (t1.userData == t2.userData) && (t1.swapIntervalChanged == t2.swapIntervalChanged);
}

namespace swappy {

class DefaultSwappyVkFunctionProvider {
public:
    static bool Init() {
        if (!mLibVulkan) {
            // This is the first time we've been called
            mLibVulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
            if (!mLibVulkan) {
                // If Vulkan doesn't exist, bail-out early:
                return false;
            }
        }
        return true;
    }
    static void* GetProcAddr(const char* name) {
        if (!mLibVulkan && !Init()) return nullptr;
        return dlsym(mLibVulkan, name);
    }
    static void Close() {
        if (mLibVulkan) {
            dlclose(mLibVulkan);
            mLibVulkan = nullptr;
        }
    }

private:
    static void* mLibVulkan;
};

void* DefaultSwappyVkFunctionProvider::mLibVulkan = nullptr;

bool SwappyVk::InitFunctions() {
    if (pFunctionProvider == nullptr) {
        static SwappyVkFunctionProvider c_provider;
        c_provider.init = &DefaultSwappyVkFunctionProvider::Init;
        c_provider.getProcAddr = &DefaultSwappyVkFunctionProvider::GetProcAddr;
        c_provider.close = &DefaultSwappyVkFunctionProvider::Close;
        pFunctionProvider = &c_provider;
    }
    if (pFunctionProvider->init != nullptr && pFunctionProvider->getProcAddr != nullptr &&
        pFunctionProvider->init()) {
        LoadVulkanFunctions(pFunctionProvider);
        return AreVulkanFunctionsLoaded();
    }
    return false;
}
void SwappyVk::SetFunctionProvider(const SwappyVkFunctionProvider* functionProvider) {
    bool currently_default = (pFunctionProvider == nullptr) ||
            (pFunctionProvider->getProcAddr == &DefaultSwappyVkFunctionProvider::GetProcAddr);
    bool new_default = (functionProvider == nullptr);

    if (currently_default && new_default) {
        return;
    }

    if (pFunctionProvider != nullptr && pFunctionProvider->close != nullptr) {
        pFunctionProvider->close();
    }
    pFunctionProvider = functionProvider;
    ResetVulkanFunctions();
}

/**
 * Generic/Singleton implementation of swappyVkDetermineDeviceExtensions.
 */
void SwappyVk::swappyVkDetermineDeviceExtensions(VkPhysicalDevice physicalDevice,
                                                 uint32_t availableExtensionCount,
                                                 VkExtensionProperties* pAvailableExtensions,
                                                 uint32_t* pRequiredExtensionCount,
                                                 char** pRequiredExtensions) {
#if (not defined ANDROID_NDK_VERSION) || ANDROID_NDK_VERSION >= 15
    // TODO: Refactor this to be more concise:
    if (!pRequiredExtensions) {
        for (uint32_t i = 0; i < availableExtensionCount; i++) {
            if (!strcmp(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
                        pAvailableExtensions[i].extensionName)) {
                (*pRequiredExtensionCount)++;
            }
        }
    } else {
        doesPhysicalDeviceHaveGoogleDisplayTiming[physicalDevice] = false;
        for (uint32_t i = 0, j = 0; i < availableExtensionCount; i++) {
            if (!strcmp(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
                        pAvailableExtensions[i].extensionName)) {
                if (j < *pRequiredExtensionCount) {
                    strcpy(pRequiredExtensions[j++], VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
                    doesPhysicalDeviceHaveGoogleDisplayTiming[physicalDevice] = true;
                }
            }
        }
    }
#else
    doesPhysicalDeviceHaveGoogleDisplayTiming[physicalDevice] = false;
#endif
}

const SwappyVk::SwapchainEntry* SwappyVk::findSwapchainEntry(VkSwapchainKHR swapchain) const {
    auto it = perSwapchainImplementation.find(swapchain);
    return it != perSwapchainImplementation.end() ? &it->second : nullptr;
}

SwappyVkBase* SwappyVk::findImplementation(VkSwapchainKHR swapchain) const {
    const SwapchainEntry* entry = findSwapchainEntry(swapchain);
    return entry != nullptr ? entry->impl.get() : nullptr;
}

std::unique_ptr<SwappyVkBase> SwappyVk::createImplementation(JNIEnv* env, jobject jactivity,
                                                             VkPhysicalDevice physicalDevice,
                                                             VkDevice device,
                                                             VkSwapchainKHR swapchain,
                                                             uint64_t* pRefreshDuration) {
    if (!InitFunctions()) {
        // If Vulkan doesn't exist, bail-out early
        return nullptr;
    }

#ifdef __ANDROID__
    if (gamesdk::GetSystemPropAsBool(SWAPPY_SYSTEM_PROP_KEY_DISABLE, false)) {
        SWAPPY_LOGI("Swappy is disabled");
        return nullptr;
    }
#endif

    auto pfnGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            pFunctionProvider->getProcAddr("vkGetDeviceProcAddr"));
    if (pfnGetDeviceProcAddr == nullptr) {
        SWAPPY_LOGE("vkGetDeviceProcAddr is not available.");
        return nullptr;
    }

    auto pfnQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(
            pfnGetDeviceProcAddr(device, "vkQueuePresentKHR"));
    if (pfnQueuePresentKHR == nullptr) {
        SWAPPY_LOGE("vkQueuePresentKHR is not available.");
        return nullptr;
    }

#if (not defined ANDROID_NDK_VERSION) || ANDROID_NDK_VERSION >= 15
    // Based on whether VK_GOOGLE_display_timing is available
    // (determined and cached by swappyVkDetermineDeviceExtensions),
    // determine which derived class to use to implement the rest of the API.
    const bool useGoogleDisplayTiming = doesPhysicalDeviceHaveGoogleDisplayTiming[physicalDevice];
    PFN_vkGetRefreshCycleDurationGOOGLE pfnGetRefreshCycleDurationGOOGLE = nullptr;
    PFN_vkGetPastPresentationTimingGOOGLE pfnGetPastPresentationTimingGOOGLE = nullptr;
    if (useGoogleDisplayTiming) {
        pfnGetRefreshCycleDurationGOOGLE = reinterpret_cast<PFN_vkGetRefreshCycleDurationGOOGLE>(
                pfnGetDeviceProcAddr(device, "vkGetRefreshCycleDurationGOOGLE"));
        pfnGetPastPresentationTimingGOOGLE =
                reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
                        pfnGetDeviceProcAddr(device, "vkGetPastPresentationTimingGOOGLE"));
        if (pfnGetRefreshCycleDurationGOOGLE == nullptr ||
            pfnGetPastPresentationTimingGOOGLE == nullptr) {
            SWAPPY_LOGE("VK_GOOGLE_display_timing functions are not available.");
            return nullptr;
        }
        VkRefreshCycleDurationGOOGLE refreshCycleDuration{};
        VkResult res = pfnGetRefreshCycleDurationGOOGLE(device, swapchain, &refreshCycleDuration);
        if (res != VK_SUCCESS) {
            SWAPPY_LOGE("mpfnGetRefreshCycleDurationGOOGLE failed %d", res);
            return nullptr;
        }
    }
#endif

    std::unique_ptr<SwappyPlatform> platform;
    if (mPlatformFactory) {
        platform = mPlatformFactory(env, jactivity);
    } else {
#ifdef __ANDROID__
        platform = createAndroidSwappyPlatform(env, jactivity);
#endif
    }
    if (!platform || !platform->isValid()) {
        SWAPPY_LOGE("SwappyCommon could not initialize correctly.");
        return nullptr;
    }

    std::unique_ptr<SwappyVkBase> impl;
#if (not defined ANDROID_NDK_VERSION) || ANDROID_NDK_VERSION >= 15
    if (useGoogleDisplayTiming) {
        impl = std::make_unique<SwappyVkGoogleDisplayTiming>(std::move(platform), physicalDevice,
                                                             device, swapchain, pfnQueuePresentKHR,
                                                             pfnGetRefreshCycleDurationGOOGLE,
                                                             pfnGetPastPresentationTimingGOOGLE,
                                                             *mClock);
        SWAPPY_LOGV("SwappyVk initialized for VkDevice %p using "
                    "VK_GOOGLE_display_timing on Android",
                    device);
    } else
#endif
    {
        impl = std::make_unique<SwappyVkFallback>(std::move(platform), physicalDevice, device,
                                                  pfnQueuePresentKHR, *mClock);
        SWAPPY_LOGV("SwappyVk initialized for VkDevice %p using Android fallback", device);
    }

    if (!impl->isValid()) {
        SWAPPY_LOGE("SwappyCommon could not initialize correctly.");
        return nullptr;
    }

    *pRefreshDuration = impl->reportRefreshCycleDuration();
    return impl;
}

void SwappyVk::SetQueueFamilyIndex(VkDevice device, VkQueue queue, uint32_t queueFamilyIndex) {
    perQueueFamilyIndex[queue] = {device, queueFamilyIndex};
}

/**
 * Generic/Singleton implementation of swappyVkGetRefreshCycleDuration.
 */
bool SwappyVk::GetRefreshCycleDuration(JNIEnv* env, jobject jactivity,
                                       VkPhysicalDevice physicalDevice, VkDevice device,
                                       VkSwapchainKHR swapchain, uint64_t* pRefreshDuration) {
    if (pRefreshDuration == nullptr || device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE) {
        return false;
    }

    auto existing = perSwapchainImplementation.find(swapchain);
    if (existing != perSwapchainImplementation.end()) {
        return existing->second.impl != nullptr &&
                existing->second.impl->doGetRefreshCycleDuration(swapchain, pRefreshDuration);
    }

    std::unique_ptr<SwappyVkBase> impl = createImplementation(env, jactivity, physicalDevice,
                                                              device, swapchain, pRefreshDuration);
    if (impl != nullptr) {
        std::lock_guard<std::mutex> lock(tracer_list_lock);
        for (const auto& tracer : tracer_list) {
            impl->addTracer(&tracer);
        }
    }

    const bool enabled = (impl != nullptr);
    perSwapchainImplementation.emplace(swapchain, SwapchainEntry{device, std::move(impl)});
    return enabled;
}

/**
 * Generic/Singleton implementation of swappyVkSetWindow.
 */
void SwappyVk::SetWindow(VkDevice device, VkSwapchainKHR swapchain, ANativeWindow* window) {
    SwappyVkBase* pImplementation = findImplementation(swapchain);
    if (!pImplementation) {
        return;
    }
    pImplementation->doSetWindow(window);
}

/**
 * Generic/Singleton implementation of swappyVkSetSwapInterval.
 */
void SwappyVk::SetSwapDuration(VkDevice device, VkSwapchainKHR swapchain, uint64_t swapNs) {
    SwappyVkBase* pImplementation = findImplementation(swapchain);
    if (!pImplementation) {
        return;
    }
    pImplementation->doSetSwapInterval(swapchain, swapNs);
}

/**
 * Generic/Singleton implementation of swappyVkQueuePresent.
 */
VkResult SwappyVk::QueuePresent(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    TRACE_SCOPE("Swappy::Frame");
    auto queueIt = perQueueFamilyIndex.find(queue);
    if (queueIt == perQueueFamilyIndex.end()) {
        SWAPPY_LOGE("Unknown queue %p. Did you call SwappyVkSetQueueFamilyIndex ?", queue);
        return VK_INCOMPLETE;
    }

    // This command doesn't have a VkDevice.  It should have at least one
    // VkSwapchainKHR's.  For this command, all VkSwapchainKHR's will have the
    // same VkDevice and VkQueue.
    if (!pPresentInfo || (pPresentInfo->swapchainCount == 0) || (!pPresentInfo->pSwapchains)) {
        // This shouldn't happen, but if it does, something is really wrong.
        return VK_ERROR_DEVICE_LOST;
    }
    const SwapchainEntry* entry = findSwapchainEntry(*pPresentInfo->pSwapchains);
    if (entry == nullptr) {
        // This should only happen if the API was used wrong (e.g. they never
        // called swappyVkGetRefreshCycleDuration).
        // NOTE: Technically, a Vulkan library shouldn't protect a user from
        // themselves, but we'll be friendlier
        return VK_ERROR_DEVICE_LOST;
    }
    if (entry->impl == nullptr) {
        SWAPPY_LOGE("Swappy is disabled.");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return entry->impl->doQueuePresent(queue, queueIt->second.queueFamilyIndex, pPresentInfo);
}

void SwappyVk::DestroySwapchain(VkDevice /*device*/, VkSwapchainKHR swapchain) {
    auto swapchain_it = perSwapchainImplementation.find(swapchain);
    if (swapchain_it == perSwapchainImplementation.end()) return;
    perSwapchainImplementation.erase(swapchain_it);
}

void SwappyVk::DestroyDevice(VkDevice device) {
    {
        // Erase swapchains
        auto it = perSwapchainImplementation.begin();
        while (it != perSwapchainImplementation.end()) {
            if (it->second.device == device) {
                it = perSwapchainImplementation.erase(it);
            } else {
                ++it;
            }
        }
    }
    {
        // Erase the device
        auto it = perQueueFamilyIndex.begin();
        while (it != perQueueFamilyIndex.end()) {
            if (it->second.device == device) {
                it = perQueueFamilyIndex.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void SwappyVk::SetAutoSwapInterval(bool enabled) {
    for (const auto& [swapchain, entry] : perSwapchainImplementation) {
        if (entry.impl != nullptr) {
            entry.impl->setAutoSwapInterval(enabled);
        }
    }
}

void SwappyVk::SetAutoPipelineMode(bool enabled) {
    for (const auto& [swapchain, entry] : perSwapchainImplementation) {
        if (entry.impl != nullptr) {
            entry.impl->setAutoPipelineMode(enabled);
        }
    }
}

void SwappyVk::SetMaxAutoSwapDuration(std::chrono::nanoseconds maxDuration) {
    for (const auto& [swapchain, entry] : perSwapchainImplementation) {
        if (entry.impl != nullptr) {
            entry.impl->setMaxAutoSwapDuration(maxDuration);
        }
    }
}

void SwappyVk::SetFenceTimeout(std::chrono::nanoseconds t) {
    for (const auto& [swapchain, entry] : perSwapchainImplementation) {
        if (entry.impl != nullptr) {
            entry.impl->setFenceTimeout(t);
        }
    }
}

std::chrono::nanoseconds SwappyVk::GetFenceTimeout() const {
    for (const auto& [swapchain, entry] : perSwapchainImplementation) {
        if (entry.impl != nullptr) {
            return entry.impl->getFenceTimeout();
        }
    }
    return std::chrono::nanoseconds(0);
}

std::chrono::nanoseconds SwappyVk::GetSwapInterval(VkSwapchainKHR swapchain) {
    SwappyVkBase* pImplementation = findImplementation(swapchain);
    if (pImplementation != nullptr) {
        return pImplementation->getSwapInterval();
    }
    return std::chrono::nanoseconds(0);
}

void SwappyVk::addTracer(const SwappyTracer* t) {
    if (t != nullptr) {
        std::lock_guard<std::mutex> lock(tracer_list_lock);
        tracer_list.push_back(*t);

        for (const auto& [swapchain, entry] : perSwapchainImplementation) {
            if (entry.impl != nullptr) {
                entry.impl->addTracer(t);
            }
        }
    }
}

void SwappyVk::removeTracer(const SwappyTracer* t) {
    if (t != nullptr) {
        std::lock_guard<std::mutex> lock(tracer_list_lock);
        tracer_list.remove(*t);

        for (const auto& [swapchain, entry] : perSwapchainImplementation) {
            if (entry.impl != nullptr) {
                entry.impl->removeTracer(t);
            }
        }
    }
}

int SwappyVk::GetSupportedRefreshPeriodsNS(uint64_t* out_refreshrates, int allocated_entries,
                                           VkSwapchainKHR swapchain) {
    SwappyVkBase* pImplementation = findImplementation(swapchain);
    if (!pImplementation) {
        return 0;
    }
    return pImplementation->getSupportedRefreshPeriodsNS(out_refreshrates, allocated_entries);
}

bool SwappyVk::IsEnabled(VkSwapchainKHR swapchain, bool* isEnabled) {
    const SwapchainEntry* entry = findSwapchainEntry(swapchain);
    if (!entry || !isEnabled) {
        return false;
    }
    *isEnabled = (entry->impl != nullptr);
    return true;
}

void SwappyVk::enableStats(VkSwapchainKHR swapchain, bool enabled) {
    SwappyVkBase* pImplementation = findImplementation(swapchain);
    if (pImplementation != nullptr) {
        pImplementation->enableStats(enabled);
    }
}

void SwappyVk::getStats(VkSwapchainKHR swapchain, SwappyStats* swappyStats) {
    if (swappyStats == nullptr) {
        return;
    }
    SwappyVkBase* pImplementation = findImplementation(swapchain);
    if (pImplementation != nullptr) {
        pImplementation->getStats(swappyStats);
    }
}

void SwappyVk::recordFrameStart(VkQueue queue, VkSwapchainKHR swapchain, uint32_t image) {
    SwappyVkBase* pImplementation = findImplementation(swapchain);
    if (pImplementation != nullptr) {
        pImplementation->recordFrameStart(queue, image);
    }
}

void SwappyVk::clearStats(VkSwapchainKHR swapchain) {
    SwappyVkBase* pImplementation = findImplementation(swapchain);
    if (pImplementation != nullptr) {
        pImplementation->clearStats();
    }
}

void SwappyVk::resetFramePacing(VkSwapchainKHR swapchain) {
    SwappyVkBase* pImplementation = findImplementation(swapchain);
    if (pImplementation != nullptr) {
        pImplementation->resetFramePacing();
    }
}

void SwappyVk::enableFramePacing(VkSwapchainKHR swapchain, bool enable) {
    SwappyVkBase* pImplementation = findImplementation(swapchain);
    if (pImplementation != nullptr) {
        pImplementation->enableFramePacing(enable);
    }
}

void SwappyVk::enableBlockingWait(VkSwapchainKHR swapchain, bool enable) {
    SwappyVkBase* pImplementation = findImplementation(swapchain);
    if (pImplementation != nullptr) {
        pImplementation->enableBlockingWait(enable);
    }
}

void SwappyVk::clearState() {
    doesPhysicalDeviceHaveGoogleDisplayTiming.clear();
    perSwapchainImplementation.clear();
    perQueueFamilyIndex.clear();
    if (pFunctionProvider != nullptr && pFunctionProvider->close != nullptr) {
        pFunctionProvider->close();
    }
    pFunctionProvider = nullptr;
    ResetVulkanFunctions();
    mPlatformFactory = nullptr;
    mClock = &SteadyClock::instance();
    std::lock_guard<std::mutex> lock(tracer_list_lock);
    tracer_list.clear();
}

} // namespace swappy

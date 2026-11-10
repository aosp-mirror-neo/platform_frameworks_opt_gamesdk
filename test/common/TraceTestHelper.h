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

#if AGDK_TRACE != 0

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace gamesdk {

struct TraceTestPeer {
    static std::unique_ptr<Trace> swapInstance(std::unique_ptr<Trace> replacement) {
        std::swap(Trace::instanceStorage(), replacement);
        return replacement;
    }
};

namespace test {

struct RecordedTraceCallbacks {
    inline static std::mutex sMutex;
    inline static std::atomic<bool> sEnabled{false};
    inline static std::vector<std::string> sEvents;
    inline static std::vector<std::pair<std::string, int64_t>> sCounters;

    static void reset(bool enabled) {
        std::lock_guard<std::mutex> lock(sMutex);
        sEnabled.store(enabled, std::memory_order_release);
        sEvents.clear();
        sCounters.clear();
    }

    static void beginSection(const char* name) {
        std::lock_guard<std::mutex> lock(sMutex);
        sEvents.push_back(std::string("B:") + name);
    }

    static void endSection() {
        std::lock_guard<std::mutex> lock(sMutex);
        sEvents.emplace_back("E");
    }

    static bool isEnabled() {
        return sEnabled.load(std::memory_order_acquire);
    }

    static void beginAsyncSection(const char* name, int32_t cookie) {
        std::lock_guard<std::mutex> lock(sMutex);
        sEvents.push_back(std::string("S:") + name + ":" + std::to_string(cookie));
    }

    static void endAsyncSection(const char* name, int32_t cookie) {
        std::lock_guard<std::mutex> lock(sMutex);
        sEvents.push_back(std::string("F:") + name + ":" + std::to_string(cookie));
    }

    static void setCounter(const char* name, int64_t value) {
        std::lock_guard<std::mutex> lock(sMutex);
        sCounters.emplace_back(name, value);
    }

    static std::vector<std::string> eventsSnapshot() {
        std::lock_guard<std::mutex> lock(sMutex);
        return sEvents;
    }

    static std::vector<std::pair<std::string, int64_t>> countersSnapshot() {
        std::lock_guard<std::mutex> lock(sMutex);
        return sCounters;
    }

    static bool hasEvent(const std::string& expectedEvent) {
        std::lock_guard<std::mutex> lock(sMutex);
        return std::find(sEvents.begin(), sEvents.end(), expectedEvent) != sEvents.end();
    }

    static std::vector<int64_t> counterValues(const std::string& counterName) {
        std::lock_guard<std::mutex> lock(sMutex);
        std::vector<int64_t> values;
        for (const auto& [name, value] : sCounters) {
            if (name == counterName) {
                values.push_back(value);
            }
        }
        return values;
    }
};

class ScopedMockTrace {
public:
    explicit ScopedMockTrace(bool enabled) {
        RecordedTraceCallbacks::reset(enabled);
        mPrevious = TraceTestPeer::swapInstance(
                std::make_unique<Trace>(&RecordedTraceCallbacks::beginSection,
                                        &RecordedTraceCallbacks::endSection,
                                        &RecordedTraceCallbacks::isEnabled,
                                        &RecordedTraceCallbacks::beginAsyncSection,
                                        &RecordedTraceCallbacks::endAsyncSection,
                                        &RecordedTraceCallbacks::setCounter));
    }

    ~ScopedMockTrace() {
        TraceTestPeer::swapInstance(std::move(mPrevious));
        RecordedTraceCallbacks::reset(false);
    }

private:
    std::unique_ptr<Trace> mPrevious;
};

} // namespace test
} // namespace gamesdk

#endif // AGDK_TRACE != 0

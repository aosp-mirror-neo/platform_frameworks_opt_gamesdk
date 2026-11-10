/*
 * Copyright 2018 The Android Open Source Project
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

#ifndef AGDK_TRACE
#define AGDK_TRACE 1
#endif

#if AGDK_TRACE == 0

#include <cstdint>
#include <memory>

namespace gamesdk {

class Trace {
public:
    static std::unique_ptr<Trace> create() {
        return std::make_unique<Trace>();
    }
    static Trace* getInstance() {
        static Trace trace;
        return &trace;
    }
    bool isAvailable() const {
        return false;
    }
    bool isEnabled() const {
        return false;
    }
    void beginSection(const char*) const {}
    void endSection() const {}
    void beginAsyncSection(const char*, int64_t) {}
    void endAsyncSection(const char*, int64_t) {}
    void setCounter(const char*, int64_t) {}
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    void instant(const char*, ...) const {
    }
    bool supportsBasicATrace() const {
        return false;
    }
    bool supportsFullATrace() const {
        return false;
    }
};

struct ScopedTrace {
    explicit ScopedTrace(const char*) {}
};

} // namespace gamesdk

#define TRACE_CALL() ((void)0)
#define TRACE_SCOPE(name) ((void)0)
#define TRACE_INT(name, value) ((void)0)
#define TRACE_INSTANT(...) ((void)0)
#define TRACE_ENABLED() false
// Declares a member that exists only when tracing is compiled in.
#define TRACE_MEMBER(...) static_assert(true, "")

#else // AGDK_TRACE != 0

#ifdef __ANDROID__
#include <android/log.h>
#include <dlfcn.h>
#endif

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace gamesdk {

class Trace {
public:
    using ATrace_beginSection_type = void (*)(const char* sectionName);
    using ATrace_endSection_type = void (*)();
    using ATrace_isEnabled_type = bool (*)();

    using ATrace_beginAsyncSection_type = void (*)(const char* sectionName, int32_t cookie);
    using ATrace_endAsyncSection_type = void (*)(const char* sectionName, int32_t cookie);
    using ATrace_setCounter_type = void (*)(const char* counterName, int64_t counterValue);

    Trace() {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "Trace", "Unable to load NDK tracing APIs");
#endif
    }

    Trace(ATrace_beginSection_type beginSection, ATrace_endSection_type endSection,
          ATrace_isEnabled_type isEnabled, ATrace_beginAsyncSection_type beginAsyncSection,
          ATrace_endAsyncSection_type endAsyncSection, ATrace_setCounter_type setCounter)
          : ATrace_beginSection(beginSection),
            ATrace_endSection(endSection),
            ATrace_isEnabled(isEnabled),
            ATrace_beginAsyncSection(beginAsyncSection),
            ATrace_endAsyncSection(endAsyncSection),
            ATrace_setCounter(setCounter) {}

    static std::unique_ptr<Trace> create() {
#ifdef __ANDROID__
        void* libandroid = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!libandroid) {
            return std::make_unique<Trace>();
        }

        auto beginSection = reinterpret_cast<ATrace_beginSection_type>(
                dlsym(libandroid, "ATrace_beginSection"));
        if (!beginSection) {
            return std::make_unique<Trace>();
        }

        auto endSection =
                reinterpret_cast<ATrace_endSection_type>(dlsym(libandroid, "ATrace_endSection"));
        if (!endSection) {
            return std::make_unique<Trace>();
        }

        auto isEnabled =
                reinterpret_cast<ATrace_isEnabled_type>(dlsym(libandroid, "ATrace_isEnabled"));
        if (!isEnabled) {
            return std::make_unique<Trace>();
        }

        auto setCounter =
                reinterpret_cast<ATrace_setCounter_type>(dlsym(libandroid, "ATrace_setCounter"));
        /* ATrace_setCounter was added in API 29, continue even if it is not
         * available */

        auto beginAsyncSection = reinterpret_cast<ATrace_beginAsyncSection_type>(
                dlsym(libandroid, "ATrace_beginAsyncSection"));
        /* ATrace_beginAsyncSection was added in API 29, continue even if it is
         * not available */

        auto endAsyncSection = reinterpret_cast<ATrace_endAsyncSection_type>(
                dlsym(libandroid, "ATrace_endAsyncSection"));
        /* ATrace_endAsyncSection was added in API 29, continue even if it is
         * not available */

        return std::make_unique<Trace>(beginSection, endSection, isEnabled, beginAsyncSection,
                                       endAsyncSection, setCounter);
#else
        return std::make_unique<Trace>();
#endif
    }

    bool isAvailable() const {
        return ATrace_beginSection != nullptr;
    }

    bool isEnabled() const {
        return (ATrace_isEnabled != nullptr) && ATrace_isEnabled();
    }

    void beginSection(const char* name) const {
        if (!ATrace_beginSection) {
            return;
        }

        ATrace_beginSection(name);
    }

    void endSection() const {
        if (!ATrace_endSection) {
            return;
        }

        ATrace_endSection();
    }

    void beginAsyncSection(const char* name, int64_t value) {
        if (!ATrace_beginAsyncSection || !isEnabled()) {
            return;
        }

        ATrace_beginAsyncSection(name, value);
    }

    void endAsyncSection(const char* name, int64_t value) {
        if (!ATrace_endAsyncSection || !isEnabled()) {
            return;
        }

        ATrace_endAsyncSection(name, value);
    }

    void setCounter(const char* name, int64_t value) {
        if (!ATrace_setCounter || !isEnabled()) {
            return;
        }

        ATrace_setCounter(name, value);
    }

    // Emits a zero-length trace section with a printf-formatted name.
    // Does nothing when tracing is disabled or unavailable.
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    void instant(const char* format, ...) const {
        if (!isAvailable() || !isEnabled()) {
            return;
        }

        char name[256];
        va_list args;
        va_start(args, format);
        const int written = vsnprintf(name, sizeof(name), format, args);
        va_end(args);
        if (written < 0) {
            return;
        }

        beginSection(name);
        endSection();
    }

    bool supportsBasicATrace() const {
        return ATrace_beginSection && ATrace_endSection;
    }

    bool supportsFullATrace() const {
        return supportsBasicATrace() && ATrace_beginAsyncSection && ATrace_endAsyncSection &&
                ATrace_setCounter;
    }

    static Trace* getInstance() {
        return instanceStorage().get();
    }

private:
    friend struct TraceTestPeer;

    static std::unique_ptr<Trace>& instanceStorage() {
        static std::unique_ptr<Trace> trace = Trace::create();
        return trace;
    }

    const ATrace_beginSection_type ATrace_beginSection = nullptr;
    const ATrace_endSection_type ATrace_endSection = nullptr;
    const ATrace_isEnabled_type ATrace_isEnabled = nullptr;

    const ATrace_beginAsyncSection_type ATrace_beginAsyncSection = nullptr;
    const ATrace_endAsyncSection_type ATrace_endAsyncSection = nullptr;
    const ATrace_setCounter_type ATrace_setCounter = nullptr;
};

struct ScopedTrace {
    explicit ScopedTrace(const char* name) {
        if (!name) {
            return;
        }
        Trace* trace = Trace::getInstance();
        if (!trace->isAvailable() || !trace->isEnabled()) {
            return;
        }

        trace->beginSection(name);
        mIsTracing = true;
    }

    ~ScopedTrace() {
        if (!mIsTracing) {
            return;
        }

        Trace* trace = Trace::getInstance();
        trace->endSection();
    }

private:
    bool mIsTracing = false;
};

} // namespace gamesdk

#define PASTE_HELPER_HELPER(a, b) a##b
#define PASTE_HELPER(a, b) PASTE_HELPER_HELPER(a, b)
#define TRACE_CALL() gamesdk::ScopedTrace PASTE_HELPER(scopedTrace, __LINE__)(__PRETTY_FUNCTION__)
#define TRACE_SCOPE(name) \
    gamesdk::ScopedTrace PASTE_HELPER(scopedTrace, __LINE__)(TRACE_ENABLED() ? (name) : nullptr)
#define TRACE_INT(name, value)                                          \
    do {                                                                \
        if (TRACE_ENABLED()) {                                          \
            gamesdk::Trace::getInstance()->setCounter((name), (value)); \
        }                                                               \
    } while (0)
#define TRACE_INSTANT(...)                                       \
    do {                                                         \
        if (TRACE_ENABLED()) {                                   \
            gamesdk::Trace::getInstance()->instant(__VA_ARGS__); \
        }                                                        \
    } while (0)
#define TRACE_ENABLED() gamesdk::Trace::getInstance()->isEnabled()
// Declares a member that exists only when tracing is compiled in.
#define TRACE_MEMBER(...) __VA_ARGS__

#endif // AGDK_TRACE != 0

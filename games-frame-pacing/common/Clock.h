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

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace swappy {

// Time source for frame pacing, and the only way frame pacing blocks on time
// or on another thread. Implementations must be thread-safe and never go
// backwards. now() neither blocks nor calls back into its caller.
class Clock {
public:
    using time_point = std::chrono::steady_clock::time_point;

    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

    // Returns the current monotonic timestamp on this clock.
    [[nodiscard]] virtual time_point now() const = 0;

    // Blocks the calling thread until now() >= deadline. Returns immediately
    // if deadline has already passed.
    virtual void sleepUntil(time_point deadline) const = 0;

    // Blocks the calling thread until predicate() returns true, like
    // std::condition_variable::wait(lock, predicate). lock must own the mutex
    // guarding the state predicate reads, and every change to that state must
    // be followed by a notification of condition. Returns with lock owned and
    // predicate() true.
    virtual void wait(std::unique_lock<std::mutex>& lock, std::condition_variable& condition,
                      const std::function<bool()>& predicate) const = 0;

protected:
    Clock() = default;
    ~Clock() = default;
};

// Clock backed by std::chrono::steady_clock and standard library waits.
class SteadyClock final : public Clock {
public:
    // Returns the process-wide instance.
    [[nodiscard]] static const SteadyClock& instance() {
        static const SteadyClock steadyClock;
        return steadyClock;
    }

    [[nodiscard]] time_point now() const override {
        return std::chrono::steady_clock::now();
    }

    void sleepUntil(time_point deadline) const override {
        std::this_thread::sleep_until(deadline);
    }

    void wait(std::unique_lock<std::mutex>& lock, std::condition_variable& condition,
              const std::function<bool()>& predicate) const override {
        condition.wait(lock, predicate);
    }

private:
    SteadyClock() = default;
};

} // namespace swappy

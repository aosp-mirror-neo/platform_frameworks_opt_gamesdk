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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>

#include "SwapIntervalLadder.h"

namespace swappy {

// Maps supported display refresh periods to physical Display.Mode IDs (or -1
// for mode-less ARR and alternative refresh rates).
//
// Example RefreshPeriodMap on Samsung Galaxy S26 Ultra (SM-S948U1, API 36,
// 1080x2340, 240 Hz TE clock):
// ```
// {
//    8,333,333ns:  2,  // 120.00 Hz (240/2, Display.Mode id=2)
//   12,499,998ns: -1,  //  80.00 Hz (240/3, mode-less ARR)
//   16,666,667ns:  5,  //  60.00 Hz (240/4, Display.Mode id=5)
//   20,833,330ns: -1,  //  48.00 Hz (240/5, mode-less ARR)
//   24,999,995ns: -1,  //  40.00 Hz (240/6, mode-less ARR)
//   29,166,661ns: -1,  //  34.29 Hz (240/7, mode-less ARR)
//   33,333,327ns: -1,  //  30.00 Hz (240/8, mode-less ARR)
//   37,499,993ns: -1,  //  26.67 Hz (240/9, mode-less ARR)
//   41,666,660ns: -1,  //  24.00 Hz (240/10, mode-less ARR)
//   45,833,329ns: -1,  //  21.82 Hz (240/11, mode-less ARR)
//   49,999,990ns: -1,  //  20.00 Hz (240/12, mode-less ARR)
// }
// ```
using RefreshPeriodMap = std::map<std::chrono::nanoseconds, int>;

// Inserts a refresh period, deduplicating entries within
// DURATION_ROUNDING_MARGIN (1us) and preserving a physical Display.Mode ID
// (>= 0) over a mode-less ARR entry (-1).
inline void insertRefreshPeriod(RefreshPeriodMap& periods, std::chrono::nanoseconds period,
                                int modeId) {
    if (period <= std::chrono::nanoseconds::zero()) {
        return;
    }

    auto it = periods.lower_bound(period - DURATION_ROUNDING_MARGIN);
    if (it != periods.end() && it->first <= period + DURATION_ROUNDING_MARGIN) {
        if (it->second < 0 && modeId >= 0) {
            it->second = modeId;
        }
        return;
    }

    periods.emplace(period, modeId);
}

// Implements Swappy*_getSupportedRefreshPeriodsNS: returns the total count when
// outPeriods is null, otherwise writes at most allocatedEntries periods and
// returns the number of entries written.
[[nodiscard]] inline int copyRefreshPeriods(const RefreshPeriodMap* periods, uint64_t* outPeriods,
                                            int allocatedEntries) {
    if (periods == nullptr) {
        return 0;
    }
    if (outPeriods == nullptr) {
        return static_cast<int>(periods->size());
    }
    if (allocatedEntries <= 0) {
        return 0;
    }

    const int entriesToWrite = std::min(static_cast<int>(periods->size()), allocatedEntries);
    int index = 0;
    for (const auto& [period, modeId] : *periods) {
        if (index >= entriesToWrite) {
            break;
        }
        outPeriods[index] = static_cast<uint64_t>(period.count());
        ++index;
    }
    return index;
}

} // namespace swappy

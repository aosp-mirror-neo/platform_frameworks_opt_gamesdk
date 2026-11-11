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

#include <cstdint>
#include <string>

#include "gtest/gtest.h"
#include "swappy/swappy_common.h"

namespace {

TEST(SwappyVersionHostTest, MatchesHeaderConstants) {
    EXPECT_EQ(Swappy_version(), static_cast<uint32_t>(SWAPPY_PACKED_VERSION));
    const std::string expectedVersion = std::to_string(SWAPPY_MAJOR_VERSION) + "." +
            std::to_string(SWAPPY_MINOR_VERSION) + "." + std::to_string(SWAPPY_BUGFIX_VERSION);
    EXPECT_STREQ(Swappy_versionString(), expectedVersion.c_str());
}

} // namespace

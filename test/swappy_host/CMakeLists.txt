#
# Copyright 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

cmake_minimum_required(VERSION 3.18.1)

project(swappy_host_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../external/googletest")
  set(ANDROID_GTEST_DIR "../../../external/googletest")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../external/googletest")
  set(ANDROID_GTEST_DIR "../../../../../external/googletest")
endif()

find_package(Threads REQUIRED)

if(ANDROID_GTEST_DIR)
  set(BUILD_GMOCK OFF)
  set(INSTALL_GTEST OFF)
  add_subdirectory("${ANDROID_GTEST_DIR}" googletest-build)
else()
  # Without the AOSP checkout around gamesdk, use an installed googletest,
  # such as the one from `brew install googletest`.
  find_package(GTest REQUIRED)
endif()

if(AGDK_ENABLE_COVERAGE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -O0 -g -Wno-error")
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O0 -g")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
endif()

include_directories(
  ../../games-frame-pacing/common
  ../../include
  ../../src/common
)

add_executable(swappy_host_test
  swappy_host_test.cpp
  choreographerfilter_test.cpp
  choreographertimer_test.cpp
  classicpacingimpl_test.cpp
  clock_test.cpp
  framedurations_test.cpp
  swapintervalladder_test.cpp
  ../../games-frame-pacing/common/ChoreographerFilter.cpp
  ../../games-frame-pacing/common/Settings.cpp
  ../../games-frame-pacing/common/Thread.cpp
)

target_link_libraries(swappy_host_test
  GTest::gtest
  GTest::gtest_main
  Threads::Threads
  ${CMAKE_DL_LIBS}
)

enable_testing()
include(GoogleTest)
gtest_discover_tests(swappy_host_test PROPERTIES TIMEOUT 10)

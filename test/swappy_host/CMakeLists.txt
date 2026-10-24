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

foreach(repo_root_rel "../../.." "../../../.." "../../../../..")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${repo_root_rel}/external/googletest")
    set(ANDROID_GTEST_DIR "${repo_root_rel}/external/googletest")
    break()
  endif()
endforeach()

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

if(DEFINED ENV{PREBUILTS_DIR} AND EXISTS "$ENV{PREBUILTS_DIR}")
  set(AGDK_PREBUILTS_DIR "$ENV{PREBUILTS_DIR}")
else()
  foreach(repo_root_rel "../../.." "../../../.." "../../../../..")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${repo_root_rel}/prebuilts")
      set(AGDK_PREBUILTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${repo_root_rel}/prebuilts")
      break()
    endif()
  endforeach()
endif()

if(APPLE)
  set(SWAPPY_HOST_JDK_ARCHS darwin-arm64 darwin-x86)
  set(SWAPPY_HOST_JNI_MD_DIR darwin)
elseif(WIN32)
  set(SWAPPY_HOST_JDK_ARCHS windows-x86)
  set(SWAPPY_HOST_JNI_MD_DIR win32)
else()
  set(SWAPPY_HOST_JDK_ARCHS linux-x86)
  set(SWAPPY_HOST_JNI_MD_DIR linux)
endif()

set(SWAPPY_HOST_JNI_INCLUDE_DIRS)
if(AGDK_PREBUILTS_DIR)
  foreach(jdk_ver jdk21 jdk17)
    foreach(jdk_arch ${SWAPPY_HOST_JDK_ARCHS})
      set(jdk_inc "${AGDK_PREBUILTS_DIR}/jdk/${jdk_ver}/${jdk_arch}/include")
      if(EXISTS "${jdk_inc}/jni.h")
        set(SWAPPY_HOST_JNI_INCLUDE_DIRS "${jdk_inc}" "${jdk_inc}/${SWAPPY_HOST_JNI_MD_DIR}")
        break()
      endif()
    endforeach()
    if(SWAPPY_HOST_JNI_INCLUDE_DIRS)
      break()
    endif()
  endforeach()
endif()
if(NOT SWAPPY_HOST_JNI_INCLUDE_DIRS)
  find_package(JNI REQUIRED)
  set(SWAPPY_HOST_JNI_INCLUDE_DIRS ${JNI_INCLUDE_DIRS})
endif()

set(SWAPPY_HOST_VULKAN_INCLUDE_DIR)
if(AGDK_PREBUILTS_DIR)
  foreach(ndk_ver r23 r21 r20)
    if(EXISTS "${AGDK_PREBUILTS_DIR}/ndk/${ndk_ver}/sources/third_party/vulkan/src/include/vulkan/vulkan.h")
      set(SWAPPY_HOST_VULKAN_INCLUDE_DIR
        "${AGDK_PREBUILTS_DIR}/ndk/${ndk_ver}/sources/third_party/vulkan/src/include")
      break()
    endif()
  endforeach()
endif()
if(NOT SWAPPY_HOST_VULKAN_INCLUDE_DIR)
  find_package(Vulkan REQUIRED)
  set(SWAPPY_HOST_VULKAN_INCLUDE_DIR ${Vulkan_INCLUDE_DIRS})
endif()

include_directories(
  ../../games-frame-pacing/common
  ../../games-frame-pacing/vulkan
  ../../include
  ../../src/common
  ../../test/common
)
include_directories(SYSTEM
  ${SWAPPY_HOST_JNI_INCLUDE_DIRS}
  ${SWAPPY_HOST_VULKAN_INCLUDE_DIR}
)

set(SWAPPY_HOST_COMMON_SOURCES
  swappy_host_test.cpp
  choreographerfilter_test.cpp
  choreographertimer_test.cpp
  classicpacingimpl_test.cpp
  clock_test.cpp
  displayratecontroller_test.cpp
  framedurations_test.cpp
  framestatistics_test.cpp
  pacingsim_test.cpp
  refreshperiodset_test.cpp
  swapintervalladder_test.cpp
  swappy_c_test.cpp
  swappycommon_test.cpp
  swappyvk_host_test.cpp
  jniutil_test.cpp
  jniutil_no_dex_test.cpp
  scoped_jni_env_test.cpp
  ../../games-frame-pacing/common/CPUTracer.cpp
  ../../games-frame-pacing/common/ChoreographerFilter.cpp
  ../../games-frame-pacing/common/FrameStatistics.cpp
  ../../games-frame-pacing/common/Settings.cpp
  ../../games-frame-pacing/common/SwappyCommon.cpp
  ../../games-frame-pacing/common/Thread.cpp
  ../../games-frame-pacing/common/swappy_c.cpp
  ../../games-frame-pacing/vulkan/SwappyVk.cpp
  ../../games-frame-pacing/vulkan/SwappyVkBase.cpp
  ../../games-frame-pacing/vulkan/SwappyVkFallback.cpp
  ../../games-frame-pacing/vulkan/SwappyVkGoogleDisplayTiming.cpp
  ../../games-frame-pacing/vulkan/swappyVk_c.cpp
)

set(SWAPPY_HOST_TEST_SOURCES
  ${SWAPPY_HOST_COMMON_SOURCES}
  ../common/trace_test.cpp
  cputracer_test.cpp
  swappy_trace_test.cpp
  swappycommon_trace_test.cpp
)

set(SWAPPY_HOST_TRACE_OFF_TEST_SOURCES
  ${SWAPPY_HOST_COMMON_SOURCES}
  ../common/trace_off_test.cpp
  swappy_trace_off_test.cpp
)

set(SWAPPY_HOST_TEST_LIBS
  GTest::gtest
  GTest::gtest_main
  Threads::Threads
  ${CMAKE_DL_LIBS}
)

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_FLAGS "-Wthread-safety -Werror=thread-safety -D_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS")
check_cxx_source_compiles("
  #include <mutex>
  struct S { std::mutex m; int x __attribute__((guarded_by(m))) = 0; };
  int main() { S s; std::lock_guard<std::mutex> l(s.m); return s.x; }
" SWAPPY_HAS_THREAD_SAFETY)
cmake_pop_check_state()

set(SWAPPY_HOST_TEST_COMPILE_OPTIONS)
set(SWAPPY_HOST_TEST_COMPILE_DEFINITIONS)
if(SWAPPY_HAS_THREAD_SAFETY)
  list(APPEND SWAPPY_HOST_TEST_COMPILE_OPTIONS -Wthread-safety)
  if(NOT AGDK_ENABLE_COVERAGE)
    list(APPEND SWAPPY_HOST_TEST_COMPILE_OPTIONS -Werror=thread-safety)
  endif()
  list(APPEND SWAPPY_HOST_TEST_COMPILE_DEFINITIONS _LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS)
endif()

enable_testing()
include(GoogleTest)

add_executable(swappy_host_test ${SWAPPY_HOST_TEST_SOURCES})
target_compile_options(swappy_host_test PRIVATE ${SWAPPY_HOST_TEST_COMPILE_OPTIONS})
target_compile_definitions(swappy_host_test PRIVATE ${SWAPPY_HOST_TEST_COMPILE_DEFINITIONS})
target_link_libraries(swappy_host_test ${SWAPPY_HOST_TEST_LIBS})
gtest_discover_tests(swappy_host_test PROPERTIES TIMEOUT 10)

add_executable(swappy_host_trace_off_test ${SWAPPY_HOST_TRACE_OFF_TEST_SOURCES})
target_compile_options(swappy_host_trace_off_test PRIVATE ${SWAPPY_HOST_TEST_COMPILE_OPTIONS})
target_compile_definitions(swappy_host_trace_off_test PRIVATE ${SWAPPY_HOST_TEST_COMPILE_DEFINITIONS} AGDK_TRACE=0)
target_link_libraries(swappy_host_trace_off_test ${SWAPPY_HOST_TEST_LIBS})
gtest_discover_tests(swappy_host_trace_off_test TEST_PREFIX "trace_off." PROPERTIES TIMEOUT 10)

if(UNIX AND NOT AGDK_ENABLE_COVERAGE)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=thread")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=thread")
  check_cxx_source_compiles("int main() { return 0; }" SWAPPY_HAS_TSAN)
  cmake_pop_check_state()

  if(SWAPPY_HAS_TSAN)
    add_executable(swappy_host_tsan_test ${SWAPPY_HOST_TEST_SOURCES})
    target_compile_options(swappy_host_tsan_test PRIVATE
      ${SWAPPY_HOST_TEST_COMPILE_OPTIONS} -fsanitize=thread -g -O1)
    target_compile_definitions(swappy_host_tsan_test PRIVATE
      ${SWAPPY_HOST_TEST_COMPILE_DEFINITIONS})
    target_link_options(swappy_host_tsan_test PRIVATE -fsanitize=thread)
    target_link_libraries(swappy_host_tsan_test ${SWAPPY_HOST_TEST_LIBS})
    gtest_discover_tests(swappy_host_tsan_test TEST_PREFIX "tsan." PROPERTIES TIMEOUT 10)
  endif()
endif()

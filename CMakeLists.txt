cmake_minimum_required(VERSION 3.25)
project(SlopCap VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SLOPCAP_WERROR "Treat compiler warnings as errors" OFF)
option(SLOPCAP_BUILD_TESTS "Build the unit tests (needs Catch2 3)" ON)
option(SLOPCAP_BUILD_CAMPROBE "Build tools/camprobe (needs the ZWO and/or QHY SDK)" ON)

# Warnings for everything we compile (not third-party code).
add_library(slopcap_warnings INTERFACE)
if(MSVC)
  target_compile_options(slopcap_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus $<$<BOOL:${SLOPCAP_WERROR}>:/WX>)
else()
  target_compile_options(slopcap_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
                         -Woverloaded-virtual -Wimplicit-fallthrough $<$<BOOL:${SLOPCAP_WERROR}>:-Werror>)
endif()

# ---------------------------------------------------------------- core library
add_library(slopcap_core STATIC
  src/io/ser.cpp
  src/camera/camera.cpp
  src/camera/sim_camera.cpp
  src/pipeline/frame_ring.cpp
  src/pipeline/capture_session.cpp
)
target_include_directories(slopcap_core PUBLIC src)
target_link_libraries(slopcap_core PRIVATE slopcap_warnings)
find_package(Threads REQUIRED)
target_link_libraries(slopcap_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------- tools
add_executable(slopcap-serinfo tools/serinfo/main.cpp)
target_link_libraries(slopcap-serinfo PRIVATE slopcap_core slopcap_warnings)
add_executable(slopcap-bench tools/bench/main.cpp)
target_link_libraries(slopcap-bench PRIVATE slopcap_core slopcap_warnings)

if(SLOPCAP_BUILD_CAMPROBE)
  if(WIN32 AND NOT WIN_SDK_DLL_DIR)
    message(STATUS "camprobe: skipped on Windows unless -DWIN_SDK_DLL_DIR=<folder with ASICamera2.dll, qhyccd.dll>")
  else()
    add_subdirectory(tools/camprobe)
  endif()
endif()

# ---------------------------------------------------------------- tests
if(SLOPCAP_BUILD_TESTS)
  find_package(Catch2 3 CONFIG QUIET)
  if(Catch2_FOUND)
    enable_testing()
    add_executable(slopcap-tests tests/test_ser.cpp tests/test_sim_camera.cpp tests/test_pipeline.cpp)
    target_link_libraries(slopcap-tests PRIVATE slopcap_core slopcap_warnings Catch2::Catch2WithMain)
    include(Catch)
    catch_discover_tests(slopcap-tests)
  else()
    message(WARNING "Catch2 3 not found: tests disabled (Linux: apt install catch2; Windows: vcpkg install catch2)")
  endif()
endif()

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
  src/io/direct_file.cpp
  src/io/ser_direct.cpp
  src/camera/camera.cpp
  src/camera/sim_camera.cpp
  src/pipeline/frame_ring.cpp
  src/pipeline/capture_session.cpp
)
target_sources(slopcap_core PRIVATE src/camera/open_camera.cpp)
target_include_directories(slopcap_core PUBLIC src)
target_link_libraries(slopcap_core PRIVATE slopcap_warnings)
find_package(Threads REQUIRED)
target_link_libraries(slopcap_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------- camera SDKs (optional backends)
# Linux: the distro / INDI-PPA packages. Windows cross-build: the target machine's DLLs in WIN_SDK_DLL_DIR plus the
# same headers. QHY's packaged headers need tools/camprobe/compat/qhy (config.h / cyapi.h shims).
set(QHY_COMPAT ${CMAKE_CURRENT_SOURCE_DIR}/tools/camprobe/compat/qhy)
if(WIN32)
  if(WIN_SDK_DLL_DIR AND EXISTS ${WIN_SDK_DLL_DIR}/ASICamera2.dll AND EXISTS /usr/include/libasi/ASICamera2.h)
    set(CORE_SDK_INC ${CMAKE_CURRENT_BINARY_DIR}/core-sdk-include)
    file(MAKE_DIRECTORY ${CORE_SDK_INC})
    file(CREATE_LINK /usr/include/libasi ${CORE_SDK_INC}/libasi SYMBOLIC)
    target_sources(slopcap_core PRIVATE src/camera/asi_camera.cpp)
    target_include_directories(slopcap_core PRIVATE ${CORE_SDK_INC})
    target_link_libraries(slopcap_core PUBLIC ${WIN_SDK_DLL_DIR}/ASICamera2.dll)
    target_compile_definitions(slopcap_core PUBLIC SLOPCAP_HAVE_ASI)
  endif()
  if(WIN_SDK_DLL_DIR AND EXISTS ${WIN_SDK_DLL_DIR}/qhyccd.dll AND EXISTS /usr/include/libqhy/qhyccd.h)
    target_sources(slopcap_core PRIVATE src/camera/qhy_camera.cpp)
    target_include_directories(slopcap_core PRIVATE ${QHY_COMPAT} /usr/include/libqhy)
    target_link_libraries(slopcap_core PUBLIC ${WIN_SDK_DLL_DIR}/qhyccd.dll)
    target_compile_definitions(slopcap_core PUBLIC SLOPCAP_HAVE_QHY)
  endif()
else()
  find_path(SLOPCAP_ASI_INCLUDE libasi/ASICamera2.h)
  find_library(SLOPCAP_ASI_LIB ASICamera2)
  if(SLOPCAP_ASI_INCLUDE AND SLOPCAP_ASI_LIB)
    target_sources(slopcap_core PRIVATE src/camera/asi_camera.cpp)
    target_include_directories(slopcap_core PRIVATE ${SLOPCAP_ASI_INCLUDE})
    target_link_libraries(slopcap_core PUBLIC ${SLOPCAP_ASI_LIB})
    target_compile_definitions(slopcap_core PUBLIC SLOPCAP_HAVE_ASI)
  endif()
  find_path(SLOPCAP_QHY_INCLUDE qhyccd.h PATHS /usr/include/libqhy NO_DEFAULT_PATH)
  find_library(SLOPCAP_QHY_LIB qhyccd PATHS /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu NO_DEFAULT_PATH)
  if(SLOPCAP_QHY_INCLUDE AND SLOPCAP_QHY_LIB)
    target_sources(slopcap_core PRIVATE src/camera/qhy_camera.cpp)
    target_include_directories(slopcap_core PRIVATE ${QHY_COMPAT} ${SLOPCAP_QHY_INCLUDE})
    target_link_libraries(slopcap_core PUBLIC ${SLOPCAP_QHY_LIB})
    target_compile_definitions(slopcap_core PUBLIC SLOPCAP_HAVE_QHY)
    # pin the packaged libqhyccd (a manual /usr/local install may otherwise be loaded first)
    get_filename_component(SLOPCAP_QHY_LIBDIR ${SLOPCAP_QHY_LIB} DIRECTORY)
    set(CMAKE_BUILD_RPATH ${SLOPCAP_QHY_LIBDIR})
    add_link_options(-Wl,--enable-new-dtags)
  endif()
endif()
get_target_property(_defs slopcap_core INTERFACE_COMPILE_DEFINITIONS)
message(STATUS "SlopCap camera backends: ${_defs}")

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
    add_executable(slopcap-tests tests/test_ser.cpp tests/test_sim_camera.cpp tests/test_pipeline.cpp tests/test_direct_writer.cpp)
    target_link_libraries(slopcap-tests PRIVATE slopcap_core slopcap_warnings Catch2::Catch2WithMain)
    include(Catch)
    catch_discover_tests(slopcap-tests)
  else()
    message(WARNING "Catch2 3 not found: tests disabled (Linux: apt install catch2; Windows: vcpkg install catch2)")
  endif()
endif()

cmake_minimum_required(VERSION 3.25)
project(SlopCap VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SLOPCAP_WERROR "Treat compiler warnings as errors" OFF)
option(SLOPCAP_BUILD_TESTS "Build the unit tests (needs Catch2 3)" ON)
option(SLOPCAP_BUILD_CAMPROBE "Build tools/camprobe (needs the ZWO and/or QHY SDK)" ON)
option(SLOPCAP_BUILD_GUI "Build the Qt GUI (needs Qt 6 Widgets + OpenGL)" ON)
option(SLOPCAP_SINGLE_EXE "Windows: embed the camera SDK DLLs in slopcap.exe (with a static Qt: one self-contained file)" OFF)

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
  src/preview/latest_frame.cpp
  src/preview/render_cpu.cpp
  src/analysis/focus.cpp
  src/analysis/sweep.cpp
  src/analysis/collimation.cpp
  src/analysis/ephemeris.cpp
  src/analysis/identify.cpp
  src/analysis/dispersion.cpp
)
target_sources(slopcap_core PRIVATE src/camera/open_camera.cpp)
target_include_directories(slopcap_core PUBLIC src)
# Lets SLOPCAP_SIMD_SUM (src/util/simd.hpp) vectorize float reductions; enables the pragma only, no OpenMP runtime.
if(NOT MSVC)
  target_compile_options(slopcap_core PRIVATE -fopenmp-simd)
endif()
target_link_libraries(slopcap_core PRIVATE slopcap_warnings)
find_package(Threads REQUIRED)
target_link_libraries(slopcap_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------- camera SDKs (optional backends)
# Linux: the distro / INDI-PPA packages. Windows cross-build: the target machine's DLLs in WIN_SDK_DLL_DIR plus the
# same headers. QHY's packaged headers need tools/camprobe/compat/qhy (config.h / cyapi.h shims).
set(QHY_COMPAT ${CMAKE_CURRENT_SOURCE_DIR}/tools/camprobe/compat/qhy)
if(WIN32)
  # Delay-loaded: executables start without the DLLs and say which one is missing instead of failing to launch.
  include(cmake/DelayImport.cmake)
  if(WIN_SDK_DLL_DIR AND EXISTS ${WIN_SDK_DLL_DIR}/ASICamera2.dll AND EXISTS /usr/include/libasi/ASICamera2.h)
    set(CORE_SDK_INC ${CMAKE_CURRENT_BINARY_DIR}/core-sdk-include)
    file(MAKE_DIRECTORY ${CORE_SDK_INC})
    file(CREATE_LINK /usr/include/libasi ${CORE_SDK_INC}/libasi SYMBOLIC)
    target_sources(slopcap_core PRIVATE src/camera/asi_camera.cpp)
    target_include_directories(slopcap_core PRIVATE ${CORE_SDK_INC})
    slopcap_delay_import(${WIN_SDK_DLL_DIR}/ASICamera2.dll ASI_DELAYLIB)
    target_link_libraries(slopcap_core PUBLIC ${ASI_DELAYLIB})
    target_compile_definitions(slopcap_core PUBLIC SLOPCAP_HAVE_ASI)
  endif()
  if(WIN_SDK_DLL_DIR AND EXISTS ${WIN_SDK_DLL_DIR}/qhyccd.dll AND EXISTS /usr/include/libqhy/qhyccd.h)
    target_sources(slopcap_core PRIVATE src/camera/qhy_camera.cpp)
    target_include_directories(slopcap_core PRIVATE ${QHY_COMPAT} /usr/include/libqhy)
    slopcap_delay_import(${WIN_SDK_DLL_DIR}/qhyccd.dll QHY_DELAYLIB)
    target_link_libraries(slopcap_core PUBLIC ${QHY_DELAYLIB})
    target_compile_definitions(slopcap_core PUBLIC SLOPCAP_HAVE_QHY)
  endif()
  # Player One, ToupTek, MallinCam: the DLL from WIN_SDK_DLL_DIR (SharpCap's folder has all three), the header from the
  # INDI packages in the build image, linked into a private include folder (all of /usr/include would bring Linux's)
  foreach(sdk "SLOPCAP_HAVE_PLAYERONE;PlayerOneCamera.dll;libplayerone;src/camera/playerone_camera.cpp"
              "SLOPCAP_HAVE_TOUPTEK;toupcam.dll;libtoupcam;src/camera/touptek_camera.cpp"
              "SLOPCAP_HAVE_MALLINCAM;mallincam.dll;libmallincam;src/camera/mallincam_camera.cpp")
    list(GET sdk 0 def)
    list(GET sdk 1 dll)
    list(GET sdk 2 incdir)
    list(GET sdk 3 src)
    if(WIN_SDK_DLL_DIR AND EXISTS ${WIN_SDK_DLL_DIR}/${dll} AND EXISTS /usr/include/${incdir})
      set(CORE_SDK_INC ${CMAKE_CURRENT_BINARY_DIR}/core-sdk-include)
      file(MAKE_DIRECTORY ${CORE_SDK_INC})
      file(CREATE_LINK /usr/include/${incdir} ${CORE_SDK_INC}/${incdir} SYMBOLIC)
      target_sources(slopcap_core PRIVATE ${src})
      target_include_directories(slopcap_core PRIVATE ${CORE_SDK_INC})
      slopcap_delay_import(${WIN_SDK_DLL_DIR}/${dll} delaylib)
      target_link_libraries(slopcap_core PUBLIC ${delaylib})
      target_compile_definitions(slopcap_core PUBLIC ${def})
    endif()
  endforeach()
elseif(APPLE)
  # macOS: the vendor SDKs as INDI's indi-3rdparty repository ships them, so a checkout of it (or a copy of its libasi/
  # and libqhy/ folders) is all it takes: libasi/ASICamera2.h + libasi/mac_arm64/libASICamera2.bin, libqhy/*.h +
  # libqhy/mac_arm64/libqhyccd.bin. The .bin files are dylibs whose install name is @rpath/lib<name>.dylib; they are
  # copied here under that name, linked, and copied into SlopCap.app/Contents/Frameworks (SLOPCAP_MAC_SDK_DYLIBS).
  set(SLOPCAP_MAC_SDK_DIR "" CACHE PATH "indi-3rdparty checkout (or a copy of its libasi/ and libqhy/) with the macOS SDKs")
  if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"))
    set(_mac_sdk_arch mac_x64)
  else()
    set(_mac_sdk_arch mac_arm64)
  endif()
  set(SDK_LIB_DIR ${CMAKE_CURRENT_BINARY_DIR}/sdk)
  set(SLOPCAP_MAC_SDK_DYLIBS "")
  set(_asi ${SLOPCAP_MAC_SDK_DIR}/libasi)
  if(SLOPCAP_MAC_SDK_DIR AND EXISTS ${_asi}/ASICamera2.h AND EXISTS ${_asi}/${_mac_sdk_arch}/libASICamera2.bin)
    configure_file(${_asi}/${_mac_sdk_arch}/libASICamera2.bin ${SDK_LIB_DIR}/libASICamera2.dylib COPYONLY)
    target_sources(slopcap_core PRIVATE src/camera/asi_camera.cpp)
    target_include_directories(slopcap_core PRIVATE ${SLOPCAP_MAC_SDK_DIR})   # for <libasi/ASICamera2.h>
    target_link_libraries(slopcap_core PUBLIC ${SDK_LIB_DIR}/libASICamera2.dylib)
    target_compile_definitions(slopcap_core PUBLIC SLOPCAP_HAVE_ASI)
    list(APPEND SLOPCAP_MAC_SDK_DYLIBS ${SDK_LIB_DIR}/libASICamera2.dylib)
    set(SLOPCAP_ASI_HEADERS ${SLOPCAP_MAC_SDK_DIR})   # for the tests' fake SDK (tests/fakes)
  endif()
  set(_qhy ${SLOPCAP_MAC_SDK_DIR}/libqhy)
  if(SLOPCAP_MAC_SDK_DIR AND EXISTS ${_qhy}/qhyccd.h AND EXISTS ${_qhy}/${_mac_sdk_arch}/libqhyccd.bin)
    configure_file(${_qhy}/${_mac_sdk_arch}/libqhyccd.bin ${SDK_LIB_DIR}/libqhyccd.dylib COPYONLY)
    target_sources(slopcap_core PRIVATE src/camera/qhy_camera.cpp)
    target_include_directories(slopcap_core PRIVATE ${QHY_COMPAT} ${_qhy})
    target_link_libraries(slopcap_core PUBLIC ${SDK_LIB_DIR}/libqhyccd.dylib)
    target_compile_definitions(slopcap_core PUBLIC SLOPCAP_HAVE_QHY)
    list(APPEND SLOPCAP_MAC_SDK_DYLIBS ${SDK_LIB_DIR}/libqhyccd.dylib)
    set(SLOPCAP_QHY_HEADERS ${QHY_COMPAT} ${_qhy})
  endif()
  # Player One, ToupTek, MallinCam: one universal dylib each, in a plain mac/ folder (indi-3rdparty's layout).
  # Not yet built on a Mac.
  foreach(sdk "SLOPCAP_HAVE_PLAYERONE;libplayerone/PlayerOneCamera.h;libplayerone/mac/libPlayerOneCamera.bin;libPlayerOneCamera;src/camera/playerone_camera.cpp;POA"
              "SLOPCAP_HAVE_TOUPTEK;libtoupcam/toupcam.h;libtoupcam/mac/libtoupcam.bin;libtoupcam;src/camera/touptek_camera.cpp;TOUPTEK"
              "SLOPCAP_HAVE_MALLINCAM;libmallincam/mallincam.h;libmallincam/mac/libmallincam.bin;libmallincam;src/camera/mallincam_camera.cpp;MALLINCAM")
    list(GET sdk 0 def)
    list(GET sdk 1 header)
    list(GET sdk 2 bin)
    list(GET sdk 3 lib)
    list(GET sdk 4 src)
    list(GET sdk 5 tag)
    if(SLOPCAP_MAC_SDK_DIR AND EXISTS ${SLOPCAP_MAC_SDK_DIR}/${header} AND EXISTS ${SLOPCAP_MAC_SDK_DIR}/${bin})
      configure_file(${SLOPCAP_MAC_SDK_DIR}/${bin} ${SDK_LIB_DIR}/${lib}.dylib COPYONLY)
      target_sources(slopcap_core PRIVATE ${src})
      target_include_directories(slopcap_core PRIVATE ${SLOPCAP_MAC_SDK_DIR})
      target_link_libraries(slopcap_core PUBLIC ${SDK_LIB_DIR}/${lib}.dylib)
      target_compile_definitions(slopcap_core PUBLIC ${def})
      list(APPEND SLOPCAP_MAC_SDK_DYLIBS ${SDK_LIB_DIR}/${lib}.dylib)
      set(SLOPCAP_${tag}_HEADERS ${SLOPCAP_MAC_SDK_DIR})   # for the tests' fake SDKs
    endif()
  endforeach()
  if(SLOPCAP_MAC_SDK_DYLIBS)
    # Both SDKs load @rpath/libusb-1.0.0.dylib. Homebrew's goes next to them, given that install name and re-signed
    # (Apple silicon won't load a dylib whose signature no longer matches).
    find_library(SLOPCAP_LIBUSB NAMES usb-1.0 PATHS /opt/homebrew/opt/libusb/lib /usr/local/opt/libusb/lib)
    if(NOT SLOPCAP_LIBUSB)
      message(FATAL_ERROR "the macOS camera SDKs need libusb: brew install libusb")
    endif()
    get_filename_component(_usb_real ${SLOPCAP_LIBUSB} REALPATH)
    set(_usb ${SDK_LIB_DIR}/libusb-1.0.0.dylib)
    execute_process(COMMAND cp -f ${_usb_real} ${_usb}
                    COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND chmod u+w ${_usb} COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND install_name_tool -id @rpath/libusb-1.0.0.dylib ${_usb} COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND codesign --force --sign - ${_usb} OUTPUT_QUIET ERROR_QUIET COMMAND_ERROR_IS_FATAL ANY)
    # The SDKs' own dylibs too: their ad-hoc signatures don't match their contents, and macOS 27 kills any process
    # that loads them (configure_file puts the originals back whenever they differ, so this runs on every configure).
    foreach(_lib ${SLOPCAP_MAC_SDK_DYLIBS})
      execute_process(COMMAND chmod u+w ${_lib} COMMAND_ERROR_IS_FATAL ANY)
      execute_process(COMMAND codesign --force --sign - ${_lib} OUTPUT_QUIET ERROR_QUIET COMMAND_ERROR_IS_FATAL ANY)
    endforeach()
    list(APPEND SLOPCAP_MAC_SDK_DYLIBS ${_usb})
    set(CMAKE_BUILD_RPATH ${SDK_LIB_DIR})   # tools and tests run from the build folder
  elseif(SLOPCAP_MAC_SDK_DIR AND EXISTS ${SLOPCAP_MAC_SDK_DIR})
    message(WARNING "SLOPCAP_MAC_SDK_DIR=${SLOPCAP_MAC_SDK_DIR} has neither libasi/${_mac_sdk_arch}/libASICamera2.bin "
                    "nor libqhy/${_mac_sdk_arch}/libqhyccd.bin")
  else()
    message(STATUS "macOS camera SDKs: none (tools/macos-sdks.sh fetches them into build/mac-sdk)")
  endif()
else()
  find_path(SLOPCAP_ASI_INCLUDE libasi/ASICamera2.h)
  find_library(SLOPCAP_ASI_LIB ASICamera2)
  if(SLOPCAP_ASI_INCLUDE AND SLOPCAP_ASI_LIB)
    target_sources(slopcap_core PRIVATE src/camera/asi_camera.cpp)
    target_include_directories(slopcap_core PRIVATE ${SLOPCAP_ASI_INCLUDE})
    target_link_libraries(slopcap_core PUBLIC ${SLOPCAP_ASI_LIB})
    target_compile_definitions(slopcap_core PUBLIC SLOPCAP_HAVE_ASI)
    set(SLOPCAP_ASI_HEADERS ${SLOPCAP_ASI_INCLUDE})
  endif()
  find_path(SLOPCAP_QHY_INCLUDE qhyccd.h PATHS /usr/include/libqhy NO_DEFAULT_PATH)
  find_library(SLOPCAP_QHY_LIB qhyccd PATHS /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu NO_DEFAULT_PATH)
  if(SLOPCAP_QHY_INCLUDE AND SLOPCAP_QHY_LIB)
    target_sources(slopcap_core PRIVATE src/camera/qhy_camera.cpp)
    target_include_directories(slopcap_core PRIVATE ${QHY_COMPAT} ${SLOPCAP_QHY_INCLUDE})
    target_link_libraries(slopcap_core PUBLIC ${SLOPCAP_QHY_LIB})
    target_compile_definitions(slopcap_core PUBLIC SLOPCAP_HAVE_QHY)
    set(SLOPCAP_QHY_HEADERS ${QHY_COMPAT} ${SLOPCAP_QHY_INCLUDE})
    # pin the packaged libqhyccd (a manual /usr/local install may otherwise be loaded first)
    get_filename_component(SLOPCAP_QHY_LIBDIR ${SLOPCAP_QHY_LIB} DIRECTORY)
    set(CMAKE_BUILD_RPATH ${SLOPCAP_QHY_LIBDIR})
    add_link_options(-Wl,--enable-new-dtags)
  endif()
  # Player One, ToupTek, MallinCam: the INDI PPA's libplayerone, libtoupcam, libmallincam
  foreach(sdk "SLOPCAP_HAVE_PLAYERONE;libplayerone/PlayerOneCamera.h;PlayerOneCamera;src/camera/playerone_camera.cpp;POA"
              "SLOPCAP_HAVE_TOUPTEK;libtoupcam/toupcam.h;toupcam;src/camera/touptek_camera.cpp;TOUPTEK"
              "SLOPCAP_HAVE_MALLINCAM;libmallincam/mallincam.h;mallincam;src/camera/mallincam_camera.cpp;MALLINCAM")
    list(GET sdk 0 def)
    list(GET sdk 1 header)
    list(GET sdk 2 lib)
    list(GET sdk 3 src)
    list(GET sdk 4 tag)
    find_path(SLOPCAP_${tag}_INCLUDE ${header})
    find_library(SLOPCAP_${tag}_LIB ${lib})
    if(SLOPCAP_${tag}_INCLUDE AND SLOPCAP_${tag}_LIB)
      target_sources(slopcap_core PRIVATE ${src})
      target_include_directories(slopcap_core PRIVATE ${SLOPCAP_${tag}_INCLUDE})
      target_link_libraries(slopcap_core PUBLIC ${SLOPCAP_${tag}_LIB})
      target_compile_definitions(slopcap_core PUBLIC ${def})
      set(SLOPCAP_${tag}_HEADERS ${SLOPCAP_${tag}_INCLUDE})   # for the tests' fake SDKs
    endif()
  endforeach()
endif()
get_target_property(_defs slopcap_core INTERFACE_COMPILE_DEFINITIONS)
if(NOT _defs)
  set(_defs "none (no camera SDK found: replays and remote viewing only)")
endif()
message(STATUS "SlopCap camera backends: ${_defs}")

# ---------------------------------------------------------------- tools
add_executable(slopcap-serinfo tools/serinfo/main.cpp)
target_link_libraries(slopcap-serinfo PRIVATE slopcap_core slopcap_warnings)
add_executable(slopcap-focusbench tools/focusbench/main.cpp)
target_link_libraries(slopcap-focusbench PRIVATE slopcap_core slopcap_warnings)
add_executable(slopcap-bench tools/bench/main.cpp)
target_link_libraries(slopcap-bench PRIVATE slopcap_core slopcap_warnings)

if(SLOPCAP_BUILD_CAMPROBE)
  if(WIN32 AND NOT WIN_SDK_DLL_DIR)
    message(STATUS "camprobe: skipped on Windows unless -DWIN_SDK_DLL_DIR=<folder with ASICamera2.dll, qhyccd.dll>")
  else()
    add_subdirectory(tools/camprobe)
  endif()
endif()

# ---------------------------------------------------------------- GUI (Qt 6, OpenGL 3.3)
if(SLOPCAP_BUILD_GUI)
  find_package(Qt6 6.4 QUIET COMPONENTS Widgets OpenGL OpenGLWidgets Network OPTIONAL_COMPONENTS Test)
  if(Qt6_FOUND)
    # SlopCap's own mount drivers (docs/design/mounts.md): LX200 / OnStep, NexStar / SynScan, Sky-Watcher's motor
    # controller, the SynScan app, a simulator; serial (own Win32 / termios class), TCP and UDP. Qt Core + Network.
    add_library(slopcap_mounts STATIC src/mounts/transport.cpp src/mounts/command_channel.cpp src/mounts/lx200.cpp
      src/mounts/nexstar.cpp src/mounts/skywatcher.cpp src/mounts/synscan_app.cpp src/mounts/sim_mount.cpp
      src/mounts/mount_worker.cpp src/mounts/open_mount.cpp)
    set_target_properties(slopcap_mounts PROPERTIES AUTOMOC ON)
    target_link_libraries(slopcap_mounts PUBLIC slopcap_core Qt6::Core Qt6::Network PRIVATE slopcap_warnings)
    if(WIN32)
      target_link_libraries(slopcap_mounts PRIVATE advapi32)   # the COM port list, from the registry
    endif()
    # Devices (focuser and mount over INDI, the mount also through slopcap_mounts). Qt Core + Network only.
    add_library(slopcap_devices STATIC src/devices/indi_client.cpp src/devices/focuser.cpp src/devices/mount.cpp)
    set_target_properties(slopcap_devices PROPERTIES AUTOMOC ON)
    target_include_directories(slopcap_devices PUBLIC src)
    target_link_libraries(slopcap_devices PUBLIC slopcap_mounts Qt6::Core Qt6::Network PRIVATE slopcap_warnings)
    # Remote viewing: one SlopCap shares its camera, others watch and steer it (src/net/protocol.hpp).
    add_library(slopcap_net STATIC src/net/protocol.cpp src/net/frame_codec.cpp src/net/discovery.cpp src/net/remote_server.cpp
      src/net/remote_client.cpp src/net/rate_control.cpp)
    set_target_properties(slopcap_net PROPERTIES AUTOMOC ON)
    target_link_libraries(slopcap_net PUBLIC slopcap_core Qt6::Gui Qt6::Network PRIVATE slopcap_warnings)
    add_library(slopcap_gui STATIC
      src/preview/gl_renderer.cpp
      src/preview/preview_widget.cpp
      src/preview/histogram_widget.cpp
    )
    set_target_properties(slopcap_gui PROPERTIES AUTOMOC ON)
    target_link_libraries(slopcap_gui PUBLIC slopcap_core slopcap_devices Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets PRIVATE slopcap_warnings)
    add_executable(slopcap-diag tools/diag/main.cpp)
    target_link_libraries(slopcap-diag PRIVATE slopcap_core Qt6::Gui slopcap_warnings)
    # The application's windows and panels: a library, so the tests drive the same code the program runs.
    add_library(slopcap_app STATIC src/app/main_window.cpp src/app/theme.cpp src/app/focus_controller.cpp
      src/app/focus_views.cpp src/app/collimation_panel.cpp src/app/settings_dialog.cpp src/app/mount_section.cpp
      src/app/raw_histogram.cpp src/app/session_log.cpp src/app/full_view_bar.cpp src/app/adc_panel.cpp src/app/live_inset.cpp)
    set_target_properties(slopcap_app PROPERTIES AUTOMOC ON)
    target_link_libraries(slopcap_app PUBLIC slopcap_gui slopcap_net PRIVATE slopcap_warnings)
    # qt_add_executable: with a static Qt it also links the platform plugin (qwindows) into the exe
    qt_add_executable(slopcap WIN32 MACOSX_BUNDLE src/app/main.cpp)
    set_target_properties(slopcap PROPERTIES AUTOMOC ON)
    # The build's git version, for the session log and the remote-viewing handshake: SLOPCAP_GIT from the environment
    # when set (docker/build.sh asks git on the host: the Windows image has no git, and a worktree's .git points at a
    # host path), else git here ("unknown" outside a checkout, or when git refuses one it doesn't own).
    if(NOT "$ENV{SLOPCAP_GIT}" STREQUAL "")
      set(SLOPCAP_GIT "$ENV{SLOPCAP_GIT}")
    else()
      execute_process(COMMAND git -c safe.directory=* describe --always --dirty WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                      OUTPUT_VARIABLE SLOPCAP_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(NOT SLOPCAP_GIT)
      set(SLOPCAP_GIT unknown)
    endif()
    target_compile_definitions(slopcap PRIVATE SLOPCAP_GIT="${SLOPCAP_GIT}")
    # macOS: build/<preset>/SlopCap.app. tools/macos-app.sh makes it self-contained (Qt inside) and signs it.
    if(APPLE)
      set_target_properties(slopcap PROPERTIES
        OUTPUT_NAME SlopCap
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER org.slopcap.SlopCap
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_INFO_STRING "SlopCap ${SLOPCAP_GIT}")
      # The camera SDKs ride inside the bundle, found through @rpath.
      if(SLOPCAP_MAC_SDK_DYLIBS)
        target_sources(slopcap PRIVATE ${SLOPCAP_MAC_SDK_DYLIBS})
        set_source_files_properties(${SLOPCAP_MAC_SDK_DYLIBS} PROPERTIES MACOSX_PACKAGE_LOCATION Frameworks)
        set_property(TARGET slopcap APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
      endif()
    endif()
    # The UI fonts (IBM Plex, OFL), embedded when the build machine has them, so every install looks the same.
    set(SLOPCAP_FONT_DIR /usr/share/fonts/truetype/ibm-plex CACHE PATH "folder with the IBM Plex TTFs to embed")
    set(font_files "")
    foreach(f IBMPlexSans-Regular IBMPlexSans-Medium IBMPlexSans-SemiBold IBMPlexMono-Regular IBMPlexMono-Medium IBMPlexMono-SemiBold)
      if(EXISTS ${SLOPCAP_FONT_DIR}/${f}.ttf)
        set_source_files_properties(${SLOPCAP_FONT_DIR}/${f}.ttf PROPERTIES QT_RESOURCE_ALIAS ${f}.ttf)
        list(APPEND font_files ${SLOPCAP_FONT_DIR}/${f}.ttf)
      endif()
    endforeach()
    if(font_files)
      qt_add_resources(slopcap fonts PREFIX /fonts FILES ${font_files})
      message(STATUS "GUI: embedding IBM Plex fonts from ${SLOPCAP_FONT_DIR}")
    endif()
    # One-file Windows build: the camera SDK DLLs ride inside the exe and are extracted on first use.
    if(WIN32 AND SLOPCAP_SINGLE_EXE AND WIN_SDK_DLL_DIR)
      set(sdk_files "")
      foreach(f ASICamera2.dll qhyccd.dll PlayerOneCamera.dll toupcam.dll mallincam.dll)
        if(EXISTS ${WIN_SDK_DLL_DIR}/${f})
          set_source_files_properties(${WIN_SDK_DLL_DIR}/${f} PROPERTIES QT_RESOURCE_ALIAS ${f})
          list(APPEND sdk_files ${WIN_SDK_DLL_DIR}/${f})
        endif()
      endforeach()
      qt_add_resources(slopcap sdk PREFIX /sdk BIG_RESOURCES FILES ${sdk_files})
      target_compile_definitions(slopcap PRIVATE SLOPCAP_EMBEDDED_SDK)
    endif()
    target_link_libraries(slopcap PRIVATE slopcap_app slopcap_warnings)
    # The remote stream's JPEG codec: make sure a static Qt links the plugin.
    if(TARGET Qt6::QJpegPlugin)
      get_target_property(_qt_core_type Qt6::Core TYPE)
      if(_qt_core_type STREQUAL "STATIC_LIBRARY")
        qt_import_plugins(slopcap INCLUDE Qt6::QJpegPlugin)
      endif()
    endif()
  else()
    message(STATUS "Qt 6 (Widgets, OpenGL, OpenGLWidgets) not found: GUI not built (Linux: apt install qt6-base-dev)")
  endif()
endif()

# ---------------------------------------------------------------- tests
if(SLOPCAP_BUILD_TESTS)
  find_package(Catch2 3 CONFIG QUIET)
  if(Catch2_FOUND)
    enable_testing()
    add_executable(slopcap-tests tests/test_ser.cpp tests/test_sim_camera.cpp tests/test_pipeline.cpp tests/test_direct_writer.cpp tests/test_preview.cpp tests/test_focus.cpp tests/test_sweep.cpp tests/test_collimation.cpp
      tests/test_identify.cpp tests/test_dispersion.cpp)
    target_link_libraries(slopcap-tests PRIVATE slopcap_core slopcap_warnings Catch2::Catch2WithMain)
    include(Catch)
    catch_discover_tests(slopcap-tests)
    # Needs an OpenGL 3.3 context: a desktop session, or Xvfb + Mesa (docker/build.sh starts Xvfb). Not under TSan:
    # the test is single-threaded, and uninstrumented Qt / X11 / Mesa only produce false reports there.
    if(TARGET slopcap_gui AND NOT CMAKE_CXX_FLAGS MATCHES "sanitize=thread")
      add_executable(slopcap-gl-tests tests/test_gl_render.cpp)
      target_link_libraries(slopcap-gl-tests PRIVATE slopcap_gui slopcap_warnings Catch2::Catch2)
      add_test(NAME gl-render COMMAND slopcap-gl-tests)
      add_executable(slopcap-qt-tests tests/test_indi.cpp)
      target_link_libraries(slopcap-qt-tests PRIVATE slopcap_devices slopcap_warnings Catch2::Catch2)
      add_test(NAME indi-devices COMMAND slopcap-qt-tests)
      # The built-in mount drivers against fakes that speak each protocol (tests/fakes/fake_mounts.*) over memory,
      # a pseudo-terminal, TCP and UDP, and the Mount they sit behind.
      add_executable(slopcap-mount-tests tests/test_mounts.cpp tests/fakes/fake_mounts.cpp)
      target_link_libraries(slopcap-mount-tests PRIVATE slopcap_devices slopcap_mounts slopcap_warnings Catch2::Catch2)
      target_include_directories(slopcap-mount-tests PRIVATE tests)
      add_test(NAME mounts COMMAND slopcap-mount-tests)
      add_executable(slopcap-net-tests tests/test_remote.cpp)
      target_link_libraries(slopcap-net-tests PRIVATE slopcap_net slopcap_warnings Catch2::Catch2)
      add_test(NAME remote COMMAND slopcap-net-tests)
      # The application: windows, panels, focus sweeps, remote viewing, the program itself (tests/test_app*.cpp), plus the
      # ZWO / QHY backends against a fake of each SDK (tests/fakes: the fakes' C functions take the SDK's place in this
      # executable), all under a real QApplication; needs a screen (Xvfb in docker/build.sh).
      if(TARGET Qt6::Test)
        add_executable(slopcap-app-tests tests/app_main.cpp tests/app_harness.cpp tests/test_app_window.cpp tests/test_app_focus.cpp
          tests/test_app_panels.cpp tests/test_app_remote.cpp tests/test_app_program.cpp tests/test_app_adc.cpp)
        target_link_libraries(slopcap-app-tests PRIVATE slopcap_app slopcap_warnings Catch2::Catch2 Qt6::Test)
        target_compile_definitions(slopcap-app-tests PRIVATE SLOPCAP_PROGRAM="$<TARGET_FILE:slopcap>")
        add_dependencies(slopcap-app-tests slopcap)
        get_target_property(_core_defs slopcap_core INTERFACE_COMPILE_DEFINITIONS)
        if(_core_defs MATCHES "SLOPCAP_HAVE_ASI" AND NOT WIN32)   # Windows: dllimport declarations, no fakes
          target_sources(slopcap-app-tests PRIVATE tests/fakes/fake_asi.cpp tests/test_camera_asi.cpp)
          target_include_directories(slopcap-app-tests PRIVATE ${SLOPCAP_ASI_HEADERS})
          target_compile_definitions(slopcap-app-tests PRIVATE SLOPCAP_FAKE_ASI)
        endif()
        if(_core_defs MATCHES "SLOPCAP_HAVE_QHY" AND NOT WIN32)
          target_sources(slopcap-app-tests PRIVATE tests/fakes/fake_qhy.cpp tests/test_camera_qhy.cpp)
          target_include_directories(slopcap-app-tests PRIVATE ${SLOPCAP_QHY_HEADERS})
          target_compile_definitions(slopcap-app-tests PRIVATE SLOPCAP_FAKE_QHY)
        endif()
        if(_core_defs MATCHES "SLOPCAP_HAVE_PLAYERONE" AND NOT WIN32)
          target_sources(slopcap-app-tests PRIVATE tests/fakes/fake_playerone.cpp tests/test_camera_playerone.cpp)
          target_include_directories(slopcap-app-tests PRIVATE ${SLOPCAP_POA_HEADERS})
        endif()
        # ToupTek's fake also holds the state MallinCam's uses, so MallinCam's tests come only with it
        if(_core_defs MATCHES "SLOPCAP_HAVE_TOUPTEK" AND _core_defs MATCHES "SLOPCAP_HAVE_MALLINCAM" AND NOT WIN32)
          target_sources(slopcap-app-tests PRIVATE tests/fakes/fake_touptek.cpp tests/fakes/fake_mallincam.cpp tests/test_camera_touptek.cpp)
          target_include_directories(slopcap-app-tests PRIVATE ${SLOPCAP_TOUPTEK_HEADERS} ${SLOPCAP_MALLINCAM_HEADERS})
        endif()
        target_include_directories(slopcap-app-tests PRIVATE tests)
        add_test(NAME app COMMAND slopcap-app-tests)
        set_tests_properties(app PROPERTIES SKIP_RETURN_CODE 4 TIMEOUT 900 ENVIRONMENT
          "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/sanitizers/lsan-gl.supp")
      endif()
      set_tests_properties(indi-devices PROPERTIES SKIP_RETURN_CODE 4)   # Catch2's "all skipped" (no INDI installed)
      set_tests_properties(gl-render PROPERTIES SKIP_RETURN_CODE 4 ENVIRONMENT   # 4: no screen at all (macOS over SSH)
        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/sanitizers/lsan-gl.supp")
    endif()
  else()
    message(WARNING "Catch2 3 not found: tests disabled (Linux: apt install catch2; Windows: vcpkg install catch2)")
  endif()
endif()

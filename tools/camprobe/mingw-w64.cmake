# Cross-compile slopcap-camprobe for Windows x64 from Linux with MinGW-w64 (posix threads variant).
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=mingw-w64.cmake -DWIN_SDK_DLL_DIR=build-win/sdk
# WIN_SDK_DLL_DIR holds ASICamera2.dll and qhyccd.dll copied from the target machine (e.g. SharpCap's folder);
# MinGW links against the DLLs directly. The SDK headers are the Linux packages' (same C API).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# one self-contained exe: no libstdc++/libgcc/winpthread DLLs to ship
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")

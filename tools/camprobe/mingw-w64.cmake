# Cross-compile SlopCap for Windows x64 from Linux with MinGW-w64 (posix threads), on Ubuntu (g++-mingw-w64-x86-64-posix)
# or Fedora (mingw64-gcc-c++, posix threads by default; used by the static single-exe build, docker/Dockerfile.mingw).
#   cmake --preset windows-cross     (command-line tools)      cmake --preset windows-static     (slopcap.exe)
# WIN_SDK_DLL_DIR holds ASICamera2.dll and qhyccd.dll copied from a Windows machine (e.g. SharpCap's folder); the SDK
# headers are the Linux packages' (same C API).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
find_program(_mingw_gcc NAMES x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-gcc REQUIRED)
find_program(_mingw_gxx NAMES x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++ REQUIRED)
set(CMAKE_C_COMPILER ${_mingw_gcc})
set(CMAKE_CXX_COMPILER ${_mingw_gxx})
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
# Ubuntu's sysroot, Fedora's, and the static Qt built in the Windows image
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 /usr/x86_64-w64-mingw32/sys-root/mingw /opt/qt6-mingw-static)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# self-contained executables: no libstdc++ / libgcc / winpthread DLLs to ship
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
# release executables without debug info (MinGW's static runtime and Qt otherwise add ~40 MB of it)
set(CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT "-s")

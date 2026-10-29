// The Win32 calls the serial transport makes on Windows, replaceable so the tests can play a COM port: without
// hardware or a virtual-port driver there is none to test the Windows serial code with (tests/test_mounts.cpp).
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace slopcap::mounts {

struct SerialApi {
    HANDLE (*open)(const char* path);   ///< CreateFileA for reading and writing, exclusive, the device must exist
    BOOL (*getState)(HANDLE, DCB*);
    BOOL (*setState)(HANDLE, DCB*);
    BOOL (*setTimeouts)(HANDLE, COMMTIMEOUTS*);
    BOOL (*purge)(HANDLE, DWORD);
    BOOL (*write)(HANDLE, const void* data, DWORD size, DWORD* written);
    BOOL (*read)(HANDLE, void* data, DWORD size, DWORD* got);
    BOOL (*close)(HANDLE);
};

/// Use `api` for serial ports opened from now on; nullptr: the real Win32 calls.
void setSerialApi(const SerialApi* api);

}  // namespace slopcap::mounts
#endif

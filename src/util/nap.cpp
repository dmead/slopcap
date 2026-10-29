#include "util/nap.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <thread>
#endif

namespace slopcap {

#ifdef _WIN32

namespace {

// One timer per thread.
struct Timer {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    Timer() {
        if (!h) h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);   // before Windows 10 1803
    }
    ~Timer() {
        if (h) CloseHandle(h);
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
};

}  // namespace

void nap(std::chrono::microseconds d) {
    if (d <= std::chrono::microseconds::zero()) return;
    thread_local Timer t;
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(d.count()) * 10;   // relative, in 100 ns units
    if (t.h && SetWaitableTimer(t.h, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(t.h, INFINITE);
    else
        Sleep(static_cast<DWORD>((d.count() + 999) / 1000));
}

#else

void nap(std::chrono::microseconds d) {
    if (d > std::chrono::microseconds::zero()) std::this_thread::sleep_for(d);
}

#endif

}  // namespace slopcap

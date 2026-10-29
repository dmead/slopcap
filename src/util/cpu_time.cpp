#include "util/cpu_time.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace slopcap {

#ifdef _WIN32

namespace {
std::chrono::nanoseconds sum(FILETIME kernel, FILETIME user) {
    auto ticks = [](FILETIME t) { return (static_cast<long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return std::chrono::nanoseconds((ticks(kernel) + ticks(user)) * 100);
}
}  // namespace

std::chrono::nanoseconds processCpuTime() {
    FILETIME c, e, k, u;
    return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? sum(k, u) : std::chrono::nanoseconds::zero();
}

std::chrono::nanoseconds threadCpuTime() {
    FILETIME c, e, k, u;
    return GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u) ? sum(k, u) : std::chrono::nanoseconds::zero();
}

#else

namespace {
std::chrono::nanoseconds clockTime(clockid_t id) {
    timespec ts{};
    if (clock_gettime(id, &ts) != 0) return std::chrono::nanoseconds::zero();
    return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
}
}  // namespace

std::chrono::nanoseconds processCpuTime() { return clockTime(CLOCK_PROCESS_CPUTIME_ID); }
std::chrono::nanoseconds threadCpuTime() { return clockTime(CLOCK_THREAD_CPUTIME_ID); }

#endif

void CpuMeter::start() {
    cpu0_ = processCpuTime();
    wall0_ = std::chrono::steady_clock::now();
}

double CpuMeter::cores() const {
    const auto wall = std::chrono::steady_clock::now() - wall0_;
    return wall > wall.zero() ? std::chrono::duration<double>(processCpuTime() - cpu0_) / wall : 0.0;
}

}  // namespace slopcap

// CPU time used so far (user + kernel): the session log reports the program's CPU use from it, and the tests check
// that waiting code sleeps instead of spinning.
#pragma once

#include <chrono>

namespace slopcap {

/// The whole process, all threads.
std::chrono::nanoseconds processCpuTime();
/// The calling thread. Windows counts both in scheduler ticks (~16 ms): compare them with tens of milliseconds.
std::chrono::nanoseconds threadCpuTime();

/// The process's average CPU use since start(), in cores (1.0 = one core kept busy).
class CpuMeter {
public:
    void start();
    double cores() const;

private:
    std::chrono::nanoseconds cpu0_ = processCpuTime();
    std::chrono::steady_clock::time_point wall0_ = std::chrono::steady_clock::now();
};

}  // namespace slopcap

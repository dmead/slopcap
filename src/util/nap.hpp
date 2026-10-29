// Short sleeps that really sleep. std::this_thread::sleep_for can't be trusted for them: built with mingw, on Windows
// anything under 1 ms returns at once (a polling loop around it spins a whole core) and 1 ms takes ~16 ms.
#pragma once

#include <chrono>

namespace slopcap {

/// Sleep for about `d`. Windows: a high-resolution waitable timer (well under a millisecond's error; plain timer
/// before Windows 10 1803). Elsewhere: std::this_thread::sleep_for.
void nap(std::chrono::microseconds d);

}  // namespace slopcap

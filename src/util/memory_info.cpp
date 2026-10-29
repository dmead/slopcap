#include "util/memory_info.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <unistd.h>

#include <fstream>
#include <string>
#endif

namespace slopcap {

#ifdef _WIN32

namespace {
std::optional<MEMORYSTATUSEX> status() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (!GlobalMemoryStatusEx(&m)) return std::nullopt;
    return m;
}
}  // namespace

std::optional<uint64_t> totalRam() {
    const auto m = status();
    return m ? std::optional<uint64_t>(m->ullTotalPhys) : std::nullopt;
}

std::optional<uint64_t> availableRam() {
    const auto m = status();
    return m ? std::optional<uint64_t>(m->ullAvailPhys) : std::nullopt;
}

#elif defined(__APPLE__)

std::optional<uint64_t> totalRam() {
    uint64_t bytes = 0;
    size_t size = sizeof bytes;
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0 || bytes == 0) return std::nullopt;
    return bytes;
}

std::optional<uint64_t> availableRam() {
    // free plus inactive pages: what macOS hands out before it compresses or pages
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) != KERN_SUCCESS) return std::nullopt;
    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS || page == 0) return std::nullopt;
    return (static_cast<uint64_t>(vm.free_count) + vm.inactive_count) * page;
}

#else

std::optional<uint64_t> totalRam() {
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page <= 0) return std::nullopt;
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page);
}

std::optional<uint64_t> availableRam() {
    // MemAvailable: the kernel's own estimate, page cache it can drop included (Linux 3.14+)
    std::ifstream f("/proc/meminfo");
    for (std::string line; std::getline(f, line);)
        if (line.starts_with("MemAvailable:")) return std::stoull(line.substr(13)) * 1024;   // "MemAvailable:  123456 kB"
    return std::nullopt;
}

#endif

}  // namespace slopcap

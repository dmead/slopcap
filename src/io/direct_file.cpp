#include "io/direct_file.hpp"

#include <algorithm>
#include <cerrno>
#include <format>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace slopcap {

namespace {
void requireAligned(const void* p, std::size_t n, uint64_t off) {
    if (reinterpret_cast<std::uintptr_t>(p) % DirectFile::kAlign || n % DirectFile::kAlign || off % DirectFile::kAlign)
        throw std::invalid_argument("direct I/O requires 4096-byte aligned buffer, length and offset");
}
}  // namespace

#ifdef _WIN32

DirectFile::DirectFile(const std::filesystem::path& path) {
    h_ = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) {
        h_ = nullptr;
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "direct open " + path.string());
    }
}

DirectFile::~DirectFile() { close(); }

void DirectFile::close() {
    if (h_) CloseHandle(h_);
    h_ = nullptr;
}

void DirectFile::writeAt(std::span<const std::byte> data, uint64_t offset) {
    requireAligned(data.data(), data.size(), offset);
    std::size_t done = 0;
    while (done < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, std::size_t{1} << 30));
        OVERLAPPED ov{};
        const uint64_t at = offset + done;
        ov.Offset = static_cast<DWORD>(at & 0xffffffffu);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateEvent");
        DWORD written = 0;
        BOOL ok = WriteFile(h_, data.data() + done, chunk, nullptr, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            const DWORD e = GetLastError();
            CloseHandle(ov.hEvent);
            throw std::system_error(static_cast<int>(e), std::system_category(), "direct write");
        }
        ok = GetOverlappedResult(h_, &ov, &written, TRUE);
        const DWORD e = GetLastError();
        CloseHandle(ov.hEvent);
        if (!ok || written == 0) throw std::system_error(static_cast<int>(e), std::system_category(), "direct write result");
        done += written;
    }
}

bool DirectFile::reserve(uint64_t bytes) {
    FILE_ALLOCATION_INFO info{};
    info.AllocationSize.QuadPart = static_cast<LONGLONG>(bytes);
    return SetFileInformationByHandle(h_, FileAllocationInfo, &info, sizeof info) != 0;
}

void DirectFile::truncate(uint64_t size) {
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFileInformationByHandle(h_, FileEndOfFileInfo, &info, sizeof info))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "truncate");
}

#else

DirectFile::DirectFile(const std::filesystem::path& path) {
#ifdef __APPLE__
    // macOS has no O_DIRECT: F_NOCACHE on the open file keeps its writes out of the unified buffer cache.
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd_ >= 0 && ::fcntl(fd_, F_NOCACHE, 1) != 0) {
        const int e = errno;
        ::close(fd_);
        throw std::system_error(e, std::generic_category(), "direct open (F_NOCACHE) " + path.string());
    }
#else
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT | O_CLOEXEC, 0644);
#endif
    if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "direct open " + path.string());
}

DirectFile::~DirectFile() { close(); }

void DirectFile::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

void DirectFile::writeAt(std::span<const std::byte> data, uint64_t offset) {
    requireAligned(data.data(), data.size(), offset);
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::pwrite(fd_, data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "direct write");
        }
        if (n == 0) throw std::runtime_error("direct write: no progress");
        done += static_cast<std::size_t>(n);
    }
}

bool DirectFile::reserve(uint64_t bytes) {
#if defined(FALLOC_FL_KEEP_SIZE)
    return ::fallocate(fd_, FALLOC_FL_KEEP_SIZE, 0, static_cast<off_t>(bytes)) == 0;
#elif defined(F_PREALLOCATE)
    // Allocated past the physical end of file; the visible size stays put. Contiguous if the disk can, else any.
    fstore_t st{F_ALLOCATECONTIG | F_ALLOCATEALL, F_PEOFPOSMODE, 0, static_cast<off_t>(bytes), 0};
    if (::fcntl(fd_, F_PREALLOCATE, &st) == 0) return true;
    st.fst_flags = F_ALLOCATEALL;
    return ::fcntl(fd_, F_PREALLOCATE, &st) == 0;
#else
    (void)bytes;
    return false;
#endif
}

void DirectFile::truncate(uint64_t size) {
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) throw std::system_error(errno, std::generic_category(), "truncate");
}

#endif

}  // namespace slopcap

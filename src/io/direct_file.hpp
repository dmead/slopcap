// A file opened for unbuffered ("direct") I/O: writes bypass the OS page cache.
// Linux: O_DIRECT. Windows: FILE_FLAG_NO_BUFFERING, opened overlapped so several threads
// can have writes in flight on one handle.
// Every write must have its buffer address, length and file offset aligned to kAlign (4096 covers 512- and
// 4096-byte-sector disks). writeAt() may be called from several threads at once for different offsets.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace slopcap {

class DirectFile {
public:
    static constexpr std::size_t kAlign = 4096;

    /// Create/truncate `path`. Throws std::system_error (e.g. the filesystem doesn't support direct I/O).
    explicit DirectFile(const std::filesystem::path& path);
    ~DirectFile();
    DirectFile(const DirectFile&) = delete;
    DirectFile& operator=(const DirectFile&) = delete;

    /// Write all of `data` at `offset` (both aligned). Blocks until done; thread-safe for distinct ranges.
    void writeAt(std::span<const std::byte> data, uint64_t offset);
    /// Reserve disk space without changing the visible file size (best effort; returns false if unsupported).
    bool reserve(uint64_t bytes);
    /// Set the file's size exactly (any value; used to trim padding at the end).
    void truncate(uint64_t size);
    void close();

private:
#ifdef _WIN32
    void* h_ = nullptr;
#else
    int fd_ = -1;
#endif
};

}  // namespace slopcap

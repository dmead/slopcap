// SER writer for high, steady throughput: unbuffered (direct) I/O, frames packed into large aligned chunks, several
// chunk writes in flight from a small pool of I/O threads.
//
// Why: buffered writes copy every frame into the OS page cache and the OS flushes it whenever it likes, which shows up
// as occasional multi-millisecond stalls. Direct I/O writes straight from our chunks to the disk.
// The 178-byte SER header would misalign every frame for direct I/O, so the file is treated as one byte stream
// (header, frames, timestamp trailer) cut into 4096-aligned chunks; the end is padded and the file trimmed to size.
// Crash safety: the first 4096 bytes (header + start of frame 0) are rewritten every couple of seconds with the
// current frame count, so an interrupted recording still opens with the right count.
#pragma once

#include "io/direct_file.hpp"
#include "io/ser.hpp"
#include "pipeline/latency.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace slopcap::ser {

struct DirectWriterOptions {
    std::size_t chunkBytes = std::size_t{8} << 20;   ///< rounded up to 4096
    int inflight = 8;                                ///< chunk buffers / concurrent writes
    uint64_t reserveBytes = 0;                       ///< preallocate this much disk space up front (0 = don't)
    std::chrono::milliseconds headerEvery{2000};     ///< how often the frame count in the header is refreshed
};

class DirectWriter final : public FrameSink {
public:
    DirectWriter(const std::filesystem::path& path, Header header, DirectWriterOptions options = {});
    ~DirectWriter() override;
    DirectWriter(const DirectWriter&) = delete;
    DirectWriter& operator=(const DirectWriter&) = delete;

    void writeFrame(std::span<const std::byte> frame, int64_t utcTicks) override;
    void finish() override;
    int64_t framesWritten() const override { return static_cast<int64_t>(timestamps_.size()); }

    /// Rewrite the header with the current frame count now (normally done every headerEvery).
    void refreshHeader();

    const LatencyHistogram& chunkWriteLatency() const { return chunkLatency_; }   ///< one chunk to disk
    const LatencyHistogram& stallLatency() const { return stallLatency_; }        ///< waiting for a free chunk
    uint64_t chunksWritten() const { return chunksDone_; }

private:
    struct Chunk {
        std::byte* data = nullptr;
        std::size_t used = 0;
        uint64_t offset = 0;
    };

    void append(std::span<const std::byte> bytes);
    void submitCurrent();
    void nextChunk();
    void worker();
    void rethrowWorkerError();

    std::filesystem::path path_;
    DirectFile file_;
    Header header_;
    DirectWriterOptions opt_;
    std::byte* memory_ = nullptr;
    std::vector<Chunk> chunks_;
    std::byte* head_ = nullptr;   ///< copy of the file's first 4096 bytes, for header refreshes

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::size_t> queue_;        ///< chunks waiting for a worker
    std::vector<std::size_t> free_;        ///< chunks available to fill
    std::size_t current_ = 0;              ///< chunk being filled
    uint64_t nextOffset_ = 0;              ///< file offset of the next chunk
    uint64_t streamBytes_ = 0;             ///< logical bytes so far (header + frames [+ trailer])
    bool headCaptured_ = false;
    bool headOnDisk_ = false;              ///< the chunk at offset 0 has been written
    bool stopping_ = false;
    std::string workerError_;
    uint64_t chunksDone_ = 0;
    std::vector<std::thread> workers_;

    std::vector<int64_t> timestamps_;
    std::chrono::steady_clock::time_point lastHeader_;
    bool finished_ = false;
    LatencyHistogram chunkLatency_, stallLatency_;
};

}  // namespace slopcap::ser

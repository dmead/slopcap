#include "io/ser_direct.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <new>

namespace slopcap::ser {

namespace {
constexpr std::size_t kAlign = DirectFile::kAlign;
std::size_t roundUp(std::size_t n) { return (n + kAlign - 1) / kAlign * kAlign; }
}  // namespace

DirectWriter::DirectWriter(const std::filesystem::path& path, Header header, DirectWriterOptions options)
    : path_(path), file_(path), header_(std::move(header)), opt_(options) {
    opt_.chunkBytes = roundUp(std::max<std::size_t>(opt_.chunkBytes, kAlign));
    opt_.inflight = std::clamp(opt_.inflight, 1, 64);
    if (opt_.reserveBytes) file_.reserve(opt_.reserveBytes);

    const std::size_t total = opt_.chunkBytes * static_cast<std::size_t>(opt_.inflight) + kAlign;
    memory_ = static_cast<std::byte*>(::operator new(total, std::align_val_t{kAlign}));
    std::memset(memory_, 0, total);   // commit the pages now, not mid-recording
    head_ = memory_ + opt_.chunkBytes * static_cast<std::size_t>(opt_.inflight);
    for (int i = 0; i < opt_.inflight; ++i) {
        chunks_.push_back({memory_ + opt_.chunkBytes * static_cast<std::size_t>(i), 0, 0});
        if (i > 0) free_.push_back(static_cast<std::size_t>(i));
    }
    current_ = 0;
    chunks_[0].offset = 0;
    nextOffset_ = opt_.chunkBytes;

    header_.frameCount = 0;
    header_.littleEndian = 0;
    const auto raw = header_.encode();
    append(raw);

    for (int i = 0; i < opt_.inflight; ++i) workers_.emplace_back([this] { worker(); });
    lastHeader_ = std::chrono::steady_clock::now();
}

DirectWriter::~DirectWriter() {
    try {
        finish();
    } catch (...) {
        // destructor must not throw; the periodically refreshed header keeps the file readable
    }
    {
        std::lock_guard lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_)
        if (w.joinable()) w.join();
    ::operator delete(memory_, std::align_val_t{kAlign});
}

void DirectWriter::worker() {
    for (;;) {
        std::size_t idx;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;   // stopping and nothing left
            idx = queue_.front();
            queue_.pop_front();
        }
        Chunk& c = chunks_[idx];
        const auto t0 = std::chrono::steady_clock::now();
        std::string err;
        try {
            file_.writeAt({c.data, roundUp(c.used)}, c.offset);
        } catch (const std::exception& e) {
            err = e.what();
        }
        chunkLatency_.record(std::chrono::steady_clock::now() - t0);
        {
            std::lock_guard lock(mu_);
            if (!err.empty() && workerError_.empty()) workerError_ = err;
            if (c.offset == 0) headOnDisk_ = true;
            ++chunksDone_;
            c.used = 0;
            free_.push_back(idx);
        }
        cv_.notify_all();
    }
}

void DirectWriter::rethrowWorkerError() {
    std::lock_guard lock(mu_);
    if (!workerError_.empty()) throw std::runtime_error("direct write failed: " + workerError_);
}

void DirectWriter::submitCurrent() {
    Chunk& c = chunks_[current_];
    if (c.used == 0) return;
    if (c.used % kAlign) std::memset(c.data + c.used, 0, roundUp(c.used) - c.used);   // pad the tail
    if (c.offset == 0 && !headCaptured_) {   // keep the first 4096 bytes for later header refreshes
        std::memcpy(head_, c.data, kAlign);
        headCaptured_ = true;
    }
    {
        std::lock_guard lock(mu_);
        queue_.push_back(current_);
    }
    cv_.notify_all();
}

void DirectWriter::nextChunk() {
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] { return !free_.empty() || !workerError_.empty(); });
    if (!workerError_.empty()) throw std::runtime_error("direct write failed: " + workerError_);
    current_ = free_.back();
    free_.pop_back();
    lock.unlock();
    stallLatency_.record(std::chrono::steady_clock::now() - t0);
    chunks_[current_].used = 0;
    chunks_[current_].offset = nextOffset_;
    nextOffset_ += opt_.chunkBytes;
}

void DirectWriter::append(std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        Chunk& c = chunks_[current_];
        const std::size_t n = std::min(bytes.size(), opt_.chunkBytes - c.used);
        std::memcpy(c.data + c.used, bytes.data(), n);
        c.used += n;
        streamBytes_ += n;
        bytes = bytes.subspan(n);
        if (c.used == opt_.chunkBytes) {
            submitCurrent();
            nextChunk();
        }
    }
}

void DirectWriter::writeFrame(std::span<const std::byte> frame, int64_t utcTicks) {
    if (finished_) throw std::logic_error("SER direct writer already finished");
    if (frame.size() != header_.frameBytes())
        throw std::invalid_argument(std::format("frame is {} bytes, expected {}", frame.size(), header_.frameBytes()));
    rethrowWorkerError();
    if (timestamps_.empty() && header_.dateTimeUtcTicks == 0) {
        header_.dateTimeUtcTicks = utcTicks;
        if (header_.dateTimeTicks == 0) header_.dateTimeTicks = utcTicks;
    }
    append(frame);
    timestamps_.push_back(utcTicks);
    if (std::chrono::steady_clock::now() - lastHeader_ >= opt_.headerEvery) refreshHeader();
}

void DirectWriter::refreshHeader() {
    lastHeader_ = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(mu_);
        if (!headOnDisk_) return;   // the first chunk hasn't reached the disk yet; it will carry its own header
    }
    header_.frameCount = static_cast<int32_t>(timestamps_.size());
    const auto raw = header_.encode();
    std::memcpy(head_, raw.data(), kHeaderSize);
    file_.writeAt({head_, kAlign}, 0);
}

void DirectWriter::finish() {
    if (finished_) return;
    finished_ = true;
    // The timestamp trailer is just more bytes of the stream.
    append(std::as_bytes(std::span(timestamps_)));
    const uint64_t finalSize = streamBytes_;
    if (chunks_[current_].used > 0) {
        submitCurrent();
    } else {
        std::lock_guard lock(mu_);   // nothing to write: hand the empty chunk straight back
        free_.push_back(current_);
    }
    {
        // every chunk is back on the free list once all writes have completed
        std::unique_lock lock(mu_);
        cv_.wait(lock, [&] { return free_.size() == chunks_.size() || !workerError_.empty(); });
    }
    rethrowWorkerError();
    // Final header: frame count and start time, written over the first 4096 bytes.
    header_.frameCount = static_cast<int32_t>(timestamps_.size());
    const auto raw = header_.encode();
    std::memcpy(head_, raw.data(), kHeaderSize);
    file_.writeAt({head_, kAlign}, 0);
    file_.truncate(finalSize);
    file_.close();
}

}  // namespace slopcap::ser

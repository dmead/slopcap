// One recording: a capture thread moves frames from the camera into the FrameRing, a writer thread streams them to
// a SER file. Capture never waits for the disk; if the ring is full the frame is read into a scratch buffer and
// counted as a ring drop, so the camera keeps flowing. Camera drops (frames the driver lost) are counted separately.
#pragma once

#include "camera/camera.hpp"
#include "io/ser_direct.hpp"
#include "pipeline/frame_ring.hpp"
#include "pipeline/latency.hpp"
#include "preview/latest_frame.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace slopcap {

struct CaptureOptions {
    std::filesystem::path output;                     ///< SER file to write; empty = live view only, nothing recorded
    std::optional<std::chrono::milliseconds> duration;  ///< stop after this long
    std::optional<uint64_t> frameLimit;                 ///< stop after this many frames
    std::size_t ringBytes = std::size_t{1} << 30;       ///< memory for the ring (default 1 GiB; see recordingRingBytes)
    std::string observer, telescope;                    ///< SER header text
    bool directIO = false;                              ///< unbuffered chunked writer (ser::DirectWriter)
    ser::DirectWriterOptions direct;                    ///< its chunk size, in-flight count, preallocation
    LatestFrame* preview = nullptr;                     ///< if set, the capture thread copies a frame in whenever it asks
    LatestFrame* remote = nullptr;                      ///< the same for the remote stream (net::RemoteServer::tap)
    std::function<void()> beforeWrite;                  ///< called by the writer thread before each frame (tests: a slow disk)
};

/// The ring for a recording: a quarter of the RAM that's free, at most an eighth of all of it, between 1 and 4 GiB.
/// Writing past the disk cache, the ring alone rides out the disk's stalls (an SSD whose fast cache is used up stalls
/// for hundreds of milliseconds; full frame at 390 MB/s filled 1 GiB to within 4 slots). Free RAM unknown: an eighth
/// of all of it; that unknown too: 1 GiB.
std::size_t recordingRingBytes(std::optional<uint64_t> totalRam, std::optional<uint64_t> availableRam);

struct CaptureStats {
    uint64_t delivered = 0;        ///< frames the camera handed us
    uint64_t written = 0;          ///< frames on disk
    uint64_t cameraDropped = 0;    ///< frames the camera/driver lost
    uint64_t ringDropped = 0;      ///< frames we lost because the ring was full (writer too slow)
    std::size_t ringCapacity = 0;
    std::size_t ringHighWater = 0;
    std::size_t ringQueued = 0;    ///< frames waiting for the writer right now
    double seconds = 0;            ///< the capture: from start until the camera stopped (or now, while capturing)
    double drainSeconds = 0;       ///< after capture stopped, until the writer had written what the ring still held
    double captureFps = 0;         ///< delivered / seconds
    double writeMBps = 0;          ///< written bytes / the writer's time (capture plus drain)
    double cpuCores = 0;           ///< the whole process's CPU use over the capture (seconds), in cores
    bool capturing = false;        ///< the camera is still being read (false while only the drain goes on)
    bool running = false;
    std::string error;             ///< first error from either thread, if any
    std::string notice;            ///< last error from a posted action (the session keeps running)
    // Time the writer thread spends handing one frame to the writer (copy + any stall), log2-bucketed.
    uint64_t writeP50Us = 0, writeP99Us = 0, writeMaxUs = 0;
    // Direct writer only: one chunk to disk, and waits for a free chunk (i.e. the disk falling behind).
    uint64_t chunkP99Us = 0, chunkMaxUs = 0, stallMaxUs = 0;
};

class CaptureSession {
public:
    CaptureSession(ICamera& camera, CaptureOptions options);
    ~CaptureSession();   ///< stops and joins if still running
    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    void start();                 ///< starts the camera stream and both threads; throws on setup errors
    void stop();                  ///< asks capture to stop; the writer drains the ring; joins both threads
    /// Wait until the limit is reached, the stream ends, an error occurs, or `timeout` passes. True if finished.
    bool wait(std::chrono::milliseconds timeout);
    CaptureStats stats() const;
    /// Run `action` on the capture thread between frames: the way to change exposure, gain etc. mid-stream, since
    /// cameras aren't thread-safe. Exceptions it throws land in stats().notice. Actions posted after the capture
    /// thread has finished never run.
    void post(std::function<void(ICamera&)> action);

private:
    void captureLoop();
    void writerLoop();
    void fail(const std::exception& e);

    ICamera& cam_;
    CaptureOptions opt_;
    std::unique_ptr<FrameRing> ring_;
    std::vector<std::byte> scratch_;   ///< receives frames when the ring is full

    std::thread captureThread_, writerThread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> captureDone_{false};
    std::atomic<bool> writerDone_{false};
    std::chrono::steady_clock::time_point started_;
    std::atomic<int64_t> finishedNs_{0};   ///< elapsed time when the writer finished (0 = still running)
    std::atomic<int64_t> captureEndNs_{0};   ///< elapsed time when the camera stopped (0 = still capturing)
    std::chrono::nanoseconds cpuStart_{0};    ///< the process's CPU time at start
    std::atomic<int64_t> cpuCaptureNs_{0};   ///< process CPU used from start until the camera stopped

    std::atomic<uint64_t> delivered_{0}, written_{0}, ringDropped_{0}, bytesWritten_{0};
    std::atomic<uint64_t> cameraDropped_{0};
    LatencyHistogram writeLatency_;
    std::atomic<const ser::DirectWriter*> direct_{nullptr};
    mutable std::mutex directMutex_;
    uint64_t finalChunkP99_ = 0, finalChunkMax_ = 0, finalStallMax_ = 0;   ///< guarded by directMutex_
    mutable std::mutex errorMutex_;
    std::string error_, notice_;
    std::mutex postMutex_;
    std::vector<std::function<void(ICamera&)>> posted_;
    std::atomic<bool> hasPosted_{false};
    void runPosted();
};

}  // namespace slopcap

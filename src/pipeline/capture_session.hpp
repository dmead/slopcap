// One recording: a capture thread moves frames from the camera into the FrameRing, a writer thread streams them to
// a SER file. Capture never waits for the disk; if the ring is full the frame is read into a scratch buffer and
// counted as a ring drop, so the camera keeps flowing. Camera drops (frames the driver lost) are counted separately.
#pragma once

#include "camera/camera.hpp"
#include "pipeline/frame_ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace slopcap {

struct CaptureOptions {
    std::filesystem::path output;                     ///< SER file to write
    std::optional<std::chrono::milliseconds> duration;  ///< stop after this long
    std::optional<uint64_t> frameLimit;                 ///< stop after this many frames
    std::size_t ringBytes = std::size_t{1} << 30;       ///< memory for the ring (default 1 GiB)
    std::string observer, telescope;                    ///< SER header text
};

struct CaptureStats {
    uint64_t delivered = 0;        ///< frames the camera handed us
    uint64_t written = 0;          ///< frames on disk
    uint64_t cameraDropped = 0;    ///< frames the camera/driver lost
    uint64_t ringDropped = 0;      ///< frames we lost because the ring was full (writer too slow)
    std::size_t ringCapacity = 0;
    std::size_t ringHighWater = 0;
    double seconds = 0;            ///< since start
    double captureFps = 0;         ///< delivered / seconds
    double writeMBps = 0;          ///< written bytes / seconds
    bool running = false;
    std::string error;             ///< first error from either thread, if any
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

    std::atomic<uint64_t> delivered_{0}, written_{0}, ringDropped_{0}, bytesWritten_{0};
    std::atomic<uint64_t> cameraDropped_{0};
    mutable std::mutex errorMutex_;
    std::string error_;
};

}  // namespace slopcap

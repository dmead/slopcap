// Sony Alpha and NEX cameras on USB in PC Remote mode (src/camera/sony_ptp.hpp), for focusing: every frame is a test
// shot, taken, downloaded and decoded to its luminance, full resolution, 8-bit mono (the ROI is a crop of it). Shots
// follow one another while streaming, "Interval" seconds apart if set. Controls: "Exposure" (microseconds, set to the
// nearest shutter speed the camera has) and "ISO", both only with the mode dial on M, and "Interval". Opening the
// camera takes one picture: its size is the frame size. A camera set to RAW alone is set to JPEG Fine while it is
// open, and back to RAW after. Windows reaches the camera through its own driver (WPD) or WinUSB, Linux and macOS
// through libusb.
#pragma once

#include "camera/camera.hpp"
#include "camera/sony_ptp.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace slopcap {

/// Where Sony cameras are found and opened: the system's (WPD on Windows, libusb elsewhere), or a test's fake.
class SonyDeviceSource {
public:
    struct Entry {
        std::string id, model;
        std::string problem;   ///< why it can't be opened, if known (permissions)
    };
    virtual ~SonyDeviceSource() = default;
    virtual std::vector<Entry> list() = 0;
    virtual std::unique_ptr<ptp::Transport> open(const std::string& id) = 0;   ///< throws CameraError
};
SonyDeviceSource& systemSonyDevices();
/// Tests: Sony cameras from here (nullptr: the system's again).
void setSonyDeviceSource(SonyDeviceSource* source);

class SonyCamera final : public ICamera {
public:
    static std::vector<SonyDeviceSource::Entry> list();
    /// `which`: empty for the first camera, a number for that one in list(), else the first whose model or id
    /// contains it. Throws CameraError (none there, not in PC Remote mode, in use, no picture).
    explicit SonyCamera(std::string_view which = "");
    ~SonyCamera() override;
    SonyCamera(const SonyCamera&) = delete;
    SonyCamera& operator=(const SonyCamera&) = delete;

    const CameraInfo& info() const override { return info_; }

    std::vector<ControlInfo> controls() const override;
    std::optional<double> control(std::string_view name) const override;
    void setControl(std::string_view name, double value) override;

    Roi roi() const override;
    void setRoi(const Roi& roi) override;
    int bitDepth() const override { return 8; }
    void setBitDepth(int bits) override;

    void startStream() override;
    void stopStream() override;
    bool streaming() const override { return streaming_; }
    ReadResult readFrame(std::span<std::byte> dst, FrameInfo& info, std::chrono::milliseconds timeout) override;
    uint64_t droppedFrames() const override;

private:
    struct Cache {
        int64_t shutter = 0, iso = 0, program = 0;
        bool shutterSettable = false, isoSettable = false;
        std::chrono::steady_clock::time_point at;
    };
    void readCache() const;                    ///< from the session's last refresh (io_ held)
    void refreshCache() const;                 ///< when stopped and stale: reads the camera (takes io_)
    std::chrono::milliseconds shotTimeout() const;
    void worker();
    void fail(const std::string& why);
    void deliver(const sony::Shot& shot, int width, int height);   ///< luma_ (width x height), cropped, as the next frame

    std::unique_ptr<ptp::Transport> t_;
    std::unique_ptr<sony::Session> s_;
    mutable std::mutex io_;                    ///< the camera: one transaction at a time
    std::atomic<int> waitingForIo_{0};         ///< callers queued for io_: the next shot waits for them
    CameraInfo info_;
    std::vector<int64_t> shutterList_, isoList_;   ///< what the camera offers, when it says (else a standard series)
    std::optional<int64_t> restoreQuality_;        ///< the camera's own Quality, when we changed it

    mutable std::mutex state_;                 ///< everything below, and the cache
    mutable Cache cache_;
    Roi roi_;
    double interval_ = 0;
    std::condition_variable cv_;
    std::vector<uint8_t> frame_;
    FrameInfo frameInfo_;
    bool fresh_ = false;
    std::string error_;
    uint64_t sequence_ = 0, dropped_ = 0;

    std::vector<uint8_t> luma_;                ///< the worker's decoded picture
    std::thread thread_;
    std::atomic<bool> stop_{false}, streaming_{false};
};

}  // namespace slopcap

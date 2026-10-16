// QHY cameras through QHY's SDK (libqhyccd / qhyccd.dll), in live (video) mode.
// Differences from ZWO that shape this class:
// - read mode and stream mode must be set before InitQHYCCD, so changing the read mode re-initialises the camera;
// - live frames are polled (GetQHYCCDLiveFrame returns "not ready" until one arrives);
// - the SDK writes into a buffer of GetQHYCCDMemLength() bytes (sized for the full sensor), so frames are staged
//   there and copied once into the caller's buffer;
// - the SDK reports no dropped-frame count; droppedFrames() is always 0 (gaps show up in timestamps instead).
#pragma once

#include "camera/camera.hpp"

#include <vector>

namespace slopcap {

struct QhyParam;   // one QHY control: our name, the SDK's CONTROL_ID, writability (defined in qhy_camera.cpp)

struct QhyCameraEntry {
    int index = 0;
    std::string id;      ///< SDK id, e.g. "QHY533M-9f8e7d6c5b4a39281"
    std::string model;
};

class QhyCamera final : public ICamera {
public:
    static std::vector<QhyCameraEntry> list();
    explicit QhyCamera(int index = 0);   ///< opens, selects live mode, initialises; throws CameraError
    ~QhyCamera() override;
    QhyCamera(const QhyCamera&) = delete;
    QhyCamera& operator=(const QhyCamera&) = delete;

    const CameraInfo& info() const override { return info_; }

    std::vector<ControlInfo> controls() const override;
    std::optional<double> control(std::string_view name) const override;
    void setControl(std::string_view name, double value) override;

    std::vector<ReadoutMode> readoutModes() const override { return modes_; }
    int readoutMode() const override { return mode_; }
    void setReadoutMode(int index) override;
    std::vector<GainLandmark> gainLandmarks() const override;

    Roi roi() const override { return roi_; }
    void setRoi(const Roi& roi) override;
    int bitDepth() const override { return bits_; }
    void setBitDepth(int bits) override;

    void startStream() override;
    void stopStream() override;
    bool streaming() const override { return streaming_; }
    ReadResult readFrame(std::span<std::byte> dst, FrameInfo& info, std::chrono::milliseconds timeout) override;

private:
    const QhyParam* param(std::string_view name) const;   ///< null if the camera lacks it
    void initialise();     ///< stream mode + InitQHYCCD + format; used at open and after a read-mode change
    void applyFormat();

    void* h_ = nullptr;
    CameraInfo info_;
    std::vector<ReadoutMode> modes_;
    int mode_ = 0;
    Roi roi_;
    int bits_ = 8;
    bool streaming_ = false;
    uint64_t sequence_ = 0;
    std::vector<std::byte> staging_;
};

}  // namespace slopcap

// ZWO ASI cameras through ZWO's SDK (libASICamera2 / ASICamera2.dll). Frames are read straight into the caller's
// buffer (ASIGetVideoData), so with FrameRing slots there is no copy between the SDK and the disk writer.
#pragma once

#include "camera/camera.hpp"

#include <map>
#include <string>
#include <vector>

namespace slopcap {

struct AsiCameraEntry {
    int index = 0;        ///< position in the SDK's list (pass to AsiCamera)
    std::string model;
};

class AsiCamera final : public ICamera {
public:
    static std::vector<AsiCameraEntry> list();
    explicit AsiCamera(int index = 0);   ///< opens and initialises; throws CameraError (e.g. in use elsewhere)
    ~AsiCamera() override;
    AsiCamera(const AsiCamera&) = delete;
    AsiCamera& operator=(const AsiCamera&) = delete;

    const CameraInfo& info() const override { return info_; }

    std::vector<ControlInfo> controls() const override { return controls_; }
    std::optional<double> control(std::string_view name) const override;
    void setControl(std::string_view name, double value) override;
    std::optional<bool> controlAuto(std::string_view name) const override;
    void setControlAuto(std::string_view name, bool on) override;

    /// ZWO has no named modes; the high-speed readout switch is presented as two.
    std::vector<ReadoutMode> readoutModes() const override;
    int readoutMode() const override;
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
    uint64_t droppedFrames() const override;

private:
    int controlType(std::string_view name) const;   ///< ASI_CONTROL_TYPE for a control name; throws
    void applyFormat();

    int id_ = -1;
    CameraInfo info_;
    std::vector<ControlInfo> controls_;
    std::map<std::string, int, std::less<>> controlTypes_;
    Roi roi_;
    int bits_ = 8;
    bool streaming_ = false;
    int dropBase_ = 0;
    mutable uint64_t lastDropped_ = 0;   ///< kept after stopStream() so the final count survives
    uint64_t sequence_ = 0;
};

}  // namespace slopcap

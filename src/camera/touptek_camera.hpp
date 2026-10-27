// ToupTek cameras, and the brands that ship ToupTek's SDK under their own name: MallinCam (libmallincam is libtoupcam
// with its symbols renamed, same version, same API). So there is one implementation (touptek_family.inc), compiled
// once per brand against that brand's library (touptek_camera.cpp, mallincam_camera.cpp).
//
// The camera runs in RAW mode, pulled frame by frame (Toupcam_WaitImageV3) straight into the caller's buffer.
// Controls: "Exposure" (microseconds), "Gain" (the SDK's percent: 100 = 1x), "Offset" (black level), "Speed", and where
// the camera has them "Temperature" (°C), "CoolerOn", "TargetTemp", "Fan", "LowNoise", "HighFullwell". Conversion gain
// (LCG / HCG / HDR) is the readout mode. Binning is ToupTek's digital binning, averaged so the bit depth stays.
#pragma once

#include "camera/camera.hpp"

#include <string>
#include <vector>

namespace slopcap {

enum class ToupBrand { ToupTek, MallinCam };

struct ToupCameraEntry {
    int index = 0;        ///< position in the SDK's list (pass to the camera's constructor)
    std::string model;    ///< display name: model, or the name the user gave the camera
};

template <ToupBrand Brand>
class ToupCamera final : public ICamera {
public:
    static std::vector<ToupCameraEntry> list();
    explicit ToupCamera(int index = 0);   ///< opens it; throws CameraError (e.g. in use elsewhere)
    ~ToupCamera() override;
    ToupCamera(const ToupCamera&) = delete;
    ToupCamera& operator=(const ToupCamera&) = delete;

    const CameraInfo& info() const override { return info_; }

    std::vector<ControlInfo> controls() const override { return controls_; }
    std::optional<double> control(std::string_view name) const override;
    void setControl(std::string_view name, double value) override;
    std::optional<bool> controlAuto(std::string_view name) const override;
    void setControlAuto(std::string_view name, bool on) override;

    std::vector<ReadoutMode> readoutModes() const override { return modes_; }
    int readoutMode() const override;
    void setReadoutMode(int index) override;

    Roi roi() const override { return roi_; }
    void setRoi(const Roi& roi) override;
    int bitDepth() const override { return bits_; }
    void setBitDepth(int bits) override;

    void startStream() override;
    void stopStream() override;
    bool streaming() const override { return streaming_; }
    ReadResult readFrame(std::span<std::byte> dst, FrameInfo& info, std::chrono::milliseconds timeout) override;
    uint64_t droppedFrames() const override { return dropped_; }

private:
    enum class Ctl { Exposure, Gain, Offset, Speed, Temperature, Cooler, TargetTemp, Fan, LowNoise, HighFullwell };
    Ctl ctl(std::string_view name) const;   ///< throws
    void addControl(Ctl c, ControlInfo info);
    void applyFormat();

    void* h_ = nullptr;   ///< the SDK's handle (HToupcam / HMallincam)
    CameraInfo info_;
    std::vector<ControlInfo> controls_;
    std::vector<Ctl> ctls_;   ///< what each of controls_ is
    std::vector<ReadoutMode> modes_;
    Roi roi_;
    int bits_ = 8;
    int shift_ = 0;           ///< 16-bit samples come LSB-aligned from an SDK that can't pad low: shift them up by this
    bool streaming_ = false;
    uint64_t sequence_ = 0, dropped_ = 0;
    unsigned lastSeq_ = 0;
    bool haveSeq_ = false;
};

using ToupTekCamera = ToupCamera<ToupBrand::ToupTek>;
using MallinCamCamera = ToupCamera<ToupBrand::MallinCam>;
extern template class ToupCamera<ToupBrand::ToupTek>;
extern template class ToupCamera<ToupBrand::MallinCam>;

}  // namespace slopcap

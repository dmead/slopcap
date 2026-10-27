// Player One Astronomy cameras (Mars, Neptune, Uranus, Saturn, Apollo, Ares, Poseidon, Zeus) through Player One's SDK
// (libPlayerOneCamera / PlayerOneCamera.dll, version 3), which is shaped much like ZWO's. Its video mode (continuous
// exposure) reads straight into the caller's buffer (POAGetImageData). The controls are the SDK's configs under its
// own names ("Gain", "Exposure" in microseconds, "Offset", ...); its sensor modes are the readout modes and its preset
// gains (highest dynamic range, HCG, unity, lowest read noise) the gain landmarks.
#pragma once

#include "camera/camera.hpp"

#include <map>
#include <string>
#include <vector>

namespace slopcap {

struct PlayerOneCameraEntry {
    int index = 0;        ///< position in the SDK's list (pass to PlayerOneCamera)
    std::string model;
};

class PlayerOneCamera final : public ICamera {
public:
    static std::vector<PlayerOneCameraEntry> list();
    explicit PlayerOneCamera(int index = 0);   ///< opens and initialises; throws CameraError (e.g. in use elsewhere)
    ~PlayerOneCamera() override;
    PlayerOneCamera(const PlayerOneCamera&) = delete;
    PlayerOneCamera& operator=(const PlayerOneCamera&) = delete;

    const CameraInfo& info() const override { return info_; }

    std::vector<ControlInfo> controls() const override { return controls_; }
    std::optional<double> control(std::string_view name) const override;
    void setControl(std::string_view name, double value) override;
    std::optional<bool> controlAuto(std::string_view name) const override;
    void setControlAuto(std::string_view name, bool on) override;

    std::vector<ReadoutMode> readoutModes() const override { return modes_; }
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
    struct Config {
        int id = 0;     ///< POAConfig
        int type = 0;   ///< POAValueType
    };
    const Config& config(std::string_view name) const;   ///< throws
    void applyFormat();

    int id_ = -1;
    CameraInfo info_;
    std::vector<ControlInfo> controls_;
    std::map<std::string, Config, std::less<>> configs_;
    std::vector<ReadoutMode> modes_;
    Roi roi_;
    int bits_ = 8;
    bool streaming_ = false;
    mutable uint64_t lastDropped_ = 0;   ///< kept after stopStream() (the SDK resets its count)
    uint64_t sequence_ = 0;
};

}  // namespace slopcap

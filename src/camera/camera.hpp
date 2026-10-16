// The camera abstraction every backend implements (ZWO, QHY, simulated). Shaped by what the real drivers report
// (tools/camprobe/fixtures): controls are described by the camera, readout modes and gain landmarks are normalised
// across brands, and frames land in a buffer the caller owns so the capture pipeline never copies.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace slopcap {

enum class Bayer { None, RGGB, GRBG, GBRG, BGGR };

const char* bayerName(Bayer b);

struct Roi {
    int x = 0, y = 0, width = 0, height = 0;
    int bin = 1;
    friend bool operator==(const Roi&, const Roi&) = default;
};

struct CameraInfo {
    std::string vendor;          ///< "ZWO", "QHY", "Simulated"
    std::string model;
    std::string serial;          ///< stable id: per-camera calibration is keyed on this
    int sensorWidth = 0, sensorHeight = 0;
    double pixelUm = 0;
    std::optional<int> adcBits;  ///< not every driver reports it
    Bayer bayer = Bayer::None;   ///< None = mono
    std::vector<int> bins;       ///< supported binning factors
    std::vector<int> bitDepths;  ///< output sample depths the camera can deliver (8, 16)
};

struct ControlInfo {
    std::string name;            ///< driver's name ("Gain", "Offset", "USBTraffic", ...)
    double min = 0, max = 0, step = 1;
    std::optional<double> defaultValue;
    bool writable = true;
    bool autoSupported = false;
};

struct ReadoutMode {
    int index = 0;
    std::string name;
    std::optional<int> width, height;   ///< when the mode changes the usable area
};

struct GainLandmark {
    enum class Kind { Unity, LowestReadNoise, HighestDynamicRange, HcgSwitch, Preset, VendorDefault };
    enum class Source { Driver, Published, Measured };
    Kind kind;
    std::string label;
    std::optional<double> gain;
    std::optional<double> offset;
    Source source = Source::Driver;
};

/// One delivered frame's metadata. Pixels go to the caller's buffer.
struct FrameInfo {
    uint64_t sequence = 0;                               ///< 0, 1, 2 ... since startStream()
    std::chrono::system_clock::time_point timestamp;     ///< UTC, end of exposure as reported/estimated
    Roi roi;
    int bitDepth = 8;                                    ///< 8 or 16 (samples are little-endian)
    std::size_t bytes = 0;
};

enum class ReadResult { Ok, Timeout, EndOfStream };

struct CameraError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// A connected camera. Not thread-safe: one capture thread drives it; configuration happens while stopped
/// unless a backend documents otherwise.
class ICamera {
public:
    virtual ~ICamera() = default;

    virtual const CameraInfo& info() const = 0;

    virtual std::vector<ControlInfo> controls() const = 0;
    virtual std::optional<double> control(std::string_view name) const = 0;
    virtual void setControl(std::string_view name, double value) = 0;                ///< throws CameraError
    virtual std::optional<bool> controlAuto(std::string_view) const { return std::nullopt; }
    virtual void setControlAuto(std::string_view name, bool) { throw CameraError(std::string(name) + ": no auto mode"); }

    virtual std::vector<ReadoutMode> readoutModes() const { return {}; }
    virtual int readoutMode() const { return 0; }
    virtual void setReadoutMode(int index) {
        if (index != 0) throw CameraError("camera has no readout modes");
    }
    virtual std::vector<GainLandmark> gainLandmarks() const { return {}; }

    virtual Roi roi() const = 0;
    virtual void setRoi(const Roi& roi) = 0;            ///< throws CameraError if invalid
    virtual int bitDepth() const = 0;
    virtual void setBitDepth(int bits) = 0;             ///< 8 or 16
    /// Bytes per frame at the current ROI and bit depth.
    std::size_t frameBytes() const;

    virtual void startStream() = 0;
    virtual void stopStream() = 0;
    virtual bool streaming() const = 0;
    /// Wait for the next frame and write it into dst (dst.size() >= frameBytes()).
    virtual ReadResult readFrame(std::span<std::byte> dst, FrameInfo& info, std::chrono::milliseconds timeout) = 0;
    /// Frames the camera/driver dropped since startStream().
    virtual uint64_t droppedFrames() const { return 0; }
};

}  // namespace slopcap

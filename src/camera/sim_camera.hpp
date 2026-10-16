// A camera that replays a SER recording through ICamera, so the pipeline, analysis and UI can be developed and
// tested without hardware. Real-time pacing follows the file's timestamps (or a fixed fps); a consumer that falls
// behind loses frames exactly like with a real camera, and those are counted as dropped.
#pragma once

#include "camera/camera.hpp"
#include "io/ser.hpp"

#include <filesystem>
#include <map>
#include <memory>

namespace slopcap {

// Declared outside SimCamera: a nested struct with default member initialisers can't be a default argument of
// its enclosing class's constructor (GCC rejects it).
enum class SimPace {
    Realtime,   ///< deliver frames at the recorded rate; slow consumers lose frames
    AsFastAsPossible,
};
struct SimOptions {
    SimPace pace = SimPace::Realtime;
    bool loop = false;           ///< restart at the first frame instead of ending the stream
    double fallbackFps = 50.0;   ///< realtime rate when the file has no timestamps
};

class SimCamera final : public ICamera {
public:
    using Pace = SimPace;
    using Options = SimOptions;

    explicit SimCamera(const std::filesystem::path& serFile, Options options = {});

    const CameraInfo& info() const override { return info_; }

    std::vector<ControlInfo> controls() const override;
    std::optional<double> control(std::string_view name) const override;
    void setControl(std::string_view name, double value) override;

    Roi roi() const override { return roi_; }
    void setRoi(const Roi& roi) override;
    int bitDepth() const override { return outDepth_; }
    void setBitDepth(int bits) override;

    void startStream() override;
    void stopStream() override { streaming_ = false; }
    bool streaming() const override { return streaming_; }
    ReadResult readFrame(std::span<std::byte> dst, FrameInfo& info, std::chrono::milliseconds timeout) override;
    uint64_t droppedFrames() const override { return dropped_; }

    int64_t sourceFrames() const { return reader_.frameCount(); }

private:
    using Clock = std::chrono::steady_clock;
    Clock::duration offsetOf(int64_t sourceIndex) const;   ///< time of a source frame relative to the first
    void copyRoi(std::span<std::byte> dst) const;

    ser::Reader reader_;
    Options opt_;
    CameraInfo info_;
    int srcDepth_;            ///< 8 or 16 bytes-per-sample in the file
    int outDepth_;
    Roi roi_;
    std::map<std::string, double, std::less<>> controls_;

    std::vector<std::byte> full_;   ///< one source frame
    bool streaming_ = false;
    Clock::time_point start_;
    int64_t next_ = 0;              ///< next source frame index (monotonic; wraps via modulo when looping)
    uint64_t sequence_ = 0;
    uint64_t dropped_ = 0;
};

}  // namespace slopcap

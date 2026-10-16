// SER video files (Grischa Hahn's format, v3 with the optional per-frame timestamp trailer).
//
// Layout: 178-byte header, FrameCount frames of Width*Height*Planes samples (1 or 2 bytes each), then optionally
// FrameCount little-endian int64 UTC timestamps. Times are .NET ticks: 100 ns units since 0001-01-01.
//
// Real-world quirks handled here (checked against FireCapture recordings):
// - The LittleEndian field is written as 0 by FireCapture, SharpCap and others although 16-bit data is
//   little-endian; the reader treats both 0 and 1 as little-endian and the writer writes 0.
// - The header's local DateTime is often UTC as well; only DateTime_UTC is trusted.
// - A recording cut off mid-write has a header frame count lower than the frames on disk (often 0); the reader
//   counts complete frames on disk and reports the file as truncated.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace slopcap::ser {

/// Colour layout codes from the SER specification.
enum class ColorId : int32_t {
    Mono = 0,
    BayerRGGB = 8, BayerGRBG = 9, BayerGBRG = 10, BayerBGGR = 11,
    BayerCYYM = 16, BayerYCMY = 17, BayerYMCY = 18, BayerMYYC = 19,
    RGB = 100, BGR = 101,
};

const char* colorName(ColorId id);
int planes(ColorId id);   ///< 3 for RGB/BGR, else 1

inline constexpr std::size_t kHeaderSize = 178;
inline constexpr int64_t kTicksPerSecond = 10'000'000;
inline constexpr int64_t kUnixEpochTicks = 621'355'968'000'000'000;   ///< 1970-01-01 in .NET ticks

/// .NET ticks <-> system_clock
int64_t toTicks(std::chrono::system_clock::time_point t);
std::chrono::system_clock::time_point fromTicks(int64_t ticks);

struct Header {
    int32_t luId = 0;
    ColorId colorId = ColorId::Mono;
    int32_t littleEndian = 0;            ///< as written; see the quirk above
    int32_t width = 0;
    int32_t height = 0;
    int32_t bitDepth = 8;                ///< per plane, 1..16
    int32_t frameCount = 0;              ///< as written in the header
    std::string observer, instrument, telescope;   ///< up to 40 bytes each
    int64_t dateTimeTicks = 0;           ///< "local" time; often UTC in practice
    int64_t dateTimeUtcTicks = 0;

    int bytesPerSample() const { return bitDepth > 8 ? 2 : 1; }
    std::size_t frameBytes() const;

    std::array<std::byte, kHeaderSize> encode() const;
    static Header decode(std::span<const std::byte, kHeaderSize> raw);   ///< throws FormatError
};

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Read-only access to a SER file. Frames are read on demand; nothing but the header and timestamps is loaded.
class Reader {
public:
    explicit Reader(const std::filesystem::path& path);   ///< throws FormatError / std::system_error

    const Header& header() const { return header_; }
    /// Complete frames actually on disk (may differ from header().frameCount).
    int64_t frameCount() const { return frames_; }
    bool truncated() const { return frames_ != header_.frameCount; }
    /// Per-frame UTC timestamps in ticks, if the file has a complete trailer.
    const std::optional<std::vector<int64_t>>& timestamps() const { return timestamps_; }
    std::uintmax_t fileSize() const { return fileSize_; }
    const std::filesystem::path& path() const { return path_; }

    /// Read frame `index` into dst (dst.size() >= header().frameBytes()). Throws std::out_of_range.
    void readFrame(int64_t index, std::span<std::byte> dst);

private:
    std::filesystem::path path_;
    std::ifstream in_;
    Header header_;
    int64_t frames_ = 0;
    std::uintmax_t fileSize_ = 0;
    std::optional<std::vector<int64_t>> timestamps_;
};

/// Anything that stores a stream of frames (the buffered Writer, the direct-I/O DirectWriter).
class FrameSink {
public:
    virtual ~FrameSink() = default;
    virtual void writeFrame(std::span<const std::byte> frame, int64_t utcTicks) = 0;
    virtual void finish() = 0;
    virtual int64_t framesWritten() const = 0;
};

/// Writes a SER file frame by frame through the OS page cache; finish() writes the frame count and timestamp trailer.
/// If the process dies before finish(), the file is still readable by Reader (as truncated).
class Writer final : public FrameSink {
public:
    Writer(const std::filesystem::path& path, Header header);   ///< header.frameCount is ignored
    ~Writer() override;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /// Append one frame (exactly header.frameBytes()) with its UTC timestamp in ticks.
    void writeFrame(std::span<const std::byte> frame, int64_t utcTicks) override;
    /// Patch the header frame count and append the timestamp trailer. Called by the destructor if needed.
    void finish() override;
    int64_t framesWritten() const override { return static_cast<int64_t>(timestamps_.size()); }

private:
    std::filesystem::path path_;
    std::ofstream out_;
    Header header_;
    std::vector<int64_t> timestamps_;
    bool finished_ = false;
};

}  // namespace slopcap::ser

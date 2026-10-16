#include "io/ser.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

namespace slopcap::ser {

static_assert(std::endian::native == std::endian::little, "SER I/O assumes a little-endian host");

namespace {

constexpr char kFileId[] = "LUCAM-RECORDER";   // 14 chars, no terminator in the file
constexpr std::size_t kTextField = 40;

template <class T>
void put(std::array<std::byte, kHeaderSize>& b, std::size_t off, T v) {
    std::memcpy(b.data() + off, &v, sizeof v);
}

template <class T>
T get(std::span<const std::byte, kHeaderSize> b, std::size_t off) {
    T v;
    std::memcpy(&v, b.data() + off, sizeof v);
    return v;
}

void putText(std::array<std::byte, kHeaderSize>& b, std::size_t off, const std::string& s) {
    std::memset(b.data() + off, 0, kTextField);
    std::memcpy(b.data() + off, s.data(), std::min(s.size(), kTextField));
}

std::string getText(std::span<const std::byte, kHeaderSize> b, std::size_t off) {
    const char* p = reinterpret_cast<const char*>(b.data() + off);
    std::size_t n = 0;
    while (n < kTextField && p[n] != '\0') ++n;
    std::string s(p, n);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

bool validColor(int32_t c) {
    switch (static_cast<ColorId>(c)) {
        case ColorId::Mono: case ColorId::BayerRGGB: case ColorId::BayerGRBG: case ColorId::BayerGBRG:
        case ColorId::BayerBGGR: case ColorId::BayerCYYM: case ColorId::BayerYCMY: case ColorId::BayerYMCY:
        case ColorId::BayerMYYC: case ColorId::RGB: case ColorId::BGR:
            return true;
    }
    return false;
}

// Field offsets (bytes) in the 178-byte header.
enum Off : std::size_t {
    kId = 0, kLuId = 14, kColor = 18, kLittleEndian = 22, kWidth = 26, kHeight = 30, kDepth = 34,
    kFrames = 38, kObserver = 42, kInstrument = 82, kTelescope = 122, kDateTime = 162, kDateTimeUtc = 170,
};

}  // namespace

const char* colorName(ColorId id) {
    switch (id) {
        case ColorId::Mono: return "MONO";
        case ColorId::BayerRGGB: return "BAYER_RGGB";
        case ColorId::BayerGRBG: return "BAYER_GRBG";
        case ColorId::BayerGBRG: return "BAYER_GBRG";
        case ColorId::BayerBGGR: return "BAYER_BGGR";
        case ColorId::BayerCYYM: return "BAYER_CYYM";
        case ColorId::BayerYCMY: return "BAYER_YCMY";
        case ColorId::BayerYMCY: return "BAYER_YMCY";
        case ColorId::BayerMYYC: return "BAYER_MYYC";
        case ColorId::RGB: return "RGB";
        case ColorId::BGR: return "BGR";
    }
    return "?";
}

int planes(ColorId id) { return id == ColorId::RGB || id == ColorId::BGR ? 3 : 1; }

int64_t toTicks(std::chrono::system_clock::time_point t) {
    auto since = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, kTicksPerSecond>>>(t.time_since_epoch());
    return kUnixEpochTicks + since.count();
}

std::chrono::system_clock::time_point fromTicks(int64_t ticks) {
    std::chrono::duration<int64_t, std::ratio<1, kTicksPerSecond>> d(ticks - kUnixEpochTicks);
    return std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(d));
}

std::size_t Header::frameBytes() const {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(planes(colorId)) *
           static_cast<std::size_t>(bytesPerSample());
}

std::array<std::byte, kHeaderSize> Header::encode() const {
    std::array<std::byte, kHeaderSize> b{};
    std::memcpy(b.data() + kId, kFileId, 14);
    put(b, kLuId, luId);
    put(b, kColor, static_cast<int32_t>(colorId));
    put(b, kLittleEndian, littleEndian);
    put(b, kWidth, width);
    put(b, kHeight, height);
    put(b, kDepth, bitDepth);
    put(b, kFrames, frameCount);
    putText(b, kObserver, observer);
    putText(b, kInstrument, instrument);
    putText(b, kTelescope, telescope);
    put(b, kDateTime, dateTimeTicks);
    put(b, kDateTimeUtc, dateTimeUtcTicks);
    return b;
}

Header Header::decode(std::span<const std::byte, kHeaderSize> b) {
    if (std::memcmp(b.data() + kId, kFileId, 14) != 0) throw FormatError("not a SER file (missing LUCAM-RECORDER id)");
    Header h;
    h.luId = get<int32_t>(b, kLuId);
    int32_t color = get<int32_t>(b, kColor);
    if (!validColor(color)) throw FormatError(std::format("unknown SER ColorID {}", color));
    h.colorId = static_cast<ColorId>(color);
    h.littleEndian = get<int32_t>(b, kLittleEndian);
    h.width = get<int32_t>(b, kWidth);
    h.height = get<int32_t>(b, kHeight);
    h.bitDepth = get<int32_t>(b, kDepth);
    h.frameCount = get<int32_t>(b, kFrames);
    h.observer = getText(b, kObserver);
    h.instrument = getText(b, kInstrument);
    h.telescope = getText(b, kTelescope);
    h.dateTimeTicks = get<int64_t>(b, kDateTime);
    h.dateTimeUtcTicks = get<int64_t>(b, kDateTimeUtc);
    if (h.width <= 0 || h.height <= 0) throw FormatError(std::format("invalid SER size {}x{}", h.width, h.height));
    if (h.bitDepth < 1 || h.bitDepth > 16) throw FormatError(std::format("invalid SER bit depth {}", h.bitDepth));
    if (h.frameCount < 0) throw FormatError(std::format("invalid SER frame count {}", h.frameCount));
    return h;
}

// ---------------------------------------------------------------- Reader

Reader::Reader(const std::filesystem::path& path) : path_(path), in_(path, std::ios::binary) {
    if (!in_) throw std::system_error(errno, std::generic_category(), "cannot open " + path.string());
    fileSize_ = std::filesystem::file_size(path);
    if (fileSize_ < kHeaderSize) throw FormatError("file shorter than a SER header");
    std::array<std::byte, kHeaderSize> raw;
    in_.read(reinterpret_cast<char*>(raw.data()), kHeaderSize);
    header_ = Header::decode(raw);

    const std::uintmax_t payload = fileSize_ - kHeaderSize;
    const std::uintmax_t fb = header_.frameBytes();
    const std::uintmax_t declared = static_cast<std::uintmax_t>(header_.frameCount);
    // Complete header + trailer: exactly declared frames followed by declared timestamps.
    if (declared > 0 && payload == declared * fb + declared * 8) {
        frames_ = header_.frameCount;
        std::vector<int64_t> ts(declared);
        in_.seekg(static_cast<std::streamoff>(kHeaderSize + declared * fb));
        in_.read(reinterpret_cast<char*>(ts.data()), static_cast<std::streamsize>(declared * 8));
        if (in_) timestamps_ = std::move(ts);
        in_.clear();
    } else if (declared > 0 && payload >= declared * fb) {
        frames_ = header_.frameCount;   // frames intact, trailer missing or partial
    } else {
        frames_ = static_cast<int64_t>(payload / fb);   // truncated: count what's on disk
    }
}

void Reader::readFrame(int64_t index, std::span<std::byte> dst) {
    if (index < 0 || index >= frames_) throw std::out_of_range(std::format("frame {} of {}", index, frames_));
    const std::size_t fb = header_.frameBytes();
    if (dst.size() < fb) throw std::invalid_argument("destination smaller than one frame");
    in_.seekg(static_cast<std::streamoff>(kHeaderSize + static_cast<std::uintmax_t>(index) * fb));
    in_.read(reinterpret_cast<char*>(dst.data()), static_cast<std::streamsize>(fb));
    if (!in_) {
        in_.clear();
        throw std::runtime_error(std::format("read of frame {} failed", index));
    }
}

// ---------------------------------------------------------------- Writer

Writer::Writer(const std::filesystem::path& path, Header header)
    : path_(path), out_(path, std::ios::binary | std::ios::trunc), header_(std::move(header)) {
    if (!out_) throw std::system_error(errno, std::generic_category(), "cannot create " + path.string());
    header_.frameCount = 0;
    header_.littleEndian = 0;   // what every common reader expects (see quirk in ser.hpp)
    auto raw = header_.encode();
    out_.write(reinterpret_cast<const char*>(raw.data()), kHeaderSize);
}

Writer::~Writer() {
    try {
        finish();
    } catch (...) {
        // destructor must not throw; the file stays readable as a truncated recording
    }
}

void Writer::writeFrame(std::span<const std::byte> frame, int64_t utcTicks) {
    if (finished_) throw std::logic_error("SER writer already finished");
    if (frame.size() != header_.frameBytes())
        throw std::invalid_argument(std::format("frame is {} bytes, expected {}", frame.size(), header_.frameBytes()));
    out_.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
    if (!out_) throw std::runtime_error("write failed: " + path_.string());
    timestamps_.push_back(utcTicks);
}

void Writer::finish() {
    if (finished_) return;
    finished_ = true;
    out_.write(reinterpret_cast<const char*>(timestamps_.data()), static_cast<std::streamsize>(timestamps_.size() * 8));
    header_.frameCount = static_cast<int32_t>(timestamps_.size());
    if (!timestamps_.empty() && header_.dateTimeUtcTicks == 0) {
        header_.dateTimeUtcTicks = timestamps_.front();
        if (header_.dateTimeTicks == 0) header_.dateTimeTicks = timestamps_.front();
    }
    auto raw = header_.encode();
    out_.seekp(0);
    out_.write(reinterpret_cast<const char*>(raw.data()), kHeaderSize);
    out_.flush();
    if (!out_) throw std::runtime_error("finishing SER file failed: " + path_.string());
    out_.close();
}

}  // namespace slopcap::ser

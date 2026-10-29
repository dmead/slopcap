#include "camera/sim_camera.hpp"
#include "util/nap.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <thread>

namespace slopcap {

namespace {

Bayer bayerOf(ser::ColorId c) {
    switch (c) {
        case ser::ColorId::BayerRGGB: return Bayer::RGGB;
        case ser::ColorId::BayerGRBG: return Bayer::GRBG;
        case ser::ColorId::BayerGBRG: return Bayer::GBRG;
        case ser::ColorId::BayerBGGR: return Bayer::BGGR;
        default: return Bayer::None;
    }
}

const ControlInfo kControls[] = {
    {"Gain", 0, 600, 1, 200, true, false},
    {"Exposure", 32, 2'000'000'000, 1, 10'000, true, false},   // microseconds
    {"Offset", 0, 200, 1, 1, true, false},
};

}  // namespace

SimCamera::SimCamera(const std::filesystem::path& serFile, Options options) : reader_(serFile), opt_(options) {
    const auto& h = reader_.header();
    if (ser::planes(h.colorId) != 1) throw CameraError("SimCamera replays mono or Bayer SER files, not RGB");
    if (reader_.frameCount() == 0) throw CameraError("SER file has no frames: " + serFile.string());
    srcDepth_ = h.bytesPerSample() == 2 ? 16 : 8;
    outDepth_ = srcDepth_;

    info_.vendor = "Simulated";
    info_.model = "SER replay: " + serFile.filename().string();
    info_.serial = "sim:" + serFile.filename().string();
    info_.sensorWidth = h.width;
    info_.sensorHeight = h.height;
    info_.adcBits = h.bitDepth;
    info_.bayer = bayerOf(h.colorId);
    info_.bins = {1};
    info_.bitDepths = {8, 16};
    roi_ = {0, 0, h.width, h.height, 1};
    for (const auto& c : kControls) controls_.emplace(c.name, *c.defaultValue);
    full_.resize(h.frameBytes());
    // A depth below 16 can mean LSB-aligned samples (the SER spec's reading: values up to 2^depth - 1), or
    // MSB-aligned ones under a header that gives the ADC's depth (SlopCap's own until late October 2026). The first
    // frame tells: LSB-aligned data has noise in the low bits and nothing above the depth's range.
    if (srcDepth_ == 16 && h.bitDepth < 16) {
        reader_.readFrame(0, full_);
        unsigned low = 0, top = 0;
        const unsigned lowMask = (1u << (16 - h.bitDepth)) - 1;
        for (std::size_t i = 0; i + 1 < full_.size(); i += 2) {
            const unsigned v = std::to_integer<unsigned>(full_[i]) | (std::to_integer<unsigned>(full_[i + 1]) << 8);
            low |= v & lowMask;
            top = std::max(top, v);
        }
        if (low != 0 && top < (1u << h.bitDepth)) lowShift_ = 16 - h.bitDepth;
    }
}

std::vector<ControlInfo> SimCamera::controls() const { return {std::begin(kControls), std::end(kControls)}; }

std::optional<double> SimCamera::control(std::string_view name) const {
    auto it = controls_.find(name);
    if (it == controls_.end()) return std::nullopt;
    return it->second;
}

void SimCamera::setControl(std::string_view name, double value) {
    for (const auto& c : kControls) {
        if (c.name != name) continue;
        if (value < c.min || value > c.max) throw CameraError(std::format("{} out of range {}..{}", name, c.min, c.max));
        controls_.find(name)->second = value;
        return;
    }
    throw CameraError(std::format("no control named {}", name));
}

void SimCamera::setRoi(const Roi& r) {
    if (streaming_) throw CameraError("stop the stream before changing the ROI");
    if (r.bin != 1) throw CameraError("SimCamera supports bin 1 only");
    if (r.width <= 0 || r.height <= 0 || r.x < 0 || r.y < 0 || r.x + r.width > info_.sensorWidth ||
        r.y + r.height > info_.sensorHeight)
        throw CameraError(std::format("ROI {}x{}+{}+{} outside the {}x{} sensor", r.width, r.height, r.x, r.y,
                                      info_.sensorWidth, info_.sensorHeight));
    if (info_.bayer != Bayer::None && (r.x % 2 || r.y % 2 || r.width % 2 || r.height % 2))
        throw CameraError("ROI must be even-aligned on a Bayer sensor");
    roi_ = r;
}

void SimCamera::setBitDepth(int bits) {
    if (streaming_) throw CameraError("stop the stream before changing the bit depth");
    if (bits != 8 && bits != 16) throw CameraError("bit depth must be 8 or 16");
    outDepth_ = bits;
}

void SimCamera::startStream() {
    streaming_ = true;
    start_ = Clock::now();
    next_ = 0;
    sequence_ = 0;
    dropped_ = 0;
}

SimCamera::Clock::duration SimCamera::offsetOf(int64_t index) const {
    const int64_t n = reader_.frameCount();
    const int64_t lap = index / n, i = index % n;
    const auto& ts = reader_.timestamps();
    Clock::duration perLap, within;
    if (ts && ts->size() >= static_cast<std::size_t>(n) && n > 1 && ts->back() > ts->front()) {
        const int64_t first = ts->front();
        // a lap lasts the recording plus one average frame interval
        const int64_t span = ts->back() - first;
        perLap = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<int64_t, std::ratio<1, ser::kTicksPerSecond>>(span + span / (n - 1)));
        within = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<int64_t, std::ratio<1, ser::kTicksPerSecond>>(std::max<int64_t>(0, (*ts)[static_cast<std::size_t>(i)] - first)));
    } else {
        const std::chrono::duration<double> period(1.0 / opt_.fallbackFps);
        perLap = std::chrono::duration_cast<Clock::duration>(period * static_cast<double>(n));
        within = std::chrono::duration_cast<Clock::duration>(period * static_cast<double>(i));
    }
    return perLap * lap + within;
}

ReadResult SimCamera::readFrame(std::span<std::byte> dst, FrameInfo& out, std::chrono::milliseconds timeout) {
    if (!streaming_) throw CameraError("stream not started");
    if (dst.size() < frameBytes()) throw CameraError("destination buffer smaller than one frame");
    const int64_t n = reader_.frameCount();
    if (!opt_.loop && next_ >= n) return ReadResult::EndOfStream;

    if (opt_.pace == Pace::Realtime) {
        const auto now = Clock::now();
        // A real camera keeps exposing while nobody reads: frames whose successor is already due were lost.
        while ((opt_.loop || next_ + 1 < n) && start_ + offsetOf(next_ + 1) <= now) {
            ++next_;
            ++dropped_;
        }
        if (!opt_.loop && next_ >= n) return ReadResult::EndOfStream;
        const auto due = start_ + offsetOf(next_);
        if (due > now) {
            if (due - now > timeout) {
                std::this_thread::sleep_for(timeout);
                return ReadResult::Timeout;
            }
            nap(std::chrono::duration_cast<std::chrono::microseconds>(due - now));   // sleep_until spins near the end on Windows
        }
    }

    reader_.readFrame(next_ % n, full_);
    copyRoi(dst);
    out.sequence = sequence_++;
    // When the light was captured, as the recording says: its per-frame timestamp, else its start time plus the
    // frame's place in it, else now. (The automatic target judges the sky, e.g. the Moon's phase, at this time.)
    const auto& ts = reader_.timestamps();
    const int64_t i = next_ % n;
    if (ts && ts->size() > static_cast<std::size_t>(i) && (*ts)[static_cast<std::size_t>(i)] > 0)
        out.timestamp = ser::fromTicks((*ts)[static_cast<std::size_t>(i)]);
    else if (reader_.header().dateTimeUtcTicks > 0)
        out.timestamp = ser::fromTicks(reader_.header().dateTimeUtcTicks) +
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(offsetOf(i));
    else
        out.timestamp = std::chrono::system_clock::now();
    out.roi = roi_;
    out.bitDepth = outDepth_;
    out.bytes = frameBytes();
    ++next_;
    return ReadResult::Ok;
}

void SimCamera::copyRoi(std::span<std::byte> dst) const {
    const std::size_t srcBps = srcDepth_ / 8, dstBps = outDepth_ / 8;
    const std::size_t srcStride = static_cast<std::size_t>(info_.sensorWidth) * srcBps;
    const std::size_t w = static_cast<std::size_t>(roi_.width);
    for (int row = 0; row < roi_.height; ++row) {
        const std::byte* s = full_.data() + static_cast<std::size_t>(roi_.y + row) * srcStride + static_cast<std::size_t>(roi_.x) * srcBps;
        std::byte* d = dst.data() + static_cast<std::size_t>(row) * w * dstBps;
        auto sample = [&](std::size_t i) {   // 16-bit, MSB-aligned
            return (std::to_integer<unsigned>(s[2 * i]) | (std::to_integer<unsigned>(s[2 * i + 1]) << 8)) << lowShift_;
        };
        if (srcBps == dstBps && lowShift_ == 0) {
            std::memcpy(d, s, w * srcBps);
        } else if (srcBps == 1) {   // 8 -> 16: value in the high byte, little-endian
            for (std::size_t i = 0; i < w; ++i) { d[2 * i] = std::byte{0}; d[2 * i + 1] = s[i]; }
        } else if (dstBps == 2) {   // LSB-aligned 16 -> MSB-aligned 16
            for (std::size_t i = 0; i < w; ++i) {
                const unsigned v = sample(i);
                d[2 * i] = static_cast<std::byte>(v & 0xff);
                d[2 * i + 1] = static_cast<std::byte>((v >> 8) & 0xff);
            }
        } else {                    // 16 -> 8: the high byte
            for (std::size_t i = 0; i < w; ++i) d[i] = static_cast<std::byte>(std::min(255u, sample(i) >> 8));
        }
    }
}

}  // namespace slopcap

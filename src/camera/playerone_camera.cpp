#include "camera/playerone_camera.hpp"

#ifdef _WIN32
#define dllexport dllimport   // the header marks its declarations for export; this side imports them
#endif
#include <libplayerone/PlayerOneCamera.h>
#ifdef _WIN32
#undef dllexport
#endif

#include <algorithm>
#include <cstring>
#include <format>

namespace slopcap {

namespace {

void check(POAErrors code, std::string_view what) {
    if (code != POA_OK) throw CameraError(std::format("Player One {}: {}", what, POAGetErrorString(code)));
}

Bayer bayerOf(POABayerPattern p) {
    switch (p) {
        case POA_BAYER_RG: return Bayer::RGGB;
        case POA_BAYER_BG: return Bayer::BGGR;
        case POA_BAYER_GR: return Bayer::GRBG;
        case POA_BAYER_GB: return Bayer::GBRG;
        case POA_BAYER_MONO: break;
    }
    return Bayer::None;
}

double asDouble(const POAConfigValue& v, int type) {
    return type == VAL_FLOAT ? v.floatValue : type == VAL_BOOL ? (v.boolValue == POA_TRUE ? 1.0 : 0.0) : static_cast<double>(v.intValue);
}

POAConfigValue valueOf(double d, int type) {
    POAConfigValue v{};
    if (type == VAL_FLOAT) v.floatValue = d;
    else if (type == VAL_BOOL) v.boolValue = d != 0 ? POA_TRUE : POA_FALSE;
    else v.intValue = static_cast<long>(d);
    return v;
}

/// Configs that aren't settings of the picture: the exposure again in seconds (POA_EXP), the flips (which act on being
/// written, whatever the value), the ST4 guide port, the deprecated heater switch.
bool skipped(POAConfig id) {
    switch (id) {
        case POA_EXP:
        case POA_FLIP_NONE:
        case POA_FLIP_HORI:
        case POA_FLIP_VERT:
        case POA_FLIP_BOTH:
        case POA_GUIDE_NORTH:
        case POA_GUIDE_SOUTH:
        case POA_GUIDE_EAST:
        case POA_GUIDE_WEST:
        case POA_HEATER: return true;
        default: return false;
    }
}

}  // namespace

std::vector<PlayerOneCameraEntry> PlayerOneCamera::list() {
    std::vector<PlayerOneCameraEntry> out;
    const int n = POAGetCameraCount();
    for (int i = 0; i < n; ++i) {
        POACameraProperties p{};
        if (POAGetCameraProperties(i, &p) == POA_OK) out.push_back({i, p.cameraModelName});
    }
    return out;
}

PlayerOneCamera::PlayerOneCamera(int index) {
    if (index < 0 || index >= POAGetCameraCount()) throw CameraError(std::format("no Player One camera at index {}", index));
    POACameraProperties p{};
    check(POAGetCameraProperties(index, &p), "camera properties");
    id_ = p.cameraID;
    check(POAOpenCamera(id_), "open (in use by another program?)");
    if (const auto e = POAInitCamera(id_); e != POA_OK) {
        POACloseCamera(id_);
        check(e, "init");
    }

    info_.vendor = "Player One";
    info_.model = p.cameraModelName;
    info_.serial = p.SN[0] ? std::string(p.SN, strnlen(p.SN, sizeof p.SN)) : std::format("{}#{}", info_.model, id_);
    info_.sensorWidth = p.maxWidth;
    info_.sensorHeight = p.maxHeight;
    info_.pixelUm = p.pixelSize;
    info_.adcBits = p.bitDepth;
    info_.bayer = p.isColorCamera == POA_TRUE ? bayerOf(p.bayerPattern) : Bayer::None;
    for (int b : p.bins) {
        if (b == 0) break;
        info_.bins.push_back(b);
    }
    if (info_.bins.empty()) info_.bins.push_back(1);
    for (POAImgFormat f : p.imgFormats) {
        if (f == POA_END) break;
        if (f == POA_RAW8) info_.bitDepths.push_back(8);
        if (f == POA_RAW16) info_.bitDepths.push_back(16);
    }

    int n = 0;
    POAGetConfigsCount(id_, &n);
    for (int i = 0; i < n; ++i) {
        POAConfigAttributes a{};
        if (POAGetConfigAttributes(id_, i, &a) != POA_OK || skipped(a.configID)) continue;
        ControlInfo c;
        c.name = a.szConfName;
        c.min = asDouble(a.minValue, a.valueType);
        c.max = asDouble(a.maxValue, a.valueType);
        c.step = a.valueType == VAL_FLOAT ? 0 : 1;
        c.defaultValue = asDouble(a.defaultValue, a.valueType);
        c.writable = a.isWritable == POA_TRUE;
        c.autoSupported = a.isSupportAuto == POA_TRUE;
        controls_.push_back(c);
        configs_.emplace(a.szConfName, Config{a.configID, a.valueType});
    }

    int modes = 0;
    if (POAGetSensorModeCount(id_, &modes) == POA_OK)
        for (int i = 0; i < modes; ++i) {
            POASensorModeInfo m{};
            if (POAGetSensorModeInfo(id_, i, &m) == POA_OK) modes_.push_back({i, m.name, std::nullopt, std::nullopt});
        }

    roi_ = {0, 0, info_.sensorWidth, info_.sensorHeight, 1};
    bits_ = 8;
    try {
        applyFormat();
    } catch (...) {
        POACloseCamera(id_);   // the destructor won't run
        throw;
    }
}

PlayerOneCamera::~PlayerOneCamera() {
    if (id_ < 0) return;
    if (streaming_) POAStopExposure(id_);
    POACloseCamera(id_);
}

const PlayerOneCamera::Config& PlayerOneCamera::config(std::string_view name) const {
    const auto it = configs_.find(name);
    if (it == configs_.end()) throw CameraError(std::format("{} has no control {}", info_.model, name));
    return it->second;
}

std::optional<double> PlayerOneCamera::control(std::string_view name) const {
    const auto it = configs_.find(name);
    if (it == configs_.end()) return std::nullopt;
    POAConfigValue v{};
    POABool a = POA_FALSE;
    if (POAGetConfig(id_, static_cast<POAConfig>(it->second.id), &v, &a) != POA_OK) return std::nullopt;
    return asDouble(v, it->second.type);
}

void PlayerOneCamera::setControl(std::string_view name, double value) {
    const auto& c = config(name);
    const auto ci = std::ranges::find(controls_, name, &ControlInfo::name);
    if (!ci->writable) throw CameraError(std::format("{} is read-only", name));
    if (value < ci->min || value > ci->max) throw CameraError(std::format("{} out of range {}..{}", name, ci->min, ci->max));
    const bool wasAuto = controlAuto(name).value_or(false);
    check(POASetConfig(id_, static_cast<POAConfig>(c.id), valueOf(value, c.type), wasAuto ? POA_TRUE : POA_FALSE), std::format("set {}", name));
}

std::optional<bool> PlayerOneCamera::controlAuto(std::string_view name) const {
    const auto it = configs_.find(name);
    if (it == configs_.end()) return std::nullopt;
    POAConfigValue v{};
    POABool a = POA_FALSE;
    if (POAGetConfig(id_, static_cast<POAConfig>(it->second.id), &v, &a) != POA_OK) return std::nullopt;
    return a == POA_TRUE;
}

void PlayerOneCamera::setControlAuto(std::string_view name, bool on) {
    const auto& c = config(name);
    const auto ci = std::ranges::find(controls_, name, &ControlInfo::name);
    if (!ci->autoSupported) throw CameraError(std::format("{} has no auto mode", name));
    POAConfigValue v{};
    POABool a = POA_FALSE;
    check(POAGetConfig(id_, static_cast<POAConfig>(c.id), &v, &a), std::format("get {}", name));
    check(POASetConfig(id_, static_cast<POAConfig>(c.id), v, on ? POA_TRUE : POA_FALSE), std::format("set {} auto", name));
}

int PlayerOneCamera::readoutMode() const {
    int m = 0;
    return !modes_.empty() && POAGetSensorMode(id_, &m) == POA_OK ? m : 0;
}

void PlayerOneCamera::setReadoutMode(int index) {
    if (modes_.empty()) {
        if (index != 0) throw CameraError("camera has no sensor modes");
        return;
    }
    if (streaming_) throw CameraError("stop the stream before changing the sensor mode");
    if (index < 0 || index >= static_cast<int>(modes_.size())) throw CameraError(std::format("no sensor mode {}", index));
    check(POASetSensorMode(id_, index), "set sensor mode");
}

std::vector<GainLandmark> PlayerOneCamera::gainLandmarks() const {
    using K = GainLandmark::Kind;
    int gHdr = 0, gHcg = 0, gUnity = 0, gLrn = 0, oHdr = 0, oHcg = 0, oUnity = 0, oLrn = 0;
    if (POAGetGainsAndOffsets(id_, &gHdr, &gHcg, &gUnity, &gLrn, &oHdr, &oHcg, &oUnity, &oLrn) != POA_OK) return {};
    return {
        {K::HighestDynamicRange, "Highest dynamic range", gHdr, oHdr, GainLandmark::Source::Driver},
        {K::HcgSwitch, "High conversion gain", gHcg, oHcg, GainLandmark::Source::Driver},
        {K::Unity, "Unity gain", gUnity, oUnity, GainLandmark::Source::Driver},
        {K::LowestReadNoise, "Lowest read noise", gLrn, oLrn, GainLandmark::Source::Driver},
    };
}

void PlayerOneCamera::setRoi(const Roi& r) {
    if (streaming_) throw CameraError("stop the stream before changing the ROI");
    if (std::ranges::find(info_.bins, r.bin) == info_.bins.end()) throw CameraError(std::format("bin {} not supported", r.bin));
    // Player One: width a multiple of 4 and height of 2, in binned pixels (the SDK would round them quietly)
    if (r.width <= 0 || r.height <= 0 || r.width % 4 || r.height % 2)
        throw CameraError(std::format("ROI {}x{}: width must be a multiple of 4 and height even", r.width, r.height));
    if (r.x < 0 || r.y < 0 || (r.x + r.width) * r.bin > info_.sensorWidth || (r.y + r.height) * r.bin > info_.sensorHeight)
        throw CameraError(std::format("ROI {}x{}+{}+{} (bin {}) outside the {}x{} sensor", r.width, r.height, r.x, r.y, r.bin,
                                      info_.sensorWidth, info_.sensorHeight));
    if (info_.bayer != Bayer::None && (r.x % 2 || r.y % 2)) throw CameraError("ROI start must be even on a colour sensor");
    const Roi old = roi_;
    roi_ = r;
    try {
        applyFormat();
    } catch (...) {
        roi_ = old;
        applyFormat();
        throw;
    }
}

void PlayerOneCamera::setBitDepth(int bits) {
    if (streaming_) throw CameraError("stop the stream before changing the bit depth");
    if (std::ranges::find(info_.bitDepths, bits) == info_.bitDepths.end()) throw CameraError(std::format("{}-bit output not supported", bits));
    bits_ = bits;
    applyFormat();
}

void PlayerOneCamera::applyFormat() {
    // binning first: it changes the size and start position, which are then set in binned pixels
    check(POASetImageBin(id_, roi_.bin), "set bin");
    check(POASetImageSize(id_, roi_.width, roi_.height), "set ROI size");
    check(POASetImageStartPos(id_, roi_.x, roi_.y), "set ROI start");
    check(POASetImageFormat(id_, bits_ > 8 ? POA_RAW16 : POA_RAW8), "set image format");
}

void PlayerOneCamera::startStream() {
    if (streaming_) return;
    check(POAStartExposure(id_, POA_FALSE), "start video");   // not a single frame: continuous
    streaming_ = true;
    sequence_ = 0;
    lastDropped_ = 0;
}

void PlayerOneCamera::stopStream() {
    if (!streaming_) return;
    droppedFrames();   // the final count, before the SDK resets it
    streaming_ = false;
    check(POAStopExposure(id_), "stop video");
}

ReadResult PlayerOneCamera::readFrame(std::span<std::byte> dst, FrameInfo& out, std::chrono::milliseconds timeout) {
    if (!streaming_) throw CameraError("stream not started");
    const std::size_t n = frameBytes();
    if (dst.size() < n) throw CameraError("destination buffer smaller than one frame");
    const auto e = POAGetImageData(id_, reinterpret_cast<unsigned char*>(dst.data()), static_cast<long>(n),
                                   static_cast<int>(std::clamp<int64_t>(timeout.count(), 0, 1'000'000)));
    if (e == POA_ERROR_TIMEOUT) return ReadResult::Timeout;
    check(e, "read frame");
    out.sequence = sequence_++;
    out.timestamp = std::chrono::system_clock::now();   // the SDK has no frame timestamps
    out.roi = roi_;
    out.bitDepth = bits_;
    out.bytes = n;
    return ReadResult::Ok;
}

uint64_t PlayerOneCamera::droppedFrames() const {
    int d = 0;
    if (streaming_ && POAGetDroppedImagesCount(id_, &d) == POA_OK) lastDropped_ = static_cast<uint64_t>(std::max(0, d));
    return lastDropped_;
}

}  // namespace slopcap

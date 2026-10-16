#include "camera/asi_camera.hpp"

#include <libasi/ASICamera2.h>

#include <algorithm>
#include <format>

namespace slopcap {

namespace {

std::string asiError(int code) {
    switch (code) {
        case ASI_SUCCESS: return "success";
        case ASI_ERROR_INVALID_INDEX: return "invalid index (no camera?)";
        case ASI_ERROR_INVALID_ID: return "invalid camera id";
        case ASI_ERROR_INVALID_CONTROL_TYPE: return "invalid control";
        case ASI_ERROR_CAMERA_CLOSED: return "camera not open";
        case ASI_ERROR_CAMERA_REMOVED: return "camera removed";
        case ASI_ERROR_INVALID_SIZE: return "invalid ROI size";
        case ASI_ERROR_INVALID_IMGTYPE: return "unsupported image format";
        case ASI_ERROR_OUTOF_BOUNDARY: return "ROI start position out of bounds";
        case ASI_ERROR_TIMEOUT: return "timeout";
        case ASI_ERROR_INVALID_SEQUENCE: return "stop capture first";
        case ASI_ERROR_BUFFER_TOO_SMALL: return "buffer too small";
        case ASI_ERROR_VIDEO_MODE_ACTIVE: return "video mode active";
        case ASI_ERROR_EXPOSURE_IN_PROGRESS: return "exposure in progress";
        case ASI_ERROR_GENERAL_ERROR: return "general error (value out of range?)";
        default: return std::format("error {}", code);
    }
}

void check(int code, std::string_view what) {
    if (code != ASI_SUCCESS) throw CameraError(std::format("ZWO {}: {}", what, asiError(code)));
}

Bayer bayerOf(ASI_BAYER_PATTERN p) {
    switch (p) {
        case ASI_BAYER_RG: return Bayer::RGGB;
        case ASI_BAYER_BG: return Bayer::BGGR;
        case ASI_BAYER_GR: return Bayer::GRBG;
        case ASI_BAYER_GB: return Bayer::GBRG;
    }
    return Bayer::None;
}

}  // namespace

std::vector<AsiCameraEntry> AsiCamera::list() {
    std::vector<AsiCameraEntry> out;
    const int n = ASIGetNumOfConnectedCameras();
    for (int i = 0; i < n; ++i) {
        ASI_CAMERA_INFO ci{};
        if (ASIGetCameraProperty(&ci, i) == ASI_SUCCESS) out.push_back({i, ci.Name});
    }
    return out;
}

AsiCamera::AsiCamera(int index) {
    if (index < 0 || index >= ASIGetNumOfConnectedCameras()) throw CameraError(std::format("no ZWO camera at index {}", index));
    ASI_CAMERA_INFO ci{};
    check(ASIGetCameraProperty(&ci, index), "camera property");
    id_ = ci.CameraID;
    check(ASIOpenCamera(id_), "open (in use by another program?)");
    if (int e = ASIInitCamera(id_); e != ASI_SUCCESS) {
        ASICloseCamera(id_);
        check(e, "init");
    }

    info_.vendor = "ZWO";
    info_.model = ci.Name;
    ASI_SN sn{};
    if (ASIGetSerialNumber(id_, &sn) == ASI_SUCCESS) {
        for (unsigned char b : sn.id) info_.serial += std::format("{:02x}", b);
    } else {
        info_.serial = std::format("{}#{}", ci.Name, id_);
    }
    info_.sensorWidth = static_cast<int>(ci.MaxWidth);
    info_.sensorHeight = static_cast<int>(ci.MaxHeight);
    info_.pixelUm = ci.PixelSize;
    info_.adcBits = ci.BitDepth;
    info_.bayer = ci.IsColorCam ? bayerOf(ci.BayerPattern) : Bayer::None;
    for (int b : ci.SupportedBins) {
        if (b == 0) break;
        info_.bins.push_back(b);
    }
    for (int f : ci.SupportedVideoFormat) {
        if (f == ASI_IMG_END) break;
        if (f == ASI_IMG_RAW8) info_.bitDepths.push_back(8);
        if (f == ASI_IMG_RAW16) info_.bitDepths.push_back(16);
    }

    int n = 0;
    ASIGetNumOfControls(id_, &n);
    for (int i = 0; i < n; ++i) {
        ASI_CONTROL_CAPS c{};
        if (ASIGetControlCaps(id_, i, &c) != ASI_SUCCESS) continue;
        ControlInfo ctl;
        ctl.name = c.Name;
        ctl.min = static_cast<double>(c.MinValue);
        ctl.max = static_cast<double>(c.MaxValue);
        ctl.step = 1;
        ctl.defaultValue = static_cast<double>(c.DefaultValue);
        ctl.writable = c.IsWritable;
        ctl.autoSupported = c.IsAutoSupported;
        controls_.push_back(ctl);
        controlTypes_.emplace(c.Name, static_cast<int>(c.ControlType));
    }

    roi_ = {0, 0, info_.sensorWidth, info_.sensorHeight, 1};
    bits_ = 8;
    applyFormat();
}

AsiCamera::~AsiCamera() {
    if (id_ < 0) return;
    if (streaming_) ASIStopVideoCapture(id_);
    ASICloseCamera(id_);
}

int AsiCamera::controlType(std::string_view name) const {
    auto it = controlTypes_.find(name);
    if (it == controlTypes_.end()) throw CameraError(std::format("{} has no control {}", info_.model, name));
    return it->second;
}

std::optional<double> AsiCamera::control(std::string_view name) const {
    auto it = controlTypes_.find(name);
    if (it == controlTypes_.end()) return std::nullopt;
    long v = 0;
    ASI_BOOL a = ASI_FALSE;
    if (ASIGetControlValue(id_, static_cast<ASI_CONTROL_TYPE>(it->second), &v, &a) != ASI_SUCCESS) return std::nullopt;
    return static_cast<double>(v);
}

void AsiCamera::setControl(std::string_view name, double value) {
    const auto type = static_cast<ASI_CONTROL_TYPE>(controlType(name));
    const auto ci = std::ranges::find(controls_, name, &ControlInfo::name);
    if (!ci->writable) throw CameraError(std::format("{} is read-only", name));
    if (value < ci->min || value > ci->max) throw CameraError(std::format("{} out of range {}..{}", name, ci->min, ci->max));
    const bool wasAuto = controlAuto(name).value_or(false);
    check(ASISetControlValue(id_, type, static_cast<long>(value), wasAuto ? ASI_TRUE : ASI_FALSE), std::format("set {}", name));
}

std::optional<bool> AsiCamera::controlAuto(std::string_view name) const {
    auto it = controlTypes_.find(name);
    if (it == controlTypes_.end()) return std::nullopt;
    long v = 0;
    ASI_BOOL a = ASI_FALSE;
    if (ASIGetControlValue(id_, static_cast<ASI_CONTROL_TYPE>(it->second), &v, &a) != ASI_SUCCESS) return std::nullopt;
    return a == ASI_TRUE;
}

void AsiCamera::setControlAuto(std::string_view name, bool on) {
    const auto type = static_cast<ASI_CONTROL_TYPE>(controlType(name));
    const auto ci = std::ranges::find(controls_, name, &ControlInfo::name);
    if (!ci->autoSupported) throw CameraError(std::format("{} has no auto mode", name));
    long v = 0;
    ASI_BOOL a = ASI_FALSE;
    check(ASIGetControlValue(id_, type, &v, &a), std::format("get {}", name));
    check(ASISetControlValue(id_, type, v, on ? ASI_TRUE : ASI_FALSE), std::format("set {} auto", name));
}

std::vector<ReadoutMode> AsiCamera::readoutModes() const {
    if (!controlTypes_.contains("HighSpeedMode")) return {};
    return {{0, "Normal (full ADC depth)", std::nullopt, std::nullopt}, {1, "High speed (reduced ADC depth)", std::nullopt, std::nullopt}};
}

int AsiCamera::readoutMode() const { return control("HighSpeedMode").value_or(0) != 0 ? 1 : 0; }

void AsiCamera::setReadoutMode(int index) {
    if (!controlTypes_.contains("HighSpeedMode")) {
        if (index != 0) throw CameraError("camera has no high-speed readout");
        return;
    }
    if (index != 0 && index != 1) throw CameraError("readout mode must be 0 (normal) or 1 (high speed)");
    setControl("HighSpeedMode", index);
}

std::vector<GainLandmark> AsiCamera::gainLandmarks() const {
    std::vector<GainLandmark> out;
    using K = GainLandmark::Kind;
    int offHdr = 0, offUnity = 0, gainLrn = 0, offLrn = 0;
    if (ASIGetGainOffset(id_, &offHdr, &offUnity, &gainLrn, &offLrn) == ASI_SUCCESS) {
        const auto g = std::ranges::find(controls_, "Gain", &ControlInfo::name);
        out.push_back({K::HighestDynamicRange, "Highest dynamic range", g != controls_.end() ? std::optional(g->min) : std::nullopt,
                       offHdr, GainLandmark::Source::Driver});
        out.push_back({K::Unity, "Unity gain (offset only; gain not reported)", std::nullopt, offUnity, GainLandmark::Source::Driver});
        out.push_back({K::LowestReadNoise, "Lowest read noise", gainLrn, offLrn, GainLandmark::Source::Driver});
    }
    int lg = 0, mg = 0, hg = 0, ho = 0;
    if (ASIGetLMHGainOffset(id_, &lg, &mg, &hg, &ho) == ASI_SUCCESS) {
        out.push_back({K::Preset, "Preset low", lg, std::nullopt, GainLandmark::Source::Driver});
        out.push_back({K::Preset, "Preset medium", mg, std::nullopt, GainLandmark::Source::Driver});
        out.push_back({K::Preset, "Preset high", hg, ho, GainLandmark::Source::Driver});
    }
    return out;
}

void AsiCamera::setRoi(const Roi& r) {
    if (streaming_) throw CameraError("stop the stream before changing the ROI");
    if (std::ranges::find(info_.bins, r.bin) == info_.bins.end()) throw CameraError(std::format("bin {} not supported", r.bin));
    // ZWO: width a multiple of 8 and height a multiple of 2 (in binned pixels); start position even on colour sensors.
    if (r.width <= 0 || r.height <= 0 || r.width % 8 || r.height % 2)
        throw CameraError(std::format("ROI {}x{}: width must be a multiple of 8 and height even", r.width, r.height));
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

void AsiCamera::setBitDepth(int bits) {
    if (streaming_) throw CameraError("stop the stream before changing the bit depth");
    if (std::ranges::find(info_.bitDepths, bits) == info_.bitDepths.end()) throw CameraError(std::format("{}-bit output not supported", bits));
    bits_ = bits;
    applyFormat();
}

void AsiCamera::applyFormat() {
    check(ASISetROIFormat(id_, roi_.width, roi_.height, roi_.bin, bits_ > 8 ? ASI_IMG_RAW16 : ASI_IMG_RAW8), "set ROI format");
    check(ASISetStartPos(id_, roi_.x, roi_.y), "set ROI start");
}

void AsiCamera::startStream() {
    if (streaming_) return;
    check(ASIStartVideoCapture(id_), "start video");
    streaming_ = true;
    sequence_ = 0;
    lastDropped_ = 0;
    ASIGetDroppedFrames(id_, &dropBase_);
}

void AsiCamera::stopStream() {
    if (!streaming_) return;
    droppedFrames();   // capture the final count before the SDK resets it
    streaming_ = false;
    check(ASIStopVideoCapture(id_), "stop video");
}

ReadResult AsiCamera::readFrame(std::span<std::byte> dst, FrameInfo& out, std::chrono::milliseconds timeout) {
    if (!streaming_) throw CameraError("stream not started");
    const std::size_t n = frameBytes();
    if (dst.size() < n) throw CameraError("destination buffer smaller than one frame");
    const int e = ASIGetVideoData(id_, reinterpret_cast<unsigned char*>(dst.data()), static_cast<long>(n),
                                  static_cast<int>(std::min<int64_t>(timeout.count(), 1'000'000)));
    if (e == ASI_ERROR_TIMEOUT) return ReadResult::Timeout;
    check(e, "read frame");
    out.sequence = sequence_++;
    out.timestamp = std::chrono::system_clock::now();   // the SDK has no frame timestamps
    out.roi = roi_;
    out.bitDepth = bits_;
    out.bytes = n;
    return ReadResult::Ok;
}

uint64_t AsiCamera::droppedFrames() const {
    int d = 0;
    if (streaming_ && ASIGetDroppedFrames(id_, &d) == ASI_SUCCESS) lastDropped_ = static_cast<uint64_t>(std::max(0, d - dropBase_));
    return lastDropped_;
}

}  // namespace slopcap

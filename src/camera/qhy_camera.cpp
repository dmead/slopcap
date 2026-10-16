#include "camera/qhy_camera.hpp"

#include <qhyccd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>

namespace slopcap {

namespace {

constexpr uint32_t OK = QHYCCD_SUCCESS;

// The SDK's global init/release, shared by every open camera.
std::mutex g_sdkMutex;
int g_sdkUsers = 0;

void sdkAcquire() {
    std::lock_guard lock(g_sdkMutex);
    if (g_sdkUsers == 0) {
        EnableQHYCCDMessage(false);   // the SDK otherwise prints debug chatter to stdout
        EnableQHYCCDLogFile(false);
    }
    if (g_sdkUsers++ == 0 && InitQHYCCDResource() != OK) {
        g_sdkUsers = 0;
        throw CameraError("QHY SDK init failed");
    }
}

void sdkRelease() {
    std::lock_guard lock(g_sdkMutex);
    if (--g_sdkUsers == 0) ReleaseQHYCCDResource();
}

struct SdkUser {
    SdkUser() { sdkAcquire(); }
    ~SdkUser() { sdkRelease(); }
};

qhyccd_handle* H(void* p) { return static_cast<qhyccd_handle*>(p); }

}  // namespace

// Controls exposed under the same names as the ZWO backend where they mean the same thing.
struct QhyParam {
    const char* name;
    CONTROL_ID id;
    bool writable;
};

namespace {
const QhyParam kParams[] = {
    {"Gain", CONTROL_GAIN, true},
    {"Offset", CONTROL_OFFSET, true},
    {"Exposure", CONTROL_EXPOSURE, true},          // microseconds, as for ZWO
    {"USBTraffic", CONTROL_USBTRAFFIC, true},
    {"Speed", CONTROL_SPEED, true},
    {"Gamma", CONTROL_GAMMA, true},
    {"WB_R", CONTROL_WBR, true},
    {"WB_G", CONTROL_WBG, true},
    {"WB_B", CONTROL_WBB, true},
    {"Brightness", CONTROL_BRIGHTNESS, true},
    {"Contrast", CONTROL_CONTRAST, true},
    {"TargetTemp", CONTROL_COOLER, true},
    {"Temperature", CONTROL_CURTEMP, false},
    {"CoolerPower", CONTROL_CURPWM, false},
    {"DDRBuffer", CONTROL_DDR, true},
    {"AmpGlow", CONTROL_AMPV, true},
    {"RowNoiseRemoval", CONTROL_ROWNOISERE, true},
};
}  // namespace

const QhyParam* QhyCamera::param(std::string_view name) const {
    for (const auto& p : kParams)
        if (name == p.name && IsQHYCCDControlAvailable(H(h_), p.id) == OK) return &p;
    return nullptr;
}

std::vector<QhyCameraEntry> QhyCamera::list() {
    SdkUser sdk;
    std::vector<QhyCameraEntry> out;
    const uint32_t n = ScanQHYCCD();
    for (uint32_t i = 0; i < n; ++i) {
        char id[64] = {}, model[64] = {};
        if (GetQHYCCDId(i, id) != OK) continue;
        GetQHYCCDModel(id, model);
        out.push_back({static_cast<int>(i), id, model[0] ? model : id});
    }
    return out;
}

QhyCamera::QhyCamera(int index) {
    sdkAcquire();
    try {
        const uint32_t n = ScanQHYCCD();
        if (index < 0 || static_cast<uint32_t>(index) >= n) throw CameraError(std::format("no QHY camera at index {}", index));
        char id[64] = {}, model[64] = {};
        if (GetQHYCCDId(static_cast<uint32_t>(index), id) != OK) throw CameraError("QHY: cannot read camera id");
        GetQHYCCDModel(id, model);
        h_ = OpenQHYCCD(id);
        if (!h_) throw CameraError(std::format("QHY open {} failed (in use by another program?)", id));

        info_.vendor = "QHY";
        info_.model = model[0] ? model : id;
        info_.serial = id;
        uint32_t nModes = 0, cur = 0;
        if (GetQHYCCDNumberOfReadModes(H(h_), &nModes) == OK) {
            for (uint32_t m = 0; m < nModes; ++m) {
                char name[128] = {};
                uint32_t w = 0, ht = 0;
                GetQHYCCDReadModeName(H(h_), m, name);
                ReadoutMode rm{static_cast<int>(m), name[0] ? name : std::format("mode {}", m), std::nullopt, std::nullopt};
                if (GetQHYCCDReadModeResolution(H(h_), m, &w, &ht) == OK && w && ht) { rm.width = static_cast<int>(w); rm.height = static_cast<int>(ht); }
                modes_.push_back(rm);
            }
        }
        if (GetQHYCCDReadMode(H(h_), &cur) == OK) mode_ = static_cast<int>(cur);
        initialise();
    } catch (...) {
        if (h_) CloseQHYCCD(H(h_));
        sdkRelease();
        throw;
    }
}

QhyCamera::~QhyCamera() {
    if (streaming_) StopQHYCCDLive(H(h_));
    CloseQHYCCD(H(h_));
    sdkRelease();
}

void QhyCamera::initialise() {
    if (!modes_.empty() && SetQHYCCDReadMode(H(h_), static_cast<uint32_t>(mode_)) != OK) throw CameraError("QHY: set read mode failed");
    if (SetQHYCCDStreamMode(H(h_), 1) != OK) throw CameraError("QHY: live mode not supported");
    if (InitQHYCCD(H(h_)) != OK) throw CameraError("QHY: init failed");

    double cw = 0, ch = 0, pw = 0, ph = 0;
    uint32_t iw = 0, ih = 0, bpp = 0;
    if (GetQHYCCDChipInfo(H(h_), &cw, &ch, &iw, &ih, &pw, &ph, &bpp) != OK) throw CameraError("QHY: chip info failed");
    info_.sensorWidth = static_cast<int>(iw);
    info_.sensorHeight = static_cast<int>(ih);
    info_.pixelUm = pw;
    const uint32_t bayer = IsQHYCCDControlAvailable(H(h_), CAM_COLOR);
    info_.bayer = bayer == BAYER_RG ? Bayer::RGGB : bayer == BAYER_GR ? Bayer::GRBG : bayer == BAYER_GB ? Bayer::GBRG
                : bayer == BAYER_BG ? Bayer::BGGR : Bayer::None;
    info_.bins.clear();
    const std::pair<CONTROL_ID, int> bins[] = {{CAM_BIN1X1MODE, 1}, {CAM_BIN2X2MODE, 2}, {CAM_BIN3X3MODE, 3}, {CAM_BIN4X4MODE, 4}};
    for (auto [c, b] : bins)
        if (IsQHYCCDControlAvailable(H(h_), c) == OK) info_.bins.push_back(b);
    if (info_.bins.empty()) info_.bins.push_back(1);
    info_.bitDepths.clear();
    if (IsQHYCCDControlAvailable(H(h_), CAM_8BITS) == OK) info_.bitDepths.push_back(8);
    if (IsQHYCCDControlAvailable(H(h_), CAM_16BITS) == OK) info_.bitDepths.push_back(16);
    if (info_.bitDepths.empty()) info_.bitDepths.push_back(8);
    if (IsQHYCCDControlAvailable(H(h_), OutputDataActualBits) == OK) info_.adcBits = static_cast<int>(GetQHYCCDParam(H(h_), OutputDataActualBits));

    if (roi_.width == 0 || roi_.x + roi_.width > info_.sensorWidth || roi_.y + roi_.height > info_.sensorHeight)
        roi_ = {0, 0, info_.sensorWidth, info_.sensorHeight, 1};
    if (std::ranges::find(info_.bitDepths, bits_) == info_.bitDepths.end()) bits_ = info_.bitDepths.front();
    applyFormat();
}

void QhyCamera::applyFormat() {
    if (SetQHYCCDBitsMode(H(h_), static_cast<uint32_t>(bits_)) != OK) throw CameraError(std::format("QHY: {}-bit mode failed", bits_));
    if (SetQHYCCDBinMode(H(h_), static_cast<uint32_t>(roi_.bin), static_cast<uint32_t>(roi_.bin)) != OK) throw CameraError("QHY: bin mode failed");
    if (SetQHYCCDResolution(H(h_), static_cast<uint32_t>(roi_.x), static_cast<uint32_t>(roi_.y), static_cast<uint32_t>(roi_.width),
                            static_cast<uint32_t>(roi_.height)) != OK)
        throw CameraError("QHY: set ROI failed");
    staging_.resize(std::max<std::size_t>(GetQHYCCDMemLength(H(h_)), frameBytes()));
}

std::vector<ControlInfo> QhyCamera::controls() const {
    std::vector<ControlInfo> out;
    for (const auto& p : kParams) {
        if (IsQHYCCDControlAvailable(H(h_), p.id) != OK) continue;
        ControlInfo c;
        c.name = p.name;
        double mn = 0, mx = 0, st = 0;
        if (GetQHYCCDParamMinMaxStep(H(h_), p.id, &mn, &mx, &st) == OK) { c.min = mn; c.max = mx; c.step = st; }
        c.writable = p.writable;
        out.push_back(c);
    }
    return out;
}

std::optional<double> QhyCamera::control(std::string_view name) const {
    const QhyParam* p = param(name);
    if (!p) return std::nullopt;
    const double v = GetQHYCCDParam(H(h_), p->id);
    if (v == static_cast<double>(QHYCCD_ERROR)) return std::nullopt;
    return v;
}

void QhyCamera::setControl(std::string_view name, double value) {
    const QhyParam* p = param(name);
    if (!p) throw CameraError(std::format("{} has no control {}", info_.model, name));
    if (!p->writable) throw CameraError(std::format("{} is read-only", name));
    double mn = 0, mx = 0, st = 0;
    if (GetQHYCCDParamMinMaxStep(H(h_), p->id, &mn, &mx, &st) == OK && (value < mn || value > mx))
        throw CameraError(std::format("{} out of range {}..{}", name, mn, mx));
    if (SetQHYCCDParam(H(h_), p->id, value) != OK) throw CameraError(std::format("QHY: set {} failed", name));
}

void QhyCamera::setReadoutMode(int index) {
    if (streaming_) throw CameraError("stop the stream before changing the read mode");
    if (modes_.empty()) {
        if (index != 0) throw CameraError("camera has no read modes");
        return;
    }
    if (index < 0 || index >= static_cast<int>(modes_.size())) throw CameraError(std::format("no read mode {}", index));
    mode_ = index;
    initialise();   // QHY requires the read mode before InitQHYCCD
}

std::vector<GainLandmark> QhyCamera::gainLandmarks() const {
    using K = GainLandmark::Kind;
    std::vector<GainLandmark> out;
    if (IsQHYCCDControlAvailable(H(h_), DefaultGain) == OK) {
        std::optional<double> off;
        if (IsQHYCCDControlAvailable(H(h_), DefaultOffset) == OK) off = GetQHYCCDParam(H(h_), DefaultOffset);
        out.push_back({K::VendorDefault, "Vendor default", GetQHYCCDParam(H(h_), DefaultGain), off, GainLandmark::Source::Driver});
    }
    const bool rn = IsQHYCCDControlAvailable(H(h_), CAM_CurveReadoutNoise) == OK;
    const bool sg = IsQHYCCDControlAvailable(H(h_), CAM_CurveSystemGain) == OK;
    double mn = 0, mx = 0, st = 0;
    if ((!rn && !sg) || GetQHYCCDParamMinMaxStep(H(h_), CONTROL_GAIN, &mn, &mx, &st) != OK) return out;
    const double stride = std::max(st > 0 ? st : 1.0, (mx - mn) / 400.0);
    double prevRn = NAN, bigDrop = 0, hcg = NAN, prevSg = NAN, unity = NAN, minRn = INFINITY, minRnGain = NAN;
    for (double g = mn; g <= mx + 1e-9; g += stride) {
        double r = NAN, s = NAN;
        if (rn && QHYCCD_curveReadoutNoise(H(h_), g, &r) != OK) r = NAN;
        if (sg && QHYCCD_curveSystemGain(H(h_), g, &s) != OK) s = NAN;
        if (!std::isnan(r)) {
            if (!std::isnan(prevRn) && prevRn - r > bigDrop) { bigDrop = prevRn - r; hcg = g; }
            if (r < minRn) { minRn = r; minRnGain = g; }
            prevRn = r;
        }
        if (!std::isnan(s)) {
            if (!std::isnan(prevSg) && std::isnan(unity) && prevSg >= 1.0 && s < 1.0) unity = g;
            prevSg = s;
        }
    }
    if (!std::isnan(unity)) out.push_back({K::Unity, "Unity gain (from e-/ADU curve)", unity, std::nullopt, GainLandmark::Source::Driver});
    if (!std::isnan(hcg) && bigDrop > 0.3) out.push_back({K::HcgSwitch, "HCG switch (read-noise step)", hcg, std::nullopt, GainLandmark::Source::Driver});
    if (!std::isnan(minRnGain)) out.push_back({K::LowestReadNoise, "Lowest read noise", minRnGain, std::nullopt, GainLandmark::Source::Driver});
    return out;
}

void QhyCamera::setRoi(const Roi& r) {
    if (streaming_) throw CameraError("stop the stream before changing the ROI");
    if (std::ranges::find(info_.bins, r.bin) == info_.bins.end()) throw CameraError(std::format("bin {} not supported", r.bin));
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

void QhyCamera::setBitDepth(int bits) {
    if (streaming_) throw CameraError("stop the stream before changing the bit depth");
    if (std::ranges::find(info_.bitDepths, bits) == info_.bitDepths.end()) throw CameraError(std::format("{}-bit output not supported", bits));
    bits_ = bits;
    applyFormat();
}

void QhyCamera::startStream() {
    if (streaming_) return;
    if (BeginQHYCCDLive(H(h_)) != OK) throw CameraError("QHY: start live mode failed");
    streaming_ = true;
    sequence_ = 0;
}

void QhyCamera::stopStream() {
    if (!streaming_) return;
    streaming_ = false;
    if (StopQHYCCDLive(H(h_)) != OK) throw CameraError("QHY: stop live mode failed");
}

ReadResult QhyCamera::readFrame(std::span<std::byte> dst, FrameInfo& out, std::chrono::milliseconds timeout) {
    if (!streaming_) throw CameraError("stream not started");
    const std::size_t n = frameBytes();
    if (dst.size() < n) throw CameraError("destination buffer smaller than one frame");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        uint32_t w = 0, ht = 0, bpp = 0, ch = 0;
        if (GetQHYCCDLiveFrame(H(h_), &w, &ht, &bpp, &ch, reinterpret_cast<uint8_t*>(staging_.data())) == OK) {
            const std::size_t got = static_cast<std::size_t>(w) * ht * ch * ((bpp + 7) / 8);
            if (got != n)
                throw CameraError(std::format("QHY delivered {}x{} {}-bit, expected {}x{} {}-bit", w, ht, bpp, roi_.width, roi_.height, bits_));
            std::memcpy(dst.data(), staging_.data(), n);
            out.sequence = sequence_++;
            out.timestamp = std::chrono::system_clock::now();
            out.roi = roi_;
            out.bitDepth = bits_;
            out.bytes = n;
            return ReadResult::Ok;
        }
        if (std::chrono::steady_clock::now() >= deadline) return ReadResult::Timeout;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

}  // namespace slopcap

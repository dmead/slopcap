// QHY probe. Read-only: opens each camera, lists read modes, feature flags and controls, derives gain landmarks
// from the SDK's per-gain curves (read noise, e-/ADU, full well) when the camera provides them, then closes it.
#include "report.hpp"

#include <chrono>
#include <cmath>
#include <format>
#include <qhyccd.h>
#include <thread>

namespace camprobe {

namespace {

constexpr uint32_t OK = QHYCCD_SUCCESS;

struct Named { CONTROL_ID id; const char* name; };

// Parameters with a value (read with GetQHYCCDParam / GetQHYCCDParamMinMaxStep).
const Named kParams[] = {
    {CONTROL_GAIN, "Gain"}, {CONTROL_OFFSET, "Offset"}, {CONTROL_EXPOSURE, "Exposure (us)"},
    {CONTROL_SPEED, "USB speed"}, {CONTROL_USBTRAFFIC, "USB traffic"}, {CONTROL_TRANSFERBIT, "Transfer bits"},
    {CONTROL_BRIGHTNESS, "Brightness"}, {CONTROL_CONTRAST, "Contrast"}, {CONTROL_GAMMA, "Gamma"},
    {CONTROL_WBR, "WB red"}, {CONTROL_WBG, "WB green"}, {CONTROL_WBB, "WB blue"},
    {CONTROL_CHANNELS, "Channels"}, {CONTROL_ROWNOISERE, "Row noise removal"}, {CONTROL_DDR, "DDR buffer"},
    {CONTROL_CURTEMP, "Sensor temp (C)"}, {CONTROL_CURPWM, "Cooler PWM"}, {CONTROL_MANULPWM, "Manual PWM"},
    {CONTROL_COOLER, "Cooler target (C)"}, {CONTROL_AMPV, "Amp glow control"}, {CONTROL_VCAM, "VCAM"},
    {CONTROL_GAINdB, "Gain (dB)"}, {CONTROL_HDR, "HDR combine"}, {CONTROL_AUTOEXPOSURE, "Auto exposure"},
    {CONTROL_AUTOWHITEBALANCE, "Auto white balance"}, {CONTROL_ImageStabilization, "Image stabilisation"},
    {CAM_LIGHT_PERFORMANCE_MODE, "Light performance mode"}, {DefaultGain, "Default gain"},
    {DefaultOffset, "Default offset"}, {OutputDataActualBits, "ADC bits (actual)"},
};

// Feature flags (present / absent).
const Named kFlags[] = {
    {CAM_8BITS, "8-bit"}, {CAM_16BITS, "16-bit"}, {CAM_LIVEVIDEOMODE, "live video"},
    {CAM_SINGLEFRAMEMODE, "single frame"}, {CAM_LIGHT_PERFORMANCE_MODE, "light performance mode"},
    {CAM_TRIGER_INTERFACE, "trigger"}, {CAM_GPS, "GPS"}, {CAM_HUMIDITY, "humidity sensor"},
    {CAM_MECHANICALSHUTTER, "mechanical shutter"}, {CAM_CHIPTEMPERATURESENSOR_INTERFACE, "chip temp sensor"},
    {CAM_GainDBConversion, "gain<->dB conversion"}, {CAM_CurveSystemGain, "e-/ADU curve"},
    {CAM_CurveFullWell, "full-well curve"}, {CAM_CurveReadoutNoise, "read-noise curve"},
    {CAM_USBREADOUTSLOWEST_INTERFACE, "slowest-readout option"}, {CAM_IGNOREOVERSCAN_INTERFACE, "overscan control"},
    {CAM_FINETONE_INTERFACE, "fine-tone control"},
};

const Named kBins[] = {
    {CAM_BIN1X1MODE, "1"}, {CAM_BIN2X2MODE, "2"}, {CAM_BIN3X3MODE, "3"}, {CAM_BIN4X4MODE, "4"},
    {CAM_BIN6X6MODE, "6"}, {CAM_BIN8X8MODE, "8"},
};

bool has(qhyccd_handle* h, CONTROL_ID id) { return IsQHYCCDControlAvailable(h, id) == OK; }

/// Derive landmarks from the SDK's per-gain curves by sampling the gain range.
void curveLandmarks(qhyccd_handle* h, CameraReport& c, double gMin, double gMax, double gStep) {
    bool rn = has(h, CAM_CurveReadoutNoise), sg = has(h, CAM_CurveSystemGain), fw = has(h, CAM_CurveFullWell);
    if (!rn && !sg && !fw) return;
    if (gStep <= 0) gStep = 1;
    int samples = static_cast<int>((gMax - gMin) / gStep) + 1;
    double stride = samples > 400 ? (gMax - gMin) / 399.0 : gStep;

    double prevRn = NAN, bigDrop = 0, hcgGain = NAN, unityGain = NAN, bestDr = -1, drGain = NAN;
    double minRn = INFINITY, minRnGain = NAN;
    double prevSg = NAN;
    for (double g = gMin; g <= gMax + 1e-9; g += stride) {
        double r = NAN, s = NAN, f = NAN;
        if (rn && QHYCCD_curveReadoutNoise(h, g, &r) != OK) r = NAN;
        if (sg && QHYCCD_curveSystemGain(h, g, &s) != OK) s = NAN;
        if (fw && QHYCCD_curveFullWell(h, g, &f) != OK) f = NAN;
        if (!std::isnan(r)) {
            if (!std::isnan(prevRn) && prevRn - r > bigDrop) { bigDrop = prevRn - r; hcgGain = g; }
            if (r < minRn) { minRn = r; minRnGain = g; }
            prevRn = r;
        }
        if (!std::isnan(s)) {
            if (!std::isnan(prevSg) && std::isnan(unityGain) && prevSg >= 1.0 && s < 1.0) unityGain = g;
            prevSg = s;
        }
        if (!std::isnan(r) && !std::isnan(f) && r > 0) {
            double dr = 20.0 * std::log10(f / r);
            if (dr > bestDr) { bestDr = dr; drGain = g; }
        }
    }
    if (!std::isnan(drGain))
        c.gainLandmarks.push_back({std::format("Highest dynamic range ({:.1f} dB)", bestDr), drGain, std::nullopt, "curves: max 20log10(fullwell/readnoise)"});
    if (!std::isnan(unityGain))
        c.gainLandmarks.push_back({"Unity gain (1 e-/ADU)", unityGain, std::nullopt, "curve: QHYCCD_curveSystemGain crosses 1"});
    // A real conversion-gain switch shows up as one step that is much larger than the gradual decline elsewhere.
    if (!std::isnan(hcgGain) && bigDrop > 0.3)
        c.gainLandmarks.push_back({std::format("HCG switch (read noise -{:.2f} e-)", bigDrop), hcgGain, std::nullopt, "curve: largest read-noise step"});
    if (!std::isnan(minRnGain))
        c.gainLandmarks.push_back({std::format("Lowest read noise ({:.2f} e-)", minRn), minRnGain, std::nullopt, "curve: QHYCCD_curveReadoutNoise minimum"});
}

}  // namespace

ProbeResult probeQhy() {
    ProbeResult r;
    r.vendor = "QHY";
    r.sdkAvailable = true;
    uint32_t y = 0, m = 0, d = 0, s = 0;
    GetQHYCCDSDKVersion(&y, &m, &d, &s);
    r.sdkVersion = std::format("{}.{}.{}.{}", y, m, d, s);
    if (InitQHYCCDResource() != OK) { r.errors.push_back("InitQHYCCDResource failed"); return r; }

    uint32_t n = 0;
#ifdef __APPLE__
    // macOS: upload firmware (built into libqhyccd), then wait for the camera to come back on USB (as PHD2 does).
    // With no camera that needs it the upload "fails", which is no error: scan once and move on.
    if (OSXInitQHYCCDFirmwareArray() == OK)
        for (int i = 0; i < 20 && (n = ScanQHYCCD()) == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    else
        n = ScanQHYCCD();
#else
    n = ScanQHYCCD();
#endif
    for (uint32_t i = 0; i < n; ++i) {
        char id[64] = {}, model[64] = {};
        if (GetQHYCCDId(i, id) != OK) { r.errors.push_back(std::format("camera {}: id read failed", i)); continue; }
        GetQHYCCDModel(id, model);
        qhyccd_handle* h = OpenQHYCCD(id);
        if (!h) { r.errors.push_back(std::format("{}: open failed (in use by another program?)", id)); continue; }

        CameraReport c;
        c.vendor = r.vendor;
        c.sdkVersion = r.sdkVersion;
        c.model = model[0] ? model : id;
        c.id = id;
        char sensor[64] = {};
        if (GetQHYCCDSensorName(h, sensor) == OK && sensor[0]) c.capabilities.push_back(std::string("sensor ") + sensor);

        // Read modes are listed before init (the mode is chosen before InitQHYCCD in normal use).
        uint32_t nModes = 0, curMode = 0;
        bool haveCur = GetQHYCCDReadMode(h, &curMode) == OK;
        if (GetQHYCCDNumberOfReadModes(h, &nModes) == OK) {
            for (uint32_t k = 0; k < nModes; ++k) {
                char name[128] = {};
                uint32_t w = 0, ht = 0;
                GetQHYCCDReadModeName(h, k, name);
                ReadoutMode rm{static_cast<int>(k), name[0] ? name : std::format("mode {}", k), std::nullopt, std::nullopt, haveCur && k == curMode};
                if (GetQHYCCDReadModeResolution(h, k, &w, &ht) == OK && w && ht) { rm.width = static_cast<int>(w); rm.height = static_cast<int>(ht); }
                c.readoutModes.push_back(rm);
            }
        }

        SetQHYCCDStreamMode(h, 1);   // live/video mode, as SlopCap would use it; some controls differ in single-frame mode
        if (InitQHYCCD(h) != OK) { r.errors.push_back(std::format("{}: init failed", id)); CloseQHYCCD(h); continue; }
        c.notes.push_back("probed in live (video) stream mode");

        double cw = 0, ch = 0, pw = 0, ph = 0;
        uint32_t iw = 0, ih = 0, bpp = 0;
        if (GetQHYCCDChipInfo(h, &cw, &ch, &iw, &ih, &pw, &ph, &bpp) == OK) {
            c.sensorWidth = static_cast<int>(iw);
            c.sensorHeight = static_cast<int>(ih);
            c.pixelUm = pw;
        }
        uint32_t bayer = IsQHYCCDControlAvailable(h, CAM_COLOR);
        if (bayer == BAYER_GB || bayer == BAYER_GR || bayer == BAYER_BG || bayer == BAYER_RG) {
            c.color = true;
            c.bayer = bayer == BAYER_GB ? "GBRG" : bayer == BAYER_GR ? "GRBG" : bayer == BAYER_BG ? "BGGR" : "RGGB";
        }
        if (has(h, CAM_8BITS)) c.formats.push_back("RAW8");
        if (has(h, CAM_16BITS)) c.formats.push_back("RAW16");
        for (const auto& b : kBins) if (has(h, b.id)) c.bins.push_back(std::stoi(b.name));
        for (const auto& f : kFlags) if (has(h, f.id)) c.capabilities.push_back(f.name);

        double gMin = 0, gMax = 0, gStep = 0;
        bool haveGain = false;
        for (const auto& p : kParams) {
            if (!has(h, p.id)) continue;
            Control k;
            k.name = p.name;
            double mn = 0, mx = 0, st = 0;
            if (GetQHYCCDParamMinMaxStep(h, p.id, &mn, &mx, &st) == OK) { k.min = mn; k.max = mx; k.step = st; }
            double v = GetQHYCCDParam(h, p.id);
            if (v != static_cast<double>(QHYCCD_ERROR)) k.current = v;
            if (p.id == CONTROL_CURTEMP || p.id == CONTROL_CURPWM || p.id == DefaultGain || p.id == DefaultOffset || p.id == OutputDataActualBits)
                k.writable = false;
            if (p.id == CONTROL_GAIN) { gMin = mn; gMax = mx; gStep = st; haveGain = true; }
            if (p.id == OutputDataActualBits && k.current) c.adcBits = static_cast<int>(*k.current);
            c.controls.push_back(k);
        }
        if (!c.adcBits) c.notes.push_back(std::format("ADC depth not reported by the SDK (current output {}-bit)", bpp));

        if (has(h, DefaultGain)) {
            double g = GetQHYCCDParam(h, DefaultGain);
            double o = has(h, DefaultOffset) ? GetQHYCCDParam(h, DefaultOffset) : NAN;
            c.gainLandmarks.push_back({"Vendor default", g, std::isnan(o) ? std::nullopt : std::optional<double>(o), "DefaultGain / DefaultOffset"});
        }
        if (haveGain) curveLandmarks(h, c, gMin, gMax, gStep);
        if (!c.readoutModes.empty())
            c.notes.push_back("read modes are the camera's own named modes (e.g. high gain / extended full well); set them before InitQHYCCD");

        CloseQHYCCD(h);
        r.cameras.push_back(std::move(c));
    }
    ReleaseQHYCCDResource();
    return r;
}

}  // namespace camprobe

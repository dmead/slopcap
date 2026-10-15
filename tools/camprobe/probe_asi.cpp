// ZWO ASI probe. Read-only: opens each camera, reads properties, controls and preset gains, closes it.
#include "report.hpp"

#include <cmath>
#include <format>
#include <libasi/ASICamera2.h>

namespace camprobe {

namespace {

const char* formatName(ASI_IMG_TYPE t) {
    switch (t) {
        case ASI_IMG_RAW8: return "RAW8";
        case ASI_IMG_RGB24: return "RGB24";
        case ASI_IMG_RAW16: return "RAW16";
        case ASI_IMG_Y8: return "Y8";
        default: return "?";
    }
}

const char* bayerName(ASI_BAYER_PATTERN b) {
    switch (b) {
        case ASI_BAYER_RG: return "RGGB";
        case ASI_BAYER_BG: return "BGGR";
        case ASI_BAYER_GR: return "GRBG";
        case ASI_BAYER_GB: return "GBRG";
    }
    return "?";
}

const char* modeName(ASI_CAMERA_MODE m) {
    switch (m) {
        case ASI_MODE_NORMAL: return "normal";
        case ASI_MODE_TRIG_SOFT_EDGE: return "soft-edge";
        case ASI_MODE_TRIG_RISE_EDGE: return "rising-edge";
        case ASI_MODE_TRIG_FALL_EDGE: return "falling-edge";
        case ASI_MODE_TRIG_SOFT_LEVEL: return "soft-level";
        case ASI_MODE_TRIG_HIGH_LEVEL: return "high-level";
        case ASI_MODE_TRIG_LOW_LEVEL: return "low-level";
        default: return "?";
    }
}

}  // namespace

ProbeResult probeAsi() {
    ProbeResult r;
    r.vendor = "ZWO ASI";
    r.sdkAvailable = true;
    r.sdkVersion = ASIGetSDKVersion();
    int n = ASIGetNumOfConnectedCameras();
    for (int i = 0; i < n; ++i) {
        ASI_CAMERA_INFO info{};
        if (ASIGetCameraProperty(&info, i) != ASI_SUCCESS) { r.errors.push_back(std::format("camera {}: property read failed", i)); continue; }
        int id = info.CameraID;
        if (ASIOpenCamera(id) != ASI_SUCCESS) { r.errors.push_back(std::format("{}: open failed (in use by another program?)", info.Name)); continue; }
        if (ASIInitCamera(id) != ASI_SUCCESS) { r.errors.push_back(std::format("{}: init failed", info.Name)); ASICloseCamera(id); continue; }

        CameraReport c;
        c.vendor = r.vendor;
        c.sdkVersion = r.sdkVersion;
        c.model = info.Name;
        c.id = std::format("camera {}", id);
        ASI_SN sn{};
        if (ASIGetSerialNumber(id, &sn) == ASI_SUCCESS) {
            std::string s;
            for (unsigned char b : sn.id) s += std::format("{:02x}", b);
            c.id += " sn " + s;
        }
        c.sensorWidth = static_cast<int>(info.MaxWidth);
        c.sensorHeight = static_cast<int>(info.MaxHeight);
        c.pixelUm = info.PixelSize;
        c.adcBits = info.BitDepth;
        c.color = info.IsColorCam;
        if (c.color) c.bayer = bayerName(info.BayerPattern);
        for (int k = 0; k < 8 && info.SupportedVideoFormat[k] != ASI_IMG_END; ++k) c.formats.push_back(formatName(info.SupportedVideoFormat[k]));
        for (int k = 0; k < 16 && info.SupportedBins[k] != 0; ++k) c.bins.push_back(info.SupportedBins[k]);
        if (info.IsUSB3Camera) c.capabilities.push_back(info.IsUSB3Host ? "USB3 (on USB3 host)" : "USB3 camera on USB2 host");
        if (info.IsCoolerCam) c.capabilities.push_back("cooler");
        if (info.ST4Port) c.capabilities.push_back("ST4 guide port");
        if (info.MechanicalShutter) c.capabilities.push_back("mechanical shutter");
        if (info.IsTriggerCam) c.capabilities.push_back("trigger input");
        c.capabilities.push_back(std::format("ElecPerADU {:.3f} at gain 0", info.ElecPerADU));

        int nc = 0;
        ASIGetNumOfControls(id, &nc);
        std::optional<double> highSpeed;
        long gainMin = 0;
        for (int k = 0; k < nc; ++k) {
            ASI_CONTROL_CAPS caps{};
            if (ASIGetControlCaps(id, k, &caps) != ASI_SUCCESS) continue;
            Control ctl;
            ctl.name = caps.Name;
            ctl.description = caps.Description;
            ctl.min = caps.MinValue;
            ctl.max = caps.MaxValue;
            ctl.step = 1;
            ctl.defaultValue = caps.DefaultValue;
            ctl.writable = caps.IsWritable;
            ctl.autoSupported = caps.IsAutoSupported;
            long v = 0;
            ASI_BOOL a = ASI_FALSE;
            if (ASIGetControlValue(id, caps.ControlType, &v, &a) == ASI_SUCCESS) { ctl.current = v; ctl.autoOn = a; }
            if (caps.ControlType == ASI_HIGH_SPEED_MODE) highSpeed = ctl.current;
            if (caps.ControlType == ASI_GAIN) gainMin = caps.MinValue;
            c.controls.push_back(ctl);
        }

        // ZWO has no named readout modes; the high-speed switch is the closest equivalent.
        if (highSpeed) {
            c.readoutModes.push_back({0, "Normal readout (full ADC depth)", std::nullopt, std::nullopt, *highSpeed == 0});
            c.readoutModes.push_back({1, "High-speed readout (reduced ADC depth)", std::nullopt, std::nullopt, *highSpeed != 0});
            c.notes.push_back("readout modes are synthesised from the ASI_HIGH_SPEED_MODE control; the SDK has no named modes");
        }
        if (info.IsTriggerCam) {
            ASI_SUPPORTED_MODE sm{};
            if (ASIGetCameraSupportMode(id, &sm) == ASI_SUCCESS)
                for (int k = 0; k < 16 && sm.SupportedCameraMode[k] != ASI_MODE_END; ++k) c.triggerModes.push_back(modeName(sm.SupportedCameraMode[k]));
        }

        int offHDR = 0, offUnity = 0, gainLRN = 0, offLRN = 0;
        if (ASIGetGainOffset(id, &offHDR, &offUnity, &gainLRN, &offLRN) == ASI_SUCCESS) {
            c.gainLandmarks.push_back({"Highest dynamic range", static_cast<double>(gainMin), offHDR, "ASIGetGainOffset (gain = minimum)"});
            std::optional<double> unity;
            if (info.ElecPerADU > 0) unity = std::round(200.0 * std::log10(info.ElecPerADU));   // gain unit is 0.1 dB
            c.gainLandmarks.push_back({"Unity gain (1 e-/ADU)", unity, offUnity,
                                       unity ? "offset: ASIGetGainOffset; gain: estimate 200*log10(ElecPerADU)" : "ASIGetGainOffset (no ElecPerADU)"});
            c.gainLandmarks.push_back({"Lowest read noise (HCG on)", gainLRN, offLRN, "ASIGetGainOffset"});
        }
        int lg = 0, mg = 0, hg = 0, ho = 0;
        if (ASIGetLMHGainOffset(id, &lg, &mg, &hg, &ho) == ASI_SUCCESS) {
            c.gainLandmarks.push_back({"Preset low gain", lg, std::nullopt, "ASIGetLMHGainOffset"});
            c.gainLandmarks.push_back({"Preset medium gain", mg, std::nullopt, "ASIGetLMHGainOffset"});
            c.gainLandmarks.push_back({"Preset high gain (lowest read noise)", hg, ho, "ASIGetLMHGainOffset"});
        }
        c.notes.push_back("HCG switching is automatic inside the driver; the lowest-read-noise gain is where it is on");

        ASICloseCamera(id);
        r.cameras.push_back(std::move(c));
    }
    return r;
}

}  // namespace camprobe

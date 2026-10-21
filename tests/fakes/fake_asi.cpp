// A fake of ZWO's camera SDK (see fake_sdk.hpp): the functions src/camera/asi_camera.cpp calls, over fake::asi().
#include "fakes/fake_sdk.hpp"

#include <libasi/ASICamera2.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

namespace fake {

void drawDisc(uint8_t* dst, int width, int height, int bps, int n) {
    const double cx = width / 2.0 + 3 * std::sin(n * 0.1), cy = height / 2.0, r = std::min(width, height) / 10.0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const double d = std::hypot(x - cx, y - cy);
            const double v = d < r ? 0.75 - 0.25 * (d / r) * (d / r) : 0.04;   // limb-darkened disc on dark sky
            const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            if (bps == 2) {
                const auto s = static_cast<uint16_t>(v * 65535);
                dst[2 * i] = static_cast<uint8_t>(s & 0xff);
                dst[2 * i + 1] = static_cast<uint8_t>(s >> 8);
            } else {
                dst[i] = static_cast<uint8_t>(v * 255);
            }
        }
}

void Asi::reset() {
    *this = Asi{};
    AsiCam c;
    c.controls = {
        {"Gain", ASI_GAIN, 0, 600, 200, 200, false, true, false},
        {"Exposure", ASI_EXPOSURE, 32, 2000000000, 10000, 10000, true, true, false},
        {"Offset", ASI_OFFSET, 0, 80, 8, 8, false, true, false},
        {"WB_R", ASI_WB_R, 1, 99, 52, 52, true, true, false},
        {"HighSpeedMode", ASI_HIGH_SPEED_MODE, 0, 1, 0, 0, false, true, false},
        {"Temperature", ASI_TEMPERATURE, -500, 1000, 0, 215, false, false, false},
    };
    cams = {c};
}

AsiControl* Asi::control(const std::string& name) {
    for (auto& c : cam().controls)
        if (c.name == name) return &c;
    return nullptr;
}

Asi& asi() {
    static Asi a = [] {
        Asi x;
        x.reset();
        return x;
    }();
    return a;
}

}  // namespace fake

namespace {

ASI_ERROR_CODE failed(const char* fn) {
    auto& once = fake::asi().failOnce;
    if (auto it = once.find(fn); it != once.end()) {
        const int code = it->second;
        once.erase(it);
        return static_cast<ASI_ERROR_CODE>(code);
    }
    auto& f = fake::asi().fail;
    auto it = f.find(fn);
    return it == f.end() ? ASI_SUCCESS : static_cast<ASI_ERROR_CODE>(it->second);
}

fake::AsiControl* byType(int type) {
    for (auto& c : fake::asi().cam().controls)
        if (c.type == type) return &c;
    return nullptr;
}

}  // namespace

int ASIGetNumOfConnectedCameras() { return static_cast<int>(fake::asi().cams.size()); }

ASI_ERROR_CODE ASIGetCameraProperty(ASI_CAMERA_INFO* info, int index) {
    if (auto e = failed("ASIGetCameraProperty")) return e;
    auto& a = fake::asi();
    if (index < 0 || index >= static_cast<int>(a.cams.size())) return ASI_ERROR_INVALID_INDEX;
    const auto& c = a.cams[static_cast<std::size_t>(index)];
    *info = ASI_CAMERA_INFO{};
    std::strncpy(info->Name, c.name.c_str(), sizeof info->Name - 1);
    info->CameraID = c.id;
    info->MaxWidth = c.maxWidth;
    info->MaxHeight = c.maxHeight;
    info->IsColorCam = c.color ? ASI_TRUE : ASI_FALSE;
    info->BayerPattern = static_cast<ASI_BAYER_PATTERN>(c.bayer);
    for (std::size_t i = 0; i < c.bins.size() && i < 15; ++i) info->SupportedBins[i] = c.bins[i];
    std::size_t k = 0;
    for (; k < c.formats.size() && k < 7; ++k) info->SupportedVideoFormat[k] = static_cast<ASI_IMG_TYPE>(c.formats[k]);
    info->SupportedVideoFormat[k] = ASI_IMG_END;
    info->PixelSize = c.pixelUm;
    info->BitDepth = c.bitDepth;
    return ASI_SUCCESS;
}

ASI_ERROR_CODE ASIOpenCamera(int id) {
    if (auto e = failed("ASIOpenCamera")) return e;
    fake::asi().openId = id;
    return ASI_SUCCESS;
}

ASI_ERROR_CODE ASIInitCamera(int) { return failed("ASIInitCamera"); }

ASI_ERROR_CODE ASICloseCamera(int) {
    fake::asi().openId = -1;
    fake::asi().capturing = false;
    return ASI_SUCCESS;
}

ASI_ERROR_CODE ASIGetSerialNumber(int, ASI_SN* sn) {
    if (!fake::asi().cam().serialOk) return ASI_ERROR_GENERAL_ERROR;
    for (int i = 0; i < 8; ++i) sn->id[i] = static_cast<unsigned char>(0xa0 + i);
    return ASI_SUCCESS;
}

ASI_ERROR_CODE ASIGetNumOfControls(int, int* n) {
    *n = static_cast<int>(fake::asi().cam().controls.size());
    return failed("ASIGetNumOfControls");
}

ASI_ERROR_CODE ASIGetControlCaps(int, int index, ASI_CONTROL_CAPS* caps) {
    auto& cs = fake::asi().cam().controls;
    if (index < 0 || index >= static_cast<int>(cs.size())) return ASI_ERROR_INVALID_CONTROL_TYPE;
    const auto& c = cs[static_cast<std::size_t>(index)];
    if (fake::asi().fail.count("ASIGetControlCaps:" + c.name)) return ASI_ERROR_GENERAL_ERROR;
    *caps = ASI_CONTROL_CAPS{};
    std::strncpy(caps->Name, c.name.c_str(), sizeof caps->Name - 1);
    caps->MaxValue = c.max;
    caps->MinValue = c.min;
    caps->DefaultValue = c.def;
    caps->IsAutoSupported = c.autoSupported ? ASI_TRUE : ASI_FALSE;
    caps->IsWritable = c.writable ? ASI_TRUE : ASI_FALSE;
    caps->ControlType = static_cast<ASI_CONTROL_TYPE>(c.type);
    return ASI_SUCCESS;
}

ASI_ERROR_CODE ASIGetControlValue(int, ASI_CONTROL_TYPE type, long* v, ASI_BOOL* a) {
    if (auto e = failed("ASIGetControlValue")) return e;
    auto* c = byType(type);
    if (!c) return ASI_ERROR_INVALID_CONTROL_TYPE;
    *v = c->value;
    *a = c->autoOn ? ASI_TRUE : ASI_FALSE;
    return ASI_SUCCESS;
}

ASI_ERROR_CODE ASISetControlValue(int, ASI_CONTROL_TYPE type, long v, ASI_BOOL a) {
    if (auto e = failed("ASISetControlValue")) return e;
    auto* c = byType(type);
    if (!c) return ASI_ERROR_INVALID_CONTROL_TYPE;
    c->value = v;
    c->autoOn = a == ASI_TRUE;
    return ASI_SUCCESS;
}

ASI_ERROR_CODE ASIGetGainOffset(int, int* hdr, int* unity, int* lrnGain, int* lrnOffset) {
    if (!fake::asi().cam().gainOffsetOk) return ASI_ERROR_GENERAL_ERROR;
    *hdr = 70, *unity = 30, *lrnGain = 252, *lrnOffset = 50;
    return ASI_SUCCESS;
}

ASI_ERROR_CODE ASIGetLMHGainOffset(int, int* l, int* m, int* h, int* ho) {
    if (!fake::asi().cam().lmhOk) return ASI_ERROR_GENERAL_ERROR;
    *l = 0, *m = 252, *h = 400, *ho = 50;
    return ASI_SUCCESS;
}

ASI_ERROR_CODE ASISetROIFormat(int, int w, int h, int bin, ASI_IMG_TYPE type) {
    if (auto e = failed("ASISetROIFormat")) return e;
    auto& a = fake::asi();
    a.width = w, a.height = h, a.bin = bin, a.imgType = type;
    return ASI_SUCCESS;
}

ASI_ERROR_CODE ASISetStartPos(int, int x, int y) {
    if (auto e = failed("ASISetStartPos")) return e;
    fake::asi().startX = x, fake::asi().startY = y;
    return ASI_SUCCESS;
}

ASI_ERROR_CODE ASIStartVideoCapture(int) {
    if (auto e = failed("ASIStartVideoCapture")) return e;
    fake::asi().capturing = true;
    return ASI_SUCCESS;
}

ASI_ERROR_CODE ASIStopVideoCapture(int) {
    fake::asi().capturing = false;
    return failed("ASIStopVideoCapture");
}

ASI_ERROR_CODE ASIGetDroppedFrames(int, int* d) {
    *d = fake::asi().dropped;
    return failed("ASIGetDroppedFrames");
}

ASI_ERROR_CODE ASIGetVideoData(int, unsigned char* buf, long size, int waitMs) {
    if (auto e = failed("ASIGetVideoData")) return e;
    auto& a = fake::asi();
    if (a.timeouts > 0) {
        --a.timeouts;
        return ASI_ERROR_TIMEOUT;
    }
    const int bps = a.imgType == ASI_IMG_RAW16 ? 2 : 1;
    if (size < static_cast<long>(a.width) * a.height * bps) return ASI_ERROR_BUFFER_TOO_SMALL;
    // a real camera paces itself by the exposure; ~100 fps keeps the GUI tests' preview busy but not swamped
    (void)waitMs;
    std::this_thread::sleep_for(std::chrono::milliseconds(a.frameMs));
    if (a.draw) a.draw(buf, a.width, a.height, bps, a.frames++);
    else fake::drawDisc(buf, a.width, a.height, bps, a.frames++);
    return ASI_SUCCESS;
}

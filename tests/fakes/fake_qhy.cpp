// A fake of QHY's camera SDK (see fake_sdk.hpp): the functions src/camera/qhy_camera.cpp calls, over fake::qhy().
#include "fakes/fake_sdk.hpp"

#include <qhyccd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace fake {

void Qhy::reset() {
    *this = Qhy{};
    QhyCam c;
    c.available = {CONTROL_GAIN, CONTROL_OFFSET, CONTROL_EXPOSURE, CONTROL_USBTRAFFIC, CONTROL_CURTEMP, CONTROL_COOLER,
                   CAM_BIN1X1MODE, CAM_BIN2X2MODE, CAM_8BITS, CAM_16BITS, OutputDataActualBits, DefaultGain, DefaultOffset,
                   CAM_CurveReadoutNoise, CAM_CurveSystemGain};
    c.params = {{CONTROL_GAIN, 30}, {CONTROL_OFFSET, 10}, {CONTROL_EXPOSURE, 10000}, {CONTROL_USBTRAFFIC, 0},
                {CONTROL_CURTEMP, 21.5}, {CONTROL_COOLER, 0}, {OutputDataActualBits, 14}, {DefaultGain, 30}, {DefaultOffset, 10}};
    c.ranges = {{CONTROL_GAIN, {0, 100, 1}}, {CONTROL_OFFSET, {0, 255, 1}}, {CONTROL_EXPOSURE, {1, 3.6e9, 1}},
                {CONTROL_USBTRAFFIC, {0, 60, 1}}, {CONTROL_COOLER, {-50, 50, 0.5}}};
    cams = {c};
}

namespace {
int g_firmwareUploads = 0;
}

int qhyFirmwareUploads() { return g_firmwareUploads; }

Qhy& qhy() {
    static Qhy q = [] {
        Qhy x;
        x.reset();
        return x;
    }();
    return q;
}

}  // namespace fake

namespace {

// One camera at a time is enough for the tests: every handle is this one.
char g_handle;

uint32_t failed(const char* fn) {
    auto& once = fake::qhy().failOnce;
    if (auto it = once.find(fn); it != once.end()) {
        const int code = it->second;
        once.erase(it);
        return static_cast<uint32_t>(code);
    }
    auto& f = fake::qhy().fail;
    auto it = f.find(fn);
    return it == f.end() ? QHYCCD_SUCCESS : static_cast<uint32_t>(it->second);
}

fake::QhyCam& cam() { return fake::qhy().cam(); }

}  // namespace

void EnableQHYCCDMessage(bool) {}
void EnableQHYCCDLogFile(bool) {}

uint32_t InitQHYCCDResource() {
    if (auto e = failed("InitQHYCCDResource")) return e;
    ++fake::qhy().resourceUsers;
    return QHYCCD_SUCCESS;
}

uint32_t ReleaseQHYCCDResource() {
    --fake::qhy().resourceUsers;
    return QHYCCD_SUCCESS;
}

#ifdef __APPLE__
uint32_t OSXInitQHYCCDFirmwareArray() {
    ++fake::g_firmwareUploads;
    return failed("OSXInitQHYCCDFirmwareArray");
}
#endif

uint32_t ScanQHYCCD() { return static_cast<uint32_t>(fake::qhy().cams.size()); }

uint32_t GetQHYCCDId(uint32_t index, char* id) {
    if (auto e = failed("GetQHYCCDId")) return e;
    auto& q = fake::qhy();
    if (index >= q.cams.size()) return QHYCCD_ERROR;
    std::strcpy(id, q.cams[index].id.c_str());
    return QHYCCD_SUCCESS;
}

uint32_t GetQHYCCDModel(char* id, char* model) {
    for (const auto& c : fake::qhy().cams)
        if (c.id == id) {
            std::strcpy(model, c.model.c_str());
            return QHYCCD_SUCCESS;
        }
    return QHYCCD_ERROR;
}

qhyccd_handle* OpenQHYCCD(char*) {
    if (fake::qhy().openFails) return nullptr;
    fake::qhy().open = true;
    return &g_handle;
}

uint32_t CloseQHYCCD(qhyccd_handle*) {
    fake::qhy().open = fake::qhy().live = false;
    return QHYCCD_SUCCESS;
}

uint32_t GetQHYCCDNumberOfReadModes(qhyccd_handle*, uint32_t* n) {
    if (auto e = failed("GetQHYCCDNumberOfReadModes")) return e;
    *n = static_cast<uint32_t>(cam().modes.size());
    return QHYCCD_SUCCESS;
}

uint32_t GetQHYCCDReadModeName(qhyccd_handle*, uint32_t m, char* name) {
    std::strcpy(name, cam().modes.at(m).name.c_str());
    return QHYCCD_SUCCESS;
}

uint32_t GetQHYCCDReadModeResolution(qhyccd_handle*, uint32_t m, uint32_t* w, uint32_t* h) {
    *w = cam().modes.at(m).width;
    *h = cam().modes.at(m).height;
    return QHYCCD_SUCCESS;
}

uint32_t GetQHYCCDReadMode(qhyccd_handle*, uint32_t* m) {
    if (auto e = failed("GetQHYCCDReadMode")) return e;
    *m = cam().currentMode;
    return QHYCCD_SUCCESS;
}

uint32_t SetQHYCCDReadMode(qhyccd_handle*, uint32_t m) {
    if (auto e = failed("SetQHYCCDReadMode")) return e;
    fake::qhy().readMode = cam().currentMode = m;
    return QHYCCD_SUCCESS;
}

uint32_t SetQHYCCDStreamMode(qhyccd_handle*, uint8_t mode) {
    if (auto e = failed("SetQHYCCDStreamMode")) return e;
    fake::qhy().streamMode = mode;
    return QHYCCD_SUCCESS;
}

uint32_t InitQHYCCD(qhyccd_handle*) {
    if (auto e = failed("InitQHYCCD")) return e;
    fake::qhy().initialised = true;
    return QHYCCD_SUCCESS;
}

uint32_t GetQHYCCDChipInfo(qhyccd_handle*, double* cw, double* ch, uint32_t* iw, uint32_t* ih, double* pw, double* ph, uint32_t* bpp) {
    if (auto e = failed("GetQHYCCDChipInfo")) return e;
    const auto& c = cam();
    const auto& m = c.modes.empty() ? fake::QhyMode{} : c.modes.at(c.currentMode);
    *iw = m.width ? m.width : c.width;
    *ih = m.height ? m.height : c.height;
    *pw = *ph = c.pixelUm;
    *cw = *iw * c.pixelUm / 1000;
    *ch = *ih * c.pixelUm / 1000;
    *bpp = 16;
    return QHYCCD_SUCCESS;
}

uint32_t IsQHYCCDControlAvailable(qhyccd_handle*, CONTROL_ID id) {
    if (id == CAM_COLOR) return cam().bayer ? cam().bayer : QHYCCD_ERROR;
    return cam().available.contains(id) ? QHYCCD_SUCCESS : QHYCCD_ERROR;
}

double GetQHYCCDParam(qhyccd_handle*, CONTROL_ID id) {
    auto it = cam().params.find(id);
    return it == cam().params.end() ? static_cast<double>(QHYCCD_ERROR) : it->second;
}

uint32_t SetQHYCCDParam(qhyccd_handle*, CONTROL_ID id, double v) {
    if (auto e = failed("SetQHYCCDParam")) return e;
    cam().params[id] = v;
    return QHYCCD_SUCCESS;
}

uint32_t GetQHYCCDParamMinMaxStep(qhyccd_handle*, CONTROL_ID id, double* mn, double* mx, double* st) {
    auto it = cam().ranges.find(id);
    if (it == cam().ranges.end()) return QHYCCD_ERROR;
    *mn = it->second[0], *mx = it->second[1], *st = it->second[2];
    return QHYCCD_SUCCESS;
}

// Curves shaped like the QHY533M's: read noise steps down at the HCG switch (gain 60) and is lowest at the top; the
// system gain falls through 1 e-/ADU at gain 26.
uint32_t QHYCCD_curveReadoutNoise(qhyccd_handle*, double g, double* rn) {
    if (!cam().readNoiseCurve) return QHYCCD_ERROR;
    *rn = g < 60 ? 3.2 - g * 0.01 : 1.6 - (g - 60) * 0.005;
    return QHYCCD_SUCCESS;
}

uint32_t QHYCCD_curveSystemGain(qhyccd_handle*, double g, double* sg) {
    if (!cam().systemGainCurve) return QHYCCD_ERROR;
    *sg = 1.5 - g / 52.0;
    return QHYCCD_SUCCESS;
}

uint32_t SetQHYCCDBitsMode(qhyccd_handle*, uint32_t bits) {
    if (auto e = failed("SetQHYCCDBitsMode")) return e;
    fake::qhy().bits = bits;
    return QHYCCD_SUCCESS;
}

uint32_t SetQHYCCDBinMode(qhyccd_handle*, uint32_t bx, uint32_t) {
    if (auto e = failed("SetQHYCCDBinMode")) return e;
    fake::qhy().bin = bx;
    return QHYCCD_SUCCESS;
}

uint32_t SetQHYCCDResolution(qhyccd_handle*, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (auto e = failed("SetQHYCCDResolution")) return e;
    auto& q = fake::qhy();
    q.x = x, q.y = y, q.w = w, q.h = h;
    return QHYCCD_SUCCESS;
}

uint32_t GetQHYCCDMemLength(qhyccd_handle*) { return cam().width * cam().height * 2; }

uint32_t BeginQHYCCDLive(qhyccd_handle*) {
    if (auto e = failed("BeginQHYCCDLive")) return e;
    fake::qhy().live = true;
    return QHYCCD_SUCCESS;
}

uint32_t StopQHYCCDLive(qhyccd_handle*) {
    fake::qhy().live = false;
    return failed("StopQHYCCDLive");
}

uint32_t GetQHYCCDLiveFrame(qhyccd_handle*, uint32_t* w, uint32_t* h, uint32_t* bpp, uint32_t* channels, uint8_t* buf) {
    auto& q = fake::qhy();
    if (q.notReady > 0) {
        --q.notReady;
        return QHYCCD_ERROR;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(q.frameMs));   // the window tests' preview: busy, not swamped
    *w = q.w + (q.wrongSize ? 4 : 0);
    *h = q.h;
    *bpp = q.bits;
    *channels = 1;
    fake::drawDisc(buf, static_cast<int>(q.w), static_cast<int>(q.h), q.bits > 8 ? 2 : 1, q.frames++);
    return QHYCCD_SUCCESS;
}

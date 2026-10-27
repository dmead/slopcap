// A fake of Player One's camera SDK (see fake_sdk.hpp): the functions src/camera/playerone_camera.cpp calls, over
// fake::poa().
#include "fakes/fake_sdk.hpp"

#include <libplayerone/PlayerOneCamera.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace fake {

void Poa::reset() {
    *this = Poa{};
    PoaCam c;
    c.configs = {
        {"Exposure", POA_EXPOSURE, VAL_INT, 10, 2000000000, 10000, 10000, true, true, false},
        {"Gain", POA_GAIN, VAL_INT, 0, 700, 100, 100, true, true, false},
        {"Offset", POA_OFFSET, VAL_INT, 0, 254, 12, 12, false, true, false},
        {"Temperature", POA_TEMPERATURE, VAL_FLOAT, -50, 100, 0, 21.5, false, false, false},
        {"HardwareBin", POA_HARDWARE_BIN, VAL_BOOL, 0, 1, 0, 0, false, true, false},
        {"Exp", POA_EXP, VAL_FLOAT, 0.00001, 7200, 0.01, 0.01, true, true, false},   // (the same exposure in seconds)
        {"FlipNone", POA_FLIP_NONE, VAL_BOOL, 0, 1, 1, 1, false, true, false},
    };
    cams = {c};
}

PoaConfig* Poa::config(const std::string& name) {
    for (auto& c : cam().configs)
        if (c.name == name) return &c;
    return nullptr;
}

Poa& poa() {
    static Poa p = [] {
        Poa x;
        x.reset();
        return x;
    }();
    return p;
}

}  // namespace fake

namespace {

POAErrors failed(const char* fn) {
    auto& once = fake::poa().failOnce;
    if (auto it = once.find(fn); it != once.end()) {
        const int code = it->second;
        once.erase(it);
        return static_cast<POAErrors>(code);
    }
    auto& f = fake::poa().fail;
    auto it = f.find(fn);
    return it == f.end() ? POA_OK : static_cast<POAErrors>(it->second);
}

fake::PoaConfig* byId(POAConfig id) {
    for (auto& c : fake::poa().cam().configs)
        if (c.id == id) return &c;
    return nullptr;
}

POAConfigValue value(double v, int type) {
    POAConfigValue out{};
    if (type == VAL_FLOAT) out.floatValue = v;
    else if (type == VAL_BOOL) out.boolValue = v != 0 ? POA_TRUE : POA_FALSE;
    else out.intValue = static_cast<long>(v);
    return out;
}

double number(POAConfigValue v, int type) {
    return type == VAL_FLOAT ? v.floatValue : type == VAL_BOOL ? (v.boolValue == POA_TRUE ? 1 : 0) : static_cast<double>(v.intValue);
}

}  // namespace

int POAGetCameraCount() { return static_cast<int>(fake::poa().cams.size()); }

POAErrors POAGetCameraProperties(int index, POACameraProperties* p) {
    if (auto e = failed("POAGetCameraProperties")) return e;
    auto& f = fake::poa();
    if (index < 0 || index >= static_cast<int>(f.cams.size())) return POA_ERROR_INVALID_INDEX;
    const auto& c = f.cams[static_cast<std::size_t>(index)];
    *p = POACameraProperties{};
    std::strncpy(p->cameraModelName, c.name.c_str(), sizeof p->cameraModelName - 1);
    std::strncpy(p->SN, c.sn.c_str(), sizeof p->SN - 1);
    p->cameraID = c.id;
    p->maxWidth = c.maxWidth;
    p->maxHeight = c.maxHeight;
    p->bitDepth = c.bitDepth;
    p->isColorCamera = c.color ? POA_TRUE : POA_FALSE;
    p->bayerPattern = c.color ? static_cast<POABayerPattern>(c.bayer) : POA_BAYER_MONO;
    p->pixelSize = c.pixelUm;
    for (std::size_t i = 0; i < c.bins.size() && i < 7; ++i) p->bins[i] = c.bins[i];
    std::size_t k = 0;
    for (; k < c.formats.size() && k < 7; ++k) p->imgFormats[k] = static_cast<POAImgFormat>(c.formats[k]);
    p->imgFormats[k] = POA_END;
    return POA_OK;
}

POAErrors POAOpenCamera(int id) {
    if (auto e = failed("POAOpenCamera")) return e;
    fake::poa().openId = id;
    return POA_OK;
}

POAErrors POAInitCamera(int) {
    if (auto e = failed("POAInitCamera")) return e;
    fake::poa().initialised = true;
    return POA_OK;
}

POAErrors POACloseCamera(int) {
    auto& f = fake::poa();
    f.openId = -1;
    f.exposing = f.initialised = false;
    return POA_OK;
}

POAErrors POAGetConfigsCount(int, int* n) {
    *n = static_cast<int>(fake::poa().cam().configs.size());
    return failed("POAGetConfigsCount");
}

POAErrors POAGetConfigAttributes(int, int index, POAConfigAttributes* a) {
    auto& cs = fake::poa().cam().configs;
    if (index < 0 || index >= static_cast<int>(cs.size())) return POA_ERROR_INVALID_INDEX;
    const auto& c = cs[static_cast<std::size_t>(index)];
    if (fake::poa().fail.count("POAGetConfigAttributes:" + c.name)) return POA_ERROR_OPERATION_FAILED;
    *a = POAConfigAttributes{};
    a->isSupportAuto = c.autoSupported ? POA_TRUE : POA_FALSE;
    a->isWritable = c.writable ? POA_TRUE : POA_FALSE;
    a->isReadable = POA_TRUE;
    a->configID = static_cast<POAConfig>(c.id);
    a->valueType = static_cast<POAValueType>(c.type);
    a->maxValue = value(c.max, c.type);
    a->minValue = value(c.min, c.type);
    a->defaultValue = value(c.def, c.type);
    std::strncpy(a->szConfName, c.name.c_str(), sizeof a->szConfName - 1);
    return POA_OK;
}

POAErrors POAGetConfig(int, POAConfig id, POAConfigValue* v, POABool* a) {
    if (auto e = failed("POAGetConfig")) return e;
    auto* c = byId(id);
    if (!c) return POA_ERROR_INVALID_CONFIG;
    *v = value(c->value, c->type);
    *a = c->autoOn ? POA_TRUE : POA_FALSE;
    return POA_OK;
}

POAErrors POASetConfig(int, POAConfig id, POAConfigValue v, POABool a) {
    if (auto e = failed("POASetConfig")) return e;
    auto* c = byId(id);
    if (!c) return POA_ERROR_INVALID_CONFIG;
    if (!c->writable) return POA_ERROR_CONF_CANNOT_WRITE;
    c->value = number(v, c->type);
    c->autoOn = a == POA_TRUE;
    return POA_OK;
}

POAErrors POAGetSensorModeCount(int, int* n) {
    *n = static_cast<int>(fake::poa().cam().modes.size());
    return failed("POAGetSensorModeCount");
}

POAErrors POAGetSensorModeInfo(int, int index, POASensorModeInfo* m) {
    const auto& modes = fake::poa().cam().modes;
    if (index < 0 || index >= static_cast<int>(modes.size())) return POA_ERROR_INVALID_ARGU;
    *m = POASensorModeInfo{};
    std::strncpy(m->name, modes[static_cast<std::size_t>(index)].c_str(), sizeof m->name - 1);
    return POA_OK;
}

POAErrors POAGetSensorMode(int, int* index) {
    if (auto e = failed("POAGetSensorMode")) return e;
    *index = fake::poa().mode;
    return POA_OK;
}

POAErrors POASetSensorMode(int, int index) {
    if (auto e = failed("POASetSensorMode")) return e;
    fake::poa().mode = index;
    return POA_OK;
}

POAErrors POAGetGainsAndOffsets(int, int* gHdr, int* gHcg, int* gUnity, int* gLrn, int* oHdr, int* oHcg, int* oUnity, int* oLrn) {
    if (!fake::poa().cam().gainsOk) return POA_ERROR_OPERATION_FAILED;
    *gHdr = 0, *gHcg = 120, *gUnity = 100, *gLrn = 350;
    *oHdr = 12, *oHcg = 30, *oUnity = 20, *oLrn = 40;
    return POA_OK;
}

POAErrors POASetImageBin(int, int bin) {
    if (auto e = failed("POASetImageBin")) return e;
    fake::poa().bin = bin;
    return POA_OK;
}

POAErrors POASetImageSize(int, int w, int h) {
    if (auto e = failed("POASetImageSize")) return e;
    fake::poa().width = w, fake::poa().height = h;
    return POA_OK;
}

POAErrors POASetImageStartPos(int, int x, int y) {
    if (auto e = failed("POASetImageStartPos")) return e;
    fake::poa().startX = x, fake::poa().startY = y;
    return POA_OK;
}

POAErrors POASetImageFormat(int, POAImgFormat f) {
    if (auto e = failed("POASetImageFormat")) return e;
    fake::poa().format = f;
    return POA_OK;
}

POAErrors POAStartExposure(int, POABool single) {
    if (auto e = failed("POAStartExposure")) return e;
    if (single == POA_TRUE) return POA_ERROR_INVALID_ARGU;   // (the backend wants video mode)
    fake::poa().exposing = true;
    return POA_OK;
}

POAErrors POAStopExposure(int) {
    fake::poa().exposing = false;
    return failed("POAStopExposure");
}

POAErrors POAGetDroppedImagesCount(int, int* d) {
    *d = fake::poa().dropped;
    return failed("POAGetDroppedImagesCount");
}

POAErrors POAGetImageData(int, unsigned char* buf, long size, int) {
    if (auto e = failed("POAGetImageData")) return e;
    auto& f = fake::poa();
    if (f.timeouts > 0) {
        --f.timeouts;
        return POA_ERROR_TIMEOUT;
    }
    const int bps = f.format == POA_RAW16 ? 2 : 1;
    if (size < static_cast<long>(f.width) * f.height * bps) return POA_ERROR_SIZE_LESS;
    std::this_thread::sleep_for(std::chrono::milliseconds(f.frameMs));
    fake::drawDisc(buf, f.width, f.height, bps, f.frames++);
    return POA_OK;
}

const char* POAGetErrorString(POAErrors err) {
    switch (err) {
        case POA_ERROR_TIMEOUT: return "timeout";
        case POA_ERROR_OPERATION_FAILED: return "operation failed";
        case POA_ERROR_INVALID_INDEX: return "invalid index";
        default: return "error";
    }
}

// A fake of ToupTek's camera SDK (see fake_sdk.hpp), written once and compiled for each brand that ships it under its
// own name: fake_touptek.cpp (Toupcam_...) and fake_mallincam.cpp (Mallincam_...) define FN / TY / CO / HANDLE /
// EVENT_CB / STATE, include their brand's header, then this.

#include <chrono>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace {

int failed(const char* fn) {
    auto& s = STATE();
    if (auto it = s.failOnce.find(fn); it != s.failOnce.end()) {
        const int hr = it->second;
        s.failOnce.erase(it);
        return hr;
    }
    auto it = s.fail.find(fn);
    return it == s.fail.end() ? 0 : it->second;
}

constexpr int kNotImpl = static_cast<int>(0x80004001u), kUnexpected = static_cast<int>(0x8000ffffu), kTimeout = static_cast<int>(0x8001011fu);

/// The models and names EnumV2 hands out stay put until the next EnumV2, as the SDK's do for the whole run.
std::deque<TY(ModelV2)> g_models;
std::deque<std::string> g_names;

int bin() {
    const auto& o = STATE().options;
    const auto it = o.find(CO(OPTION_BINNING));
    return it == o.end() ? 1 : std::max(1, it->second & 0x3f);
}

int option(unsigned id, int def = 0) {
    const auto& o = STATE().options;
    const auto it = o.find(id);
    return it == o.end() ? def : it->second;
}

}  // namespace

unsigned FN(EnumV2)(TY(DeviceV2) arr[CO(MAX)]) {
    auto& s = STATE();
    g_models.clear();
    g_names.clear();
    unsigned n = 0;
    for (const auto& c : s.cams) {
        if (n >= CO(MAX)) break;
        TY(ModelV2) m{};
        g_names.push_back(c.name);
        m.name = g_names.back().c_str();
        m.flag = c.flags;
        m.maxspeed = static_cast<unsigned>(c.maxSpeed);
        m.preview = m.still = 1;
        m.maxfanspeed = static_cast<unsigned>(c.maxFan);
        m.xpixsz = m.ypixsz = c.pixelUm;
        m.res[0].width = c.width;
        m.res[0].height = c.height;
        g_models.push_back(m);
        if (arr) {
            arr[n] = TY(DeviceV2){};
            std::strncpy(arr[n].displayname, c.name.c_str(), sizeof arr[n].displayname - 1);
            std::strncpy(arr[n].id, c.id.c_str(), sizeof arr[n].id - 1);
            arr[n].model = &g_models.back();
        }
        ++n;
    }
    return n;
}

HANDLE FN(Open)(const char* id) {
    auto& s = STATE();
    if (s.openFails || !id) return nullptr;
    s.open = true;
    s.running = false;
    s.options.clear();
    const auto& c = s.cam();
    s.roiX = s.roiY = 0, s.roiW = c.width, s.roiH = c.height;
    return reinterpret_cast<HANDLE>(&s);
}

void FN(Close)(HANDLE) {
    STATE().open = false;
    STATE().running = false;
}

HRESULT FN(StartPullModeWithCallback)(HANDLE, EVENT_CB, void*) {
    if (auto e = failed("StartPullModeWithCallback")) return e;
    STATE().running = true;
    return 0;
}

HRESULT FN(Stop)(HANDLE) {
    STATE().running = false;
    return failed("Stop");
}

HRESULT FN(WaitImageV3)(HANDLE, unsigned, void* dst, int, int, int, TY(FrameInfoV3)* info) {
    if (auto e = failed("WaitImageV3")) return e;
    auto& s = STATE();
    if (!s.running) return kUnexpected;
    if (s.timeouts > 0) {
        --s.timeouts;
        return kTimeout;
    }
    const int n = bin();
    const unsigned w = (s.roiW / static_cast<unsigned>(n)) & ~1u, h = (s.roiH / static_cast<unsigned>(n)) & ~1u;
    const int bps = option(CO(OPTION_BITDEPTH)) ? 2 : 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(s.frameMs));
    auto* p = static_cast<uint8_t*>(dst);
    fake::drawDisc(p, static_cast<int>(w), static_cast<int>(h), bps, s.frames++);
    if (bps == 2 && option(CO(OPTION_ZERO_PADDING)) != 1)   // the SDK's default: zeros on top, the data in the low bits
        for (std::size_t i = 0; i + 1 < std::size_t{w} * h * 2; i += 2) {
            const auto v = static_cast<uint16_t>((p[i] | p[i + 1] << 8) >> (16 - s.cam().maxBits));
            p[i] = static_cast<uint8_t>(v & 0xff), p[i + 1] = static_cast<uint8_t>(v >> 8);
        }
    *info = TY(FrameInfoV3){};
    info->width = w + (s.wrongSize ? 2 : 0);
    info->height = h;
    info->flag = CO(FRAMEINFO_FLAG_SEQ);
    s.seq += 1 + s.seqJump;
    s.seqJump = 0;
    info->seq = s.seq;
    return 0;
}

HRESULT FN(put_Option)(HANDLE, unsigned id, int v) {
    if (auto e = failed("put_Option")) return e;
    auto& s = STATE();
    if (id == CO(OPTION_ZERO_PADDING) && !s.cam().zeroPadding) return kNotImpl;
    if ((id == CO(OPTION_BITDEPTH) || id == CO(OPTION_BINNING)) && s.running) return kUnexpected;
    s.options[id] = v;
    return 0;
}

HRESULT FN(get_Option)(HANDLE, unsigned id, int* v) {
    if (auto e = failed("get_Option")) return e;
    auto& s = STATE();
    if (id == CO(OPTION_TECTARGET_RANGE)) {
        if (!s.cam().tecRange) return kNotImpl;
        *v = s.cam().tecRange;
        return 0;
    }
    *v = option(id);
    return 0;
}

HRESULT FN(put_Roi)(HANDLE, unsigned x, unsigned y, unsigned w, unsigned h) {
    if (auto e = failed("put_Roi")) return e;
    auto& s = STATE();
    if (s.running) return kUnexpected;
    s.roiX = x, s.roiY = y, s.roiW = w, s.roiH = h;
    return 0;
}

HRESULT FN(get_Roi)(HANDLE, unsigned* x, unsigned* y, unsigned* w, unsigned* h) {
    const auto& s = STATE();
    *x = s.roiX, *y = s.roiY, *w = s.roiW, *h = s.roiH;
    return failed("get_Roi");
}

HRESULT FN(get_ExpTimeRange)(HANDLE, unsigned* lo, unsigned* hi, unsigned* def) {
    *lo = 15, *hi = 3600000000u, *def = 10000;
    return failed("get_ExpTimeRange");
}

HRESULT FN(get_ExpoTime)(HANDLE, unsigned* t) {
    *t = STATE().expo;
    return failed("get_ExpoTime");
}

HRESULT FN(put_ExpoTime)(HANDLE, unsigned t) {
    if (auto e = failed("put_ExpoTime")) return e;
    STATE().expo = t;
    return 0;
}

HRESULT FN(get_ExpoAGainRange)(HANDLE, unsigned short* lo, unsigned short* hi, unsigned short* def) {
    *lo = 100, *hi = 10000, *def = 100;
    return failed("get_ExpoAGainRange");
}

HRESULT FN(get_ExpoAGain)(HANDLE, unsigned short* g) {
    *g = STATE().gain;
    return failed("get_ExpoAGain");
}

HRESULT FN(put_ExpoAGain)(HANDLE, unsigned short g) {
    if (auto e = failed("put_ExpoAGain")) return e;
    STATE().gain = g;
    return 0;
}

HRESULT FN(get_AutoExpoEnable)(HANDLE, int* m) {
    *m = STATE().autoExpo;
    return failed("get_AutoExpoEnable");
}

HRESULT FN(put_AutoExpoEnable)(HANDLE, int m) {
    if (auto e = failed("put_AutoExpoEnable")) return e;
    STATE().autoExpo = m;
    return 0;
}

HRESULT FN(get_MaxBitDepth)(HANDLE) { return STATE().cam().maxBits; }
HRESULT FN(get_MaxSpeed)(HANDLE) { return STATE().cam().maxSpeed; }

HRESULT FN(get_Speed)(HANDLE, unsigned short* v) {
    *v = STATE().speed;
    return failed("get_Speed");
}

HRESULT FN(put_Speed)(HANDLE, unsigned short v) {
    if (auto e = failed("put_Speed")) return e;
    STATE().speed = v;
    return 0;
}

HRESULT FN(get_RawFormat)(HANDLE, unsigned* fourcc, unsigned* bits) {
    *fourcc = STATE().cam().fourcc;
    *bits = static_cast<unsigned>(STATE().cam().maxBits);
    return failed("get_RawFormat");
}

HRESULT FN(get_SerialNumber)(HANDLE, char sn[32]) {
    if (auto e = failed("get_SerialNumber")) return e;
    std::strncpy(sn, STATE().cam().sn.c_str(), 31);
    return 0;
}

HRESULT FN(get_Temperature)(HANDLE, short* t) {
    *t = STATE().temperature;
    return failed("get_Temperature");
}

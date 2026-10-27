// The ToupTek-family camera (touptek_camera.hpp), written once against ToupSdk<Brand>: each brand's .cpp includes its
// own SDK header, defines ToupSdk<its brand> with SLOPCAP_TOUP_SDK (the same calls, under that brand's prefix), then
// includes this and instantiates the class. Only included by those files.
#pragma once

#include "camera/touptek_camera.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>

#ifdef _WIN32
#include <windows.h>
#endif

namespace slopcap {

template <ToupBrand B>
struct ToupSdk;

// F: function prefix (Toupcam_), T: type prefix (Toupcam), C: constant prefix (TOUPCAM_)
#define SLOPCAP_TOUP_SDK(F, T, C, VENDOR)                                                                                  \
    using Handle = H##T;                                                                                                   \
    using Device = T##DeviceV2;                                                                                            \
    using Info = T##FrameInfoV3;                                                                                           \
    static constexpr const char* vendor = VENDOR;                                                                          \
    static constexpr unsigned maxCameras = C##MAX;                                                                         \
    static constexpr unsigned long long flagMono = C##FLAG_MONO, flagCg = C##FLAG_CG, flagCgHdr = C##FLAG_CGHDR;           \
    static constexpr unsigned long long flagTec = C##FLAG_TEC_ONOFF, flagTemp = C##FLAG_GETTEMPERATURE, flagFan = C##FLAG_FAN; \
    static constexpr unsigned long long flagBlack = C##FLAG_BLACKLEVEL, flagLowNoise = C##FLAG_LOW_NOISE;                  \
    static constexpr unsigned long long flagFullwell = C##FLAG_HIGH_FULLWELL;                                              \
    static constexpr unsigned optRaw = C##OPTION_RAW, optBits = C##OPTION_BITDEPTH, optBin = C##OPTION_BINNING;            \
    static constexpr unsigned optBlack = C##OPTION_BLACKLEVEL, optCg = C##OPTION_CG, optTec = C##OPTION_TEC;               \
    static constexpr unsigned optTecTarget = C##OPTION_TECTARGET, optTecRange = C##OPTION_TECTARGET_RANGE;                 \
    static constexpr unsigned optFan = C##OPTION_FAN, optLowNoise = C##OPTION_LOW_NOISE;                                   \
    static constexpr unsigned optFullwell = C##OPTION_HIGH_FULLWELL, optZeroPad = C##OPTION_ZERO_PADDING;                  \
    static constexpr unsigned infoSeq = C##FRAMEINFO_FLAG_SEQ;                                                             \
    static unsigned enumerate(Device* a) { return F##EnumV2(a); }                                                          \
    static Handle open(const Device& d) { return F##Open(d.id); }                                                          \
    static void close(Handle h) { F##Close(h); }                                                                           \
    static void __stdcall noEvent(unsigned, void*) {}                                                                      \
    static int startPull(Handle h) { return F##StartPullModeWithCallback(h, &noEvent, nullptr); }                          \
    static int stop(Handle h) { return F##Stop(h); }                                                                       \
    static int wait(Handle h, unsigned ms, void* dst, Info* i) { return F##WaitImageV3(h, ms, dst, 0, 0, 0, i); }          \
    static int putOption(Handle h, unsigned o, int v) { return F##put_Option(h, o, v); }                                   \
    static int getOption(Handle h, unsigned o, int* v) { return F##get_Option(h, o, v); }                                  \
    static int putRoi(Handle h, unsigned x, unsigned y, unsigned w, unsigned hh) { return F##put_Roi(h, x, y, w, hh); }    \
    static int getRoi(Handle h, unsigned* x, unsigned* y, unsigned* w, unsigned* hh) { return F##get_Roi(h, x, y, w, hh); } \
    static int expoRange(Handle h, unsigned* lo, unsigned* hi, unsigned* def) { return F##get_ExpTimeRange(h, lo, hi, def); } \
    static int getExpo(Handle h, unsigned* t) { return F##get_ExpoTime(h, t); }                                            \
    static int putExpo(Handle h, unsigned t) { return F##put_ExpoTime(h, t); }                                             \
    static int gainRange(Handle h, unsigned short* lo, unsigned short* hi, unsigned short* def) {                          \
        return F##get_ExpoAGainRange(h, lo, hi, def);                                                                      \
    }                                                                                                                      \
    static int getGain(Handle h, unsigned short* g) { return F##get_ExpoAGain(h, g); }                                     \
    static int putGain(Handle h, unsigned short g) { return F##put_ExpoAGain(h, g); }                                      \
    static int getAutoExpo(Handle h, int* m) { return F##get_AutoExpoEnable(h, m); }                                       \
    static int putAutoExpo(Handle h, int m) { return F##put_AutoExpoEnable(h, m); }                                        \
    static int maxBitDepth(Handle h) { return F##get_MaxBitDepth(h); }                                                     \
    static int maxSpeed(Handle h) { return F##get_MaxSpeed(h); }                                                           \
    static int getSpeed(Handle h, unsigned short* s) { return F##get_Speed(h, s); }                                        \
    static int putSpeed(Handle h, unsigned short s) { return F##put_Speed(h, s); }                                         \
    static int rawFormat(Handle h, unsigned* fourcc, unsigned* bits) { return F##get_RawFormat(h, fourcc, bits); }         \
    static int serial(Handle h, char* sn) { return F##get_SerialNumber(h, sn); }                                           \
    static int temperature(Handle h, short* t) { return F##get_Temperature(h, t); }

namespace toup {

constexpr unsigned fourcc(char a, char b, char c, char d) {
    return static_cast<unsigned char>(a) | static_cast<unsigned char>(b) << 8 | static_cast<unsigned char>(c) << 16 |
           static_cast<unsigned>(static_cast<unsigned char>(d)) << 24;
}

inline std::string narrow(const char* s) { return s ? s : ""; }
#ifdef _WIN32
inline std::string narrow(const wchar_t* s) {
    if (!s || !*s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(0, n - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}
#endif

/// The SDK's HRESULTs: >= 0 succeeded (S_FALSE too), < 0 failed.
inline std::string error(int hr) {
    switch (static_cast<uint32_t>(hr)) {
        case 0x80070005u: return "permission denied (on Linux: the camera's udev rules)";
        case 0x800700aau: return "in use (by another program?)";
        case 0x80004001u: return "not supported by this camera";
        case 0x8007001fu: return "device not functioning (cable, USB port or power?)";
        case 0x8000ffffu: return "not possible now (stop the stream first?)";
        case 0x80070057u: return "invalid value";
        case 0x8001011fu: return "timeout";
    }
    return std::format("error 0x{:08x}", static_cast<uint32_t>(hr));
}

constexpr bool timedOut(int hr) { return static_cast<uint32_t>(hr) == 0x8001011fu || static_cast<uint32_t>(hr) == 0x8000000au; }

}  // namespace toup

template <ToupBrand B>
std::vector<ToupCameraEntry> ToupCamera<B>::list() {
    using S = ToupSdk<B>;
    std::vector<typename S::Device> arr(S::maxCameras);
    const unsigned n = std::min(S::enumerate(arr.data()), S::maxCameras);
    std::vector<ToupCameraEntry> out;
    for (unsigned i = 0; i < n; ++i) out.push_back({static_cast<int>(i), toup::narrow(arr[i].displayname)});
    return out;
}

template <ToupBrand B>
ToupCamera<B>::ToupCamera(int index) {
    using S = ToupSdk<B>;
    auto check = [](int hr, std::string_view what) {
        if (hr < 0) throw CameraError(std::format("{} {}: {}", S::vendor, what, toup::error(hr)));
    };
    std::vector<typename S::Device> arr(S::maxCameras);
    const int n = static_cast<int>(std::min(S::enumerate(arr.data()), S::maxCameras));
    if (index < 0 || index >= n) throw CameraError(std::format("no {} camera at index {}", S::vendor, index));
    const auto& d = arr[static_cast<std::size_t>(index)];
    const auto h = S::open(d);
    if (!h) throw CameraError(std::format("{}: can't open {} (in use by another program?)", S::vendor, toup::narrow(d.displayname)));
    h_ = h;
    try {
        const auto* m = d.model;
        const unsigned long long flags = m->flag;
        info_.vendor = S::vendor;
        info_.model = toup::narrow(d.displayname);
        char sn[32] = {};
        info_.serial = S::serial(h, sn) >= 0 && sn[0] ? std::string(sn, strnlen(sn, sizeof sn)) : toup::narrow(d.id);
        info_.sensorWidth = static_cast<int>(m->res[0].width);   // resolution 0 is the whole sensor
        info_.sensorHeight = static_cast<int>(m->res[0].height);
        info_.pixelUm = m->xpixsz;
        check(S::putOption(h, S::optRaw, 1), "RAW mode");
        const int maxBits = std::max(8, S::maxBitDepth(h));
        info_.adcBits = maxBits;
        info_.bitDepths = {8};
        if (maxBits > 8) info_.bitDepths.push_back(16);
        info_.bins = {1, 2, 3, 4};
        unsigned four = 0, bpp = 0;
        if (!(flags & S::flagMono) && S::rawFormat(h, &four, &bpp) >= 0) {
            info_.bayer = four == toup::fourcc('R', 'G', 'G', 'B')   ? Bayer::RGGB
                          : four == toup::fourcc('G', 'R', 'B', 'G') ? Bayer::GRBG
                          : four == toup::fourcc('G', 'B', 'R', 'G') ? Bayer::GBRG
                          : four == toup::fourcc('B', 'G', 'G', 'R') ? Bayer::BGGR
                                                                     : Bayer::None;
        }
        // 16-bit samples MSB-aligned, as from every other camera: the SDK pads the high bits unless told otherwise,
        // and an older one that can't be told gets its samples shifted up here
        if (maxBits > 8 && S::putOption(h, S::optZeroPad, 1) < 0) shift_ = 16 - maxBits;

        unsigned eLo = 0, eHi = 0, eDef = 0;
        check(S::expoRange(h, &eLo, &eHi, &eDef), "exposure range");
        addControl(Ctl::Exposure, {"Exposure", static_cast<double>(eLo), static_cast<double>(eHi), 1, static_cast<double>(eDef), true, true});
        unsigned short gLo = 0, gHi = 0, gDef = 0;
        check(S::gainRange(h, &gLo, &gHi, &gDef), "gain range");
        addControl(Ctl::Gain, {"Gain", static_cast<double>(gLo), static_cast<double>(gHi), 1, static_cast<double>(gDef), true, false});
        if (flags & S::flagBlack)   // the maximum grows with the ADC depth: 31 at 8 bits
            addControl(Ctl::Offset, {"Offset", 0, 31.0 * (1 << (maxBits - 8)), 1, std::nullopt, true, false});
        if (const int speed = S::maxSpeed(h); speed > 0) addControl(Ctl::Speed, {"Speed", 0, static_cast<double>(speed), 1, std::nullopt, true, false});
        if (flags & S::flagTemp) addControl(Ctl::Temperature, {"Temperature", -100, 100, 0, std::nullopt, false, false});
        if (flags & S::flagTec) {
            int range = 0;
            double lo = -40, hi = 30;
            if (S::getOption(h, S::optTecRange, &range) >= 0 && range)
                lo = static_cast<short>(range & 0xffff) / 10.0, hi = static_cast<short>((range >> 16) & 0xffff) / 10.0;
            addControl(Ctl::Cooler, {"CoolerOn", 0, 1, 1, std::nullopt, true, false});
            addControl(Ctl::TargetTemp, {"TargetTemp", lo, hi, 0.5, std::nullopt, true, false});
        }
        if ((flags & S::flagFan) && m->maxfanspeed > 0)
            addControl(Ctl::Fan, {"Fan", 0, static_cast<double>(m->maxfanspeed), 1, std::nullopt, true, false});
        if (flags & S::flagLowNoise) addControl(Ctl::LowNoise, {"LowNoise", 0, 1, 1, std::nullopt, true, false});
        if (flags & S::flagFullwell) addControl(Ctl::HighFullwell, {"HighFullwell", 0, 1, 1, std::nullopt, true, false});
        if (flags & (S::flagCg | S::flagCgHdr)) {
            modes_ = {{0, "Low conversion gain", std::nullopt, std::nullopt}, {1, "High conversion gain", std::nullopt, std::nullopt}};
            if (flags & S::flagCgHdr) modes_.push_back({2, "HDR", std::nullopt, std::nullopt});
        }

        roi_ = {0, 0, info_.sensorWidth, info_.sensorHeight, 1};
        bits_ = 8;
        applyFormat();
    } catch (...) {
        S::close(h);   // the destructor won't run
        h_ = nullptr;
        throw;
    }
}

template <ToupBrand B>
ToupCamera<B>::~ToupCamera() {
    using S = ToupSdk<B>;
    if (!h_) return;
    const auto h = static_cast<typename S::Handle>(h_);
    if (streaming_) S::stop(h);
    S::close(h);
}

template <ToupBrand B>
void ToupCamera<B>::addControl(Ctl c, ControlInfo info) {
    controls_.push_back(std::move(info));
    ctls_.push_back(c);
}

template <ToupBrand B>
typename ToupCamera<B>::Ctl ToupCamera<B>::ctl(std::string_view name) const {
    for (std::size_t i = 0; i < controls_.size(); ++i)
        if (controls_[i].name == name) return ctls_[i];
    throw CameraError(std::format("{} has no control {}", info_.model, name));
}

template <ToupBrand B>
std::optional<double> ToupCamera<B>::control(std::string_view name) const {
    using S = ToupSdk<B>;
    const auto h = static_cast<typename S::Handle>(h_);
    Ctl c;
    try {
        c = ctl(name);
    } catch (const CameraError&) {
        return std::nullopt;
    }
    unsigned u = 0;
    unsigned short s = 0;
    short t = 0;
    int v = 0;
    switch (c) {
        case Ctl::Exposure: return S::getExpo(h, &u) >= 0 ? std::optional<double>(u) : std::nullopt;
        case Ctl::Gain: return S::getGain(h, &s) >= 0 ? std::optional<double>(s) : std::nullopt;
        case Ctl::Speed: return S::getSpeed(h, &s) >= 0 ? std::optional<double>(s) : std::nullopt;
        case Ctl::Temperature: return S::temperature(h, &t) >= 0 ? std::optional<double>(t / 10.0) : std::nullopt;   // 0.1 °C
        case Ctl::Offset: return S::getOption(h, S::optBlack, &v) >= 0 ? std::optional<double>(v) : std::nullopt;
        case Ctl::Cooler: return S::getOption(h, S::optTec, &v) >= 0 ? std::optional<double>(v) : std::nullopt;
        case Ctl::TargetTemp: return S::getOption(h, S::optTecTarget, &v) >= 0 ? std::optional<double>(v / 10.0) : std::nullopt;
        case Ctl::Fan: return S::getOption(h, S::optFan, &v) >= 0 ? std::optional<double>(v) : std::nullopt;
        case Ctl::LowNoise: return S::getOption(h, S::optLowNoise, &v) >= 0 ? std::optional<double>(v) : std::nullopt;
        case Ctl::HighFullwell: return S::getOption(h, S::optFullwell, &v) >= 0 ? std::optional<double>(v) : std::nullopt;
    }
    return std::nullopt;
}

template <ToupBrand B>
void ToupCamera<B>::setControl(std::string_view name, double value) {
    using S = ToupSdk<B>;
    const auto h = static_cast<typename S::Handle>(h_);
    const Ctl c = ctl(name);
    const auto ci = std::ranges::find(controls_, name, &ControlInfo::name);
    if (!ci->writable) throw CameraError(std::format("{} is read-only", name));
    if (value < ci->min || value > ci->max) throw CameraError(std::format("{} out of range {}..{}", name, ci->min, ci->max));
    const int iv = static_cast<int>(value);
    int hr = 0;
    switch (c) {
        case Ctl::Exposure: hr = S::putExpo(h, static_cast<unsigned>(value)); break;
        case Ctl::Gain: hr = S::putGain(h, static_cast<unsigned short>(value)); break;
        case Ctl::Speed: hr = S::putSpeed(h, static_cast<unsigned short>(value)); break;
        case Ctl::Offset: hr = S::putOption(h, S::optBlack, iv); break;
        case Ctl::Cooler: hr = S::putOption(h, S::optTec, iv); break;
        case Ctl::TargetTemp: hr = S::putOption(h, S::optTecTarget, static_cast<int>(value * 10)); break;
        case Ctl::Fan: hr = S::putOption(h, S::optFan, iv); break;
        case Ctl::LowNoise: hr = S::putOption(h, S::optLowNoise, iv); break;
        case Ctl::HighFullwell: hr = S::putOption(h, S::optFullwell, iv); break;
        case Ctl::Temperature: break;   // (read-only: refused above)
    }
    if (hr < 0) throw CameraError(std::format("{} set {}: {}", S::vendor, name, toup::error(hr)));
}

template <ToupBrand B>
std::optional<bool> ToupCamera<B>::controlAuto(std::string_view name) const {
    using S = ToupSdk<B>;
    if (name != "Exposure") return std::nullopt;
    int m = 0;
    if (S::getAutoExpo(static_cast<typename S::Handle>(h_), &m) < 0) return std::nullopt;
    return m != 0;
}

template <ToupBrand B>
void ToupCamera<B>::setControlAuto(std::string_view name, bool on) {
    using S = ToupSdk<B>;
    if (name != "Exposure") throw CameraError(std::format("{} has no auto mode", name));
    if (const int hr = S::putAutoExpo(static_cast<typename S::Handle>(h_), on ? 1 : 0); hr < 0)
        throw CameraError(std::format("{} set auto exposure: {}", S::vendor, toup::error(hr)));
}

template <ToupBrand B>
int ToupCamera<B>::readoutMode() const {
    using S = ToupSdk<B>;
    int v = 0;
    return !modes_.empty() && S::getOption(static_cast<typename S::Handle>(h_), S::optCg, &v) >= 0 ? v : 0;
}

template <ToupBrand B>
void ToupCamera<B>::setReadoutMode(int index) {
    using S = ToupSdk<B>;
    if (modes_.empty()) {
        if (index != 0) throw CameraError("camera has no conversion gain modes");
        return;
    }
    if (index < 0 || index >= static_cast<int>(modes_.size())) throw CameraError(std::format("no readout mode {}", index));
    if (const int hr = S::putOption(static_cast<typename S::Handle>(h_), S::optCg, index); hr < 0)
        throw CameraError(std::format("{} set conversion gain: {}", S::vendor, toup::error(hr)));
}

template <ToupBrand B>
void ToupCamera<B>::setRoi(const Roi& r) {
    if (streaming_) throw CameraError("stop the stream before changing the ROI");
    if (std::ranges::find(info_.bins, r.bin) == info_.bins.end()) throw CameraError(std::format("bin {} not supported", r.bin));
    // ToupTek: offsets and sizes even, at least 8 sensor pixels
    if (r.width <= 0 || r.height <= 0 || r.width % 2 || r.height % 2 || r.width * r.bin < 8 || r.height * r.bin < 8)
        throw CameraError(std::format("ROI {}x{}: width and height must be even, and 8 sensor pixels at least", r.width, r.height));
    if (r.x < 0 || r.y < 0 || (r.x + r.width) * r.bin > info_.sensorWidth || (r.y + r.height) * r.bin > info_.sensorHeight)
        throw CameraError(std::format("ROI {}x{}+{}+{} (bin {}) outside the {}x{} sensor", r.width, r.height, r.x, r.y, r.bin,
                                      info_.sensorWidth, info_.sensorHeight));
    if (r.x % 2 || r.y % 2) throw CameraError("ROI start must be even");
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

template <ToupBrand B>
void ToupCamera<B>::setBitDepth(int bits) {
    if (streaming_) throw CameraError("stop the stream before changing the bit depth");
    if (std::ranges::find(info_.bitDepths, bits) == info_.bitDepths.end()) throw CameraError(std::format("{}-bit output not supported", bits));
    bits_ = bits;
    applyFormat();
}

template <ToupBrand B>
void ToupCamera<B>::applyFormat() {
    using S = ToupSdk<B>;
    const auto h = static_cast<typename S::Handle>(h_);
    auto check = [](int hr, std::string_view what) {
        if (hr < 0) throw CameraError(std::format("{} {}: {}", S::vendor, what, toup::error(hr)));
    };
    check(S::putOption(h, S::optBits, bits_ > 8 ? 1 : 0), "set bit depth");
    check(S::putOption(h, S::optBin, roi_.bin == 1 ? 1 : 0x80 | roi_.bin), "set binning");   // 0x80: averaged
    // the ROI is in sensor pixels; binning then divides it
    const int b = roi_.bin;
    check(S::putRoi(h, static_cast<unsigned>(roi_.x * b), static_cast<unsigned>(roi_.y * b), static_cast<unsigned>(roi_.width * b),
                    static_cast<unsigned>(roi_.height * b)),
          "set ROI");
}

template <ToupBrand B>
void ToupCamera<B>::startStream() {
    using S = ToupSdk<B>;
    if (streaming_) return;
    if (const int hr = S::startPull(static_cast<typename S::Handle>(h_)); hr < 0)
        throw CameraError(std::format("{} start video: {}", S::vendor, toup::error(hr)));
    streaming_ = true;
    sequence_ = dropped_ = 0;
    haveSeq_ = false;
}

template <ToupBrand B>
void ToupCamera<B>::stopStream() {
    using S = ToupSdk<B>;
    if (!streaming_) return;
    streaming_ = false;
    if (const int hr = S::stop(static_cast<typename S::Handle>(h_)); hr < 0)
        throw CameraError(std::format("{} stop video: {}", S::vendor, toup::error(hr)));
}

template <ToupBrand B>
ReadResult ToupCamera<B>::readFrame(std::span<std::byte> dst, FrameInfo& out, std::chrono::milliseconds timeout) {
    using S = ToupSdk<B>;
    if (!streaming_) throw CameraError("stream not started");
    const std::size_t n = frameBytes();
    if (dst.size() < n) throw CameraError("destination buffer smaller than one frame");
    typename S::Info fi{};
    const int hr = S::wait(static_cast<typename S::Handle>(h_), static_cast<unsigned>(std::clamp<int64_t>(timeout.count(), 1, 1'000'000)),
                           dst.data(), &fi);
    if (toup::timedOut(hr)) return ReadResult::Timeout;
    if (hr < 0) throw CameraError(std::format("{} read frame: {}", S::vendor, toup::error(hr)));
    if (static_cast<int>(fi.width) != roi_.width || static_cast<int>(fi.height) != roi_.height)
        throw CameraError(std::format("{} sent a {}x{} frame for a {}x{} ROI", S::vendor, fi.width, fi.height, roi_.width, roi_.height));
    if (fi.flag & S::infoSeq) {   // the camera numbers its frames: a gap is frames lost on the way
        if (haveSeq_ && fi.seq > lastSeq_ + 1) dropped_ += fi.seq - lastSeq_ - 1;
        lastSeq_ = fi.seq;
        haveSeq_ = true;
    }
    if (bits_ > 8 && shift_ > 0) {
        auto* p = reinterpret_cast<unsigned char*>(dst.data());
        for (std::size_t i = 0; i + 1 < n; i += 2) {
            const auto v = static_cast<uint16_t>((p[i] | p[i + 1] << 8) << shift_);
            p[i] = static_cast<unsigned char>(v & 0xff), p[i + 1] = static_cast<unsigned char>(v >> 8);
        }
    }
    out.sequence = sequence_++;
    out.timestamp = std::chrono::system_clock::now();
    out.roi = roi_;
    out.bitDepth = bits_;
    out.bytes = n;
    return ReadResult::Ok;
}

}  // namespace slopcap

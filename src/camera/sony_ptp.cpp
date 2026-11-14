#include "camera/sony_ptp.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <thread>

namespace slopcap::sony {

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

/// A property value of any type: arrays (0x4001..0x400A) are skipped and read as their length, strings as 0.
int64_t readValue(ptp::Reader& r, uint16_t type) {
    if ((type & 0x4000) && type != ptp::kString) {
        const uint32_t n = r.u32();
        const std::size_t each = ptp::typeSize(type & 0x0FFF);
        if (!each) throw ptp::Error(std::format("array of data type 0x{:04X}", type));
        r.skip(n * each);
        return n;
    }
    return r.value(type);
}

void writeValue(ptp::Writer& w, uint16_t type, int64_t v) {
    if ((type & 0x4000) && type != ptp::kString) w.u32(0);   // (arrays: empty)
    else w.value(type, v);
}

std::string hex(int64_t v) { return std::format("0x{:X}", v); }

}  // namespace

std::vector<uint16_t> ExtInfo::codes() const {
    std::vector<uint16_t> all;
    for (const auto& l : lists) all.insert(all.end(), l.begin(), l.end());
    return all;
}

ExtInfo parseExtInfo(std::span<const uint8_t> data) {
    ptp::Reader r(data);
    ExtInfo e;
    e.version = r.u16();
    while (r.left() >= 4) {
        try {
            e.lists.push_back(r.u16array());
        } catch (const ptp::Error&) {
            break;   // a list that runs past the end: what came before stands
        }
    }
    return e;
}

std::vector<uint8_t> packExtInfo(const ExtInfo& e) {
    ptp::Writer w;
    w.u16(e.version);
    for (const auto& l : e.lists) w.u16array(l);
    return w.take();
}

std::vector<Prop> parseAllProps(std::span<const uint8_t> data, uint16_t version, std::size_t* stoppedAt) {
    std::vector<Prop> out;
    if (stoppedAt) *stoppedAt = 0;
    if (data.size() < 8) return out;
    ptp::Reader r(data);
    r.skip(8);   // the number of properties (and 4 bytes that are 0)
    while (r.left() > 0) {
        const std::size_t at = r.pos();
        try {
            Prop p;
            p.code = r.u16();
            p.type = r.u16();
            p.getSet = r.u8();
            p.enabled = r.u8();
            p.factory = readValue(r, p.type);
            p.current = readValue(r, p.type);
            const uint8_t form = r.u8();
            if (form == 1) {
                p.form = Prop::Form::Range;
                p.min = readValue(r, p.type);
                p.max = readValue(r, p.type);
                p.step = readValue(r, p.type);
            } else if (form == 2) {
                p.form = Prop::Form::Enum;
                for (uint16_t n = r.u16(); n > 0; --n) p.values.push_back(readValue(r, p.type));
                if (version >= 300)   // a second list: every value the property can show
                    for (uint16_t n = r.u16(); n > 0; --n) readValue(r, p.type);
            } else if (form != 0) {
                throw ptp::Error(std::format("form {}", form));
            }
            out.push_back(std::move(p));
        } catch (const ptp::Error&) {
            if (stoppedAt) *stoppedAt = at;
            break;
        }
    }
    return out;
}

std::vector<uint8_t> packAllProps(const std::vector<Prop>& props, uint16_t version) {
    ptp::Writer w;
    w.u32(static_cast<uint32_t>(props.size())).u32(0);
    for (const auto& p : props) {
        w.u16(p.code).u16(p.type).u8(p.getSet).u8(p.enabled);
        writeValue(w, p.type, p.factory);
        writeValue(w, p.type, p.current);
        w.u8(static_cast<uint8_t>(p.form));
        if (p.form == Prop::Form::Range) {
            writeValue(w, p.type, p.min);
            writeValue(w, p.type, p.max);
            writeValue(w, p.type, p.step);
        } else if (p.form == Prop::Form::Enum) {
            for (int list = 0; list < (version >= 300 ? 2 : 1); ++list) {
                w.u16(static_cast<uint16_t>(p.values.size()));
                for (auto v : p.values) writeValue(w, p.type, v);
            }
        }
    }
    return w.take();
}

const char* propName(uint16_t code) {
    switch (code) {
        case 0x5001: return "Battery level";
        case 0x5003: return "Image size";
        case kCompression: return "Quality";
        case 0x5005: return "White balance";
        case kFNumber: return "F-number";
        case 0x5008: return "Focal length";
        case kFocusMode: return "Focus mode";
        case 0x500B: return "Metering mode";
        case 0x500C: return "Flash mode";
        case kExposureProgram: return "Exposure mode";
        case 0x5010: return "Exposure compensation";
        case 0x5013: return "Drive mode";
        case 0xD200: return "Flash compensation";
        case 0xD201: return "DRO / Auto HDR";
        case kImageSize: return "Image size (Sony)";
        case kShutterSpeed: return "Shutter speed";
        case 0xD20F: return "Colour temperature";
        case 0xD210: return "Creative style";
        case kAspectRatio: return "Aspect ratio";
        case kFocusIndication: return "Focus indication";
        case kObjectInMemory: return "Pictures waiting";
        case kBatteryLevel: return "Battery (Sony)";
        case 0xD21B: return "Picture effect";
        case kIso: return "ISO";
        case kSaveDestination: return "Save to";
        case kS1Button: return "Shutter half press (S1)";
        case kS2Button: return "Shutter full press (S2)";
        case 0xD2C3: return "Movie button";
        case 0xD2C7: return "Still image button";
        case 0xD2D1: return "Focus near / far";
        default: return nullptr;
    }
}

std::optional<double> shutterSeconds(int64_t v) {
    const auto num = static_cast<double>(v >> 16 & 0xFFFF), den = static_cast<double>(v & 0xFFFF);
    if (v == 0 || den == 0) return std::nullopt;
    return num / den;
}

int64_t shutterValue(int numerator, int denominator) { return static_cast<int64_t>(numerator) << 16 | denominator; }

std::optional<int> isoNumber(int64_t v) {
    if ((v & 0xFFFFFF) == kIsoAuto) return std::nullopt;
    return static_cast<int>(v & 0xFFFFFF);
}

std::string formatValue(uint16_t code, int64_t v) {
    switch (code) {
        case kShutterSpeed: {
            if (v == 0) return "bulb";
            const int64_t num = v >> 16 & 0xFFFF, den = v & 0xFFFF;
            if (den == 0) return hex(v);
            if (num == 1 && den > 1) return std::format("1/{}", den);
            if (num % den == 0) return std::format("{}\"", num / den);
            return std::format("{:g}\"", static_cast<double>(num) / static_cast<double>(den));
        }
        case kIso: {
            const auto iso = isoNumber(v);
            const std::string nr = v >> 24 & 0xFF ? " (multi-frame NR)" : "";
            return iso ? std::format("ISO {}{}", *iso, nr) : "ISO auto" + nr;
        }
        case kFNumber: return std::format("f/{:g}", static_cast<double>(v) / 100);
        case kExposureProgram:
            switch (v) {
                case 1: return "M";
                case 2: return "P";
                case 3: return "A";
                case 4: return "S";
                case 0x8000: return "Intelligent Auto";
                case 0x8001: return "Superior Auto";
                default: return hex(v);
            }
        case kCompression:
            switch (v) {
                case 0x02: return "Standard";
                case 0x03: return "Fine";
                case 0x04: return "Extra fine";
                case 0x10: return "RAW";
                case 0x13: return "RAW+JPEG";
                default: return hex(v);
            }
        case kFocusMode:
            switch (v) {
                case 1: return "MF";
                case 2: return "AF-S";
                case 0x8004: return "AF-C";
                case 0x8005: return "AF-A";
                case 0x8006: return "DMF";
                default: return hex(v);
            }
        case kObjectInMemory: return v > 0x8000 ? std::format("{} ({})", v - 0x8000, hex(v)) : std::format("none ({})", hex(v));
        case kSaveDestination:
            switch (v) {
                case 0x01: return "PC";
                case 0x10: return "camera";
                case 0x11: return "PC and camera";
                default: return hex(v);
            }
        default: return v > 9 || v < 0 ? std::format("{} ({})", v, hex(v)) : std::to_string(v);
    }
}

double sensorWidthMm(std::string_view model) {
    static const std::pair<std::string_view, double> sizes[] = {
        {"ZV-E10", 23.5}, {"ZV-E1", 35.6}, {"ZV-1", 13.2}, {"ILCE-7", 35.9}, {"ILCE-9", 35.6}, {"ILCE-1", 35.9}, {"ILCA-99", 35.9},
        {"ILCE-6", 23.5}, {"ILCE-5", 23.5}, {"ILCE-3", 23.5}, {"ILCA-", 23.5}, {"NEX-", 23.5}, {"SLT-A99", 35.8}, {"SLT-", 23.5},
        {"DSC-RX100", 13.2}, {"DSC-RX10", 13.2}, {"DSC-RX1R", 35.9}, {"DSC-RX1", 35.8},
    };
    for (const auto& [prefix, mm] : sizes)
        if (model.starts_with(prefix)) return mm;
    return 0;
}

std::span<const uint8_t> findJpeg(std::span<const uint8_t> b) {
    std::size_t soi = b.size();
    for (std::size_t i = 0; i + 2 < b.size(); ++i)
        if (b[i] == 0xFF && b[i + 1] == 0xD8 && b[i + 2] == 0xFF) {
            soi = i;
            break;
        }
    for (std::size_t i = b.size(); soi < b.size() && i >= soi + 4; --i)
        if (b[i - 2] == 0xFF && b[i - 1] == 0xD9) return b.subspan(soi, i - soi);
    return {};
}

const ShotObject* Shot::jpeg() const {
    for (const auto& o : objects)
        if (o.info.format == ptp::kFormatJpeg) return &o;
    return nullptr;
}

// ---------------------------------------------------------------- Session

const ExtInfo& Session::connect() {
    ptp::receive(t_, kSdioConnect, {1, 0, 0}, "Sony handshake, step 1");
    ptp::receive(t_, kSdioConnect, {2, 0, 0}, "Sony handshake, step 2");
    ext_ = parseExtInfo(ptp::receive(t_, kGetSdioExtDeviceInfo, {kProtocol200}, "Sony extension info"));
    ptp::receive(t_, kSdioConnect, {3, 0, 0}, "Sony handshake, step 3");
    // the settings: right after the handshake the camera may say it is busy for a moment
    const auto t0 = Clock::now();
    for (;;) {
        std::vector<uint8_t> d;
        const auto r = t_.transact(kGetAllDevicePropData, {}, ptp::Data::In, {}, &d);
        if (r.ok()) {
            propData_ = std::move(d);
            props_ = parseAllProps(propData_, ext_.version, &stoppedAt_);
            return ext_;
        }
        if (r.code != ptp::kDeviceBusy || Clock::now() - t0 > std::chrono::seconds(3))
            throw CameraError(std::format("read the camera's settings: camera answered {}", ptp::responseText(r.code)));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Session::refresh() {
    propData_ = ptp::receive(t_, kGetAllDevicePropData, {}, "read the camera's settings");
    props_ = parseAllProps(propData_, ext_.version, &stoppedAt_);
}

bool Session::settle(std::chrono::milliseconds max) {
    return waitFor(
        [&] {
            refresh();
            if (const Prop* p = prop(kIso)) return p->current != 0;
            if (const Prop* p = prop(kShutterSpeed)) return p->current != 0;
            return true;
        },
        max);
}

const Prop* Session::prop(uint16_t code) const {
    const auto it = std::ranges::find(props_, code, &Prop::code);
    return it == props_.end() ? nullptr : &*it;
}

uint16_t Session::setA(uint16_t code, int64_t value) {
    const Prop* p = prop(code);
    if (!p) throw CameraError(std::format("the camera has no property 0x{:04X}", code));
    ptp::Writer w;
    writeValue(w, p->type, value);
    const uint32_t param = code;
    return t_.transact(kSetControlDeviceA, std::span(&param, 1), ptp::Data::Out, w.data(), nullptr).code;
}

uint16_t Session::setB(uint16_t code, uint16_t type, int64_t value) {
    ptp::Writer w;
    w.value(type, value);
    const uint32_t param = code;
    return t_.transact(kSetControlDeviceB, std::span(&param, 1), ptp::Data::Out, w.data(), nullptr).code;
}

bool Session::waitFor(const std::function<bool()>& done, std::chrono::milliseconds timeout, const std::function<bool()>& stop) {
    const auto t0 = Clock::now();
    for (;;) {
        if (done()) return true;
        if ((stop && stop()) || Clock::now() - t0 > timeout) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs_));
    }
}

Session::SetMethod Session::setValue(uint16_t code, int64_t target, const std::function<double(int64_t)>& order) {
    const char* n = propName(code);
    const std::string name = n ? n : std::format("property 0x{:04X}", code);
    refresh();
    const Prop* p = prop(code);
    if (!p) throw CameraError(std::format("the camera has no {}", name));
    if (!p->settable()) {
        const bool exposure = code == kShutterSpeed || code == kIso;
        throw CameraError(std::format("{} can't be set from the computer now{}", name, exposure ? " (turn the mode dial to M)" : ""));
    }
    if (p->current == target) return SetMethod::None;
    auto current = [&] { return prop(code) ? prop(code)->current : target; };
    // directly
    if (setA(code, target) == ptp::kOk && waitFor([&] { return refresh(), current() == target; }, std::chrono::milliseconds(1500)))
        return SetMethod::Direct;
    // else a notch at a time
    const int64_t start = current();
    const double goal = order(target);
    for (int i = 0; i < 100; ++i) {
        const int64_t was = current();
        if (was == target) return SetMethod::Steps;
        int& sign = stepSign_[code];
        const bool up = goal > order(was);
        const int64_t step = sign == 0 || (sign > 0) == up ? kStepUp : kStepDown;
        if (setB(code, ptp::kUint8, step) != ptp::kOk) break;
        if (!waitFor([&] { return refresh(), current() != was; }, std::chrono::milliseconds(1500))) break;   // an end of the range
        const int64_t now = current();
        if (sign == 0) sign = (step == kStepUp) == (order(now) > order(was)) ? 1 : -1;
        const double before = order(was) - goal, after = order(now) - goal;
        if (before * after < 0) {   // stepped past it: the camera doesn't offer it. The nearer one of the two.
            if (std::abs(before) < std::abs(after)) {
                setB(code, ptp::kUint8, step == kStepUp ? kStepDown : kStepUp);
                waitFor([&] { return refresh(), current() != now; }, std::chrono::milliseconds(1500));
            }
            return SetMethod::Steps;
        }
    }
    if (current() != start) return SetMethod::Steps;   // as far as the camera goes
    throw CameraError(std::format("{}: the camera didn't take {}", name, formatValue(code, target)));
}

int Session::objectsInMemory() const {
    const Prop* p = prop(kObjectInMemory);
    return p && p->current > 0x8000 ? static_cast<int>(p->current - 0x8000) : 0;
}

ShotObject Session::fetch() {
    const auto t0 = Clock::now();
    ShotObject o;
    o.info = ptp::parseObjectInfo(ptp::receive(t_, ptp::kGetObjectInfo, {kShotObject}, "picture information"));
    o.data = ptp::receive(t_, ptp::kGetObject, {kShotObject}, "picture download");
    o.downloadMs = msSince(t0);
    return o;
}

int Session::drain() {
    int n = 0;
    refresh();
    for (; objectsInMemory() > 0 && n < 8; ++n) {
        fetch();
        refresh();
    }
    return n;
}

std::optional<Shot> Session::shoot(std::chrono::milliseconds timeout, const std::function<bool()>& stop) {
    // A picture still to come from a shot abandoned before (a stream stopped while the camera worked on it), or one
    // still in the camera (its download failed), would be taken for this one: let it come, and drop it.
    if (owed_) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*owed_ - Clock::now());
        const bool came = left.count() > 0 && waitFor([&] { return refresh(), objectsInMemory() > 0; }, left, stop);
        if (!came && stop && stop()) return std::nullopt;
        owed_.reset();
    }
    refresh();
    if (objectsInMemory() > 0) drain();
    Shot s;
    const auto t0 = Clock::now();
    auto press = [&](uint16_t button, int64_t state, const char* what) {
        if (const uint16_t r = setB(button, ptp::kUint16, state); r != ptp::kOk) {
            setB(kS2Button, ptp::kUint16, kButtonUp);   // let go of everything before giving up
            setB(kS1Button, ptp::kUint16, kButtonUp);
            throw CameraError(std::format("shutter {}: camera answered {}", what, ptp::responseText(r)));
        }
    };
    press(kS1Button, kButtonDown, "half press");
    press(kS2Button, kButtonDown, "full press");
    s.triggered = Clock::now();
    owed_ = s.triggered + timeout;   // until the camera holds it
    press(kS2Button, kButtonUp, "release");
    press(kS1Button, kButtonUp, "release");
    s.pressMs = msSince(t0);
    if (!waitFor([&] { return refresh(), objectsInMemory() > 0; }, timeout, stop)) {
        if (stop && stop()) return std::nullopt;
        throw CameraError(std::format("no picture from the camera within {:.0f} s (on a telescope: is Release w/o Lens set to Enable?)",
                                      std::chrono::duration<double>(timeout).count()));
    }
    owed_.reset();
    for (int i = 0; i < 4 && objectsInMemory() > 0; ++i) {
        const double ready = msSince(s.triggered);
        s.objects.push_back(fetch());
        s.objects.back().readyMs = ready;
        refresh();
        // RAW+JPEG with the raw file first: the JPEG may still be on its way
        if (objectsInMemory() == 0 && !s.jpeg() && i == 0)
            waitFor([&] { return refresh(), objectsInMemory() > 0; }, std::chrono::seconds(3), stop);
    }
    return s;
}

std::vector<uint8_t> Session::liveView(uint16_t* response) {
    std::vector<uint8_t> d;
    const uint32_t handle = kLiveViewObject;
    const auto r = t_.transact(ptp::kGetObject, std::span(&handle, 1), ptp::Data::In, {}, &d);
    if (response) *response = r.code;
    if (!r.ok()) return {};
    const auto j = findJpeg(d);
    return {j.begin(), j.end()};
}

}  // namespace slopcap::sony

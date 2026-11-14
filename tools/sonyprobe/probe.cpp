#include "probe.hpp"

#include "analysis/focus.hpp"
#include "camera/jpeg_luma.hpp"
#include "camera/sony_ptp.hpp"

#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <thread>

namespace sonyprobe {

namespace ptp = slopcap::ptp;
namespace sony = slopcap::sony;

// ---------------------------------------------------------------- Json

Json Json::array() {
    Json j;
    j.kind_ = Kind::Array;
    return j;
}

Json Json::object() {
    Json j;
    j.kind_ = Kind::Object;
    return j;
}

Json& Json::operator[](const std::string& key) {
    kind_ = Kind::Object;
    for (auto& [k, v] : o_)
        if (k == key) return v;
    o_.emplace_back(key, Json());
    return o_.back().second;
}

void Json::push(Json v) {
    kind_ = Kind::Array;
    a_.push_back(std::move(v));
}

namespace {

std::string quoted(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) o += std::format("\\u{:04x}", c);
                else o += c;
        }
    }
    return o + "\"";
}

}  // namespace

std::string Json::dump(int indent) const {
    const std::string pad(static_cast<std::size_t>(indent + 2), ' '), end(static_cast<std::size_t>(indent), ' ');
    switch (kind_) {
        case Kind::Null: return "null";
        case Kind::Bool: return b_ ? "true" : "false";
        case Kind::Number:
            if (!std::isfinite(n_)) return "null";
            if (n_ == std::floor(n_) && std::fabs(n_) < 1e15) return std::format("{}", static_cast<long long>(n_));
            return std::format("{:.6g}", n_);
        case Kind::String: return quoted(s_);
        case Kind::Array: {
            if (a_.empty()) return "[]";
            // short arrays of scalars on one line (code lists, values)
            bool flat = true;
            for (const auto& v : a_) flat = flat && v.kind_ != Kind::Array && v.kind_ != Kind::Object;
            std::string o = "[";
            for (std::size_t i = 0; i < a_.size(); ++i) o += (i ? flat ? ", " : ",\n" + pad : "\n" + pad) + a_[i].dump(indent + 2);
            return o + (flat ? "]" : "\n" + end + "]");
        }
        case Kind::Object: {
            if (o_.empty()) return "{}";
            std::string o = "{";
            for (std::size_t i = 0; i < o_.size(); ++i) o += (i ? ",\n" : "\n") + pad + quoted(o_[i].first) + ": " + o_[i].second.dump(indent + 2);
            return o + "\n" + end + "}";
        }
    }
    return "null";
}

std::string hex4(uint32_t v) { return std::format("0x{:04X}", v); }

// ---------------------------------------------------------------- the checks

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

Json codes(const std::vector<uint16_t>& v) {
    Json a = Json::array();
    for (auto c : v) a.push(hex4(c));
    return a;
}

std::string hexBytes(const std::vector<uint8_t>& d) {
    std::string s;
    s.reserve(d.size() * 2);
    for (auto b : d) s += std::format("{:02x}", b);
    return s;
}

std::string name(uint16_t code) {
    const char* n = sony::propName(code);
    return n ? n : hex4(code);
}

double shutterOrder(int64_t v) { return std::log(sony::shutterSeconds(v).value_or(1000.0)); }
double isoOrder(int64_t v) { return std::log(static_cast<double>(sony::isoNumber(v).value_or(50))); }
double asIs(int64_t v) { return static_cast<double>(v); }

std::string kb(std::size_t bytes) { return bytes >= 1 << 20 ? std::format("{:.1f} MB", bytes / 1048576.0) : std::format("{:.0f} KB", bytes / 1024.0); }

const char* formName(sony::Prop::Form f) {
    switch (f) {
        case sony::Prop::Form::Range: return "range";
        case sony::Prop::Form::Enum: return "enum";
        default: return "none";
    }
}

Json listProps(const sony::Session& s) {
    Json props = Json::array();
    for (const auto& p : s.props()) {
        Json j = Json::object();
        j["code"] = hex4(p.code);
        j["name"] = name(p.code);
        j["type"] = hex4(p.type);
        j["getSet"] = p.getSet;
        j["enabled"] = p.enabled;
        j["current"] = p.current;
        j["currentText"] = sony::formatValue(p.code, p.current);
        j["factory"] = p.factory;
        j["form"] = formName(p.form);
        if (p.form == sony::Prop::Form::Range) {
            j["min"] = p.min;
            j["max"] = p.max;
            j["step"] = p.step;
        }
        if (p.form == sony::Prop::Form::Enum) {
            Json v = Json::array(), vt = Json::array();
            for (auto x : p.values) v.push(x), vt.push(sony::formatValue(p.code, x));
            j["values"] = std::move(v);
            j["valuesText"] = std::move(vt);
        }
        props.push(std::move(j));
    }
    return props;
}

/// Every transaction through it, timed, into the report: where a camera got stuck shows in the last ones.
class Tracer final : public ptp::Transport {
public:
    explicit Tracer(ptp::Transport& t) : t_(t) {}
    ptp::Response transact(uint16_t op, std::span<const uint32_t> params, ptp::Data phase, std::span<const uint8_t> out,
                           std::vector<uint8_t>* in) override {
        Json e = Json::object();
        e["atMs"] = msSince(t0_);
        e["op"] = hex4(op);
        if (!params.empty()) {
            Json p = Json::array();
            for (auto x : params) p.push(std::format("0x{:X}", x));
            e["params"] = std::move(p);
        }
        if (phase == ptp::Data::Out) e["out"] = out.size();
        const auto t0 = Clock::now();
        try {
            auto r = t_.transact(op, params, phase, out, in);
            e["ms"] = msSince(t0);
            e["response"] = hex4(r.code);
            if (in && phase == ptp::Data::In) e["in"] = in->size();
            add(std::move(e));
            return r;
        } catch (const std::exception& x) {
            e["ms"] = msSince(t0);
            e["error"] = x.what();
            add(std::move(e));
            throw;
        }
    }
    std::string kind() const override { return t_.kind(); }
    std::vector<ptp::Event> events() const override { return t_.events(); }
    bool recover() override { return t_.recover(); }
    Json take() { return std::move(trace_); }

private:
    void add(Json e) {
        if (count_++ < 3000) trace_.push(std::move(e));
    }
    ptp::Transport& t_;
    Clock::time_point t0_ = Clock::now();
    Json trace_ = Json::array();
    int count_ = 0;
};

/// The handshake's operations one by one, their answers, for a camera that refused it.
Json handshakeDiagnosis(ptp::Transport& t, std::ostream& log) {
    Json d = Json::array();
    const std::pair<uint16_t, std::vector<uint32_t>> ops[] = {{sony::kSdioConnect, {1, 0, 0}},
                                                              {sony::kSdioConnect, {2, 0, 0}},
                                                              {sony::kGetSdioExtDeviceInfo, {sony::kProtocol200}},
                                                              {sony::kSdioConnect, {3, 0, 0}},
                                                              {sony::kGetAllDevicePropData, {}}};
    for (const auto& [op, params] : ops) {
        Json r = Json::object();
        r["op"] = hex4(op);
        Json p = Json::array();
        for (auto x : params) p.push(x);
        r["params"] = std::move(p);
        try {
            std::vector<uint8_t> in;
            const auto res = t.transact(op, params, ptp::Data::In, {}, &in);
            r["response"] = ptp::responseText(res.code);
            r["bytes"] = in.size();
            log << std::format("    {} {}: {} ({} bytes)\n", hex4(op), params.empty() ? 0u : params[0], ptp::responseText(res.code), in.size());
        } catch (const std::exception& e) {
            r["error"] = e.what();
            log << std::format("    {}: {}\n", hex4(op), e.what());
        }
        d.push(std::move(r));
    }
    return d;
}

}  // namespace

Json run(ptp::Transport& raw, const Options& o, std::ostream& log, std::vector<File>& files) {
    Tracer t(raw);
    Json cam = Json::object();
    Json steps = Json::array();
    auto s = std::make_unique<sony::Session>(t);
    // After a transaction got stuck: PTP's own reset, a new session, Sony's handshake again; then the next check.
    auto recover = [&](Json& rec) {
        log << "  recovering (USB reset, new session, Sony handshake): ";
        bool ok = t.recover();
        if (ok) {
            try {
                s = std::make_unique<sony::Session>(t);
                s->connect();
                s->settle(3s);
            } catch (const std::exception& e) {
                ok = false;
                rec["recoverError"] = e.what();
            }
        }
        rec["recovered"] = ok;
        log << (ok ? "done\n" : "failed\n");
        return ok;
    };
    auto step = [&](const std::string& what, auto&& fn) {
        const auto t0 = Clock::now();
        Json rec = Json::object();
        rec["step"] = what;
        bool ok = true, stuck = false;
        try {
            fn(rec);
        } catch (const ptp::LinkError& e) {
            ok = false, stuck = true;
            rec["error"] = e.what();
            log << "  " << what << ": FAILED: " << e.what() << "\n";
        } catch (const std::exception& e) {
            ok = false;
            rec["error"] = e.what();
            log << "  " << what << ": FAILED: " << e.what() << "\n";
        }
        rec["ok"] = ok;
        rec["ms"] = msSince(t0);
        if (stuck) recover(rec);
        steps.push(std::move(rec));
        return ok;
    };
    auto finish = [&] {
        Json ev = Json::array();
        std::map<uint16_t, int> count;
        for (const auto& e : t.events()) {
            Json j = Json::object();
            j["atMs"] = e.atMs;
            j["code"] = hex4(e.code);
            Json p = Json::array();
            for (auto x : e.params) p.push(hex4(x));
            j["params"] = std::move(p);
            ev.push(std::move(j));
            ++count[e.code];
        }
        std::string summary;
        for (const auto& [code, n] : count) summary += std::format(" {} x{}", hex4(code), n);
        log << std::format("  events from the camera: {}{}\n", t.events().size(), summary);
        cam["events"] = std::move(ev);
        cam["steps"] = std::move(steps);
        cam["trace"] = t.take();
    };
    cam["transport"] = t.kind();

    // ---- who it is
    ptp::DeviceInfo di;
    if (step("device info (GetDeviceInfo)", [&](Json&) { di = ptp::parseDeviceInfo(ptp::receive(t, ptp::kGetDeviceInfo, {}, "GetDeviceInfo")); })) {
        Json d = Json::object();
        d["manufacturer"] = di.manufacturer;
        d["model"] = di.model;
        d["firmware"] = di.deviceVersion;
        d["serial"] = o.keepSerial ? di.serial : std::format("(left out: {} characters)", di.serial.size());
        d["standardVersion"] = di.standardVersion;
        d["vendorExtensionId"] = hex4(di.vendorExtensionId);
        d["vendorExtensionVersion"] = di.vendorExtensionVersion;
        d["vendorExtensionDesc"] = di.vendorExtensionDesc;
        d["functionalMode"] = di.functionalMode;
        d["operations"] = codes(di.operations);
        d["events"] = codes(di.events);
        d["properties"] = codes(di.properties);
        d["captureFormats"] = codes(di.captureFormats);
        d["imageFormats"] = codes(di.imageFormats);
        cam["deviceInfo"] = std::move(d);
        log << std::format("  camera: {} {}, firmware {}\n", di.manufacturer, di.model, di.deviceVersion);
        std::string sonyOps;
        for (auto op : di.operations)
            if (op >= 0x9200 && op < 0x9300) sonyOps += " " + hex4(op);
        log << std::format("  {} operations, {} properties; Sony's:{}\n", di.operations.size(), di.properties.size(),
                           sonyOps.empty() ? " none (USB Connection not set to PC Remote?)" : sonyOps);
    }

    // ---- Sony's extension
    if (!step("Sony handshake (SDIO connect 1, 2, extension info, connect 3, all properties)", [&](Json&) { s->connect(); }) &&
        s->props().empty()) {
        log << "  the handshake's operations one by one:\n";
        cam["handshakeDiagnosis"] = handshakeDiagnosis(t, log);
        finish();
        return cam;
    }
    {
        Json x = Json::object();
        x["version"] = s->ext().version;
        Json lists = Json::array();
        for (const auto& l : s->ext().lists) lists.push(codes(l));
        x["lists"] = std::move(lists);
        cam["sonyExtension"] = std::move(x);
        log << std::format("  Sony protocol version {:.2f}, {} codes listed\n", s->ext().version / 100.0, s->ext().codes().size());
    }
    cam["propertiesAtConnect"] = listProps(*s);
    cam["propertiesRawAtConnect"] = hexBytes(s->lastPropData());

    // ---- the camera fills in its values a moment after the handshake (they read 0 until then)
    step("wait for the camera's values", [&](Json& r) {
        const auto t0 = Clock::now();
        const bool ready = s->settle(5s);
        r["ready"] = ready;
        log << std::format("  the camera's values {} after {:.1f} s\n", ready ? "came" : "still read 0", msSince(t0) / 1000);
    });

    // ---- properties
    cam["properties"] = listProps(*s);
    cam["propertiesRaw"] = hexBytes(s->lastPropData());
    cam["propertiesStoppedAt"] = s->propsStoppedAt();
    log << std::format("  {} properties read ({} bytes){}\n", s->props().size(), s->lastPropData().size(),
                       s->propsStoppedAt() ? std::format("; could not read past byte {}", s->propsStoppedAt()) : "");
    for (uint16_t c : {sony::kExposureProgram, sony::kShutterSpeed, sony::kIso, sony::kCompression, sony::kFocusMode, sony::kSaveDestination,
                       sony::kObjectInMemory, sony::kBatteryLevel}) {
        if (const auto* p = s->prop(c))
            log << std::format("    {:<20} {:<18} {}\n", name(c), sony::formatValue(c, p->current),
                               p->settable() ? "settable" : p->getSet ? "not settable now" : "read-only");
        else log << std::format("    {:<20} (not reported)\n", name(c));
    }
    step("read all properties 3 times", [&](Json& j) {
        const auto t0 = Clock::now();
        for (int i = 0; i < 3; ++i) s->refresh();
        j["msEach"] = msSince(t0) / 3;
        log << std::format("  reading all properties takes {:.0f} ms\n", msSince(t0) / 3);
    });

    // ---- changing the shutter speed and ISO one notch, and back (in notches even if the camera calls them fixed)
    if (o.setTest) {
        Json tests = Json::array();
        // one notch with SetControlDeviceB: the value it moved to, or nothing
        auto notch = [&](uint16_t code, int64_t dir, Json& j) -> std::optional<int64_t> {
            const int64_t was = s->prop(code)->current;
            const uint16_t r = s->setB(code, ptp::kUint8, dir);
            j[dir == sony::kStepUp ? "stepUpAnswer" : "stepDownAnswer"] = ptp::responseText(r);
            for (int i = 0; i < 15; ++i) {
                std::this_thread::sleep_for(100ms);
                s->refresh();
                if (s->prop(code)->current != was) return s->prop(code)->current;
            }
            return std::nullopt;
        };
        auto test = [&](uint16_t code, double (*order)(int64_t), std::vector<int64_t> standard) {
            Json j = Json::object();
            j["property"] = name(code);
            step(std::format("set {}", name(code)), [&](Json&) {
                const auto* p = s->prop(code);
                if (!p) {
                    j["result"] = "not reported";
                    return;
                }
                const int64_t from = p->current;
                j["from"] = sony::formatValue(code, from);
                j["getSet"] = p->getSet;
                j["enabled"] = p->enabled;
                if (!p->settable()) {   // the a6000 calls them fixed even on M: try a notch anyway
                    const auto moved = notch(code, sony::kStepUp, j);
                    if (!moved) {
                        j["result"] = "flagged not settable, and a notch didn't move it";
                        log << std::format("  set {}: flagged not settable; a notch didn't move it\n", name(code));
                        return;
                    }
                    const auto back = notch(code, sony::kStepDown, j);
                    j["result"] = std::format("flagged not settable, but notches work: {} -> {} -> {}", sony::formatValue(code, from),
                                              sony::formatValue(code, *moved), back ? sony::formatValue(code, *back) : "(didn't move back)");
                    log << std::format("  set {}: flagged not settable, but notches work ({} -> {})\n", name(code), sony::formatValue(code, from),
                                       sony::formatValue(code, *moved));
                    return;
                }
                const auto& choices = p->form == sony::Prop::Form::Enum && !p->values.empty() ? p->values : standard;
                std::optional<int64_t> to;
                for (auto v : choices)   // the nearest other value
                    if (std::abs(order(v) - order(from)) > 1e-6 && std::abs(order(v)) < 50 &&
                        (!to || std::abs(order(v) - order(from)) < std::abs(order(*to) - order(from))))
                        to = v;
                if (!to) return;
                j["to"] = sony::formatValue(code, *to);
                const auto t0 = Clock::now();
                const auto m = s->setValue(code, *to, order);
                j["method"] = m == sony::Session::SetMethod::Direct ? "direct" : m == sony::Session::SetMethod::Steps ? "steps" : "none";
                j["got"] = sony::formatValue(code, s->prop(code)->current);
                j["ms"] = msSince(t0);
                log << std::format("  set {}: {} -> {}: {} ({}, {:.0f} ms)\n", name(code), sony::formatValue(code, from), sony::formatValue(code, *to),
                                   sony::formatValue(code, s->prop(code)->current),
                                   m == sony::Session::SetMethod::Direct ? "set directly" : "in steps", msSince(t0));
                s->setValue(code, from, order);
                j["restored"] = s->prop(code)->current == from;
            });
            tests.push(std::move(j));
        };
        std::vector<int64_t> shutters, isos;
        for (int d : {30, 60, 125, 250, 500}) shutters.push_back(sony::shutterValue(1, d));
        for (int i : {100, 200, 400, 800, 1600}) isos.push_back(i);
        test(sony::kShutterSpeed, shutterOrder, shutters);
        test(sony::kIso, isoOrder, isos);
        cam["setTests"] = std::move(tests);
    }

    // ---- pictures (RAW alone has no JPEG: RAW+JPEG for them, and back)
    if (o.shot) {
        std::optional<int64_t> quality;
        step("Quality for the pictures", [&](Json& r) {
            const auto* q = s->prop(sony::kCompression);
            if (!q) return;
            r["was"] = sony::formatValue(sony::kCompression, q->current);
            if (q->current != sony::kQualityRaw) return;
            if (!q->settable()) {
                log << "  Quality is RAW and can't be set from here: no JPEG to score (set Fine on the camera)\n";
                return;
            }
            s->setValue(sony::kCompression, sony::kQualityRawJpeg, asIs);
            quality = sony::kQualityRaw;
            r["now"] = sony::formatValue(sony::kCompression, s->prop(sony::kCompression)->current);
            log << "  Quality RAW: RAW+JPEG for the pictures (put back after)\n";
        });
        step("drop pictures left in the camera", [&](Json& j) { j["dropped"] = s->drain(); });
        Json shots = Json::array();
        for (int n = 1; n <= o.shots; ++n) {
            step(std::format("picture {}", n), [&](Json&) {
                const auto* sp = s->prop(sony::kShutterSpeed);
                const double secs = sp ? sony::shutterSeconds(sp->current).value_or(30) : 30;
                const auto timeout = std::chrono::milliseconds(static_cast<int64_t>((2 * secs + 15) * 1000));
                auto shot = s->shoot(timeout);
                if (!shot) throw slopcap::CameraError("no picture");
                // RAW+JPEG: the second file may come a moment later
                const auto t0 = Clock::now();
                while (n == 1 && msSince(t0) < 3000) {
                    s->refresh();
                    if (s->objectsInMemory() > 0) {
                        shot->objects.push_back(s->fetch());
                        shot->objects.back().readyMs = std::chrono::duration<double, std::milli>(Clock::now() - shot->triggered).count();
                    } else {
                        std::this_thread::sleep_for(100ms);
                    }
                }
                Json pic = Json::object();
                pic["pressMs"] = shot->pressMs;
                Json objs = Json::array();
                int k = 0;
                for (const auto& ob : shot->objects) {
                    Json x = Json::object();
                    x["format"] = ptp::formatName(ob.info.format);
                    x["bytes"] = ob.data.size();
                    x["infoSize"] = ob.info.compressedSize;
                    x["width"] = ob.info.imageWidth;
                    x["height"] = ob.info.imageHeight;
                    x["filename"] = ob.info.filename;
                    x["readyMs"] = ob.readyMs;
                    x["downloadMs"] = ob.downloadMs;
                    objs.push(std::move(x));
                    log << std::format("  picture {}: {} {}, ready {:.2f} s after the shutter, downloaded in {:.2f} s\n", n,
                                       ptp::formatName(ob.info.format), kb(ob.data.size()), ob.readyMs / 1000, ob.downloadMs / 1000);
                    const std::string ext = ob.info.format == ptp::kFormatJpeg ? "jpg" : ob.info.format == ptp::kFormatSonyArw ? "arw" : "bin";
                    files.push_back({std::format("shot{}{}.{}", n, k++ ? std::format("-{}", k) : "", ext), ob.data});
                }
                pic["objects"] = std::move(objs);
                if (const auto* jp = shot->jpeg()) {
                    std::vector<uint8_t> luma;
                    const auto d0 = Clock::now();
                    const auto sz = slopcap::jpeg::decodeLuma(jp->data, luma);
                    const double dms = msSince(d0);
                    const auto score = slopcap::analysis::scoreFrame(std::as_bytes(std::span(luma)), sz.width, sz.height, 8, slopcap::Bayer::None);
                    pic["decodeMs"] = dms;
                    pic["lumaWidth"] = sz.width;
                    pic["lumaHeight"] = sz.height;
                    pic["sharpness"] = score.channels ? score.ch[0].sharpness : 0.0;
                    log << std::format("  picture {}: luminance {} x {} decoded in {:.2f} s; focus score {:.4g}\n", n, sz.width, sz.height,
                                       dms / 1000, score.channels ? score.ch[0].sharpness : 0.0);
                } else {
                    log << std::format("  picture {}: no JPEG (Quality set to RAW alone?)\n", n);
                }
                shots.push(std::move(pic));
            });
        }
        cam["pictures"] = std::move(shots);
        if (quality)
            step("Quality back", [&](Json& r) {
                s->setValue(sony::kCompression, *quality, asIs);
                r["now"] = sony::formatValue(sony::kCompression, s->prop(sony::kCompression)->current);
            });
    }

    // ---- live view (GetObject 0xFFFFC002), last: the a6000 went silent on it and spoiled what came after
    if (o.liveFrames > 0) {
        Json lv = Json::object();
        int frames = 0;
        double ms = 0;
        uint16_t first = 0, last = 0;
        const bool ok = step("live view (GetObject 0xFFFFC002)", [&](Json&) {
            const auto t0 = Clock::now();
            for (int i = 0; i < o.liveFrames; ++i) {
                std::vector<uint8_t> d;
                const auto f0 = Clock::now();
                const uint32_t handle = sony::kLiveViewObject;
                const uint16_t code = t.transact(ptp::kGetObject, std::span(&handle, 1), ptp::Data::In, {}, &d).code;
                if (i == 0) first = code;
                last = code;
                if (code != ptp::kOk) {   // not ready right after connecting: some bodies need a moment
                    if (frames == 0 && msSince(t0) < 2000) {
                        std::this_thread::sleep_for(200ms);
                        --i;
                        continue;
                    }
                    break;
                }
                ms += msSince(f0);
                const auto jpeg = sony::findJpeg(d);
                if (frames++ == 0) {
                    lv["blobBytes"] = d.size();
                    lv["jpegBytes"] = jpeg.size();
                    lv["jpegOffset"] = jpeg.empty() ? 0 : jpeg.data() - d.data();
                    if (!jpeg.empty()) {
                        try {
                            const auto sz = slopcap::jpeg::size(jpeg);
                            lv["width"] = sz.width, lv["height"] = sz.height;
                        } catch (const std::exception& e) {
                            lv["jpegError"] = e.what();
                        }
                        files.push_back({"liveview.jpg", {jpeg.begin(), jpeg.end()}});
                    }
                    files.push_back({"liveview.bin", d});
                }
            }
        });
        lv["firstResponse"] = ptp::responseText(first);
        lv["lastResponse"] = ptp::responseText(last);
        lv["frames"] = frames;
        if (frames) lv["msPerFrame"] = ms / frames;
        if (!ok) lv["error"] = "no answer: see steps";
        if (frames)
            log << std::format("  live view over USB: YES, {} frames, {:.0f} ms a frame\n", frames, ms / frames);
        else
            log << std::format("  live view over USB: no ({})\n", ok ? "camera answered " + ptp::responseText(first) : std::string("no answer at all"));
        cam["liveView"] = std::move(lv);
    }

    // ---- the settings once more, after all that
    step("read all properties at the end", [&](Json&) {
        s->refresh();
        cam["propertiesAtEnd"] = listProps(*s);
    });
    finish();
    return cam;
}

}  // namespace sonyprobe

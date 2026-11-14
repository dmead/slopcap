#include "fakes/fake_sony.hpp"

#include "fakes/jpeg_writer.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace fake {

namespace {

using Clock = std::chrono::steady_clock;

bool hasDataOut(uint16_t op) { return op == sony::kSetControlDeviceA || op == sony::kSetControlDeviceB; }

}  // namespace

void drawSoftDisc(uint8_t* px, int w, int h, double blur) {
    const double r = std::min(w, h) / 4.0, cx = w / 2.0, cy = h / 2.0, edge = 1 + blur;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double v = std::clamp((r - std::hypot(x - cx, y - cy)) / edge + 0.5, 0.0, 1.0);
            const double band = 1 + 0.25 * std::sin(y / (2.0 * edge));   // belts, softer when out of focus
            px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                static_cast<uint8_t>(std::clamp(12 + 190 * v * band, 0.0, 255.0));
        }
}

// ---------------------------------------------------------------- the camera

SonyCam::SonyCam() {
    operations = {ptp::kGetDeviceInfo, ptp::kOpenSession,  ptp::kCloseSession,         ptp::kGetObjectInfo,       ptp::kGetObject,
                  ptp::kGetDevicePropDesc, sony::kSdioConnect, sony::kGetSdioExtDeviceInfo, sony::kSetControlDeviceA, sony::kSetControlDeviceB,
                  sony::kGetAllDevicePropData};
    // shortest exposure first, bulb last; ISO: auto, the values, a multi-frame noise reduction variant
    for (int d : {4000, 1000, 500, 250, 125, 60, 15, 4}) shutterList.push_back(sony::shutterValue(1, d));
    for (int n : {10, 40, 150, 300}) shutterList.push_back(sony::shutterValue(n, 10));
    shutterList.push_back(0);
    isoList = {sony::kIsoAuto, 100, 200, 400, 800, 1600, 3200, int64_t{1} << 24 | 800};
}

void SonyCam::setDraw(std::function<void(uint8_t*, int, int, int)> draw) {
    std::lock_guard l(m_);
    draw_ = std::move(draw);
}

void SonyCam::setUnplugged(bool gone) {
    std::lock_guard l(m_);
    unplugged_ = gone;
}

void SonyCam::event(uint16_t code, uint32_t param) {
    ptp::Writer w;
    w.u32(16).u16(4).u16(code).u32(0).u32(param);
    events_.push_back(w.take());
    if (events_.size() > 100) events_.pop_front();   // (nobody reads them through the plain transport)
}

std::optional<std::vector<uint8_t>> SonyCam::takeEvent() {
    std::lock_guard l(m_);
    if (events_.empty()) return std::nullopt;
    auto e = std::move(events_.front());
    events_.pop_front();
    return e;
}

int SonyCam::eventsWaiting() const {
    std::lock_guard l(m_);
    return static_cast<int>(events_.size());
}

void SonyCam::resetPtp() {
    std::lock_guard l(m_);
    sessionOpen = false;
    sdioStep = 0;
    events_.clear();
}

std::vector<uint16_t> SonyCam::ops() const {
    std::lock_guard l(m_);
    return ops_;
}

std::vector<uint8_t> SonyCam::jpegOf(int w, int h, int n) {
    std::vector<uint8_t> luma(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    if (draw_) draw_(luma.data(), w, h, n);
    else drawSoftDisc(luma.data(), w, h, 0);
    JpegOptions o;
    o.quality = 92;
    o.exifThumbnail = true;
    return encodeJpeg(luma.data(), w, h, o);
}

void SonyCam::addShot(Clock::time_point at) {
    const int n = shots.load();
    auto jpeg = [&](Clock::time_point t) { objects_.push_back({ptp::kFormatJpeg, jpegOf(width, height, n), width, height, t, n}); };
    auto arw = [&](Clock::time_point t) {
        std::vector<uint8_t> raw(64 * 1024, 0);
        raw[0] = 'I', raw[1] = 'I', raw[2] = 42;
        objects_.push_back({ptp::kFormatSonyArw, std::move(raw), width, height, t, n});
    };
    switch (quality) {
        case Quality::Jpeg: jpeg(at); break;
        case Quality::RawOnly: arw(at); break;
        case Quality::RawPlusJpeg: jpeg(at), arw(at); break;
        case Quality::RawFirst: arw(at), jpeg(at + std::chrono::milliseconds(jpegLagMs)); break;
    }
}

void SonyCam::step(int64_t& value, const std::vector<int64_t>& list, int64_t dir) {
    auto it = std::ranges::find(list, value);
    const auto i = it == list.end() ? 0 : it - list.begin();
    value = list[static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(i + dir, 0, static_cast<std::ptrdiff_t>(list.size()) - 1))];
}

std::vector<uint8_t> SonyCam::props() {
    using F = sony::Prop::Form;
    const auto now = Clock::now();
    const auto ready = std::ranges::count_if(objects_, [&](const Object& o) { return o.readyAt <= now; });
    const int64_t q = quality == Quality::Jpeg ? 0x03 : quality == Quality::RawOnly ? 0x10 : 0x13;
    const uint8_t on = manual ? 1 : 0, set = flaggedFixed ? 0 : 1;
    const bool zero = readsSinceHandshake_++ < zeroReads;   // (a camera still catching up after the handshake)
    std::vector<sony::Prop> p = {
        {sony::kCompression, ptp::kUint8, static_cast<uint8_t>(qualitySettable ? 1 : 0), 1, 3, q, F::Enum, 0, 0, 0, {2, 3, 0x10, 0x13}},
        {sony::kExposureProgram, ptp::kUint16, 0, 1, 2, program, F::Enum, 0, 0, 0, {1, 2, 3, 4}},
        {sony::kFocusMode, ptp::kUint16, 1, 1, 2, 1, F::Enum, 0, 0, 0, {1, 2, 0x8004}},
        {sony::kShutterSpeed, ptp::kUint32, set, on, shutter, zero ? 0 : shutter, listValues ? F::Enum : F::None, 0, 0, 0,
         listValues ? shutterList : std::vector<int64_t>{}},
        {sony::kIso, ptp::kUint32, set, on, sony::kIsoAuto, zero ? 0 : iso, listValues ? F::Enum : F::None, 0, 0, 0,
         listValues ? isoList : std::vector<int64_t>{}},
        {sony::kObjectInMemory, ptp::kUint16, 0, 1, 0x8000, 0x8000 + ready, F::None, 0, 0, 0, {}},
        {sony::kBatteryLevel, ptp::kInt8, 0, 1, 0, 80, F::Range, 0, 100, 1, {}},
        {sony::kSaveDestination, ptp::kUint16, 1, 1, 1, 1, F::Enum, 0, 0, 0, {1, 0x10, 0x11}},
        {sony::kS1Button, ptp::kUint16, 1, 1, 1, 1, F::None, 0, 0, 0, {}},
        {sony::kS2Button, ptp::kUint16, 1, 1, 1, 1, F::None, 0, 0, 0, {}},
    };
    if (oddProps) {
        p.push_back({0xD2FE, 0x4004, 0, 1, 0, 0, F::None, 0, 0, 0, {}});            // an array of UINT16
        p.push_back({0xD2FF, ptp::kString, 0, 1, 0, 0, F::None, 0, 0, 0, {}});      // a string
        p.push_back({0xD2FD, ptp::kInt16, 1, 1, 0, -3, F::Range, -30, 30, 1, {}});  // signed, after the odd ones
    }
    return sony::packAllProps(p, version);
}

ptp::Response SonyCam::handle(uint16_t op, std::span<const uint32_t> params, ptp::Data, std::span<const uint8_t> out,
                              std::vector<uint8_t>* in) {
    std::lock_guard l(m_);
    ops_.push_back(op);
    if (unplugged_) throw ptp::LinkError("fake: the camera is gone");
    if (auto it = linkFailTimes.find(op); it != linkFailTimes.end() && it->second > 0) {
        --it->second;
        throw ptp::LinkError("fake: the link broke");
    }
    if (auto it = fail.find(op); it != fail.end()) return {it->second, {}};
    if (auto it = failTimes.find(op); it != failTimes.end() && it->second > 0) {
        --it->second;
        return {ptp::kGeneralError, {}};
    }
    auto data = [&](std::vector<uint8_t> d) {
        if (in) *in = std::move(d);
    };
    const uint32_t p0 = params.empty() ? 0 : params[0];
    const auto now = Clock::now();
    switch (op) {
        case ptp::kOpenSession:
            if (sessionOpen) return {ptp::kSessionAlreadyOpen, {}};
            sessionOpen = true;
            return {ptp::kOk, {}};
        case ptp::kCloseSession: sessionOpen = false; return {ptp::kOk, {}};
        case ptp::kGetDeviceInfo: {
            ptp::DeviceInfo d;
            d.standardVersion = 100;
            d.vendorExtensionId = 0x11;
            d.vendorExtensionVersion = 100;
            d.vendorExtensionDesc = "Sony PTP Extensions";
            d.operations = operations;
            d.events = {0xC201, 0xC203};
            d.properties = {sony::kCompression, sony::kShutterSpeed, sony::kIso};
            d.imageFormats = {ptp::kFormatJpeg, ptp::kFormatSonyArw};
            d.manufacturer = manufacturer, d.model = model, d.deviceVersion = firmware, d.serial = serial;
            data(ptp::packDeviceInfo(d));
            return {ptp::kOk, {}};
        }
        case sony::kSdioConnect:
            sdioStep = static_cast<int>(p0);
            if (p0 == 3) {
                busyLeft_ = busyAfterHandshake;
                readsSinceHandshake_ = 0;
                for (int i = 0; i < leftover; ++i) objects_.push_back({ptp::kFormatJpeg, jpegOf(width, height, 0), width, height, now, 0});
                leftover = 0;
            }
            data(std::vector<uint8_t>(8, 0));
            return {ptp::kOk, {}};
        case sony::kGetSdioExtDeviceInfo:
            data(sony::packExtInfo({version, {{sony::kCompression, sony::kShutterSpeed, sony::kIso, sony::kObjectInMemory},
                                              {sony::kS1Button, sony::kS2Button}}}));
            return {ptp::kOk, {}};
        case sony::kGetAllDevicePropData:
            if (busyLeft_ > 0) {
                --busyLeft_;
                return {ptp::kDeviceBusy, {}};
            }
            data(props());
            return {ptp::kOk, {}};
        case sony::kSetControlDeviceA: {
            ptp::Reader r(out);
            if (p0 == sony::kShutterSpeed || p0 == sony::kIso) {
                const int64_t v = r.u32();
                if (!manual) return {ptp::kAccessDenied, {}};
                if (direct == Direct::Refused) return {ptp::kParameterNotSupported, {}};
                if (direct == Direct::Ignored) return {ptp::kOk, {}};
                const auto& list = p0 == sony::kShutterSpeed ? shutterList : isoList;
                if (std::ranges::find(list, v) == list.end()) return {0x201C, {}};
                (p0 == sony::kShutterSpeed ? shutter : iso) = v;
                ++directSets;
            } else if (p0 == sony::kCompression) {
                const int64_t v = r.u8();
                if (!qualitySettable) return {ptp::kAccessDenied, {}};
                quality = v == 0x10 ? Quality::RawOnly : v == 0x13 ? Quality::RawPlusJpeg : Quality::Jpeg;
            }
            event(0xC203, p0);   // a property changed
            return {ptp::kOk, {}};
        }
        case sony::kSetControlDeviceB: {
            ptp::Reader r(out);
            if (p0 == sony::kS1Button || p0 == sony::kS2Button) {
                const int64_t v = r.u16();
                if (p0 == sony::kS2Button && v == sony::kButtonDown) {
                    ++shots;
                    if (!noPicture) addShot(now + std::chrono::milliseconds(shotMs));
                    event(0xC203, sony::kObjectInMemory);
                }
            } else if (p0 == sony::kShutterSpeed || p0 == sony::kIso) {
                const int64_t v = r.u8();
                if (!manual) return {ptp::kAccessDenied, {}};
                const int64_t dir = v == sony::kStepUp ? stepSign : -stepSign;
                if (p0 == sony::kShutterSpeed) step(shutter, shutterList, dir);
                else step(iso, isoList, dir);
                ++stepsTaken;
                event(0xC203, p0);
            }
            return {ptp::kOk, {}};
        }
        case ptp::kGetObjectInfo:
        case ptp::kGetObject: {
            if (op == ptp::kGetObject && p0 == sony::kLiveViewObject) {
                if (liveViewSilent) return {0, {}};   // (no answer at all: the transports wait for one)
                if (!liveView) return {ptp::kInvalidObjectHandle, {}};
                ++liveFrames;
                ptp::Writer w;
                w.u32(16).u32(0).u32(0).u32(0);   // a header before the JPEG, as Sony's live view frames have
                w.bytes(jpegOf(liveWidth, liveHeight, liveFrames));
                w.u32(0);
                data(w.take());
                return {ptp::kOk, {}};
            }
            if (p0 != sony::kShotObject || objects_.empty() || objects_.front().readyAt > now) return {ptp::kInvalidObjectHandle, {}};
            const auto& o = objects_.front();
            if (op == ptp::kGetObjectInfo) {
                ptp::ObjectInfo info;
                info.storageId = 0x10001;
                info.format = o.format;
                info.compressedSize = static_cast<uint32_t>(o.data.size());
                info.imageWidth = static_cast<uint32_t>(o.w), info.imageHeight = static_cast<uint32_t>(o.h);
                info.imageBitDepth = o.format == ptp::kFormatJpeg ? 24 : 16;
                info.filename = std::format("DSC{:05}.{}", o.shot, o.format == ptp::kFormatJpeg ? "JPG" : "ARW");
                data(ptp::packObjectInfo(info));
            } else {
                data(o.data);
                objects_.pop_front();
            }
            return {ptp::kOk, {}};
        }
        default: return {ptp::kOperationNotSupported, {}};
    }
}

namespace {

class Direct final : public ptp::Transport {
public:
    explicit Direct(SonyCam& c) : c_(c) {}
    ptp::Response transact(uint16_t op, std::span<const uint32_t> params, ptp::Data phase, std::span<const uint8_t> out,
                           std::vector<uint8_t>* in) override {
        if (in) in->clear();
        const auto r = c_.handle(op, params, phase, out, in);
        if (r.code == 0) throw ptp::LinkError("fake: no answer in time");
        return r;
    }
    std::string kind() const override { return "direct"; }
    bool recover() override {
        c_.resetPtp();
        return true;
    }

private:
    SonyCam& c_;
};

}  // namespace

std::unique_ptr<ptp::Transport> directTransport(SonyCam& cam) { return std::make_unique<Direct>(cam); }

// ---------------------------------------------------------------- WPD

ptp::wpd::Values WpdDevice::send(ptp::wpd::Command c, const ptp::wpd::Values& in) {
    using C = ptp::wpd::Command;
    log.push_back(c);
    if (linkFail.contains(c)) throw ptp::LinkError("fake WPD: SendCommand failed");
    ptp::wpd::Values out;
    if (auto it = hresult.find(c); it != hresult.end()) {
        out.hresult = it->second;
        return out;
    }
    auto context = [&]() -> Context* {
        const auto it = contexts_.find(in.context);
        return it == contexts_.end() ? nullptr : &it->second;
    };
    switch (c) {
        case C::VendorOpcodes: out.params = {sony::kSdioConnect, sony::kGetSdioExtDeviceInfo, sony::kSetControlDeviceA, sony::kSetControlDeviceB, sony::kGetAllDevicePropData}; break;
        case C::ExecuteNoData: {
            const auto r = cam_.handle(static_cast<uint16_t>(in.opCode.value_or(0)), in.params, ptp::Data::None, {}, nullptr);
            out.responseCode = r.code;
            out.params = r.params;
            break;
        }
        case C::ExecuteRead: {
            Context ctx{static_cast<uint16_t>(in.opCode.value_or(0)), in.params, {}, 0, false, {}};
            ctx.response = cam_.handle(ctx.op, ctx.params, ptp::Data::In, {}, &ctx.data);
            if (ctx.response.code == 0) {   // no answer: the driver times out
                out.hresult = static_cast<int32_t>(0x80070079);
                break;
            }
            out.totalSize = ctx.data.size();
            ctx.data.resize(ctx.data.size() - std::min<std::size_t>(ctx.data.size(), static_cast<std::size_t>(shortBy)));
            out.context = std::format("ctx{}", next_++);
            out.optimalBuffer = optimal_;
            contexts_[out.context] = std::move(ctx);
            break;
        }
        case C::ExecuteWrite: {
            out.context = std::format("ctx{}", next_++);
            contexts_[out.context] = {static_cast<uint16_t>(in.opCode.value_or(0)), in.params, {}, 0, true, {}};
            out.optimalBuffer = optimal_;
            break;
        }
        case C::ReadData: {
            auto* x = context();
            if (!x) {
                out.hresult = static_cast<int32_t>(0x80070057);
                break;
            }
            const std::size_t n = std::min<std::size_t>(in.bytes.value_or(0), x->data.size() - x->at);
            out.data.assign(x->data.begin() + static_cast<std::ptrdiff_t>(x->at), x->data.begin() + static_cast<std::ptrdiff_t>(x->at + n));
            out.bytes = static_cast<uint32_t>(n);
            x->at += n;
            break;
        }
        case C::WriteData: {
            auto* x = context();
            if (!x) {
                out.hresult = static_cast<int32_t>(0x80070057);
                break;
            }
            x->data.insert(x->data.end(), in.data.begin(), in.data.end());
            out.bytes = static_cast<uint32_t>(in.data.size());
            break;
        }
        case C::EndTransfer: {
            const auto it = contexts_.find(in.context);
            if (it == contexts_.end()) {
                out.hresult = static_cast<int32_t>(0x80070057);
                break;
            }
            Context x = std::move(it->second);   // released whatever the camera makes of it
            contexts_.erase(it);
            if (x.write) x.response = cam_.handle(x.op, x.params, ptp::Data::Out, x.data, nullptr);
            out.responseCode = x.response.code;
            out.params = x.response.params;
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------- USB

void UsbPipe::answer(uint16_t type, uint16_t code, uint32_t tid, std::span<const uint8_t> payload) {
    ptp::Writer w;
    w.u32(static_cast<uint32_t>(12 + payload.size())).u16(type).u16(code).u32(tid).bytes(payload);
    out_.push_back(w.take());
}

void UsbPipe::write(std::span<const uint8_t> data) {
    ++writes;
    if (data.empty()) {
        ++emptyWrites;
        return;
    }
    ptp::Reader r(data);
    r.u32();
    const uint16_t type = r.u16();
    if (type == 1 && hasDataOut(ptp::Reader(data.subspan(6)).u16())) {
        pendingCommand_.assign(data.begin(), data.end());   // its data comes next
        return;
    }
    std::span<const uint8_t> payload;
    std::vector<uint8_t> command;
    if (type == 2) {
        command = std::move(pendingCommand_);
        payload = data.subspan(12);
    } else {
        command.assign(data.begin(), data.end());
    }
    ptp::Reader c(command);
    c.u32();
    c.u16();
    const uint16_t op = c.u16();
    const uint32_t tid = c.u32();
    std::vector<uint32_t> params;
    while (c.left() >= 4) params.push_back(c.u32());
    std::vector<uint8_t> in;
    const auto res = cam_.handle(op, params, type == 2 ? ptp::Data::Out : ptp::Data::In, payload, &in);
    if (res.code == 0) return;   // no answer at all
    if (badContainer) {
        badContainer = false;
        answer(7, 0, tid, {});
        return;
    }
    if (!in.empty()) answer(2, op, tid, in);
    if (events) {
        ptp::Writer e;
        e.u32(sony::kObjectInMemory);
        answer(4, 0xC203, 0, e.data());
    }
    if (staleResponse) {
        staleResponse = false;
        answer(3, ptp::kOk, tid + 1000, {});
    }
    ptp::Writer rp;
    for (auto p : res.params) rp.u32(p);
    answer(3, res.code, tid, rp.data());
}

std::size_t UsbPipe::readEvent(std::span<uint8_t> buf, std::chrono::milliseconds timeout) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (auto e = cam_.takeEvent()) {
            const std::size_t n = std::min(buf.size(), e->size());
            std::copy_n(e->begin(), n, buf.begin());
            return n;
        }
        if (std::chrono::steady_clock::now() >= until) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void UsbPipe::reset() {
    ++resets;
    out_.clear();
    outAt_ = 0;
    zlpDue_ = false;
    pendingCommand_.clear();
    cam_.resetPtp();
}

std::size_t UsbPipe::read(std::span<uint8_t> buf, std::chrono::milliseconds) {
    if (holdForEvents) {   // the camera won't answer while an event of its waits to be read
        for (int i = 0; i < 250 && cam_.eventsWaiting() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (cam_.eventsWaiting() > 0) throw ptp::LinkError("fake USB: no answer in time (an event waits)");
    }
    if (zlpDue_) {
        zlpDue_ = false;
        return 0;
    }
    if (out_.empty()) throw ptp::LinkError("fake USB: no answer in time");
    std::size_t n = 0;
    while (!out_.empty() && n < buf.size()) {
        const auto& c = out_.front();
        const std::size_t take = std::min(buf.size() - n, c.size() - outAt_);
        std::copy_n(c.begin() + static_cast<std::ptrdiff_t>(outAt_), take, buf.begin() + static_cast<std::ptrdiff_t>(n));
        n += take;
        outAt_ += take;
        if (outAt_ < c.size()) break;   // the buffer is full
        const bool whole = c.size() % packet_ == 0;
        out_.pop_front();
        outAt_ = 0;
        if (!whole) break;              // a short packet ended the transfer
        if (zlp) {                      // an empty packet ends it: now, or as the next transfer
            if (n == buf.size()) zlpDue_ = true;
            break;
        }
        // no empty packet: the transfer runs on into the next container
    }
    return n;
}

// ---------------------------------------------------------------- source

std::vector<slopcap::SonyDeviceSource::Entry> SonySource::list() {
    std::vector<Entry> out;
    for (std::size_t i = 0; i < cams.size(); ++i) out.push_back({std::format("fake:{}", i), cams[i]->model, problem});
    return out;
}

std::unique_ptr<ptp::Transport> SonySource::open(const std::string& id) {
    auto& cam = *cams.at(std::stoul(id.substr(5)));
    switch (via) {
        case Via::Wpd: return std::make_unique<ptp::wpd::WpdTransport>(std::make_unique<WpdDevice>(cam));
        case Via::Usb: return std::make_unique<ptp::UsbTransport>(std::make_unique<UsbPipe>(cam));
        default: return directTransport(cam);
    }
}

}  // namespace fake

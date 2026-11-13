// Sony cameras in PC Remote mode: the JPEG decoder, PTP over USB and over WPD's MTP-extension commands, Sony's
// extension, the camera backend and the probe, against the fake camera in tests/fakes/fake_sony.* (no real Sony has
// run any of this yet).
#include "camera/jpeg_luma.hpp"
#include "camera/open_camera.hpp"
#include "camera/ptp_usb.hpp"
#include "camera/ptp_wpd.hpp"
#include "camera/sony_camera.hpp"
#include "camera/sony_ptp.hpp"
#include "fakes/fake_sony.hpp"
#include "fakes/jpeg_writer.hpp"
#include "probe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <sstream>
#include <thread>

using namespace slopcap;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;

namespace {

std::string errorOf(auto&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "(no error)";
}

/// A smooth picture with a soft disc: what a JPEG keeps well.
std::vector<uint8_t> scene(int w, int h) {
    std::vector<uint8_t> v(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double d = std::hypot(x - w / 2.0, y - h / 2.0), r = std::min(w, h) / 3.0;
            const double disc = std::clamp((r - d) / 4 + 0.5, 0.0, 1.0);
            v[static_cast<std::size_t>(y * w + x)] = static_cast<uint8_t>(30 + 100.0 * x / w + 100 * disc);
        }
    return v;
}

double meanAbs(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double s = 0;
    for (std::size_t i = 0; i < a.size(); ++i) s += std::abs(a[i] - b[i]);
    return s / static_cast<double>(a.size());
}

/// The fakes for one test: openCamera("sony") finds `cam`, through `via`.
struct Fixture {
    fake::SonyCam cam;
    fake::SonySource source;
    explicit Fixture(fake::SonySource::Via via = fake::SonySource::Via::Direct) {
        source.cams = {&cam};
        source.via = via;
        setSonyDeviceSource(&source);
    }
    ~Fixture() { setSonyDeviceSource(nullptr); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};

double shutterOrder(int64_t v) { return std::log(sony::shutterSeconds(v).value_or(1000.0)); }

}  // namespace

// ---------------------------------------------------------------- JPEG

TEST_CASE("jpeg: luminance of every layout a camera may write", "[sony][jpeg]") {
    struct Case {
        const char* name;
        fake::JpegOptions o;
        int w, h;
        double tolerance;
    };
    auto opt = [](bool color, int yh, int yv, int ch, int cv, int restart, bool separate, bool exif) {
        fake::JpegOptions o;
        o.quality = 95, o.color = color, o.yh = yh, o.yv = yv, o.ch = ch, o.cv = cv, o.restart = restart, o.separateScans = separate,
        o.exifThumbnail = exif;
        return o;
    };
    const Case cases[] = {
        {"grey", opt(false, 1, 1, 1, 1, 0, false, false), 64, 48, 2},
        {"4:4:4", opt(true, 1, 1, 1, 1, 0, false, false), 64, 48, 2},
        {"4:2:2 (Sony's), EXIF thumbnail", opt(true, 2, 1, 1, 1, 0, false, true), 96, 64, 2},
        {"4:2:0, restart markers", opt(true, 2, 2, 1, 1, 3, false, false), 96, 64, 2},
        {"odd size", opt(true, 2, 1, 1, 1, 0, false, false), 37, 23, 2},
        {"one scan per component, restarts", opt(true, 2, 2, 1, 1, 5, true, false), 70, 50, 2},
        {"grey with restarts", opt(false, 1, 1, 1, 1, 2, false, false), 40, 40, 2},
        {"luminance subsampled", opt(true, 1, 1, 2, 2, 0, false, false), 64, 48, 12},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        const auto img = scene(c.w, c.h);
        const auto jpg = fake::encodeJpeg(img.data(), c.w, c.h, c.o);
        const auto sz = jpeg::size(jpg);
        CHECK(sz.width == c.w);
        CHECK(sz.height == c.h);
        std::vector<uint8_t> y;
        const auto got = jpeg::decodeLuma(jpg, y);
        REQUIRE(got.width == c.w);
        REQUIRE(y.size() == img.size());
        CHECK(meanAbs(y, img) < c.tolerance);
    }
}

TEST_CASE("jpeg: what it refuses, and damaged data", "[sony][jpeg]") {
    const auto img = scene(48, 32);
    const auto good = fake::encodeJpeg(img.data(), 48, 32);
    std::vector<uint8_t> y;
    auto decode = [&](std::vector<uint8_t> d) { return errorOf([&] { jpeg::decodeLuma(d, y); }); };
    auto find = [&](uint8_t marker) {
        for (std::size_t i = 0; i + 1 < good.size(); ++i)
            if (good[i] == 0xFF && good[i + 1] == marker) return i;
        FAIL("no marker");
        return std::size_t{0};
    };
    CHECK_THAT(decode({1, 2, 3, 4, 5}), ContainsSubstring("not a JPEG"));
    CHECK_THAT(decode({0xFF, 0xD8, 0xFF, 0xD9}), ContainsSubstring("no frame header"));
    CHECK_THAT(decode({good.begin(), good.end() - 40}), ContainsSubstring("ends"));   // cut in its picture data
    auto patched = [&](std::size_t at, uint8_t v) {
        auto d = good;
        d[at] = v;
        return d;
    };
    const auto sof = find(0xC0);
    CHECK_THAT(decode(patched(sof + 1, 0xC2)), ContainsSubstring("progressive"));
    CHECK_THAT(decode(patched(sof + 1, 0xC3)), ContainsSubstring("unsupported JPEG type"));
    CHECK_THAT(decode(patched(sof + 4, 12)), ContainsSubstring("12-bit"));
    CHECK_THAT(decode(patched(sof + 2, 0xFF)), ContainsSubstring("past the end"));
    CHECK_THAT(decode(patched(find(0xC4) + 5, 0xFF)), ContainsSubstring("Huffman table"));
    CHECK_THAT(decode(patched(find(0xDA) + 5, 7)), ContainsSubstring("component the frame doesn't have"));
    {   // the tables gone: DHT renamed to a comment
        CHECK_THAT(decode(patched(find(0xC4) + 1, 0xFE)), ContainsSubstring("without its Huffman tables"));
    }
    {   // the scan first: SOF renamed
        CHECK_THAT(decode(patched(sof + 1, 0xFE)), ContainsSubstring("scan before the frame header"));
    }
    CHECK_THAT(errorOf([&] { jpeg::size(std::vector<uint8_t>{0xFF, 0xD8, 0xFF, 0xD9}); }), ContainsSubstring("no frame header"));
}

// ---------------------------------------------------------------- PTP basics

TEST_CASE("ptp: datasets, strings and errors", "[sony][ptp]") {
    ptp::Writer w;
    w.u8(1).u16(0x1234).u32(0xDEADBEEF).u64(0x0102030405060708).str("ILCE-6000").str("");
    const uint16_t arr[] = {1, 2, 3};
    w.u16array(arr);
    for (uint16_t t : {ptp::kInt8, ptp::kUint8, ptp::kInt16, ptp::kUint16, ptp::kInt32, ptp::kUint32, ptp::kInt64, ptp::kUint64})
        w.value(t, -2);
    ptp::Reader r(w.data());
    CHECK(r.u8() == 1);
    CHECK(r.u16() == 0x1234);
    CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.u64() == 0x0102030405060708);
    CHECK(r.str() == "ILCE-6000");
    CHECK(r.str().empty());
    CHECK(r.u16array() == std::vector<uint16_t>{1, 2, 3});
    CHECK(r.value(ptp::kInt8) == -2);
    CHECK(r.value(ptp::kUint8) == 254);
    CHECK(r.value(ptp::kInt16) == -2);
    CHECK(r.value(ptp::kUint16) == 65534);
    CHECK(r.value(ptp::kInt32) == -2);
    CHECK(r.value(ptp::kUint32) == 4294967294);
    CHECK(r.value(ptp::kInt64) == -2);
    CHECK(r.value(ptp::kUint64) == -2);
    CHECK(r.left() == 0);
    CHECK_THROWS_AS(r.u8(), ptp::Error);
    CHECK_THROWS_AS(r.value(0x0099), ptp::Error);
    CHECK_THROWS_AS(ptp::Writer().value(0x0099, 0), ptp::Error);
    CHECK(ptp::typeSize(ptp::kUint32) == 4);
    CHECK(ptp::typeSize(ptp::kString) == 0);

    // UTF-16 beyond ASCII comes out as UTF-8; arrays and strings that run past the end throw
    const std::vector<uint8_t> u = {3, 0xE9, 0x00, 0x13, 0x27, 0x00, 0x00};
    CHECK(ptp::Reader(u).str() == "\xC3\xA9\xE2\x9C\x93");
    CHECK_THROWS_AS(ptp::Reader(std::vector<uint8_t>{5, 'a', 0}).str(), ptp::Error);
    CHECK_THROWS_AS(ptp::Reader(std::vector<uint8_t>{9, 0, 0, 0, 1, 0}).u32array(), ptp::Error);
    ptp::Writer w32;
    w32.u32(2).u32(7).u32(9);
    CHECK(ptp::Reader(w32.data()).u32array() == std::vector<uint32_t>{7, 9});

    ptp::DeviceInfo d;
    d.vendorExtensionId = 0x11;
    d.operations = {0x1001, 0x9201};
    d.model = "ILCE-6000";
    d.serial = "x";
    const auto back = ptp::parseDeviceInfo(ptp::packDeviceInfo(d));
    CHECK(back.model == "ILCE-6000");
    CHECK(back.supports(0x9201));
    CHECK_FALSE(back.supports(0x9209));
    ptp::ObjectInfo o;
    o.format = ptp::kFormatJpeg;
    o.compressedSize = 123;
    o.imageWidth = 6000;
    o.filename = "DSC00001.JPG";
    const auto ob = ptp::parseObjectInfo(ptp::packObjectInfo(o));
    CHECK(ob.compressedSize == 123);
    CHECK(ob.imageWidth == 6000);
    CHECK(ob.filename == "DSC00001.JPG");

    CHECK(ptp::responseText(0x2019) == "0x2019 device busy");
    CHECK(ptp::responseText(0x2BAD) == "0x2BAD");
    for (uint16_t c : {0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2009, 0x200A, 0x200F, 0x2014, 0x2017, 0x201B, 0x201C, 0x201D, 0x201E, 0x201F})
        CHECK(ptp::responseText(c).size() > 7);
    CHECK(ptp::formatName(ptp::kFormatJpeg) == "JPEG");
    CHECK(ptp::formatName(ptp::kFormatSonyArw) == "ARW");
    for (uint16_t f : {0x3000, 0x3808, 0x380D, 0xB110}) CHECK(ptp::formatName(f).size() > 3);
    CHECK(ptp::formatName(0x1234) == "0x1234");

    fake::SonyCam cam;
    auto t = fake::directTransport(cam);
    cam.fail[ptp::kGetDeviceInfo] = ptp::kDeviceBusy;
    CHECK_THAT(errorOf([&] { ptp::receive(*t, ptp::kGetDeviceInfo, {}, "info"); }), ContainsSubstring("info: camera answered 0x2019 device busy"));
    CHECK_THAT(errorOf([&] { ptp::command(*t, 0x1234, {}, "nothing"); }), ContainsSubstring("operation not supported"));
    CHECK_THAT(errorOf([&] { ptp::send(*t, 0x1234, {}, std::vector<uint8_t>{1}, "nothing"); }), ContainsSubstring("operation not supported"));
    CHECK_NOTHROW(ptp::command(*t, ptp::kOpenSession, {1}, "open"));
    CHECK(t->kind() == "direct");
}

// ---------------------------------------------------------------- PTP over USB

TEST_CASE("ptp over USB: containers, packets, empty packets, events", "[sony][usb]") {
    for (std::size_t packet : {512u, 64u, 12u}) {
        for (bool zlp : {true, false}) {
            INFO("packet " << packet << (zlp ? " with" : " without") << " empty packets");
            fake::SonyCam cam;
            auto pipe = std::make_unique<fake::UsbPipe>(cam, packet);
            auto* p = pipe.get();
            p->zlp = zlp;
            {
                ptp::UsbTransport t(std::move(pipe));
                CHECK(t.kind() == "USB");
                CHECK(cam.sessionOpen);
                const auto info = ptp::parseDeviceInfo(ptp::receive(t, ptp::kGetDeviceInfo, {}, "info"));
                CHECK(info.model == "ILCE-6000");
                sony::Session s(t);
                s.connect();
                const auto shot = s.shoot(5s);
                REQUIRE(shot);
                REQUIRE(shot->jpeg());
                std::vector<uint8_t> y;
                CHECK(jpeg::decodeLuma(shot->jpeg()->data, y).width == cam.width);
                p->events = true;
                p->staleResponse = true;
                CHECK(s.setValue(sony::kShutterSpeed, sony::shutterValue(1, 125), shutterOrder) == sony::Session::SetMethod::Direct);
                p->events = false;
                if (packet == 12) CHECK(p->emptyWrites > 0);   // a 12-byte command fills a packet: an empty one follows
            }
            CHECK_FALSE(cam.sessionOpen);   // closed on the way out
        }
    }

    fake::SonyCam cam;
    cam.sessionOpen = true;   // left open by another program: closed and opened again
    auto pipe = std::make_unique<fake::UsbPipe>(cam);
    auto* p = pipe.get();
    ptp::UsbTransport t(std::move(pipe));
    const auto ops = cam.ops();
    CHECK(ops == std::vector<uint16_t>{ptp::kOpenSession, ptp::kCloseSession, ptp::kOpenSession});
    const uint32_t six[6] = {};
    CHECK_THROWS_AS(t.transact(0x1001, six, ptp::Data::None, {}, nullptr), ptp::Error);
    p->badContainer = true;
    CHECK_THROWS_AS(ptp::receive(t, ptp::kGetDeviceInfo, {}, "info"), ptp::LinkError);
    struct Huge final : ptp::UsbPipe {   // a damaged length: refused, not made room for
        void write(std::span<const uint8_t>) override {}
        std::size_t read(std::span<uint8_t> buf, std::chrono::milliseconds) override {
            const uint8_t h[12] = {0xFF, 0xFF, 0xFF, 0xF0, 2, 0, 0x01, 0x10, 0, 0, 0, 0};
            std::copy(std::begin(h), std::end(h), buf.begin());
            return 12;
        }
        std::size_t packetSize() const override { return 512; }
    };
    CHECK_THAT(errorOf([] { ptp::UsbTransport u(std::make_unique<Huge>()); }), ContainsSubstring("4043309055 bytes"));
    cam.setUnplugged(true);
    CHECK_THROWS_AS(ptp::receive(t, ptp::kGetDeviceInfo, {}, "info"), ptp::LinkError);

    fake::SonyCam refusing;
    refusing.sessionOpen = true;
    refusing.fail[ptp::kCloseSession] = ptp::kGeneralError;
    refusing.fail[ptp::kOpenSession] = ptp::kSessionAlreadyOpen;
    CHECK_THAT(errorOf([&] { ptp::UsbTransport u(std::make_unique<fake::UsbPipe>(refusing)); }), ContainsSubstring("open a PTP session"));
    // a camera that never answers
    struct Silent final : ptp::UsbPipe {
        void write(std::span<const uint8_t>) override {}
        std::size_t read(std::span<uint8_t>, std::chrono::milliseconds) override { throw ptp::LinkError("timeout"); }
        std::size_t packetSize() const override { return 512; }
    };
    CHECK_THROWS_AS(ptp::UsbTransport(std::make_unique<Silent>()), ptp::LinkError);
}

TEST_CASE("ptp over USB: the system's devices", "[sony][usb]") {
#ifdef _WIN32
    CHECK(ptp::listUsbCameras().empty());
#else
    CHECK_NOTHROW(ptp::listUsbCameras());   // (none in a container; whatever is plugged in elsewhere)
    CHECK_NOTHROW(ptp::libusbProblem());
    CHECK_THROWS_AS(ptp::openUsbCamera("usb:999:999"), CameraError);
#endif
}

// ---------------------------------------------------------------- PTP over WPD

TEST_CASE("ptp over WPD: MTP-extension commands, chunks, failures", "[sony][wpd]") {
    using C = ptp::wpd::Command;
    fake::SonyCam cam;
    auto dev = std::make_unique<fake::WpdDevice>(cam, 7);   // a tiny buffer: every transfer in many pieces
    auto* d = dev.get();
    ptp::wpd::WpdTransport t(std::move(dev));
    CHECK(t.kind() == "WPD");
    CHECK(t.vendorOpcodes().size() == 5);
    CHECK_NOTHROW(ptp::command(t, ptp::kOpenSession, {1}, "open"));
    const auto info = ptp::parseDeviceInfo(ptp::receive(t, ptp::kGetDeviceInfo, {}, "info"));
    CHECK(info.serial == cam.serial);
    sony::Session s(t);
    s.connect();
    CHECK(s.setValue(sony::kShutterSpeed, sony::shutterValue(1, 60), shutterOrder) == sony::Session::SetMethod::Direct);
    CHECK(cam.shutter == sony::shutterValue(1, 60));
    CHECK(std::ranges::count(d->log, C::WriteData) >= 1);
    CHECK(d->openContexts() == 0);

    // the camera sends less than it announced: what came is kept
    d->shortBy = 10;
    std::vector<uint8_t> in;
    CHECK(t.transact(ptp::kGetDeviceInfo, {}, ptp::Data::In, {}, &in).ok());
    CHECK(in.size() == ptp::packDeviceInfo(info).size() - 10);
    d->shortBy = 0;

    // a size past any picture: refused
    struct Huge final : ptp::wpd::Device {
        ptp::wpd::Values send(ptp::wpd::Command, const ptp::wpd::Values&) override {
            ptp::wpd::Values v;
            v.totalSize = uint64_t{1} << 40;
            v.context = "c";
            return v;
        }
    };
    ptp::wpd::WpdTransport huge(std::make_unique<Huge>());
    CHECK_THAT(errorOf([&] { ptp::receive(huge, ptp::kGetDeviceInfo, {}, "info"); }), ContainsSubstring("bytes announced"));

    // a failed command's HRESULT, and the transfer released after a failure in the middle
    d->hresult[C::ExecuteRead] = static_cast<int32_t>(0x800700AA);
    CHECK_THAT(errorOf([&] { ptp::receive(t, ptp::kGetDeviceInfo, {}, "info"); }), ContainsSubstring("0x800700AA busy"));
    d->hresult.clear();
    d->hresult[C::ReadData] = static_cast<int32_t>(0x80070079);
    CHECK_THAT(errorOf([&] { ptp::receive(t, ptp::kGetDeviceInfo, {}, "info"); }), ContainsSubstring("timed out"));
    CHECK(d->openContexts() == 0);
    d->hresult.clear();
    d->hresult[C::WriteData] = static_cast<int32_t>(0x80004005);
    CHECK_THAT(errorOf([&] { s.setA(sony::kShutterSpeed, sony::shutterValue(1, 125)); }), ContainsSubstring("unspecified failure"));
    CHECK(d->openContexts() == 0);
    d->hresult.clear();
    d->linkFail.insert(C::ExecuteNoData);
    CHECK_THROWS_AS(ptp::command(t, ptp::kCloseSession, {}, "close"), ptp::LinkError);
    const uint32_t six[6] = {};
    CHECK_THROWS_AS(t.transact(0x1001, six, ptp::Data::None, {}, nullptr), ptp::Error);

    CHECK(ptp::wpd::hresultText(static_cast<int32_t>(0x80070005)) == "0x80070005 access denied");
    for (uint32_t h : {0x80070015u, 0x8007001Fu, 0x80070032u, 0x80070057u, 0x8007048Fu, 0x800704C7u, 0x8000FFFFu})
        CHECK(ptp::wpd::hresultText(static_cast<int32_t>(h)).size() > 11);
    CHECK(ptp::wpd::hresultText(static_cast<int32_t>(0x80001234)) == "0x80001234");
    CHECK(ptp::wpd::usbIdsOf("\\\\?\\usb#vid_054c&pid_094e#0123456789#{6ac27878-a6fa-4155-ba85-f98f491d4f33}") == "vid_054c&pid_094e");
    CHECK(ptp::wpd::usbIdsOf("\\\\?\\USB#VID_054C#x").starts_with("vid_054c"));
    CHECK(ptp::wpd::usbIdsOf("\\\\?\\swd#wpdbusenum#x").empty());
#ifdef _WIN32
    CHECK_NOTHROW(ptp::wpd::listDevices());   // this computer's phones and cameras, if any: only listed
    CHECK_THROWS_AS(ptp::wpd::openDevice("\\\\?\\usb#vid_0000&pid_0000#none#{6ac27878-a6fa-4155-ba85-f98f491d4f33}"), CameraError);
#else
    CHECK(ptp::wpd::listDevices().empty());
    CHECK_THROWS_AS(ptp::wpd::openDevice("x"), CameraError);
#endif
}

// ---------------------------------------------------------------- Sony's extension

TEST_CASE("sony: datasets, names and values", "[sony]") {
    using F = sony::Prop::Form;
    for (uint16_t version : {200, 300}) {
        const std::vector<sony::Prop> props = {
            {sony::kShutterSpeed, ptp::kUint32, 1, 1, 0, sony::shutterValue(1, 250), F::Enum, 0, 0, 0, {sony::shutterValue(1, 250), 0}},
            {0xD2FD, ptp::kInt16, 1, 2, 0, -3, F::Range, -30, 30, 1, {}},
            {0xD2FE, 0x4004, 0, 1, 0, 0, F::None, 0, 0, 0, {}},
            {0xD2FF, ptp::kString, 0, 1, 0, 0, F::None, 0, 0, 0, {}},
            {sony::kBatteryLevel, ptp::kInt8, 0, 1, 0, -1, F::None, 0, 0, 0, {}},
        };
        std::size_t stopped = 99;
        const auto back = sony::parseAllProps(sony::packAllProps(props, version), version, &stopped);
        REQUIRE(back.size() == props.size());
        CHECK(stopped == 0);
        CHECK(back[0].values.size() == 2);
        CHECK(back[0].settable());
        CHECK(back[1].current == -3);
        CHECK(back[1].min == -30);
        CHECK_FALSE(back[1].settable());   // shown only
        CHECK(back[4].current == -1);
    }
    // a property it can't read stops the list there, and says where
    auto data = sony::packAllProps({{0x5004, ptp::kUint8, 1, 1, 0, 3, F::None, 0, 0, 0, {}}}, 200);
    const std::size_t at = data.size();
    data.insert(data.end(), {0x01, 0xD2, 0x02, 0x00, 1, 1, 0, 0, 7});   // form 7
    std::size_t stopped = 0;
    CHECK(sony::parseAllProps(data, 200, &stopped).size() == 1);
    CHECK(stopped == at);
    data.resize(at);
    data.insert(data.end(), {0x01, 0xD2, 0x01, 0x40, 1, 1});   // an array of a type it doesn't know
    CHECK(sony::parseAllProps(data, 200, &stopped).size() == 1);
    CHECK(sony::parseAllProps(std::vector<uint8_t>{1, 2}, 200).empty());

    const auto ext = sony::parseExtInfo(sony::packExtInfo({200, {{1, 2}, {3}}}));
    CHECK(ext.version == 200);
    CHECK(ext.codes() == std::vector<uint16_t>{1, 2, 3});
    auto cut = sony::packExtInfo({200, {{1, 2}, {3, 4, 5}}});
    cut.resize(cut.size() - 2);
    CHECK(sony::parseExtInfo(cut).lists.size() == 1);

    CHECK(sony::formatValue(sony::kShutterSpeed, sony::shutterValue(1, 250)) == "1/250");
    CHECK(sony::formatValue(sony::kShutterSpeed, sony::shutterValue(300, 10)) == "30\"");
    CHECK(sony::formatValue(sony::kShutterSpeed, sony::shutterValue(13, 10)) == "1.3\"");
    CHECK(sony::formatValue(sony::kShutterSpeed, 0) == "bulb");
    CHECK(sony::formatValue(sony::kShutterSpeed, sony::shutterValue(1, 0)) == "0x10000");
    CHECK(sony::formatValue(sony::kIso, 800) == "ISO 800");
    CHECK(sony::formatValue(sony::kIso, sony::kIsoAuto) == "ISO auto");
    CHECK(sony::formatValue(sony::kIso, int64_t{1} << 24 | 1600) == "ISO 1600 (multi-frame NR)");
    CHECK(sony::formatValue(sony::kFNumber, 560) == "f/5.6");
    CHECK(sony::formatValue(sony::kExposureProgram, 1) == "M");
    CHECK(sony::formatValue(sony::kObjectInMemory, 0x8001) == "1 (0x8001)");
    CHECK(sony::formatValue(sony::kObjectInMemory, 0x8000) == "none (0x8000)");
    CHECK(sony::formatValue(0x1234, 5) == "5");
    CHECK(sony::formatValue(0x1234, 300) == "300 (0x12C)");
    for (int64_t v : {2, 3, 4, 0x8000, 0x8001, 0x77}) CHECK_FALSE(sony::formatValue(sony::kExposureProgram, v).empty());
    for (int64_t v : {2, 3, 4, 0x10, 0x13, 0x77}) CHECK_FALSE(sony::formatValue(sony::kCompression, v).empty());
    for (int64_t v : {1, 2, 0x8004, 0x8005, 0x8006, 0x77}) CHECK_FALSE(sony::formatValue(sony::kFocusMode, v).empty());
    for (int64_t v : {1, 0x10, 0x11, 0x77}) CHECK_FALSE(sony::formatValue(sony::kSaveDestination, v).empty());
    for (uint16_t c : {0x5001, 0x5003, 0x5004, 0x5005, 0x5007, 0x5008, 0x500A, 0x500B, 0x500C, 0x500E, 0x5010, 0x5013, 0xD200, 0xD201,
                       0xD203, 0xD20D, 0xD20F, 0xD210, 0xD211, 0xD213, 0xD215, 0xD218, 0xD21B, 0xD21E, 0xD222, 0xD2C1, 0xD2C2, 0xD2C3,
                       0xD2C7, 0xD2D1})
        CHECK(sony::propName(c));
    CHECK_FALSE(sony::propName(0x1234));
    CHECK(sony::shutterSeconds(sony::shutterValue(1, 4)) == 0.25);
    CHECK_FALSE(sony::shutterSeconds(0));
    CHECK(sony::isoNumber(100) == 100);
    CHECK_FALSE(sony::isoNumber(sony::kIsoAuto));
    CHECK(sony::sensorWidthMm("ILCE-6000") == 23.5);
    CHECK(sony::sensorWidthMm("ILCE-7M3") == 35.9);
    CHECK(sony::sensorWidthMm("ZV-E10") == 23.5);
    CHECK(sony::sensorWidthMm("DSC-RX100M7") == 13.2);
    CHECK(sony::sensorWidthMm("Cyber-shot") == 0);

    const std::vector<uint8_t> blob = {9, 9, 0xFF, 0xD8, 0xFF, 0xE0, 1, 0xFF, 0xD9, 7, 0xFF, 0xD9, 0, 0};
    const auto j = sony::findJpeg(blob);
    CHECK(j.size() == 10);
    CHECK(j[0] == 0xFF);
    CHECK(sony::findJpeg(std::vector<uint8_t>{1, 2, 3}).empty());
}

TEST_CASE("sony: handshake, settings, shutter", "[sony]") {
    using M = sony::Session::SetMethod;
    fake::SonyCam cam;
    auto t = fake::directTransport(cam);
    sony::Session s(*t, 5);

    SECTION("handshake: busy for a moment, then the settings") {
        cam.busyAfterHandshake = 2;
        CHECK(s.connect().version == 200);
        CHECK(cam.sdioStep == 3);
        CHECK(s.prop(sony::kShutterSpeed));
        CHECK(s.props().size() == 13);
        CHECK(s.propsStoppedAt() == 0);
        CHECK_FALSE(s.lastPropData().empty());
        CHECK(s.ext().codes().size() == 6);
    }
    SECTION("handshake refused, or busy for good") {
        cam.fail[sony::kSdioConnect] = ptp::kOperationNotSupported;
        CHECK_THAT(errorOf([&] { s.connect(); }), ContainsSubstring("Sony handshake, step 1"));
        cam.fail.clear();
        cam.fail[sony::kGetAllDevicePropData] = ptp::kAccessDenied;
        CHECK_THAT(errorOf([&] { s.connect(); }), ContainsSubstring("read the camera's settings"));
        cam.fail.clear();
        cam.busyAfterHandshake = 1000;
        CHECK_THAT(errorOf([&] { s.connect(); }), ContainsSubstring("device busy"));
    }
    SECTION("values: directly, in steps either way, the nearer when skipped, as far as it goes") {
        s.connect();
        CHECK(s.setValue(sony::kShutterSpeed, cam.shutter, shutterOrder) == M::None);
        CHECK(s.setValue(sony::kShutterSpeed, sony::shutterValue(1, 60), shutterOrder) == M::Direct);
        cam.direct = fake::SonyCam::Direct::Refused;
        for (int sign : {1, -1}) {
            INFO("step sign " << sign);
            cam.stepSign = sign;
            sony::Session fresh(*t, 5);   // a new session learns the direction again
            fresh.connect();
            CHECK(fresh.setValue(sony::kShutterSpeed, sony::shutterValue(1, 1000), shutterOrder) == M::Steps);
            CHECK(cam.shutter == sony::shutterValue(1, 1000));
            CHECK(fresh.setValue(sony::kShutterSpeed, sony::shutterValue(40, 10), shutterOrder) == M::Steps);
            CHECK(cam.shutter == sony::shutterValue(40, 10));
        }
        // a value the camera doesn't have: it stops on the nearer of the two either side
        CHECK(s.setValue(sony::kShutterSpeed, sony::shutterValue(1, 200), shutterOrder) == M::Steps);
        CHECK(cam.shutter == sony::shutterValue(1, 250));
        CHECK(s.setValue(sony::kShutterSpeed, sony::shutterValue(1, 80), shutterOrder) == M::Steps);
        CHECK(cam.shutter == sony::shutterValue(1, 60));
        // past the end of what it has: as far as it goes
        CHECK(s.setValue(sony::kShutterSpeed, sony::shutterValue(1, 8000), shutterOrder) == M::Steps);
        CHECK(cam.shutter == sony::shutterValue(1, 4000));
        CHECK_THAT(errorOf([&] { s.setValue(sony::kShutterSpeed, sony::shutterValue(1, 9000), shutterOrder); }), ContainsSubstring("didn't take 1/9000"));
        // a direct set the camera answers OK but ignores: a moment's wait, then steps
        cam.direct = fake::SonyCam::Direct::Ignored;
        CHECK(s.setValue(sony::kShutterSpeed, sony::shutterValue(1, 500), shutterOrder) == M::Steps);
        CHECK(cam.shutter == sony::shutterValue(1, 500));
        // not settable with the dial off M; not there at all
        cam.manual = false;
        CHECK_THAT(errorOf([&] { s.setValue(sony::kIso, 400, shutterOrder); }), ContainsSubstring("turn the mode dial to M"));
        CHECK_THAT(errorOf([&] { s.setValue(sony::kExposureProgram, 2, shutterOrder); }), ContainsSubstring("can't be set"));
        CHECK_THAT(errorOf([&] { s.setValue(0x1234, 1, shutterOrder); }), ContainsSubstring("has no property 0x1234"));
        CHECK_THAT(errorOf([&] { s.setA(0x1234, 1); }), ContainsSubstring("no property"));
    }
    SECTION("pictures: JPEG, RAW+JPEG either way round, RAW alone, none, abandoned") {
        cam.leftover = 2;
        s.connect();
        CHECK(s.objectsInMemory() == 2);
        CHECK(s.drain() == 2);
        auto shot = s.shoot(5s);
        REQUIRE(shot);
        CHECK(shot->objects.size() == 1);
        CHECK(shot->jpeg());
        CHECK(shot->objects[0].info.filename == "DSC00001.JPG");
        cam.quality = fake::SonyCam::Quality::RawPlusJpeg;
        shot = s.shoot(5s);
        CHECK(shot->objects.size() == 2);
        CHECK(shot->jpeg());
        cam.quality = fake::SonyCam::Quality::RawFirst;
        cam.jpegLagMs = 150;
        shot = s.shoot(5s);
        CHECK(shot->objects.size() == 2);
        CHECK(shot->jpeg());
        cam.quality = fake::SonyCam::Quality::RawOnly;
        CHECK_FALSE(s.shoot(5s)->jpeg());
        s.drain();
        cam.quality = fake::SonyCam::Quality::Jpeg;
        cam.noPicture = true;
        CHECK_THAT(errorOf([&] { s.shoot(100ms); }), ContainsSubstring("Release w/o Lens"));
        std::atomic<int> asked{0};
        CHECK_FALSE(s.shoot(10s, [&] { return ++asked > 3; }));
        cam.noPicture = false;
        s.drain();
        // a shot abandoned while the camera works on it: its picture, when it comes, isn't the next shot's
        cam.shotMs = 150;
        CHECK_FALSE(s.shoot(10s, [] { return true; }));
        const int abandoned = cam.shots.load();
        shot = s.shoot(10s);
        REQUIRE(shot);
        REQUIRE(shot->jpeg());
        CHECK(shot->jpeg()->info.filename == std::format("DSC{:05}.JPG", abandoned + 1));
        // and stopping while waiting for that picture: nothing pressed
        cam.shotMs = 1000;
        CHECK_FALSE(s.shoot(10s, [] { return true; }));
        cam.shotMs = 20;
        const int before = cam.shots.load();
        std::atomic<int> polls{0};
        CHECK_FALSE(s.shoot(10s, [&] { return ++polls > 0; }));
        CHECK(cam.shots.load() == before);
        s.shoot(10s);
        cam.fail[sony::kSetControlDeviceB] = ptp::kAccessDenied;
        CHECK_THAT(errorOf([&] { s.shoot(1s); }), ContainsSubstring("shutter half press"));
    }
    SECTION("live view, when the camera has it") {
        s.connect();
        uint16_t code = 0;
        CHECK(s.liveView(&code).empty());
        CHECK(code == ptp::kInvalidObjectHandle);
        cam.liveView = true;
        const auto lv = s.liveView(&code);
        CHECK(code == ptp::kOk);
        CHECK(jpeg::size(lv).width == cam.liveWidth);
    }
}

// ---------------------------------------------------------------- the camera backend

TEST_CASE("sony camera: finding and opening", "[sony][camera]") {
    {
        fake::SonySource none;
        setSonyDeviceSource(&none);
        CHECK_THAT(errorOf([] { openCamera("sony"); }), ContainsSubstring("no Sony camera found"));
        setSonyDeviceSource(nullptr);
    }
    for (auto via : {fake::SonySource::Via::Direct, fake::SonySource::Via::Wpd, fake::SonySource::Via::Usb}) {
        Fixture f(via);
        f.cam.leftover = 1;
        auto cam = openCamera("sony");
        const auto& i = cam->info();
        CHECK(i.vendor == "Sony");
        CHECK(i.model == "ILCE-6000");
        CHECK(i.serial == f.cam.serial);
        CHECK(i.sensorWidth == 320);
        CHECK(i.sensorHeight == 200);
        CHECK(i.pixelUm == Catch::Approx(23.5 * 1000 / 320));
        CHECK(i.stills);
        CHECK(i.bayer == Bayer::None);
        CHECK(i.bitDepths == std::vector<int>{8});
        CHECK(f.cam.shots.load() == 1);   // the picture that gave the size (the one left over was dropped first)
    }
    Fixture f;
    f.source.problem = "no permission";
    const auto lines = listCameras();
    CHECK(std::ranges::find(lines, std::string("sony:0  ILCE-6000  (no permission)")) != lines.end());
    CHECK(openCamera("sony:0")->info().model == "ILCE-6000");
    CHECK(openCamera("sony:ILCE")->info().model == "ILCE-6000");
    CHECK_THAT(errorOf([] { openCamera("sony:3"); }), ContainsSubstring("no Sony camera 3"));
    CHECK_THAT(errorOf([] { openCamera("sony:NEX"); }), ContainsSubstring("no Sony camera NEX"));
    CHECK_THAT(errorOf([] { openCamera("sonyx"); }), ContainsSubstring("bad camera spec"));
    CHECK_THAT(errorOf([] { openCamera("nikon"); }), ContainsSubstring("sony[:N]"));
    f.cam.operations = {ptp::kGetDeviceInfo, ptp::kGetObject};
    CHECK_THAT(errorOf([] { openCamera("sony"); }), ContainsSubstring("set the camera's USB Connection to PC Remote"));
    f.cam.operations = fake::SonyCam().operations;
    f.cam.quality = fake::SonyCam::Quality::RawOnly;
    CHECK_THAT(errorOf([] { openCamera("sony"); }), ContainsSubstring("sent no JPEG"));
    f.cam.quality = fake::SonyCam::Quality::Jpeg;
    f.cam.model = "Unknown-1";
    CHECK(openCamera("sony")->info().pixelUm == 0);
}

TEST_CASE("sony camera: controls", "[sony][camera]") {
    Fixture f;
    f.cam.direct = fake::SonyCam::Direct::Refused;   // as older bodies: shutter and ISO only in steps
    SonyCamera c;
    const auto ctl = c.controls();
    REQUIRE(ctl.size() == 3);
    CHECK(ctl[0].name == "Exposure");
    CHECK(ctl[0].min == Catch::Approx(250));
    CHECK(ctl[0].max == Catch::Approx(30e6));
    CHECK(ctl[0].writable);
    CHECK(ctl[1].name == "ISO");
    CHECK(ctl[1].min == 100);
    CHECK(ctl[1].max == 3200);   // auto and the noise-reduction variants left out
    CHECK(ctl[2].name == "Interval");
    CHECK(c.control("Exposure") == Catch::Approx(4000));
    CHECK(c.control("ISO") == 800);
    CHECK(c.control("Interval") == 0);
    CHECK_FALSE(c.control("Gain"));

    c.setControl("Exposure", 0.9e6);   // the nearest the camera has: 1"
    CHECK(f.cam.shutter == sony::shutterValue(10, 10));
    CHECK(c.control("Exposure") == Catch::Approx(1e6));
    c.setControl("ISO", 700);
    CHECK(f.cam.iso == 800);
    c.setControl("ISO", 2000);
    CHECK(f.cam.iso == 1600);
    c.setControl("Interval", 2);
    CHECK(c.control("Interval") == 2);
    CHECK_THAT(errorOf([&] { c.setControl("Exposure", 0); }), ContainsSubstring("more than 0"));
    CHECK_THAT(errorOf([&] { c.setControl("ISO", -5); }), ContainsSubstring("more than 0"));
    CHECK_THAT(errorOf([&] { c.setControl("Interval", 601); }), ContainsSubstring("out of range"));
    CHECK_THAT(errorOf([&] { c.setControl("Gain", 1); }), ContainsSubstring("no control Gain"));
    CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 400, 100, 1}); }), ContainsSubstring("outside"));
    CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 100, 100, 2}); }), ContainsSubstring("bin 2"));
    CHECK_THAT(errorOf([&] { c.setBitDepth(16); }), ContainsSubstring("16-bit"));
    CHECK_NOTHROW(c.setBitDepth(8));

    // the camera without lists: the usual series, the camera stopping on the nearest it has
    f.cam.listValues = false;
    SonyCamera bare;
    CHECK(bare.controls()[0].min == Catch::Approx(250));
    CHECK(bare.controls()[1].max == 25600);
    bare.setControl("Exposure", 1e6 / 60);
    CHECK(f.cam.shutter == sony::shutterValue(1, 60));

    // the mode dial off M: neither can be set
    f.cam.manual = false;
    SonyCamera auto_;
    CHECK_FALSE(auto_.controls()[0].writable);
    CHECK_FALSE(auto_.controls()[1].writable);
    CHECK_THAT(errorOf([&] { auto_.setControl("Exposure", 1000); }), ContainsSubstring("mode dial to M"));
    f.cam.iso = sony::kIsoAuto;
    SonyCamera isoAuto;
    CHECK_FALSE(isoAuto.control("ISO"));
}

TEST_CASE("sony camera: test shots as frames", "[sony][camera]") {
    Fixture f;
    f.cam.setDraw([](uint8_t* px, int w, int h, int) {   // a smooth ramp: the JPEG keeps it closely
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) px[y * w + x] = static_cast<uint8_t>(20 + x / 2 + y / 4);
    });
    SonyCamera c;
    c.setRoi({100, 50, 64, 32, 1});
    CHECK(c.roi() == Roi{100, 50, 64, 32, 1});
    CHECK(c.frameBytes() == 64 * 32);
    std::vector<std::byte> buf(c.frameBytes());
    FrameInfo fi;
    CHECK_THROWS_AS(c.readFrame(buf, fi, 10ms), CameraError);   // not streaming
    const auto before = std::chrono::steady_clock::now();
    c.startStream();
    c.startStream();   // (again: no effect)
    CHECK(c.streaming());
    CHECK_THROWS_AS(c.setRoi({0, 0, 64, 32, 1}), CameraError);
    REQUIRE(c.readFrame(buf, fi, 5s) == ReadResult::Ok);
    CHECK(fi.sequence == 0);
    CHECK(fi.roi == Roi{100, 50, 64, 32, 1});
    CHECK(fi.bitDepth == 8);
    CHECK(fi.bytes == 64 * 32);
    REQUIRE(fi.exposureStart);
    CHECK(*fi.exposureStart >= before);
    double err = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) err += std::abs(static_cast<int>(buf[static_cast<std::size_t>(y * 64 + x)]) - (20 + (100 + x) / 2 + (50 + y) / 4));
    CHECK(err / (64 * 32) < 2);
    REQUIRE(c.readFrame(buf, fi, 5s) == ReadResult::Ok);
    CHECK(fi.sequence == 1);
    std::vector<std::byte> small(10);
    CHECK_THROWS_AS(c.readFrame(small, fi, 5s), CameraError);
    REQUIRE(c.readFrame(buf, fi, 5s) == ReadResult::Ok);

    // a pause between shots; a frame nobody read counts as dropped
    c.setControl("Interval", 0.3);
    REQUIRE(c.readFrame(buf, fi, 5s) == ReadResult::Ok);   // the one on its way, or the next
    REQUIRE(c.readFrame(buf, fi, 5s) == ReadResult::Ok);   // after a pause
    CHECK(c.readFrame(buf, fi, 20ms) == ReadResult::Timeout);
    c.setControl("Interval", 0);
    for (int i = 0; i < 200 && c.droppedFrames() == 0; ++i) std::this_thread::sleep_for(25ms);   // (slow under sanitizers)
    CHECK(c.droppedFrames() > 0);
    c.setControl("Exposure", 1e6 / 125);   // while shooting: between two shots
    c.stopStream();
    c.stopStream();
    CHECK_FALSE(c.streaming());
    CHECK(f.cam.shutter == sony::shutterValue(1, 125));

    // a picture of another size (the camera's settings changed): what overlaps, the rest black
    f.cam.width = 120;
    c.startStream();
    REQUIRE(c.readFrame(buf, fi, 5s) == ReadResult::Ok);
    CHECK(static_cast<int>(buf[0]) > 0);
    CHECK(static_cast<int>(buf[63]) == 0);
    c.stopStream();
}

TEST_CASE("sony camera: failures while shooting", "[sony][camera]") {
    Fixture f;
    SonyCamera c;
    std::vector<std::byte> buf(c.frameBytes());
    FrameInfo fi;

    // one failed download: the next shot carries on
    f.cam.failTimes[ptp::kGetObject] = 1;
    c.startStream();
    REQUIRE(c.readFrame(buf, fi, 5s) == ReadResult::Ok);
    c.stopStream();
    // three in a row: the stream ends with the camera's answer
    f.cam.failTimes[ptp::kGetObject] = 3;
    c.startStream();
    CHECK_THAT(errorOf([&] { c.readFrame(buf, fi, 5s); }), ContainsSubstring("picture download"));
    c.stopStream();
    // no JPEG among them
    f.cam.quality = fake::SonyCam::Quality::RawOnly;
    c.startStream();
    CHECK_THAT(errorOf([&] { c.readFrame(buf, fi, 5s); }), ContainsSubstring("sent no JPEG"));
    c.stopStream();
    f.cam.quality = fake::SonyCam::Quality::Jpeg;
    // unplugged: at once
    c.startStream();
    REQUIRE(c.readFrame(buf, fi, 5s) == ReadResult::Ok);
    f.cam.setUnplugged(true);
    CHECK_THAT(errorOf([&] {
                   for (int i = 0; i < 10; ++i) c.readFrame(buf, fi, 5s);
               }),
               ContainsSubstring("gone"));
    c.stopStream();
    f.cam.setUnplugged(false);
    // stopping while the camera takes long over a picture
    f.cam.noPicture = true;
    c.startStream();
    std::this_thread::sleep_for(100ms);
    const auto t0 = std::chrono::steady_clock::now();
    c.stopStream();
    CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    // controls read while stopped come from the camera; a camera that stopped answering leaves the last values
    f.cam.noPicture = false;
    f.cam.setUnplugged(true);
    std::this_thread::sleep_for(600ms);
    CHECK(c.control("ISO") == 800);
    f.cam.setUnplugged(false);
}

// ---------------------------------------------------------------- the probe

TEST_CASE("sony probe: a report from the fake camera", "[sony][probe]") {
    fake::SonyCam cam;
    cam.liveView = true;
    cam.quality = fake::SonyCam::Quality::RawPlusJpeg;
    cam.direct = fake::SonyCam::Direct::Refused;
    auto t = fake::directTransport(cam);
    std::ostringstream log;
    std::vector<sonyprobe::File> files;
    sonyprobe::Options o;
    o.liveFrames = 3;
    o.shots = 2;
    const auto report = sonyprobe::run(*t, o, log, files).dump();
    CHECK_THAT(report, ContainsSubstring("\"model\": \"ILCE-6000\""));
    CHECK_THAT(report, ContainsSubstring("(left out: 16 characters)"));
    CHECK_THAT(report, ContainsSubstring("\"method\": \"steps\""));
    CHECK_THAT(report, ContainsSubstring("\"restored\": true"));
    CHECK_THAT(report, ContainsSubstring("\"format\": \"ARW\""));
    CHECK_THAT(report, ContainsSubstring("\"frames\": 3"));
    CHECK_THAT(log.str(), ContainsSubstring("live view over USB: YES"));
    CHECK_THAT(log.str(), ContainsSubstring("focus score"));
    CHECK(std::ranges::count_if(files, [](const sonyprobe::File& x) { return x.suffix.ends_with(".jpg"); }) >= 3);
    CHECK(std::ranges::count_if(files, [](const sonyprobe::File& x) { return x.suffix.ends_with(".arw"); }) >= 1);

    // the dial off M, no live view, no Sony extension
    fake::SonyCam plain;
    plain.manual = false;
    plain.fail[sony::kSdioConnect] = ptp::kOperationNotSupported;
    auto t2 = fake::directTransport(plain);
    std::ostringstream log2;
    files.clear();
    const auto r2 = sonyprobe::run(*t2, o, log2, files).dump();
    CHECK_THAT(r2, ContainsSubstring("handshakeDiagnosis"));
    CHECK_THAT(log2.str(), ContainsSubstring("FAILED"));
    plain.fail.clear();
    sonyprobe::Options quick;
    quick.shot = false;
    std::ostringstream log3;
    const auto r3 = sonyprobe::run(*t2, quick, log3, files).dump();
    CHECK_THAT(log3.str(), ContainsSubstring("live view over USB: no"));
    CHECK_THAT(log3.str(), ContainsSubstring("not settable now"));
    CHECK(sonyprobe::Json(true).isTrue());
    CHECK(sonyprobe::Json().dump() == "null");
    CHECK(sonyprobe::Json(1.5).dump() == "1.5");
    CHECK(sonyprobe::Json("a\"\\\n\t\r\x01").dump() == "\"a\\\"\\\\\\n\\t\\r\\u0001\"");
    CHECK(sonyprobe::Json::array().dump() == "[]");
    CHECK(sonyprobe::Json::object().dump() == "{}");
}

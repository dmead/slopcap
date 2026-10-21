// The QHY backend (src/camera/qhy_camera.cpp) against the fake SDK in tests/fakes/fake_qhy.cpp.
#include "camera/open_camera.hpp"
#include "camera/qhy_camera.hpp"
#include "fakes/fake_sdk.hpp"

#include <qhyccd.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>

using namespace slopcap;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;

namespace {

std::string errorOf(auto&& fn) {
    try {
        fn();
    } catch (const CameraError& e) {
        return e.what();
    }
    return "(no error)";
}

}  // namespace

TEST_CASE("QHY: listing and opening", "[qhy]") {
    auto& f = fake::qhy();
    f.reset();
    f.cams.push_back(f.cam());
    f.cams[1].id = "QHY462C-77";
    f.cams[1].model = "";   // no model name: the id stands in
    auto list = QhyCamera::list();
    REQUIRE(list.size() == 2);
    CHECK(list[0].model == "QHY533M");
    CHECK(list[1].model == "QHY462C-77");
    CHECK(f.resourceUsers == 0);
    const auto names = listCameras();
    CHECK(std::ranges::find(names, std::string("qhy:0  QHY533M  [QHY533M-0123456789abcdef]")) != names.end());
    prepareCameras();
#ifdef __APPLE__
    CHECK(f.firmwareUploads >= 1);   // once per process
#endif

    {
        auto cam = openCamera("qhy:1");
        CHECK(cam->info().model == "QHY462C-77");
        CHECK(cam->info().serial == "QHY462C-77");
        CHECK(f.open);
        CHECK(f.streamMode == 1);
        CHECK(f.initialised);
        CHECK(f.resourceUsers == 1);
        CHECK(QhyCamera::list().size() == 2);   // the SDK stays initialised while a camera is open
        CHECK(f.resourceUsers == 1);
    }
    CHECK_FALSE(f.open);
    CHECK(f.resourceUsers == 0);
    CHECK(openCamera("qhy")->info().vendor == "QHY");

    f.fail["GetQHYCCDId"] = 1;
    CHECK(QhyCamera::list().empty());
}

TEST_CASE("QHY: opening fails cleanly", "[qhy]") {
    auto& f = fake::qhy();
    const std::pair<const char*, const char*> failures[] = {
        {"GetQHYCCDId", "cannot read camera id"}, {"SetQHYCCDReadMode", "set read mode failed"},
        {"SetQHYCCDStreamMode", "live mode not supported"}, {"InitQHYCCD", "init failed"},
        {"GetQHYCCDChipInfo", "chip info failed"}, {"SetQHYCCDBitsMode", "8-bit mode failed"},
        {"SetQHYCCDBinMode", "bin mode failed"}, {"SetQHYCCDResolution", "set ROI failed"},
        {"InitQHYCCDResource", "SDK init failed"},
    };
    for (auto [fn, text] : failures) {
        f.reset();
        f.fail[fn] = 1;
        CHECK_THAT(errorOf([] { QhyCamera c; }), ContainsSubstring(text));
        CHECK_FALSE(f.open);
        CHECK(f.resourceUsers == 0);
    }
    f.reset();
    f.openFails = true;
    CHECK_THAT(errorOf([] { QhyCamera c; }), ContainsSubstring("in use by another program"));
    CHECK(f.resourceUsers == 0);
    f.reset();
    CHECK_THAT(errorOf([] { QhyCamera c(1); }), ContainsSubstring("no QHY camera at index 1"));
    CHECK_THAT(errorOf([] { QhyCamera c(-1); }), ContainsSubstring("no QHY camera at index -1"));
    CHECK(f.resourceUsers == 0);
}

TEST_CASE("QHY: camera description", "[qhy]") {
    auto& f = fake::qhy();
    f.reset();
    {
        QhyCamera c;
        const auto& i = c.info();
        CHECK(i.sensorWidth == 3008);
        CHECK(i.sensorHeight == 3008);
        CHECK(i.pixelUm == 3.76);
        CHECK(i.bayer == Bayer::None);
        CHECK(i.bins == std::vector<int>{1, 2});
        CHECK(i.bitDepths == std::vector<int>{8, 16});
        CHECK(i.adcBits == 14);
        CHECK(c.roi() == Roi{0, 0, 3008, 3008, 1});
        CHECK(c.bitDepth() == 8);
        CHECK(f.bits == 8);
        CHECK(f.w == 3008);
        const auto modes = c.readoutModes();
        REQUIRE(modes.size() == 3);
        CHECK(modes[1].name == "High Gain Mode");
        CHECK_FALSE(modes[0].width);
        CHECK(modes[2].height == 3000);
    }
    const std::pair<uint32_t, Bayer> patterns[] = {{BAYER_RG, Bayer::RGGB}, {BAYER_GR, Bayer::GRBG}, {BAYER_GB, Bayer::GBRG},
                                                   {BAYER_BG, Bayer::BGGR}};
    for (auto [p, b] : patterns) {
        f.cam().bayer = p;
        CHECK(QhyCamera().info().bayer == b);
    }
    // a camera that reports almost nothing
    f.reset();
    f.cam().available = {CONTROL_GAIN};
    f.cam().modes[1].name = "";
    f.cam().currentMode = 1;
    QhyCamera c;
    CHECK(c.info().bins == std::vector<int>{1});
    CHECK(c.info().bitDepths == std::vector<int>{8});
    CHECK_FALSE(c.info().adcBits);
    CHECK(c.readoutModes()[1].name == "mode 1");
    CHECK(c.readoutMode() == 1);
}

TEST_CASE("QHY: controls", "[qhy]") {
    auto& f = fake::qhy();
    f.reset();
    f.cam().available.insert(CONTROL_SPEED);   // available, but with no value or range
    QhyCamera c;
    const auto ctl = c.controls();
    REQUIRE(ctl.size() == 7);
    const auto gain = std::ranges::find(ctl, "Gain", &ControlInfo::name);
    CHECK(gain->max == 100);
    const auto temp = std::ranges::find(ctl, "Temperature", &ControlInfo::name);
    CHECK_FALSE(temp->writable);
    const auto speed = std::ranges::find(ctl, "Speed", &ControlInfo::name);
    CHECK(speed->max == 0);

    CHECK(c.control("Gain") == 30);
    CHECK(c.control("Temperature") == 21.5);
    CHECK_FALSE(c.control("Speed"));
    CHECK_FALSE(c.control("Gamma"));
    c.setControl("Gain", 60);
    CHECK(f.cam().params[CONTROL_GAIN] == 60);
    c.setControl("TargetTemp", -10);
    c.setControl("Speed", 2);   // no range to check against
    CHECK_THAT(errorOf([&] { c.setControl("Gain", 101); }), ContainsSubstring("out of range 0..100"));
    CHECK_THAT(errorOf([&] { c.setControl("Temperature", 1); }), ContainsSubstring("read-only"));
    CHECK_THAT(errorOf([&] { c.setControl("Gamma", 1); }), ContainsSubstring("has no control Gamma"));
    f.fail["SetQHYCCDParam"] = 1;
    CHECK_THAT(errorOf([&] { c.setControl("Gain", 1); }), ContainsSubstring("set Gain failed"));
}

TEST_CASE("QHY: gain landmarks from the SDK's curves", "[qhy]") {
    auto& f = fake::qhy();
    f.reset();
    {
        QhyCamera c;
        const auto marks = c.gainLandmarks();
        REQUIRE(marks.size() == 4);
        CHECK(marks[0].kind == GainLandmark::Kind::VendorDefault);
        CHECK(marks[0].gain == 30);
        CHECK(marks[0].offset == 10);
        CHECK(marks[1].kind == GainLandmark::Kind::Unity);
        CHECK(marks[1].gain == 27);
        CHECK(marks[2].kind == GainLandmark::Kind::HcgSwitch);
        CHECK(marks[2].gain == 60);
        CHECK(marks[3].kind == GainLandmark::Kind::LowestReadNoise);
        CHECK(marks[3].gain == 100);

        f.cam().readNoiseCurve = f.cam().systemGainCurve = false;   // listed, but the calls fail
        CHECK(c.gainLandmarks().size() == 1);
        f.cam().ranges.erase(CONTROL_GAIN);
        f.cam().readNoiseCurve = true;
        CHECK(c.gainLandmarks().size() == 1);
        f.cam().available.erase(DefaultOffset);
        f.cam().available.erase(CAM_CurveReadoutNoise);
        f.cam().available.erase(CAM_CurveSystemGain);
        const auto one = c.gainLandmarks();
        REQUIRE(one.size() == 1);
        CHECK_FALSE(one[0].offset);
        f.cam().available.erase(DefaultGain);
        CHECK(c.gainLandmarks().empty());
    }
    f.reset();
    f.cam().ranges[CONTROL_GAIN] = {0, 100, 0};   // no step reported
    QhyCamera c;
    CHECK(c.gainLandmarks().size() == 4);
}

TEST_CASE("QHY: read modes, ROI and bit depth", "[qhy]") {
    auto& f = fake::qhy();
    f.reset();
    {
        QhyCamera c;
        c.setBitDepth(16);
        CHECK(f.bits == 16);
        c.setReadoutMode(2);   // a smaller area: the ROI falls back to the whole of it
        CHECK(f.readMode == 2);
        CHECK(c.readoutMode() == 2);
        CHECK(c.info().sensorHeight == 3000);
        CHECK(c.roi() == Roi{0, 0, 3008, 3000, 1});
        CHECK(c.bitDepth() == 16);
        f.cam().available.erase(CAM_16BITS);
        c.setReadoutMode(0);   // 16-bit gone: back to what the camera offers
        CHECK(c.bitDepth() == 8);
        CHECK(c.roi() == Roi{0, 0, 3008, 3000, 1});   // still inside the sensor: kept
        CHECK_THAT(errorOf([&] { c.setReadoutMode(3); }), ContainsSubstring("no read mode 3"));
        CHECK_THAT(errorOf([&] { c.setReadoutMode(-1); }), ContainsSubstring("no read mode -1"));
        CHECK_THAT(errorOf([&] { c.setBitDepth(16); }), ContainsSubstring("16-bit output not supported"));

        c.setRoi({100, 200, 640, 480, 1});
        CHECK(f.x == 100);
        CHECK(f.h == 480);
        c.setRoi({0, 0, 1000, 1000, 2});
        CHECK(f.bin == 2);
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 640, 480, 4}); }), ContainsSubstring("bin 4 not supported"));
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 642, 480, 1}); }), ContainsSubstring("multiple of 4"));
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 640, 0, 1}); }), ContainsSubstring("multiple of 4"));
        CHECK_THAT(errorOf([&] { c.setRoi({2800, 0, 640, 480, 1}); }), ContainsSubstring("outside the 3008x3008 sensor"));
        f.failOnce["SetQHYCCDResolution"] = 1;
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 320, 240, 1}); }), ContainsSubstring("set ROI failed"));
        CHECK(c.roi() == Roi{0, 0, 1000, 1000, 2});
        CHECK(f.w == 1000);

        c.startStream();
        CHECK_THAT(errorOf([&] { c.setReadoutMode(1); }), ContainsSubstring("stop the stream"));
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 320, 240, 1}); }), ContainsSubstring("stop the stream"));
        CHECK_THAT(errorOf([&] { c.setBitDepth(8); }), ContainsSubstring("stop the stream"));
    }
    f.reset();
    f.cam().bayer = BAYER_RG;
    f.fail["GetQHYCCDNumberOfReadModes"] = 1;
    f.fail["GetQHYCCDReadMode"] = 1;
    QhyCamera c;
    CHECK(c.readoutModes().empty());
    c.setReadoutMode(0);
    CHECK_THAT(errorOf([&] { c.setReadoutMode(1); }), ContainsSubstring("no read modes"));
    CHECK_THAT(errorOf([&] { c.setRoi({1, 0, 640, 480, 1}); }), ContainsSubstring("must be even"));
}

TEST_CASE("QHY: streaming", "[qhy]") {
    auto& f = fake::qhy();
    f.reset();
    {
        QhyCamera c;
        c.setRoi({0, 0, 320, 240, 1});
        std::vector<std::byte> buf(c.frameBytes());
        FrameInfo fi;
        CHECK_THAT(errorOf([&] { c.readFrame(buf, fi, 10ms); }), ContainsSubstring("not started"));
        c.stopStream();
        c.startStream();
        c.startStream();
        CHECK(f.live);
        f.notReady = 3;   // polled until a frame is there
        REQUIRE(c.readFrame(buf, fi, 1s) == ReadResult::Ok);
        CHECK(fi.sequence == 0);
        CHECK(fi.bytes == 320u * 240u);
        CHECK(std::to_integer<int>(buf[120 * 320 + 160]) > 150);
        REQUIRE(c.readFrame(buf, fi, 1s) == ReadResult::Ok);
        CHECK(fi.sequence == 1);
        CHECK(c.droppedFrames() == 0);
        f.notReady = 1'000'000;
        CHECK(c.readFrame(buf, fi, 5ms) == ReadResult::Timeout);
        f.notReady = 0;
        std::vector<std::byte> small(16);
        CHECK_THAT(errorOf([&] { c.readFrame(small, fi, 10ms); }), ContainsSubstring("smaller than one frame"));
        f.wrongSize = true;
        CHECK_THAT(errorOf([&] { c.readFrame(buf, fi, 10ms); }), ContainsSubstring("QHY delivered 324x240 8-bit, expected 320x240"));
        f.wrongSize = false;
        c.stopStream();
        CHECK_FALSE(f.live);

        f.fail["BeginQHYCCDLive"] = 1;
        CHECK_THAT(errorOf([&] { c.startStream(); }), ContainsSubstring("start live mode failed"));
        f.fail.clear();
        c.startStream();
        f.fail["StopQHYCCDLive"] = 1;
        CHECK_THAT(errorOf([&] { c.stopStream(); }), ContainsSubstring("stop live mode failed"));
        f.fail.clear();
        c.startStream();
    }
    CHECK_FALSE(f.live);   // destroyed while streaming
    CHECK_FALSE(f.open);
}

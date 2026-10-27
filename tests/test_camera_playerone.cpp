// The Player One backend (src/camera/playerone_camera.cpp) against the fake SDK in tests/fakes/fake_playerone.cpp.
#include "camera/open_camera.hpp"
#include "camera/playerone_camera.hpp"
#include "fakes/fake_sdk.hpp"

#include <libplayerone/PlayerOneCamera.h>

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

TEST_CASE("Player One: listing, opening and errors", "[playerone]") {
    auto& f = fake::poa();
    f.reset();
    f.cams.push_back(f.cam());
    f.cams[1].name = "Neptune-M";
    f.cams[1].id = 1;
    const auto list = PlayerOneCamera::list();
    REQUIRE(list.size() == 2);
    CHECK(list[1].model == "Neptune-M");
    const auto names = listCameras();
    CHECK(std::ranges::find(names, std::string("playerone:1  Neptune-M")) != names.end());

    auto cam = openCamera("playerone:1");
    CHECK(cam->info().model == "Neptune-M");
    CHECK(f.openId == 1);
    cam.reset();
    CHECK(f.openId == -1);
    CHECK(openCamera("playerone")->info().vendor == "Player One");
    CHECK_THAT(errorOf([] { openCamera("playerone:7"); }), ContainsSubstring("no Player One camera at index 7"));
    CHECK_THAT(errorOf([] { openCamera("playerone4"); }), ContainsSubstring("bad camera spec"));

    f.fail["POAGetCameraProperties"] = POA_ERROR_OPERATION_FAILED;
    CHECK(PlayerOneCamera::list().empty());
    CHECK_THAT(errorOf([] { PlayerOneCamera c; }), ContainsSubstring("camera properties: operation failed"));
    f.fail.clear();
    f.fail["POAOpenCamera"] = POA_ERROR_OPERATION_FAILED;
    CHECK_THAT(errorOf([] { PlayerOneCamera c; }), ContainsSubstring("open (in use by another program?)"));
    f.fail.clear();
    // a failed init or format closes the camera again
    f.fail["POAInitCamera"] = POA_ERROR_OPERATION_FAILED;
    CHECK_THAT(errorOf([] { PlayerOneCamera c; }), ContainsSubstring("init"));
    CHECK(f.openId == -1);
    f.fail.clear();
    f.fail["POASetImageFormat"] = POA_ERROR_OPERATION_FAILED;
    CHECK_THAT(errorOf([] { PlayerOneCamera c; }), ContainsSubstring("set image format"));
    CHECK(f.openId == -1);
    f.fail.clear();
}

TEST_CASE("Player One: camera description and controls", "[playerone]") {
    auto& f = fake::poa();
    f.reset();
    {
        PlayerOneCamera c;
        const auto& i = c.info();
        CHECK(i.vendor == "Player One");
        CHECK(i.model == "Uranus-C");
        CHECK(i.serial == "c0ffee0123");
        CHECK(i.sensorWidth == 3856);
        CHECK(i.adcBits == 12);
        CHECK(i.bayer == Bayer::RGGB);
        CHECK(i.bins == std::vector<int>{1, 2, 4});
        CHECK(i.bitDepths == std::vector<int>{8, 16});
        // the exposure in seconds and the flips aren't offered
        const auto infos = c.controls();
        CHECK(infos.size() == 5);
        CHECK(std::ranges::find(infos, "Exp", &ControlInfo::name) == infos.end());
        CHECK(c.control("Gain") == 100);
        CHECK(c.control("Temperature") == 21.5);   // a float config
        CHECK(c.control("HardwareBin") == 0);      // a bool one
        CHECK_FALSE(c.control("Nope"));
        c.setControl("Gain", 300);
        CHECK(f.config("Gain")->value == 300);
        c.setControl("HardwareBin", 1);
        CHECK(f.config("HardwareBin")->value == 1);
        CHECK_THAT(errorOf([&] { c.setControl("Gain", 701); }), ContainsSubstring("out of range 0..700"));
        CHECK_THAT(errorOf([&] { c.setControl("Temperature", 1); }), ContainsSubstring("read-only"));
        CHECK_THAT(errorOf([&] { c.setControl("Nope", 1); }), ContainsSubstring("has no control Nope"));
        // auto: setting a value keeps the control's auto mode
        CHECK(c.controlAuto("Exposure") == false);
        c.setControlAuto("Exposure", true);
        c.setControl("Exposure", 5000);
        CHECK(f.config("Exposure")->autoOn);
        CHECK(f.config("Exposure")->value == 5000);
        CHECK_FALSE(c.controlAuto("Nope"));
        CHECK_THAT(errorOf([&] { c.setControlAuto("Offset", true); }), ContainsSubstring("no auto mode"));
        f.fail["POAGetConfig"] = POA_ERROR_OPERATION_FAILED;
        CHECK_FALSE(c.control("Gain"));
        CHECK_FALSE(c.controlAuto("Exposure"));
        CHECK_THAT(errorOf([&] { c.setControlAuto("Exposure", false); }), ContainsSubstring("get Exposure"));
        f.fail.clear();
        f.fail["POASetConfig"] = POA_ERROR_OPERATION_FAILED;
        CHECK_THAT(errorOf([&] { c.setControl("Gain", 1); }), ContainsSubstring("set Gain"));
        f.fail.clear();
    }
    // mono, no serial, no 8-bit, a config it fails to describe
    f.cam().color = false;
    f.cam().sn.clear();
    f.cam().formats = {POA_RAW16, POA_RGB24};
    f.cam().bins.clear();
    f.fail["POAGetConfigAttributes:Offset"] = 1;
    PlayerOneCamera c;
    CHECK(c.info().bayer == Bayer::None);
    CHECK(c.info().serial == "Uranus-C#0");
    CHECK(c.info().bitDepths == std::vector<int>{16});
    CHECK(c.info().bins == std::vector<int>{1});
    CHECK_FALSE(c.control("Offset"));
    const std::pair<int, Bayer> patterns[] = {{POA_BAYER_BG, Bayer::BGGR}, {POA_BAYER_GR, Bayer::GRBG}, {POA_BAYER_GB, Bayer::GBRG}};
    f.cam().color = true;
    f.fail.clear();
    for (auto [p, b] : patterns) {
        f.cam().bayer = p;
        CHECK(PlayerOneCamera().info().bayer == b);
    }
}

TEST_CASE("Player One: sensor modes and preset gains", "[playerone]") {
    auto& f = fake::poa();
    f.reset();
    {
        PlayerOneCamera c;
        REQUIRE(c.readoutModes().size() == 2);
        CHECK(c.readoutModes()[1].name == "LowNoise");
        c.setReadoutMode(1);
        CHECK(c.readoutMode() == 1);
        CHECK(f.mode == 1);
        CHECK_THAT(errorOf([&] { c.setReadoutMode(2); }), ContainsSubstring("no sensor mode 2"));
        c.startStream();
        CHECK_THAT(errorOf([&] { c.setReadoutMode(0); }), ContainsSubstring("stop the stream"));
        c.stopStream();
        f.fail["POAGetSensorMode"] = POA_ERROR_ACCESS_DENIED;
        CHECK(c.readoutMode() == 0);
        f.fail.clear();
        const auto marks = c.gainLandmarks();
        REQUIRE(marks.size() == 4);
        CHECK(marks[1].kind == GainLandmark::Kind::HcgSwitch);
        CHECK(marks[1].gain == 120);
        CHECK(marks[2].kind == GainLandmark::Kind::Unity);
        CHECK(marks[2].offset == 20);
        CHECK(marks[3].gain == 350);
    }
    f.cam().modes.clear();
    f.cam().gainsOk = false;
    PlayerOneCamera c;
    CHECK(c.readoutModes().empty());
    CHECK(c.readoutMode() == 0);
    c.setReadoutMode(0);
    CHECK_THAT(errorOf([&] { c.setReadoutMode(1); }), ContainsSubstring("no sensor modes"));
    CHECK(c.gainLandmarks().empty());
}

TEST_CASE("Player One: ROI, bit depth and streaming", "[playerone]") {
    auto& f = fake::poa();
    f.reset();
    {
        PlayerOneCamera c;
        c.setRoi({64, 32, 640, 480, 1});
        CHECK(f.width == 640);
        CHECK(f.startX == 64);
        c.setRoi({0, 0, 960, 540, 2});
        CHECK(f.bin == 2);
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 640, 480, 3}); }), ContainsSubstring("bin 3 not supported"));
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 642, 480, 1}); }), ContainsSubstring("multiple of 4"));
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 640, 481, 1}); }), ContainsSubstring("multiple of 4"));
        CHECK_THAT(errorOf([&] { c.setRoi({3600, 0, 640, 480, 1}); }), ContainsSubstring("outside"));
        CHECK_THAT(errorOf([&] { c.setRoi({1, 0, 640, 480, 1}); }), ContainsSubstring("must be even"));
        f.failOnce["POASetImageSize"] = POA_ERROR_OPERATION_FAILED;
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 320, 240, 1}); }), ContainsSubstring("set ROI size"));
        CHECK(c.roi() == Roi{0, 0, 960, 540, 2});   // the previous ROI, restored
        c.setRoi({0, 0, 320, 240, 1});
        c.setBitDepth(16);
        CHECK(f.format == POA_RAW16);
        CHECK(c.frameBytes() == 320u * 240u * 2u);
        CHECK_THAT(errorOf([&] { c.setBitDepth(12); }), ContainsSubstring("12-bit output not supported"));

        std::vector<std::byte> buf(c.frameBytes());
        FrameInfo fi;
        CHECK_THAT(errorOf([&] { c.readFrame(buf, fi, 10ms); }), ContainsSubstring("not started"));
        c.stopStream();
        c.startStream();
        c.startStream();
        CHECK(f.exposing);
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 640, 480, 1}); }), ContainsSubstring("stop the stream"));
        CHECK_THAT(errorOf([&] { c.setBitDepth(8); }), ContainsSubstring("stop the stream"));
        REQUIRE(c.readFrame(buf, fi, 100ms) == ReadResult::Ok);
        CHECK(fi.sequence == 0);
        CHECK(fi.bitDepth == 16);
        CHECK(fi.bytes == buf.size());
        f.timeouts = 1;
        CHECK(c.readFrame(buf, fi, 100ms) == ReadResult::Timeout);
        std::vector<std::byte> small(10);
        CHECK_THAT(errorOf([&] { c.readFrame(small, fi, 10ms); }), ContainsSubstring("smaller than one frame"));
        f.fail["POAGetImageData"] = POA_ERROR_OPERATION_FAILED;
        CHECK_THAT(errorOf([&] { c.readFrame(buf, fi, 10ms); }), ContainsSubstring("read frame"));
        f.fail.clear();
        f.dropped = 4;
        CHECK(c.droppedFrames() == 4);
        c.stopStream();
        f.dropped = 0;   // the SDK resets its count; ours stays
        CHECK(c.droppedFrames() == 4);
        f.fail["POAStartExposure"] = POA_ERROR_OPERATION_FAILED;
        CHECK_THAT(errorOf([&] { c.startStream(); }), ContainsSubstring("start video"));
        f.fail.clear();
        c.startStream();
        f.fail["POAStopExposure"] = POA_ERROR_OPERATION_FAILED;
        CHECK_THAT(errorOf([&] { c.stopStream(); }), ContainsSubstring("stop video"));
        f.fail.clear();
        c.startStream();
    }
    // destroyed while streaming: stopped and closed
    CHECK_FALSE(f.exposing);
    CHECK(f.openId == -1);
}

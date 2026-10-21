// The ZWO backend (src/camera/asi_camera.cpp) against the fake SDK in tests/fakes/fake_asi.cpp.
#include "camera/asi_camera.hpp"
#include "camera/open_camera.hpp"
#include "fakes/fake_sdk.hpp"

#include <libasi/ASICamera2.h>

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

TEST_CASE("ZWO: listing and opening", "[asi]") {
    auto& f = fake::asi();
    f.reset();
    f.cams.push_back(f.cam());
    f.cams[1].name = "ZWO ASI224MC";
    f.cams[1].id = 1;
    const auto list = AsiCamera::list();
    REQUIRE(list.size() == 2);
    CHECK(list[1].index == 1);
    CHECK(list[1].model == "ZWO ASI224MC");
    const auto names = listCameras();
    CHECK(std::ranges::find(names, std::string("asi:1  ZWO ASI224MC")) != names.end());

    f.fail["ASIGetCameraProperty"] = ASI_ERROR_GENERAL_ERROR;
    CHECK(AsiCamera::list().empty());
    CHECK_THAT(errorOf([] { AsiCamera c(0); }), ContainsSubstring("camera property"));
    f.fail.clear();

    CHECK_THAT(errorOf([] { AsiCamera c(2); }), ContainsSubstring("no ZWO camera at index 2"));
    CHECK_THAT(errorOf([] { AsiCamera c(-1); }), ContainsSubstring("no ZWO camera"));

    auto cam = openCamera("asi:1");
    CHECK(cam->info().model == "ZWO ASI224MC");
    CHECK(f.openId == 1);
    cam.reset();
    CHECK(f.openId == -1);
    CHECK(openCamera("asi")->info().vendor == "ZWO");
    CHECK_THAT(errorOf([] { openCamera("asi:x"); }), ContainsSubstring("bad camera index"));
    CHECK_THAT(errorOf([] { openCamera("asi5"); }), ContainsSubstring("bad camera spec"));
    prepareCameras();
}

TEST_CASE("ZWO: SDK errors become readable messages", "[asi]") {
    auto& f = fake::asi();
    f.reset();
    const std::pair<int, const char*> codes[] = {
        {ASI_ERROR_INVALID_INDEX, "invalid index"}, {ASI_ERROR_INVALID_ID, "invalid camera id"},
        {ASI_ERROR_INVALID_CONTROL_TYPE, "invalid control"}, {ASI_ERROR_CAMERA_CLOSED, "camera not open"},
        {ASI_ERROR_CAMERA_REMOVED, "camera removed"}, {ASI_ERROR_INVALID_SIZE, "invalid ROI size"},
        {ASI_ERROR_INVALID_IMGTYPE, "unsupported image format"}, {ASI_ERROR_OUTOF_BOUNDARY, "out of bounds"},
        {ASI_ERROR_TIMEOUT, "timeout"}, {ASI_ERROR_INVALID_SEQUENCE, "stop capture first"},
        {ASI_ERROR_BUFFER_TOO_SMALL, "buffer too small"}, {ASI_ERROR_VIDEO_MODE_ACTIVE, "video mode active"},
        {ASI_ERROR_EXPOSURE_IN_PROGRESS, "exposure in progress"}, {ASI_ERROR_GENERAL_ERROR, "general error"},
        {ASI_ERROR_INVALID_PATH, "error 6"},
    };
    for (auto [code, text] : codes) {
        f.fail["ASIOpenCamera"] = code;
        const auto msg = errorOf([] { AsiCamera c; });
        CHECK_THAT(msg, ContainsSubstring("ZWO open") && ContainsSubstring(text));
    }
    f.fail.clear();

    // a failed init closes the camera again
    f.fail["ASIInitCamera"] = ASI_ERROR_CAMERA_REMOVED;
    CHECK_THAT(errorOf([] { AsiCamera c; }), ContainsSubstring("init: camera removed"));
    CHECK(f.openId == -1);
    f.fail.clear();
    f.fail["ASISetROIFormat"] = ASI_ERROR_INVALID_SIZE;
    CHECK_THAT(errorOf([] { AsiCamera c; }), ContainsSubstring("set ROI format"));
    CHECK(f.openId == -1);
    f.fail.clear();
    f.fail["ASISetStartPos"] = ASI_ERROR_OUTOF_BOUNDARY;
    CHECK_THAT(errorOf([] { AsiCamera c; }), ContainsSubstring("set ROI start"));
}

TEST_CASE("ZWO: camera description", "[asi]") {
    auto& f = fake::asi();
    f.reset();
    {
        AsiCamera c;
        const auto& i = c.info();
        CHECK(i.vendor == "ZWO");
        CHECK(i.serial == "a0a1a2a3a4a5a6a7");
        CHECK(i.sensorWidth == 1920);
        CHECK(i.sensorHeight == 1080);
        CHECK(i.pixelUm == 2.9);
        CHECK(i.adcBits == 12);
        CHECK(i.bayer == Bayer::RGGB);
        CHECK(i.bins == std::vector<int>{1, 2});
        CHECK(i.bitDepths == std::vector<int>{8, 16});
        CHECK(c.controls().size() == 6);
        CHECK(c.roi() == Roi{0, 0, 1920, 1080, 1});
        CHECK(c.bitDepth() == 8);
        CHECK(f.width == 1920);
        CHECK(f.imgType == ASI_IMG_RAW8);
    }
    const std::pair<int, Bayer> patterns[] = {{ASI_BAYER_RG, Bayer::RGGB}, {ASI_BAYER_BG, Bayer::BGGR}, {ASI_BAYER_GR, Bayer::GRBG},
                                              {ASI_BAYER_GB, Bayer::GBRG}};
    for (auto [p, b] : patterns) {
        f.cam().bayer = p;
        CHECK(AsiCamera().info().bayer == b);
    }
    f.cam().color = false;
    f.cam().serialOk = false;
    f.cam().formats = {ASI_IMG_RGB24, ASI_IMG_RAW16, ASI_IMG_Y8};
    f.cam().controls.erase(f.cam().controls.begin() + 2);   // no Offset
    f.fail["ASIGetControlCaps:Exposure"] = 1;               // and one the SDK fails to describe
    AsiCamera c;
    CHECK(c.info().bayer == Bayer::None);
    CHECK(c.info().serial == "ZWO ASI585MC#0");
    CHECK(c.info().bitDepths == std::vector<int>{16});
    CHECK(c.controls().size() == 4);
    CHECK_FALSE(c.control("Exposure"));
}

TEST_CASE("ZWO: controls", "[asi]") {
    auto& f = fake::asi();
    f.reset();
    AsiCamera c;
    CHECK(c.control("Gain") == 200);
    CHECK_FALSE(c.control("Nope"));
    CHECK_FALSE(c.controlAuto("Nope"));
    c.setControl("Gain", 300);
    CHECK(f.control("Gain")->value == 300);
    CHECK_THAT(errorOf([&] { c.setControl("Gain", 601); }), ContainsSubstring("out of range 0..600"));
    CHECK_THAT(errorOf([&] { c.setControl("Temperature", 1); }), ContainsSubstring("read-only"));
    CHECK_THAT(errorOf([&] { c.setControl("Nope", 1); }), ContainsSubstring("has no control Nope"));

    // auto: setting a value keeps the control's auto mode
    CHECK(c.controlAuto("Exposure") == false);
    c.setControlAuto("Exposure", true);
    CHECK(c.controlAuto("Exposure") == true);
    c.setControl("Exposure", 5000);
    CHECK(f.control("Exposure")->autoOn);
    CHECK(f.control("Exposure")->value == 5000);
    CHECK_THAT(errorOf([&] { c.setControlAuto("Gain", true); }), ContainsSubstring("no auto mode"));
    CHECK_THAT(errorOf([&] { c.setControlAuto("Nope", true); }), ContainsSubstring("has no control"));

    f.fail["ASIGetControlValue"] = ASI_ERROR_CAMERA_REMOVED;
    CHECK_FALSE(c.control("Gain"));
    CHECK_FALSE(c.controlAuto("Exposure"));
    CHECK_THAT(errorOf([&] { c.setControlAuto("Exposure", false); }), ContainsSubstring("get Exposure"));
    f.fail.clear();
    f.fail["ASISetControlValue"] = ASI_ERROR_GENERAL_ERROR;
    CHECK_THAT(errorOf([&] { c.setControl("Gain", 1); }), ContainsSubstring("set Gain"));
}

TEST_CASE("ZWO: readout modes and gain landmarks", "[asi]") {
    auto& f = fake::asi();
    f.reset();
    {
        AsiCamera c;
        REQUIRE(c.readoutModes().size() == 2);
        CHECK(c.readoutMode() == 0);
        c.setReadoutMode(1);
        CHECK(c.readoutMode() == 1);
        CHECK(f.control("HighSpeedMode")->value == 1);
        CHECK_THAT(errorOf([&] { c.setReadoutMode(2); }), ContainsSubstring("must be 0"));

        const auto marks = c.gainLandmarks();
        REQUIRE(marks.size() == 6);
        CHECK(marks[0].kind == GainLandmark::Kind::HighestDynamicRange);
        CHECK(marks[0].gain == 0);
        CHECK(marks[0].offset == 70);
        CHECK(marks[1].kind == GainLandmark::Kind::Unity);
        CHECK_FALSE(marks[1].gain);
        CHECK(marks[2].gain == 252);
        CHECK(marks[5].label == "Preset high");
    }
    std::erase_if(f.cam().controls, [](const fake::AsiControl& c) { return c.name == "HighSpeedMode" || c.name == "Gain"; });
    f.cam().lmhOk = false;
    AsiCamera c;
    CHECK(c.readoutModes().empty());
    CHECK(c.readoutMode() == 0);
    c.setReadoutMode(0);
    CHECK_THAT(errorOf([&] { c.setReadoutMode(1); }), ContainsSubstring("no high-speed readout"));
    auto marks = c.gainLandmarks();
    REQUIRE(marks.size() == 3);
    CHECK_FALSE(marks[0].gain);   // no Gain control to take the minimum from
    f.cam().gainOffsetOk = false;
    CHECK(c.gainLandmarks().empty());
}

TEST_CASE("ZWO: ROI and bit depth", "[asi]") {
    auto& f = fake::asi();
    f.reset();
    AsiCamera c;
    c.setRoi({64, 32, 640, 480, 1});
    CHECK(f.width == 640);
    CHECK(f.startX == 64);
    c.setRoi({0, 0, 960, 540, 2});
    CHECK(f.bin == 2);
    CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 640, 480, 3}); }), ContainsSubstring("bin 3 not supported"));
    CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 644, 480, 1}); }), ContainsSubstring("multiple of 8"));
    CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 640, 481, 1}); }), ContainsSubstring("multiple of 8"));
    CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 0, 480, 1}); }), ContainsSubstring("multiple of 8"));
    CHECK_THAT(errorOf([&] { c.setRoi({1600, 0, 640, 480, 1}); }), ContainsSubstring("outside the 1920x1080 sensor"));
    CHECK_THAT(errorOf([&] { c.setRoi({-2, 0, 640, 480, 1}); }), ContainsSubstring("outside"));
    CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 960, 544, 2}); }), ContainsSubstring("outside"));
    CHECK_THAT(errorOf([&] { c.setRoi({1, 0, 640, 480, 1}); }), ContainsSubstring("must be even"));

    // the SDK refuses: the previous ROI is restored
    f.failOnce["ASISetROIFormat"] = ASI_ERROR_INVALID_SIZE;
    CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 320, 240, 1}); }), ContainsSubstring("invalid ROI size"));
    CHECK(c.roi() == Roi{0, 0, 960, 540, 2});
    CHECK(f.width == 960);

    c.setBitDepth(16);
    CHECK(f.imgType == ASI_IMG_RAW16);
    CHECK(c.frameBytes() == 960u * 540u * 2u);
    CHECK_THAT(errorOf([&] { c.setBitDepth(12); }), ContainsSubstring("12-bit output not supported"));

    c.startStream();
    CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 640, 480, 1}); }), ContainsSubstring("stop the stream"));
    CHECK_THAT(errorOf([&] { c.setBitDepth(8); }), ContainsSubstring("stop the stream"));
}

TEST_CASE("ZWO: streaming", "[asi]") {
    auto& f = fake::asi();
    f.reset();
    {
        AsiCamera c;
        c.setRoi({0, 0, 320, 240, 1});
        std::vector<std::byte> buf(c.frameBytes());
        FrameInfo fi;
        CHECK_THAT(errorOf([&] { c.readFrame(buf, fi, 10ms); }), ContainsSubstring("not started"));
        c.stopStream();   // not streaming: nothing to do
        f.dropped = 5;
        c.startStream();
        c.startStream();
        CHECK(c.streaming());
        CHECK(f.capturing);
        REQUIRE(c.readFrame(buf, fi, 100ms) == ReadResult::Ok);
        CHECK(fi.sequence == 0);
        CHECK(fi.roi == c.roi());
        CHECK(fi.bytes == buf.size());
        CHECK(std::to_integer<int>(buf[120 * 320 + 160]) > 150);   // the disc is in the middle
        REQUIRE(c.readFrame(buf, fi, 100ms) == ReadResult::Ok);
        CHECK(fi.sequence == 1);
        f.timeouts = 1;
        CHECK(c.readFrame(buf, fi, 100ms) == ReadResult::Timeout);
        std::vector<std::byte> small(10);
        CHECK_THAT(errorOf([&] { c.readFrame(small, fi, 10ms); }), ContainsSubstring("smaller than one frame"));
        f.fail["ASIGetVideoData"] = ASI_ERROR_CAMERA_REMOVED;
        CHECK_THAT(errorOf([&] { c.readFrame(buf, fi, 10ms); }), ContainsSubstring("read frame: camera removed"));
        f.fail.clear();

        CHECK(c.droppedFrames() == 0);   // counted from the start of the stream
        f.dropped = 8;
        CHECK(c.droppedFrames() == 3);
        c.stopStream();
        f.dropped = 0;                   // the SDK resets its count; ours stays
        CHECK(c.droppedFrames() == 3);
        CHECK_FALSE(f.capturing);

        f.fail["ASIStartVideoCapture"] = ASI_ERROR_VIDEO_MODE_ACTIVE;
        CHECK_THAT(errorOf([&] { c.startStream(); }), ContainsSubstring("start video"));
        f.fail.clear();
        c.startStream();
        f.fail["ASIStopVideoCapture"] = ASI_ERROR_CAMERA_REMOVED;
        CHECK_THAT(errorOf([&] { c.stopStream(); }), ContainsSubstring("stop video"));
        f.fail.clear();
        c.startStream();
    }
    // destroyed while streaming: stopped and closed
    CHECK_FALSE(f.capturing);
    CHECK(f.openId == -1);
}

// The ToupTek-family backend (src/camera/touptek_family.inc) for both brands built on it, ToupTek and MallinCam,
// against the fake SDK compiled for each brand's symbols (tests/fakes/fake_toup.inc).
#include "camera/open_camera.hpp"
#include "camera/touptek_camera.hpp"
#include "fakes/fake_sdk.hpp"

#include <catch2/catch_approx.hpp>
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

constexpr int kNotImpl = static_cast<int>(0x80004001u), kBusy = static_cast<int>(0x800700aau), kDenied = static_cast<int>(0x80070005u);

template <ToupBrand B>
void exercise(fake::Toup& f, const std::string& vendor, const std::string& spec) {
    using Cam = ToupCamera<B>;
    f.reset();
    const std::string model = f.cam().name;

    // listing and opening
    REQUIRE(Cam::list().size() == 1);
    CHECK(Cam::list()[0].model == model);
    const auto names = listCameras();
    CHECK(std::ranges::find(names, spec + ":0  " + model) != names.end());
    CHECK(openCamera(spec)->info().vendor == vendor);
    CHECK_THAT(errorOf([&] { openCamera(spec + ":3"); }), ContainsSubstring("no " + vendor + " camera at index 3"));
    f.openFails = true;
    CHECK_THAT(errorOf([] { Cam c; }), ContainsSubstring("can't open " + model));
    f.openFails = false;
    f.fail["put_Option"] = kDenied;
    CHECK_THAT(errorOf([] { Cam c; }), ContainsSubstring("RAW mode: permission denied"));
    CHECK_FALSE(f.open);   // closed again
    f.fail.clear();

    {
        Cam c;
        const auto& i = c.info();
        CHECK(i.vendor == vendor);
        CHECK(i.model == model);
        CHECK(i.serial == f.cam().sn);
        CHECK(i.sensorWidth == 3840);
        CHECK(i.pixelUm == Catch::Approx(2.9));
        CHECK(i.adcBits == 12);
        CHECK(i.bayer == Bayer::RGGB);
        CHECK(i.bitDepths == std::vector<int>{8, 16});
        CHECK(f.options[0x04] == 1);   // RAW mode
        // every control the flags promise
        const auto infos = c.controls();
        std::vector<std::string> ctls;
        for (const auto& ci : infos) ctls.push_back(ci.name);
        CHECK(ctls == std::vector<std::string>{"Exposure", "Gain", "Offset", "Speed", "Temperature", "CoolerOn", "TargetTemp", "Fan",
                                               "LowNoise", "HighFullwell"});
        CHECK(std::ranges::find(infos, "Offset", &ControlInfo::name)->max == 31 * 16);   // 12-bit ADC
        CHECK(c.control("Exposure") == 10000);
        CHECK(c.control("Temperature") == 21.5);
        c.setControl("Exposure", 2500);
        CHECK(f.expo == 2500);
        c.setControl("Gain", 300);
        CHECK(c.control("Gain") == 300);
        c.setControl("Speed", 2);
        CHECK(c.control("Speed") == 2);
        c.setControl("Offset", 40);
        CHECK(c.control("Offset") == 40);
        c.setControl("CoolerOn", 1);
        CHECK(c.control("CoolerOn") == 1);
        c.setControl("TargetTemp", -10.5);
        CHECK(f.options[0x0f] == -105);   // 0.1 °C
        CHECK(c.control("TargetTemp") == -10.5);
        c.setControl("Fan", 1);
        CHECK(c.control("Fan") == 1);
        c.setControl("LowNoise", 1);
        CHECK(c.control("LowNoise") == 1);
        c.setControl("HighFullwell", 1);
        CHECK(c.control("HighFullwell") == 1);
        CHECK_FALSE(c.control("Nope"));
        CHECK_THAT(errorOf([&] { c.setControl("Gain", 50); }), ContainsSubstring("out of range 100..10000"));
        CHECK_THAT(errorOf([&] { c.setControl("Temperature", 1); }), ContainsSubstring("read-only"));
        CHECK_THAT(errorOf([&] { c.setControl("Nope", 1); }), ContainsSubstring("has no control Nope"));
        f.fail["put_ExpoAGain"] = kBusy;
        CHECK_THAT(errorOf([&] { c.setControl("Gain", 200); }), ContainsSubstring("set Gain: in use"));
        f.fail.clear();
        f.fail["get_ExpoTime"] = kNotImpl;
        CHECK_FALSE(c.control("Exposure"));
        f.fail.clear();
        // auto exposure only
        CHECK(c.controlAuto("Exposure") == false);
        c.setControlAuto("Exposure", true);
        CHECK(f.autoExpo == 1);
        CHECK(c.controlAuto("Exposure") == true);
        CHECK_FALSE(c.controlAuto("Gain"));
        CHECK_THAT(errorOf([&] { c.setControlAuto("Gain", true); }), ContainsSubstring("no auto mode"));
        f.fail["put_AutoExpoEnable"] = kNotImpl;
        CHECK_THAT(errorOf([&] { c.setControlAuto("Exposure", false); }), ContainsSubstring("not supported"));
        f.fail.clear();
        // conversion gain as the readout mode
        REQUIRE(c.readoutModes().size() == 3);
        CHECK(c.readoutModes()[2].name == "HDR");
        c.setReadoutMode(1);
        CHECK(c.readoutMode() == 1);
        CHECK_THAT(errorOf([&] { c.setReadoutMode(3); }), ContainsSubstring("no readout mode 3"));
        f.fail["put_Option"] = kNotImpl;
        CHECK_THAT(errorOf([&] { c.setReadoutMode(2); }), ContainsSubstring("conversion gain"));
        f.fail.clear();

        // ROI in sensor pixels, binning averaged
        c.setRoi({100, 50, 640, 480, 1});
        CHECK(f.roiX == 100);
        CHECK(f.roiW == 640);
        c.setRoi({50, 20, 480, 270, 2});
        CHECK(f.roiX == 100);
        CHECK(f.roiW == 960);
        CHECK(f.options[0x17] == (0x80 | 2));
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 640, 480, 5}); }), ContainsSubstring("bin 5 not supported"));
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 641, 480, 1}); }), ContainsSubstring("must be even"));
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 4, 480, 1}); }), ContainsSubstring("8 sensor pixels"));
        CHECK_THAT(errorOf([&] { c.setRoi({3600, 0, 640, 480, 1}); }), ContainsSubstring("outside"));
        CHECK_THAT(errorOf([&] { c.setRoi({1, 0, 640, 480, 1}); }), ContainsSubstring("start must be even"));
        f.failOnce["put_Roi"] = static_cast<int>(0x80070057u);
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 320, 240, 1}); }), ContainsSubstring("set ROI: invalid value"));
        CHECK(c.roi() == Roi{50, 20, 480, 270, 2});   // restored
        c.setRoi({0, 0, 320, 240, 1});
        c.setBitDepth(16);
        CHECK(f.options[0x06] == 1);
        CHECK_THAT(errorOf([&] { c.setBitDepth(12); }), ContainsSubstring("12-bit output not supported"));

        // streaming, numbered frames, MSB-aligned samples
        std::vector<std::byte> buf(c.frameBytes());
        FrameInfo fi;
        CHECK_THAT(errorOf([&] { c.readFrame(buf, fi, 10ms); }), ContainsSubstring("not started"));
        c.stopStream();
        c.startStream();
        c.startStream();
        CHECK(f.running);
        CHECK_THAT(errorOf([&] { c.setRoi({0, 0, 640, 480, 1}); }), ContainsSubstring("stop the stream"));
        CHECK_THAT(errorOf([&] { c.setBitDepth(8); }), ContainsSubstring("stop the stream"));
        REQUIRE(c.readFrame(buf, fi, 100ms) == ReadResult::Ok);
        CHECK(fi.sequence == 0);
        CHECK(fi.bitDepth == 16);
        const auto centre = (std::size_t{120} * 320 + 160) * 2;
        CHECK(std::to_integer<int>(buf[centre + 1]) > 150);   // the disc's high byte: full scale, not 12-bit
        f.seqJump = 3;
        REQUIRE(c.readFrame(buf, fi, 100ms) == ReadResult::Ok);
        CHECK(c.droppedFrames() == 3);
        f.timeouts = 1;
        CHECK(c.readFrame(buf, fi, 100ms) == ReadResult::Timeout);
        std::vector<std::byte> small(10);
        CHECK_THAT(errorOf([&] { c.readFrame(small, fi, 10ms); }), ContainsSubstring("smaller than one frame"));
        f.wrongSize = true;
        CHECK_THAT(errorOf([&] { c.readFrame(buf, fi, 100ms); }), ContainsSubstring("322x240 frame for a 320x240 ROI"));
        f.wrongSize = false;
        f.fail["WaitImageV3"] = static_cast<int>(0x8007001fu);
        CHECK_THAT(errorOf([&] { c.readFrame(buf, fi, 10ms); }), ContainsSubstring("device not functioning"));
        f.fail.clear();
        f.fail["Stop"] = static_cast<int>(0x8000ffffu);
        CHECK_THAT(errorOf([&] { c.stopStream(); }), ContainsSubstring("stop video"));
        f.fail.clear();
        f.fail["StartPullModeWithCallback"] = kBusy;
        CHECK_THAT(errorOf([&] { c.startStream(); }), ContainsSubstring("start video: in use"));
        f.fail.clear();
        c.startStream();
    }
    // destroyed while streaming: stopped and closed
    CHECK_FALSE(f.running);
    CHECK_FALSE(f.open);

    // an older SDK without low zero padding: the backend shifts the 12-bit samples up itself
    f.cam().zeroPadding = false;
    {
        Cam c;
        c.setRoi({0, 0, 320, 240, 1});
        c.setBitDepth(16);
        c.startStream();
        std::vector<std::byte> buf(c.frameBytes());
        FrameInfo fi;
        REQUIRE(c.readFrame(buf, fi, 100ms) == ReadResult::Ok);
        CHECK(std::to_integer<int>(buf[(std::size_t{120} * 320 + 160) * 2 + 1]) > 150);
    }
    // mono, no serial, a camera with nothing extra
    f.reset();
    auto& cam = f.cam();
    cam.flags = 0x10 /* MONO */;
    cam.fourcc = 'Y' | 'Y' << 8 | 'Y' << 16 | static_cast<unsigned>('Y') << 24;
    cam.maxBits = 8;
    cam.maxSpeed = 0;
    cam.tecRange = 0;
    f.fail["get_SerialNumber"] = kNotImpl;
    {
        Cam c;
        CHECK(c.info().bayer == Bayer::None);
        CHECK(c.info().serial == cam.id);
        CHECK(c.info().bitDepths == std::vector<int>{8});
        CHECK(c.controls().size() == 2);
        CHECK(c.readoutModes().empty());
        CHECK(c.readoutMode() == 0);
        c.setReadoutMode(0);
        CHECK_THAT(errorOf([&] { c.setReadoutMode(1); }), ContainsSubstring("no conversion gain"));
    }
    f.fail.clear();
    // a cooled camera that says its cooler's range, and the other Bayer patterns
    f.reset();
    f.cam().tecRange = static_cast<int>((static_cast<unsigned>(static_cast<unsigned short>(250)) << 16) | static_cast<unsigned short>(-450));
    {
        Cam c;
        const auto infos = c.controls();
        const auto t = std::ranges::find(infos, "TargetTemp", &ControlInfo::name);
        CHECK(t->min == -45);
        CHECK(t->max == 25);
    }
    const std::pair<const char*, Bayer> patterns[] = {{"GRBG", Bayer::GRBG}, {"GBRG", Bayer::GBRG}, {"BGGR", Bayer::BGGR}, {"Y411", Bayer::None}};
    for (auto [p, b] : patterns) {
        f.cam().fourcc = static_cast<unsigned char>(p[0]) | static_cast<unsigned char>(p[1]) << 8 | static_cast<unsigned char>(p[2]) << 16 |
                         static_cast<unsigned>(static_cast<unsigned char>(p[3])) << 24;
        CHECK(Cam().info().bayer == b);
    }
    f.fail["get_ExpTimeRange"] = kNotImpl;
    CHECK_THAT(errorOf([] { Cam c; }), ContainsSubstring("exposure range"));
    f.fail.clear();
    f.reset();
}

}  // namespace

TEST_CASE("ToupTek: open, controls, ROI, streaming", "[touptek]") { exercise<ToupBrand::ToupTek>(fake::touptek(), "ToupTek", "touptek"); }

TEST_CASE("MallinCam: the same, through MallinCam's own SDK", "[touptek]") {
    exercise<ToupBrand::MallinCam>(fake::mallincam(), "MallinCam", "mallincam");
    CHECK(fake::touptek().frames == 0);   // (ToupTek's library wasn't touched)
}

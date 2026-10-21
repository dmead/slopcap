#include "camera/sim_camera.hpp"
#include "io/ser.hpp"
#include "test_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <thread>

using namespace slopcap;
using namespace std::chrono_literals;
using slopcap::test::TempDir;

namespace {

// Frame i holds value (x + y*W + i) & 0xff at (x, y), so any crop can be checked exactly.
std::filesystem::path makeSer(const TempDir& dir, int w, int h, int frames, int64_t tickStep, int bitDepth = 8) {
    const auto path = dir / "sim.ser";
    ser::Header hd;
    hd.colorId = ser::ColorId::BayerRGGB;
    hd.width = w;
    hd.height = h;
    hd.bitDepth = bitDepth;
    ser::Writer wr(path, hd);
    std::vector<std::byte> f(hd.frameBytes());
    for (int i = 0; i < frames; ++i) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const auto v = static_cast<unsigned>((x + y * w + i) & 0xff);
                const std::size_t p = static_cast<std::size_t>(y * w + x);
                if (bitDepth > 8) {   // 16-bit sample, value in the high byte
                    f[2 * p] = std::byte{0};
                    f[2 * p + 1] = static_cast<std::byte>(v);
                } else {
                    f[p] = static_cast<std::byte>(v);
                }
            }
        wr.writeFrame(f, 639270383770360000 + i * tickStep);
    }
    wr.finish();
    return path;
}

std::byte expected(int w, int x, int y, int frame) { return static_cast<std::byte>((x + y * w + frame) & 0xff); }

}  // namespace

TEST_CASE("SimCamera describes the recording as a camera") {
    TempDir dir;
    SimCamera cam(makeSer(dir, 32, 16, 3, 59'500));
    CHECK(cam.info().vendor == "Simulated");
    CHECK(cam.info().sensorWidth == 32);
    CHECK(cam.info().bayer == Bayer::RGGB);
    CHECK(cam.roi() == Roi{0, 0, 32, 16, 1});
    CHECK(cam.frameBytes() == 32u * 16u);
    CHECK(cam.control("Gain") == 200.0);
    cam.setControl("Gain", 450);
    CHECK(cam.control("Gain") == 450.0);
    CHECK_THROWS_AS(cam.setControl("Gain", 9999), CameraError);
    CHECK_THROWS_AS(cam.setControl("Nope", 1), CameraError);
}

TEST_CASE("SimCamera delivers every frame, cropped to the ROI, then ends") {
    TempDir dir;
    SimCamera cam(makeSer(dir, 32, 16, 4, 59'500), {SimCamera::Pace::AsFastAsPossible});
    cam.setRoi({8, 4, 12, 6, 1});
    cam.startStream();
    std::vector<std::byte> buf(cam.frameBytes());
    FrameInfo fi;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(cam.readFrame(buf, fi, 100ms) == ReadResult::Ok);
        CHECK(fi.sequence == static_cast<uint64_t>(i));
        CHECK(fi.roi == Roi{8, 4, 12, 6, 1});
        CHECK(fi.bytes == 72u);
        CHECK(buf[0] == expected(32, 8, 4, i));                 // top-left of the ROI
        CHECK(buf[71] == expected(32, 8 + 11, 4 + 5, i));       // bottom-right
    }
    CHECK(cam.readFrame(buf, fi, 100ms) == ReadResult::EndOfStream);
    CHECK(cam.droppedFrames() == 0);
}

TEST_CASE("SimCamera rejects invalid ROIs") {
    TempDir dir;
    SimCamera cam(makeSer(dir, 32, 16, 1, 59'500));
    CHECK_THROWS_AS(cam.setRoi({1, 0, 8, 8, 1}), CameraError);    // odd offset on a Bayer sensor
    CHECK_THROWS_AS(cam.setRoi({0, 0, 40, 8, 1}), CameraError);   // wider than the sensor
    CHECK_THROWS_AS(cam.setRoi({0, 0, 8, 8, 2}), CameraError);    // binning
    cam.startStream();
    CHECK_THROWS_AS(cam.setRoi({0, 0, 8, 8, 1}), CameraError);    // while streaming
}

TEST_CASE("SimCamera converts between 8 and 16 bits") {
    TempDir dir;
    SECTION("8-bit file read as 16-bit") {
        SimCamera cam(makeSer(dir, 8, 4, 1, 59'500), {SimCamera::Pace::AsFastAsPossible});
        cam.setBitDepth(16);
        cam.startStream();
        std::vector<std::byte> buf(cam.frameBytes());
        FrameInfo fi;
        REQUIRE(cam.readFrame(buf, fi, 100ms) == ReadResult::Ok);
        CHECK(fi.bitDepth == 16);
        CHECK(buf[2 * 5] == std::byte{0});
        CHECK(buf[2 * 5 + 1] == expected(8, 5, 0, 0));   // value in the high byte
    }
    SECTION("16-bit file read as 8-bit") {
        SimCamera cam(makeSer(dir, 8, 4, 1, 59'500, 16), {SimCamera::Pace::AsFastAsPossible});
        cam.setBitDepth(8);
        cam.startStream();
        std::vector<std::byte> buf(cam.frameBytes());
        FrameInfo fi;
        REQUIRE(cam.readFrame(buf, fi, 100ms) == ReadResult::Ok);
        CHECK(buf[13] == expected(8, 5, 1, 0));
    }
}

TEST_CASE("SimCamera loops when asked") {
    TempDir dir;
    SimCamera cam(makeSer(dir, 8, 4, 3, 59'500), {SimCamera::Pace::AsFastAsPossible, true});
    cam.startStream();
    std::vector<std::byte> buf(cam.frameBytes());
    FrameInfo fi;
    for (int i = 0; i < 7; ++i) {
        REQUIRE(cam.readFrame(buf, fi, 100ms) == ReadResult::Ok);
        CHECK(buf[0] == expected(8, 0, 0, i % 3));
    }
    CHECK(fi.sequence == 6);
}

TEST_CASE("SimCamera paces in real time and counts frames a slow reader misses") {
    TempDir dir;
    // 60 frames, 20 ms apart: 1.2 s of recording, so a slow machine's late wake-ups can't run past its end (with 10
    // frames, a sleep that overslept by 100 ms on a busy CI Mac made the short read below return EndOfStream)
    SimCamera cam(makeSer(dir, 8, 4, 60, 200'000));
    cam.startStream();
    std::vector<std::byte> buf(cam.frameBytes());
    FrameInfo fi;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(cam.readFrame(buf, fi, 1s) == ReadResult::Ok);   // frame 0, immediately
    REQUIRE(cam.readFrame(buf, fi, 1s) == ReadResult::Ok);   // frame 1, after ~20 ms
    CHECK(std::chrono::steady_clock::now() - t0 >= 15ms);
    CHECK(cam.droppedFrames() == 0);

    std::this_thread::sleep_for(70ms);                       // fall ~3 frames behind
    REQUIRE(cam.readFrame(buf, fi, 1s) == ReadResult::Ok);
    CHECK(cam.droppedFrames() >= 2);
    CHECK(fi.sequence == 2);                                  // sequence counts delivered frames only

    SECTION("a short timeout returns Timeout without losing the frame") {
        const auto dropsBefore = cam.droppedFrames();
        const auto r = cam.readFrame(buf, fi, 1ms);
        CHECK((r == ReadResult::Timeout || r == ReadResult::Ok));
        CHECK(cam.droppedFrames() == dropsBefore);
    }
}

TEST_CASE("SimCamera replays a real recording", "[real-data]") {
    // Opt-in: SLOPCAP_TEST_SER=/path/to/recording.ser
    const char* path = std::getenv("SLOPCAP_TEST_SER");
    if (!path) SKIP("set SLOPCAP_TEST_SER to run against a real recording");
    SimCamera cam(path, {SimCamera::Pace::AsFastAsPossible});
    REQUIRE(cam.sourceFrames() > 0);
    const auto& info = cam.info();
    const int w = std::min(info.sensorWidth, 640) / 2 * 2, h = std::min(info.sensorHeight, 480) / 2 * 2;
    cam.setRoi({(info.sensorWidth - w) / 4 * 2, (info.sensorHeight - h) / 4 * 2, w, h, 1});
    cam.startStream();
    std::vector<std::byte> buf(cam.frameBytes());
    FrameInfo fi;
    for (int i = 0; i < 50; ++i) REQUIRE(cam.readFrame(buf, fi, 1s) == ReadResult::Ok);
    CHECK(fi.sequence == 49);
}

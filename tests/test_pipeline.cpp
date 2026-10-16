#include "camera/sim_camera.hpp"
#include "io/ser.hpp"
#include "pipeline/capture_session.hpp"
#include "pipeline/frame_ring.hpp"
#include "test_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <thread>

using namespace slopcap;
using namespace std::chrono_literals;
using slopcap::test::TempDir;

TEST_CASE("FrameRing hands slots from producer to consumer in order") {
    FrameRing ring(4, 1000);
    CHECK(ring.capacity() == 4);
    CHECK(ring.slotBytes() == 4096);   // rounded up to a page
    CHECK(reinterpret_cast<std::uintptr_t>(ring.slot(0).pixels.data()) % 4096 == 0);

    std::vector<uint32_t> got;
    for (int i = 0; i < 4; ++i) {
        auto s = ring.acquire();
        REQUIRE(s);
        ring.slot(*s).info.sequence = static_cast<uint64_t>(i);
        ring.publish(*s);
    }
    CHECK_FALSE(ring.acquire());   // all four slots queued
    CHECK(ring.queued() == 4);
    CHECK(ring.highWater() == 4);
    for (uint64_t i = 0; i < 4; ++i) {
        auto s = ring.next(10ms);
        REQUIRE(s);
        CHECK(ring.slot(*s).info.sequence == i);
        ring.release(*s);
    }
    CHECK_FALSE(ring.next(1ms));
    CHECK(ring.acquire());
}

TEST_CASE("FrameRing close() lets the consumer drain and stop") {
    FrameRing ring(2, 64);
    auto s = ring.acquire();
    ring.publish(*s);
    ring.close();
    CHECK(ring.next(1ms));
    CHECK_FALSE(ring.next(1ms));
}

TEST_CASE("FrameRing passes 200k frames between threads without loss or reordering") {
    FrameRing ring(8, 64);
    constexpr uint64_t kFrames = 200'000;
    uint64_t droppedFull = 0;
    std::thread producer([&] {
        uint64_t seq = 0;
        while (seq < kFrames) {
            auto s = ring.acquire();
            if (!s) {
                ++droppedFull;
                std::this_thread::yield();
                continue;   // a real camera would drop here; the test retries so every number gets through
            }
            auto& slot = ring.slot(*s);
            std::memcpy(slot.pixels.data(), &seq, sizeof seq);
            slot.info.sequence = seq++;
            ring.publish(*s);
        }
        ring.close();
    });
    uint64_t expect = 0;
    bool ordered = true;
    while (auto s = ring.next(1s)) {
        auto& slot = ring.slot(*s);
        uint64_t payload;
        std::memcpy(&payload, slot.pixels.data(), sizeof payload);
        ordered = ordered && slot.info.sequence == expect && payload == expect;
        ++expect;
        ring.release(*s);
    }
    producer.join();
    CHECK(ordered);
    CHECK(expect == kFrames);
    CHECK(ring.highWater() <= 8);
}

namespace {

std::filesystem::path makeSer(const TempDir& dir, int w, int h, int frames, int64_t tickStep) {
    const auto path = dir / "src.ser";
    ser::Header hd;
    hd.colorId = ser::ColorId::BayerRGGB;
    hd.width = w;
    hd.height = h;
    ser::Writer wr(path, hd);
    std::vector<std::byte> f(hd.frameBytes());
    for (int i = 0; i < frames; ++i) {
        for (std::size_t p = 0; p < f.size(); ++p) f[p] = static_cast<std::byte>((p + static_cast<std::size_t>(i)) & 0xff);
        wr.writeFrame(f, 639270383770360000 + i * tickStep);
    }
    return path;
}

}  // namespace

TEST_CASE("CaptureSession records every frame of a simulated camera to SER") {
    TempDir dir;
    SimCamera cam(makeSer(dir, 64, 32, 300, 59'500), {SimPace::AsFastAsPossible});
    cam.setRoi({16, 8, 32, 16, 1});
    CaptureOptions opt;
    opt.output = dir / "out.ser";
    opt.ringBytes = 400 * 4096;   // room for every frame: an unpaced source may outrun the writer
    opt.observer = "test";
    CaptureSession session(cam, opt);
    session.start();
    REQUIRE(session.wait(10s));
    const auto s = session.stats();
    CHECK(s.error.empty());
    CHECK(s.ringCapacity >= 300);
    CHECK(s.delivered == 300);
    CHECK(s.written == 300);
    CHECK(s.ringDropped == 0);
    CHECK(s.cameraDropped == 0);
    CHECK_FALSE(s.running);

    ser::Reader r(opt.output);
    CHECK(r.frameCount() == 300);
    CHECK(r.header().width == 32);
    CHECK(r.header().colorId == ser::ColorId::BayerRGGB);
    CHECK(r.header().observer == "test");
    REQUIRE(r.timestamps());
    std::vector<std::byte> buf(r.header().frameBytes());
    r.readFrame(299, buf);
    // pixel (16, 8) of source frame 299: index 8*64+16
    CHECK(buf[0] == static_cast<std::byte>((8u * 64u + 16u + 299u) & 0xff));
}

TEST_CASE("CaptureSession counts frames lost to a full ring instead of stalling capture") {
    TempDir dir;
    SimCamera cam(makeSer(dir, 512, 256, 2000, 59'500), {SimPace::AsFastAsPossible});
    CaptureOptions opt;
    opt.output = dir / "small-ring.ser";
    opt.ringBytes = 0;   // clamps to the 8-slot minimum
    CaptureSession session(cam, opt);
    session.start();
    REQUIRE(session.wait(30s));
    const auto s = session.stats();
    CHECK(s.ringCapacity == 8);
    CHECK(s.delivered == 2000);
    CHECK(s.written + s.ringDropped == s.delivered);   // every frame is either on disk or counted
    CHECK(ser::Reader(opt.output).frameCount() == static_cast<int64_t>(s.written));
}

TEST_CASE("CaptureSession stops at a frame limit and at a duration") {
    TempDir dir;
    const auto src = makeSer(dir, 32, 16, 50, 59'500);
    SECTION("frame limit") {
        SimCamera cam(src, {SimPace::AsFastAsPossible, true});
        CaptureOptions opt;
        opt.output = dir / "limit.ser";
        opt.frameLimit = 120;
        CaptureSession session(cam, opt);
        session.start();
        REQUIRE(session.wait(10s));
        CHECK(session.stats().written == 120);
        CHECK(ser::Reader(opt.output).frameCount() == 120);
    }
    SECTION("duration, real-time pacing") {
        SimCamera cam(src, {SimPace::Realtime, true});   // ~168 fps
        CaptureOptions opt;
        opt.output = dir / "duration.ser";
        opt.duration = 300ms;
        CaptureSession session(cam, opt);
        session.start();
        REQUIRE(session.wait(10s));
        const auto s = session.stats();
        CHECK(s.written == s.delivered);
        CHECK(s.delivered >= 30);    // generous bounds: sanitizer builds are slow
        CHECK(s.delivered <= 60);
    }
}

TEST_CASE("CaptureSession reports a bad output path before starting the camera") {
    TempDir dir;
    SimCamera cam(makeSer(dir, 16, 8, 5, 59'500), {SimPace::AsFastAsPossible});
    CaptureOptions opt;
    opt.output = dir / "no-such-dir" / "out.ser";
    CaptureSession session(cam, opt);
    CHECK_THROWS(session.start());
    CHECK_FALSE(cam.streaming());
}

TEST_CASE("CaptureSession with no output is a live view: frames reach the preview tap, nothing is written") {
    TempDir dir;
    SimCamera cam(makeSer(dir, 64, 32, 200, 59'500), {SimPace::AsFastAsPossible});
    LatestFrame tap;
    CaptureOptions opt;
    opt.ringBytes = 16 * 4096;
    opt.preview = &tap;
    CaptureSession session(cam, opt);
    session.start();

    // Take frames while it runs; each one must be a whole, consistent source frame.
    uint64_t seen = 0, lastSeq = 0;
    while (session.stats().running || seen == 0) {
        if (const auto* f = tap.take()) {
            REQUIRE(f->info.bytes == 64u * 32u);
            CHECK(f->pixels[0] == static_cast<std::byte>(f->info.sequence & 0xff));
            CHECK(f->pixels[100] == static_cast<std::byte>((100 + f->info.sequence) & 0xff));
            if (seen) CHECK(f->info.sequence > lastSeq);
            lastSeq = f->info.sequence;
            ++seen;
        }
        if (!session.stats().running) break;
        std::this_thread::sleep_for(200us);
    }
    REQUIRE(session.wait(10s));
    const auto s = session.stats();
    CHECK(s.error.empty());
    CHECK(s.delivered == 200);
    CHECK(s.written == 0);
    CHECK(s.ringDropped == 0);
    CHECK(seen >= 1);
    CHECK(std::distance(std::filesystem::directory_iterator(dir.path()), {}) == 1);   // only the source file
}

TEST_CASE("CaptureSession runs posted actions on the capture thread and reports their errors as notices") {
    TempDir dir;
    SimCamera cam(makeSer(dir, 32, 16, 50, 59'500), {SimPace::Realtime, true});
    CaptureOptions opt;
    opt.duration = 2s;
    CaptureSession session(cam, opt);
    session.start();
    std::atomic<int> ran{0};
    std::thread::id where;
    session.post([&](ICamera&) {
        where = std::this_thread::get_id();
        ++ran;
    });
    session.post([](ICamera& c) { c.setControl("NoSuchControl", 1); });
    const auto t0 = std::chrono::steady_clock::now();
    while (ran == 0 && std::chrono::steady_clock::now() - t0 < 2s) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);
    CHECK(ran == 1);
    CHECK(where != std::this_thread::get_id());
    session.stop();
    const auto s = session.stats();
    CHECK(s.error.empty());
    CHECK_FALSE(s.notice.empty());
}

#include "camera/sim_camera.hpp"
#include "io/ser.hpp"
#include "io/ser_direct.hpp"
#include "pipeline/capture_session.hpp"
#include "test_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace slopcap;
using namespace std::chrono_literals;
using slopcap::test::TempDir;

namespace {

std::vector<std::byte> frameData(std::size_t n, int i) {
    std::vector<std::byte> v(n);
    for (std::size_t p = 0; p < n; ++p) v[p] = static_cast<std::byte>((p * 13 + static_cast<std::size_t>(i) * 7) & 0xff);
    return v;
}

ser::Header mono(int w, int h, int bits = 8) {
    ser::Header hd;
    hd.width = w;
    hd.height = h;
    hd.bitDepth = bits;
    hd.instrument = "direct-test";
    return hd;
}

void checkFile(const std::filesystem::path& p, const ser::Header& h, int frames) {
    ser::Reader r(p);
    REQUIRE(r.frameCount() == frames);
    CHECK_FALSE(r.truncated());
    CHECK(r.header().instrument == "direct-test");
    REQUIRE(r.timestamps());
    CHECK(r.timestamps()->size() == static_cast<std::size_t>(frames));
    CHECK(std::filesystem::file_size(p) == ser::kHeaderSize + static_cast<uint64_t>(frames) * (h.frameBytes() + 8));
    std::vector<std::byte> buf(h.frameBytes());
    for (int i = 0; i < frames; ++i) {
        r.readFrame(i, buf);
        if (buf != frameData(h.frameBytes(), i)) FAIL("frame " << i << " differs");
        CHECK((*r.timestamps())[static_cast<std::size_t>(i)] == 639270383770360000 + i);
    }
}

}  // namespace

TEST_CASE("DirectWriter writes a valid SER when frames straddle tiny chunks") {
    TempDir dir;
    const auto h = mono(37, 13);   // 481-byte frames: never aligned to anything
    ser::DirectWriterOptions o;
    o.chunkBytes = 8192;
    o.inflight = 2;
    {
        ser::DirectWriter w(dir / "tiny-chunks.ser", h, o);
        for (int i = 0; i < 500; ++i) w.writeFrame(frameData(h.frameBytes(), i), 639270383770360000 + i);
        w.finish();
        CHECK(w.chunksWritten() > 20);
    }
    checkFile(dir / "tiny-chunks.ser", h, 500);
}

TEST_CASE("DirectWriter handles a recording smaller than one disk block") {
    TempDir dir;
    const auto h = mono(10, 10);
    {
        ser::DirectWriter w(dir / "small.ser", h);
        for (int i = 0; i < 2; ++i) w.writeFrame(frameData(h.frameBytes(), i), 639270383770360000 + i);
    }   // destructor finishes
    checkFile(dir / "small.ser", h, 2);
}

TEST_CASE("DirectWriter handles zero frames and 16-bit data") {
    TempDir dir;
    SECTION("no frames") {
        { ser::DirectWriter w(dir / "empty.ser", mono(16, 16)); }
        ser::Reader r(dir / "empty.ser");
        CHECK(r.frameCount() == 0);
        CHECK(std::filesystem::file_size(dir / "empty.ser") == ser::kHeaderSize);
    }
    SECTION("16-bit, default 8 MB chunks") {
        const auto h = mono(640, 480, 12);
        {
            ser::DirectWriter w(dir / "16.ser", h);
            for (int i = 0; i < 60; ++i) w.writeFrame(frameData(h.frameBytes(), i), 639270383770360000 + i);
        }
        checkFile(dir / "16.ser", h, 60);
    }
}

TEST_CASE("DirectWriter keeps an interrupted recording readable via header refreshes") {
    TempDir dir;
    const auto h = mono(37, 13);
    ser::DirectWriterOptions o;
    o.chunkBytes = 8192;
    o.inflight = 2;
    o.headerEvery = std::chrono::hours(1);   // refresh manually below
    ser::DirectWriter w(dir / "crash.ser", h, o);
    for (int i = 0; i < 200; ++i) w.writeFrame(frameData(h.frameBytes(), i), 639270383770360000 + i);
    std::this_thread::sleep_for(100ms);      // let the first chunk reach the disk
    w.refreshHeader();
    // Read while the writer is still open, as if the process had died here.
    ser::Reader r(dir / "crash.ser");
    CHECK(r.header().frameCount == 200);
    CHECK(r.frameCount() > 150);             // whole frames already on disk (the last partial chunk isn't)
    CHECK(r.frameCount() <= 200);
    std::vector<std::byte> buf(h.frameBytes());
    r.readFrame(r.frameCount() - 1, buf);
    CHECK(buf == frameData(h.frameBytes(), static_cast<int>(r.frameCount() - 1)));
}

TEST_CASE("DirectWriter reports an unusable path") {
    TempDir dir;
    CHECK_THROWS(ser::DirectWriter(dir / "missing" / "x.ser", mono(8, 8)));
}

TEST_CASE("CaptureSession records through the direct writer") {
    TempDir dir;
    const auto src = dir / "src.ser";
    {
        ser::Writer wr(src, mono(64, 32));
        for (int i = 0; i < 400; ++i) wr.writeFrame(frameData(64 * 32, i), 639270383770360000 + i * 59'500);
    }
    SimCamera cam(src, {SimPace::AsFastAsPossible});
    CaptureOptions opt;
    opt.output = dir / "out.ser";
    opt.ringBytes = 512 * 4096;
    opt.directIO = true;
    opt.direct.chunkBytes = 64 * 1024;
    CaptureSession session(cam, opt);
    session.start();
    REQUIRE(session.wait(30s));
    const auto s = session.stats();
    CHECK(s.error.empty());
    CHECK(s.written == 400);
    CHECK(s.ringDropped == 0);
    CHECK(s.writeMaxUs > 0);
    ser::Reader r(opt.output);
    REQUIRE(r.frameCount() == 400);
    std::vector<std::byte> buf(64 * 32);
    r.readFrame(399, buf);
    CHECK(buf == frameData(64 * 32, 399));
}

#include "io/ser.hpp"
#include "test_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <format>
#include <fstream>

using namespace slopcap;
using slopcap::test::TempDir;

namespace {

ser::Header bayer8(int w, int h) {
    ser::Header hd;
    hd.colorId = ser::ColorId::BayerRGGB;
    hd.width = w;
    hd.height = h;
    hd.bitDepth = 8;
    hd.observer = "dan";
    hd.instrument = "ZWO ASI585MC";
    hd.telescope = "LX200 + 2.5x";
    return hd;
}

std::vector<std::byte> pattern(std::size_t n, int seed) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::byte>((i * 7 + static_cast<std::size_t>(seed) * 31) & 0xff);
    return v;
}

template <class T>
T at(const std::array<std::byte, ser::kHeaderSize>& b, std::size_t off) {
    T v;
    std::memcpy(&v, b.data() + off, sizeof v);
    return v;
}

}  // namespace

TEST_CASE("SER header has the specified byte layout") {
    auto h = bayer8(1040, 560);
    h.frameCount = 30244;
    h.dateTimeUtcTicks = 639270383770360000;
    const auto b = h.encode();
    REQUIRE(b.size() == 178);
    CHECK(std::memcmp(b.data(), "LUCAM-RECORDER", 14) == 0);
    CHECK(at<int32_t>(b, 18) == 8);        // ColorID
    CHECK(at<int32_t>(b, 26) == 1040);     // width
    CHECK(at<int32_t>(b, 30) == 560);      // height
    CHECK(at<int32_t>(b, 34) == 8);        // bit depth
    CHECK(at<int32_t>(b, 38) == 30244);    // frame count
    CHECK(std::memcmp(b.data() + 82, "ZWO ASI585MC", 12) == 0);
    CHECK(at<int64_t>(b, 170) == 639270383770360000);
}

TEST_CASE("SER header round-trips and truncates text to 40 bytes") {
    auto h = bayer8(64, 32);
    h.telescope = std::string(60, 'x');
    const auto back = ser::Header::decode(h.encode());
    CHECK(back.width == 64);
    CHECK(back.height == 32);
    CHECK(back.colorId == ser::ColorId::BayerRGGB);
    CHECK(back.instrument == "ZWO ASI585MC");
    CHECK(back.telescope == std::string(40, 'x'));
}

TEST_CASE("SER header rejects garbage") {
    std::array<std::byte, ser::kHeaderSize> raw{};
    CHECK_THROWS_AS(ser::Header::decode(raw), ser::FormatError);
    auto h = bayer8(8, 8).encode();
    int32_t badColor = 42;
    std::memcpy(h.data() + 18, &badColor, 4);
    CHECK_THROWS_AS(ser::Header::decode(h), ser::FormatError);
}

TEST_CASE(".NET ticks convert to and from UTC") {
    // First timestamp of 2026-10-08-0641_1-L-Sat.ser (FireCapture): 2026-10-08 06:39:37.036 UTC
    const int64_t ticks = 639270383770360000;
    const auto tp = ser::fromTicks(ticks);
    CHECK(std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::floor<std::chrono::milliseconds>(tp)) == "2026-10-08 06:39:37.036");
    CHECK(ser::toTicks(tp) == ticks);
    CHECK(ser::toTicks(std::chrono::system_clock::time_point{}) == ser::kUnixEpochTicks);
}

TEST_CASE("SER writer and reader round-trip 8-bit Bayer frames with timestamps") {
    TempDir dir;
    const auto path = dir / "rt8.ser";
    const auto h = bayer8(16, 8);
    std::vector<int64_t> ts;
    {
        ser::Writer w(path, h);
        for (int i = 0; i < 5; ++i) {
            ts.push_back(639270383770360000 + i * 59'500);   // ~168 fps
            w.writeFrame(pattern(h.frameBytes(), i), ts.back());
        }
        w.finish();
    }
    ser::Reader r(path);
    CHECK(r.frameCount() == 5);
    CHECK_FALSE(r.truncated());
    CHECK(r.header().littleEndian == 0);
    CHECK(r.header().dateTimeUtcTicks == ts.front());
    REQUIRE(r.timestamps());
    CHECK(*r.timestamps() == ts);
    std::vector<std::byte> buf(h.frameBytes());
    for (int i = 0; i < 5; ++i) {
        r.readFrame(i, buf);
        CHECK(buf == pattern(h.frameBytes(), i));
    }
    CHECK_THROWS_AS(r.readFrame(5, buf), std::out_of_range);
}

TEST_CASE("SER writer and reader round-trip 16-bit mono frames") {
    TempDir dir;
    const auto path = dir / "rt16.ser";
    ser::Header h;
    h.width = 10;
    h.height = 6;
    h.bitDepth = 12;   // 12-bit data in 16-bit samples
    REQUIRE(h.frameBytes() == 120);
    {
        ser::Writer w(path, h);
        w.writeFrame(pattern(120, 1), 1);
        w.writeFrame(pattern(120, 2), 2);
    }   // destructor finishes the file
    ser::Reader r(path);
    CHECK(r.frameCount() == 2);
    CHECK(r.header().bitDepth == 12);
    std::vector<std::byte> buf(120);
    r.readFrame(1, buf);
    CHECK(buf == pattern(120, 2));
}

TEST_CASE("SER reader recovers frames from a recording that was cut off") {
    TempDir dir;
    const auto path = dir / "cut.ser";
    const auto h = bayer8(16, 8);   // header frame count stays 0, as after a crash
    {
        std::ofstream f(path, std::ios::binary);
        const auto raw = h.encode();
        f.write(reinterpret_cast<const char*>(raw.data()), ser::kHeaderSize);
        for (int i = 0; i < 3; ++i) {
            const auto fr = pattern(h.frameBytes(), i);
            f.write(reinterpret_cast<const char*>(fr.data()), static_cast<std::streamsize>(fr.size()));
        }
        f.write("partial", 7);   // half-written fourth frame
    }
    ser::Reader r(path);
    CHECK(r.frameCount() == 3);
    CHECK(r.truncated());
    CHECK_FALSE(r.timestamps());
    std::vector<std::byte> buf(h.frameBytes());
    r.readFrame(2, buf);
    CHECK(buf == pattern(h.frameBytes(), 2));
}

TEST_CASE("SER reader accepts LittleEndian = 1 as well as 0") {
    TempDir dir;
    const auto path = dir / "le1.ser";
    auto h = bayer8(4, 4);
    h.frameCount = 1;
    h.littleEndian = 1;
    {
        std::ofstream f(path, std::ios::binary);
        const auto raw = h.encode();
        f.write(reinterpret_cast<const char*>(raw.data()), ser::kHeaderSize);
        const auto fr = pattern(h.frameBytes(), 0);
        f.write(reinterpret_cast<const char*>(fr.data()), static_cast<std::streamsize>(fr.size()));
    }
    ser::Reader r(path);
    CHECK(r.frameCount() == 1);
    CHECK_FALSE(r.truncated());
}

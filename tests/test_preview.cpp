#include "preview/latest_frame.hpp"
#include "preview/render_cpu.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using namespace slopcap;
using namespace slopcap::preview;

namespace {

std::vector<std::byte> filled(std::size_t n, uint8_t v) { return std::vector<std::byte>(n, static_cast<std::byte>(v)); }

}  // namespace

TEST_CASE("LatestFrame gives the consumer only the newest frame, once") {
    LatestFrame lf;
    CHECK(lf.take() == nullptr);   // nothing published yet
    CHECK(lf.wanted());

    FrameInfo fi;
    for (uint64_t i = 1; i <= 3; ++i) {
        fi.sequence = i;
        lf.publish(filled(10, static_cast<uint8_t>(i)), fi);
    }
    CHECK_FALSE(lf.wanted());   // the consumer hasn't asked since
    const auto* f = lf.take();
    REQUIRE(f);
    CHECK(f->info.sequence == 3);
    CHECK(f->info.bytes == 10);
    CHECK(f->pixels[9] == std::byte{3});
    CHECK(lf.take() == nullptr);   // already taken
    CHECK(lf.wanted());
}

TEST_CASE("LatestFrame never hands out a frame the producer is writing") {
    // Producer fills every byte of a frame with its sequence number; the consumer checks every frame is uniform.
    LatestFrame lf;
    constexpr std::size_t kBytes = 1 << 16;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        std::vector<std::byte> buf(kBytes);
        FrameInfo fi;
        for (uint64_t i = 1; i <= 20000; ++i) {
            if (!lf.wanted()) continue;
            std::memset(buf.data(), static_cast<int>(i & 0xff), kBytes);
            fi.sequence = i;
            lf.publish(buf, fi);
        }
        done = true;
    });
    uint64_t got = 0, last = 0;
    bool torn = false;
    while (!done || got == 0) {
        if (const auto* f = lf.take()) {
            const auto v = static_cast<std::byte>(f->info.sequence & 0xff);
            for (std::size_t i = 0; i < kBytes; i += 997) torn |= f->pixels[i] != v;
            torn |= f->pixels[kBytes - 1] != v;
            CHECK(f->info.sequence > last);
            last = f->info.sequence;
            ++got;
        }
    }
    producer.join();
    CHECK_FALSE(torn);
    CHECK(got > 0);
}

TEST_CASE("histogram bins 8-bit samples directly and 16-bit by the high byte") {
    std::vector<std::byte> p8 = {std::byte{0}, std::byte{0}, std::byte{7}, std::byte{255}};
    auto h = histogram(p8, 8);
    CHECK(h[0] == 2);
    CHECK(h[7] == 1);
    CHECK(h[255] == 1);

    std::vector<std::byte> p16 = {std::byte{0xff}, std::byte{0x01}, std::byte{0x00}, std::byte{0x80}};   // 0x01ff, 0x8000
    h = histogram(p16, 16);
    CHECK(h[1] == 1);
    CHECK(h[0x80] == 1);
}

TEST_CASE("autoLevels puts black at the background and white at the bright tail") {
    std::array<uint32_t, 256> h{};
    h[20] = 9000;   // background
    h[21] = 900;
    h[200] = 100;   // planet
    const auto lv = autoLevels(h, 0.5, 0.995);
    CHECK(lv.black == Catch::Approx(20 / 255.0));
    CHECK(lv.white == Catch::Approx(201 / 255.0));
    CHECK(autoLevels({}).white == 1.0);   // empty histogram: identity
}

TEST_CASE("renderRgb demosaics a flat colour field exactly") {
    // RGGB 4x4 with R=200, G=100, B=50 everywhere: every output pixel must be (200,100,50).
    const int w = 4, h = 4;
    std::vector<std::byte> raw(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool ry = (y & 1) == 0, rx = (x & 1) == 0;
            raw[static_cast<std::size_t>(y * w + x)] = static_cast<std::byte>(ry && rx ? 200 : (!ry && !rx ? 50 : 100));
        }
    const auto rgb = renderRgb(raw, w, h, 8, Bayer::RGGB, {});
    for (int i = 0; i < w * h; ++i) {
        INFO("pixel " << i);
        CHECK(rgb[static_cast<std::size_t>(i) * 3 + 0] == 200);
        CHECK(rgb[static_cast<std::size_t>(i) * 3 + 1] == 100);
        CHECK(rgb[static_cast<std::size_t>(i) * 3 + 2] == 50);
    }

    // Same field as BGGR swaps red and blue.
    const auto bgr = renderRgb(raw, w, h, 8, Bayer::BGGR, {});
    CHECK(bgr[0] == 50);
    CHECK(bgr[2] == 200);
}

TEST_CASE("renderRgb stretches and handles 16-bit mono") {
    // 16-bit mono: 0x4000 (0.25) with black 0.25 -> 0; 0x8000 (~0.5) with white 0.5 -> 255.
    std::vector<std::byte> raw = {std::byte{0x00}, std::byte{0x40}, std::byte{0x00}, std::byte{0x80}};
    const auto rgb = renderRgb(raw, 2, 1, 16, Bayer::None, {0.25, 0.5, 1.0});
    CHECK(rgb[0] == 0);
    CHECK(rgb[1] == 0);
    CHECK(rgb[3] == 255);
    CHECK(rgb[5] == 255);

    // gamma 2 lifts the midtones: 0.25 -> 0.5
    std::vector<std::byte> mid = {std::byte{64}};
    CHECK(renderRgb(mid, 1, 1, 8, Bayer::None, {0.0, 1.0, 2.0})[0] == Catch::Approx(128).margin(1));
}

#include "analysis/focus.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace slopcap;
using namespace slopcap::analysis;

namespace {

/// A banded disc ("planet") centred at (cx, cy), radius r, brightness 0..1, box-blurred by `blur` pixels.
std::vector<float> planet(int w, int h, double cx, double cy, double r, double bright, int blur) {
    std::vector<float> img(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double d = std::hypot(x - cx, y - cy);
            const double band = 0.75 + 0.25 * std::sin((y - cy) * 0.6);   // belts give it internal detail
            img[static_cast<std::size_t>(y) * w + x] = static_cast<float>(d < r ? bright * band : 0.02);
        }
    if (blur <= 0) return img;
    std::vector<float> tmp(img.size());
    for (int pass = 0; pass < 2; ++pass) {   // separable box blur, horizontal then vertical
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double s = 0;
                int n = 0;
                for (int k = -blur; k <= blur; ++k) {
                    const int xx = pass == 0 ? x + k : x, yy = pass == 0 ? y : y + k;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                    s += img[static_cast<std::size_t>(yy) * w + xx];
                    ++n;
                }
                tmp[static_cast<std::size_t>(y) * w + x] = static_cast<float>(s / n);
            }
        img.swap(tmp);
    }
    return img;
}

/// RGGB 8-bit mosaic from three full-resolution channel images.
std::vector<std::byte> mosaic(int w, int h, const std::vector<float>& r, const std::vector<float>& g, const std::vector<float>& b) {
    std::vector<std::byte> out(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const float v = (y % 2 == 0) ? (x % 2 == 0 ? r[i] : g[i]) : (x % 2 == 0 ? g[i] : b[i]);
            out[i] = static_cast<std::byte>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255));
        }
    return out;
}

std::vector<std::byte> mono16(int w, int h, const std::vector<float>& img) {
    std::vector<std::byte> out(static_cast<std::size_t>(w) * h * 2);
    for (std::size_t i = 0; i < img.size(); ++i) {
        const auto v = static_cast<unsigned>(std::lround(std::clamp(img[i], 0.0f, 1.0f) * 65535));
        out[2 * i] = static_cast<std::byte>(v & 0xff);
        out[2 * i + 1] = static_cast<std::byte>(v >> 8);
    }
    return out;
}

}  // namespace

TEST_CASE("focus: sharpness falls as the image blurs, and ignores exposure") {
    const int w = 320, h = 240;
    double last = 1e9;
    for (int blur : {0, 1, 2, 4}) {
        const auto s = scoreFrame(mono16(w, h, planet(w, h, 150, 110, 50, 0.8, blur)), w, h, 16, Bayer::None);
        REQUIRE(s.channels == 1);
        INFO("blur " << blur);
        CHECK(s.ch[0].sharpness < last);
        last = s.ch[0].sharpness;
    }
    const auto bright = scoreFrame(mono16(w, h, planet(w, h, 150, 110, 50, 0.8, 1)), w, h, 16, Bayer::None);
    const auto dim = scoreFrame(mono16(w, h, planet(w, h, 150, 110, 50, 0.4, 1)), w, h, 16, Bayer::None);
    CHECK(dim.ch[0].sharpness == Catch::Approx(bright.ch[0].sharpness).epsilon(0.05));
    CHECK(bright.ch[0].cx == Catch::Approx(150).margin(0.5));
    CHECK(bright.ch[0].cy == Catch::Approx(110).margin(0.5));
}

TEST_CASE("focus: each colour channel is scored on its own") {
    // Red out of focus, green and blue sharp: only red's score drops.
    const int w = 400, h = 300;
    const auto sharpR = planet(w, h, 200, 150, 60, 0.7, 0), blurR = planet(w, h, 200, 150, 60, 0.7, 3);
    const auto g = planet(w, h, 200, 150, 60, 0.9, 0), b = planet(w, h, 200, 150, 60, 0.5, 0);
    const auto a = scoreFrame(mosaic(w, h, sharpR, g, b), w, h, 8, Bayer::RGGB);
    const auto c = scoreFrame(mosaic(w, h, blurR, g, b), w, h, 8, Bayer::RGGB);
    REQUIRE(a.channels == 3);
    CHECK(c.ch[0].sharpness < 0.7 * a.ch[0].sharpness);
    CHECK(c.ch[1].sharpness == Catch::Approx(a.ch[1].sharpness).epsilon(0.001));
    CHECK(c.ch[2].sharpness == Catch::Approx(a.ch[2].sharpness).epsilon(0.001));
}

TEST_CASE("focus: centroids show dispersion between channels") {
    const int w = 400, h = 300;
    const auto r = planet(w, h, 200, 146, 50, 0.8, 1), g = planet(w, h, 200, 150, 50, 0.8, 1), b = planet(w, h, 200, 154, 50, 0.8, 1);
    const auto s = scoreFrame(mosaic(w, h, r, g, b), w, h, 8, Bayer::RGGB);
    REQUIRE(s.channels == 3);
    CHECK(s.ch[2].cy - s.ch[0].cy == Catch::Approx(8).margin(0.6));
    CHECK(s.ch[1].cx == Catch::Approx(200).margin(0.6));
}

TEST_CASE("focus: an empty frame has no target") {
    std::vector<std::byte> dark(64 * 48, std::byte{10});
    CHECK(scoreFrame(dark, 64, 48, 8, Bayer::RGGB).channels == 0);
}

TEST_CASE("focus: every metric falls as the image blurs and ignores exposure") {
    const int w = 512, h = 384;
    for (Metric m : {Metric::Gradient, Metric::Laplacian, Metric::Variance, Metric::Fourier}) {
        FocusOptions opt;
        opt.metric = m;
        INFO("metric " << metricName(m));
        double last = 1e18;
        for (int blur : {0, 2, 4, 8}) {
            const auto s = scoreFrame(mono16(w, h, planet(w, h, 250, 190, 70, 0.8, blur)), w, h, 16, Bayer::None, opt);
            REQUIRE(s.channels == 1);
            INFO("blur " << blur);
            CHECK(s.ch[0].sharpness < last);
            last = s.ch[0].sharpness;
        }
        const auto a = scoreFrame(mono16(w, h, planet(w, h, 250, 190, 70, 0.8, 2)), w, h, 16, Bayer::None, opt);
        const auto b = scoreFrame(mono16(w, h, planet(w, h, 250, 190, 70, 0.4, 2)), w, h, 16, Bayer::None, opt);
        CHECK(b.ch[0].sharpness == Catch::Approx(a.ch[0].sharpness).epsilon(0.08));
    }
}

TEST_CASE("focus: clipping is reported per channel") {
    const int w = 256, h = 192;
    const auto r = planet(w, h, 128, 96, 40, 1.5, 0), g = planet(w, h, 128, 96, 40, 0.6, 0), b = planet(w, h, 128, 96, 40, 0.4, 0);
    const auto s = scoreFrame(mosaic(w, h, r, g, b), w, h, 8, Bayer::RGGB);
    REQUIRE(s.channels == 3);
    CHECK(s.ch[0].clipped > 0.05);
    CHECK(s.ch[1].clipped == 0);
}

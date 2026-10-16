#include "analysis/collimation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace slopcap;
using namespace slopcap::analysis;

namespace {

/// 8-bit RGGB frame with a defocused star: ring of outer radius R centred at (cx, cy), shadow radius r at (sx, sy).
std::vector<std::byte> donut(int w, int h, double cx, double cy, double R, double sx, double sy, double r) {
    std::vector<std::byte> f(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // supersample 4x4 for soft edges
            double v = 0;
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) {
                    const double px = x + (i + 0.5) / 4, py = y + (j + 0.5) / 4;
                    v += (std::hypot(px - cx, py - cy) < R && std::hypot(px - sx, py - sy) > r) ? 1.0 : 0.0;
                }
            f[static_cast<std::size_t>(y) * w + x] = static_cast<std::byte>(std::lround(12 + 180 * v / 16));
        }
    return f;
}

}  // namespace

TEST_CASE("collimation: finds the shadow's offset in a defocused star") {
    const int w = 400, h = 300;
    const auto f = donut(w, h, 210, 140, 70, 216, 136, 28);
    const auto d = analyzeDonut(f, w, h, 8, Bayer::RGGB);
    INFO(d.problem);
    REQUIRE(d.found);
    CHECK(d.outerR == Catch::Approx(70).margin(1.5));
    CHECK(d.innerR == Catch::Approx(28).margin(1.5));
    CHECK(d.offsetX == Catch::Approx(6).margin(0.8));
    CHECK(d.offsetY == Catch::Approx(-4).margin(0.8));
    CHECK(d.offsetFraction == Catch::Approx(std::hypot(6, 4) / 70).margin(0.012));
    CHECK(d.fromCentreX == Catch::Approx(10).margin(1.5));
}

TEST_CASE("collimation: a centred shadow reads near zero, and problems are named") {
    const int w = 300, h = 300;
    const auto good = analyzeDonut(donut(w, h, 150, 150, 60, 150, 150, 24), w, h, 8, Bayer::RGGB);
    REQUIRE(good.found);
    CHECK(good.offsetFraction < 0.01);

    const auto solid = analyzeDonut(donut(w, h, 150, 150, 20, 150, 150, 0), w, h, 8, Bayer::RGGB);
    CHECK_FALSE(solid.found);
    CHECK(solid.problem.find("defocus more") != std::string::npos);

    std::vector<std::byte> dark(static_cast<std::size_t>(w) * h, std::byte{12});
    CHECK(analyzeDonut(dark, w, h, 8, Bayer::RGGB).problem == "no star");
}

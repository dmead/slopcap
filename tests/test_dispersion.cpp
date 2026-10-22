#include "analysis/dispersion.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace slopcap;
using namespace slopcap::analysis;
using Catch::Approx;

namespace {

/// An RGGB frame of a planet of radius r at (cx, cy) (pixel coordinates) whose red image sits (dx, dy) from its blue
/// one, green between, its limb softened over 3 px as seeing does. 8-bit, or 16-bit little-endian.
std::vector<std::byte> dispersed(int w, int h, double cx, double cy, double r, double dx, double dy, int bits = 8) {
    const int bps = bits > 8 ? 2 : 1;
    std::vector<std::byte> f(static_cast<std::size_t>(w) * h * bps);
    const double sx[3] = {dx / 2, 0, -dx / 2}, sy[3] = {dy / 2, 0, -dy / 2};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int site = (y & 1) * 2 + (x & 1);
            const int c = site == 0 ? 0 : site == 3 ? 2 : 1;
            const double edge = std::clamp((r - std::hypot(x - cx - sx[c], y - cy - sy[c])) / 3 + 0.5, 0.0, 1.0);
            const double level = 0.05 + 0.7 * edge;
            const auto at = (static_cast<std::size_t>(y) * w + x) * bps;
            if (bps == 2) {
                const auto q = static_cast<uint16_t>(level * 60000);
                f[at] = static_cast<std::byte>(q & 0xff), f[at + 1] = static_cast<std::byte>(q >> 8);
            } else {
                f[at] = static_cast<std::byte>(std::lround(level * 250));
            }
        }
    return f;
}

}  // namespace

TEST_CASE("dispersion: red minus blue, to a fraction of a pixel") {
    for (int bits : {8, 16}) {
        for (auto [dx, dy] : {std::pair{0.0, 4.0}, {3.0, -2.0}, {0.0, 0.0}, {-1.5, 0.5}}) {
            const auto f = dispersed(320, 240, 161, 118, 40, dx, dy, bits);
            const auto d = measureDispersion(f, 320, 240, bits, Bayer::RGGB);
            INFO(bits << "-bit, " << dx << ", " << dy);
            REQUIRE(d.found);
            CHECK(d.dx() == Approx(dx).margin(0.1));
            CHECK(d.dy() == Approx(dy).margin(0.1));
            CHECK(d.magnitude() == Approx(std::hypot(dx, dy)).margin(0.1));
            CHECK(d.gx == Approx(161).margin(0.1));
            CHECK(d.gy == Approx(118).margin(0.1));
            CHECK_FALSE(d.atEdge);
        }
    }
}

TEST_CASE("dispersion: what it can't measure") {
    const auto f = dispersed(320, 240, 160, 120, 40, 0, 3);
    auto d = measureDispersion(f, 320, 240, 8, Bayer::None);
    CHECK_FALSE(d.found);
    CHECK(d.problem.find("colour camera") != std::string::npos);
    const std::vector<std::byte> dark(320 * 240, std::byte{10});
    d = measureDispersion(dark, 320, 240, 8, Bayer::RGGB);
    CHECK_FALSE(d.found);
    CHECK(d.problem == "No planet in the frame.");
    // a planet cut by the frame's edge is measured, and flagged
    const auto edge = dispersed(320, 240, 20, 120, 40, 0, 3);
    d = measureDispersion(edge, 320, 240, 8, Bayer::RGGB);
    REQUIRE(d.found);
    CHECK(d.atEdge);
}

TEST_CASE("dispersion: what the atmosphere does") {
    // the standard refraction at 45° altitude is about a minute of arc
    CHECK(refractionArcsec(0.55, 45) == Approx(58).margin(3));
    CHECK(refractionArcsec(0.55, 0) == 0);
    CHECK(refractionArcsec(0.55, -5) == 0);
    // blue is lifted more than red: ~1.3" at 30°, nothing at the zenith, more the lower it gets
    CHECK(dispersionArcsec(30) == Approx(1.28).margin(0.1));
    CHECK(dispersionArcsec(90) == Approx(0).margin(1e-9));
    CHECK(dispersionArcsec(95) == Approx(0).margin(1e-9));
    CHECK(dispersionArcsec(20) > dispersionArcsec(30));
    CHECK(dispersionArcsec(10) > 3);
    // and stays finite at the horizon: about 25″ (tan z alone would go to infinity)
    CHECK(dispersionArcsec(0.2) == Approx(25).margin(4));
    CHECK(dispersionArcsec(5) < dispersionArcsec(0.2));
    // thinner, warmer air bends less
    const Atmosphere mountain{780, 10}, summer{1013.25, 30};
    CHECK(dispersionArcsec(30, mountain) == Approx(dispersionArcsec(30) * 780 / 1013.25).epsilon(0.05));
    CHECK(dispersionArcsec(30, summer) < dispersionArcsec(30));
    // between other wavelengths: R to IR is smaller than B to R
    CHECK(dispersionArcsec(30, {}, 0.62, 0.85) < dispersionArcsec(30));
}

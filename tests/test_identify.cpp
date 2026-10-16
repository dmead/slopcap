// Target identification: ephemerides against libnova (values computed with libnova 0.16 and precessed to date), the
// image measurements on synthetic targets, and the decision rules. The thresholds themselves were calibrated on real
// recordings (ASI585MC Jupiter and Saturn, mono lunar captures), quoted in analysis/identify.cpp.
#include "analysis/ephemeris.hpp"
#include "analysis/identify.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

using namespace slopcap;
using namespace slopcap::analysis;

namespace {

constexpr double kPi = 3.14159265358979323846;
const double kOct2026 = 2461321.5 + 5.5 / 24;   // 2026-10-08 05:30 UTC (Saturn rings 7.4° open)

/// A raw RGGB 8-bit frame: `rgb(x, y)` gives the scene's colour (0..1 per channel) at each pixel, over a dark sky.
std::vector<std::byte> bayerFrame(int w, int h, const std::function<std::array<double, 3>(double, double)>& rgb) {
    std::vector<std::byte> px(static_cast<std::size_t>(w * h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const auto c = rgb(x + 0.5, y + 0.5);
            const int site = (y & 1) * 2 + (x & 1);   // RGGB
            const double v = 0.03 + (site == 0 ? c[0] : site == 3 ? c[2] : c[1]);
            px[static_cast<std::size_t>(y * w + x)] = static_cast<std::byte>(std::clamp(static_cast<int>(std::lround(v * 255)), 0, 255));
        }
    return px;
}

/// Inside an ellipse with semi-axes (a, b) centred at (cx, cy), long axis at `deg`.
bool inEllipse(double x, double y, double cx, double cy, double a, double b, double deg) {
    const double t = deg * kPi / 180, dx = x - cx, dy = y - cy;
    const double u = dx * std::cos(t) + dy * std::sin(t), v = -dx * std::sin(t) + dy * std::cos(t);
    return (u * u) / (a * a) + (v * v) / (b * b) <= 1;
}

IdentifyInput imageOnly(const TargetFeatures& f) {
    IdentifyInput in;
    in.image = f;
    in.jdUtc = kOct2026;
    return in;
}

}  // namespace

TEST_CASE("ephemeris: planets and the Moon where libnova puts them") {
    struct Ref {
        Body b;
        double ra, dec, dist;
    };
    const Ref refs[] = {{Body::Mars, 8.5678, 19.9139, 1.615},  {Body::Jupiter, 9.5629, 15.1276, 5.831}, {Body::Saturn, 0.7452, 1.8554, 8.436},
                        {Body::Uranus, 4.2325, 21.0455, 18.783}, {Body::Neptune, 0.2002, -0.2466, 28.901}};
    for (const auto& r : refs) {
        const auto p = bodyPosition(r.b, kOct2026);
        INFO(bodyName(r.b));
        CHECK(separationDeg(p.ra, p.dec, r.ra, r.dec) < 0.25);   // the elements are good to ~10' here
        CHECK(p.distanceAu == Catch::Approx(r.dist).epsilon(0.005));
    }
    CHECK(bodyPosition(Body::Jupiter, kOct2026).diameterArcsec == Catch::Approx(33.8).margin(0.2));
    CHECK(bodyPosition(Body::Uranus, kOct2026).diameterArcsec == Catch::Approx(3.75).margin(0.05));

    const double fullMoon = 2461043.5 + 7.2 / 24;   // 2026-01-03 07:12 UTC
    const auto m = bodyPosition(Body::Moon, fullMoon);
    CHECK(separationDeg(m.ra, m.dec, 6.8431, 27.3050) < 0.4);
    CHECK(m.distanceAu * 149597870.7 == Catch::Approx(362014).epsilon(0.005));
    CHECK(m.diameterArcsec == Catch::Approx(1976).margin(15));
}

TEST_CASE("ephemeris: Saturn's ring tilt, separations and altitudes") {
    CHECK(std::abs(bodyPosition(Body::Saturn, 2460757.5).ringTiltDeg) < 0.5);    // ring-plane crossing, 2025-03-23
    CHECK(std::abs(bodyPosition(Body::Saturn, 2458042.5).ringTiltDeg) == Catch::Approx(27).margin(0.5));   // widest, Oct 2017
    CHECK(std::abs(bodyPosition(Body::Saturn, kOct2026).ringTiltDeg) == Catch::Approx(7.4).margin(0.3));

    CHECK(separationDeg(0, 0, 0, 10) == Catch::Approx(10));
    CHECK(separationDeg(1, 0, 23, 0) == Catch::Approx(30));      // across 0h
    CHECK(separationDeg(5, 89.9, 17, 89.9) == Catch::Approx(0.2).margin(1e-6));
    CHECK(altitudeDeg(3.0, 90.0, kOct2026, 51.5, 0) == Catch::Approx(51.5));   // the pole stands at the latitude
    // An object on the meridian: altitude = 90 - |latitude - dec|. Local sidereal time at 0° longitude = its RA.
    const double t = (kOct2026 - 2451545.0) / 36525;
    const double lst = std::fmod(280.46061837 + 360.98564736629 * (kOct2026 - 2451545.0) + 0.000387933 * t * t, 360.0) / 15;
    CHECK(altitudeDeg(lst, 20, kOct2026, 51.5, 0) == Catch::Approx(58.5).margin(0.01));
}

TEST_CASE("measureTarget: Saturn's shape is elongated and symmetric at any rotation") {
    for (double rot : {0.0, 35.0, 90.0}) {
        // globe 22 px radius, rings 2.27x wider, open ~7°, at the globe's brightness
        const auto px = bayerFrame(320, 240, [&](double x, double y) -> std::array<double, 3> {
            const bool globe = inEllipse(x, y, 160, 120, 22, 20, rot);
            const bool ring = inEllipse(x, y, 160, 120, 50, 7, rot) && !inEllipse(x, y, 160, 120, 33, 4.5, rot);
            if (globe || ring) return {0.55, 0.6, 0.27};
            return {0, 0, 0};
        });
        const auto f = measureTarget(px, 320, 240, 8, Bayer::RGGB);
        INFO("rotation " << rot);
        REQUIRE(f.found);
        CHECK(f.elongation >= 1.6);
        CHECK(f.symmetry >= 0.86);
        CHECK(!f.touchesEdge);
        CHECK(colourIndex(f).value() == Catch::Approx(std::log(0.55 / 0.27)).margin(0.05));
        const auto id = identifyTarget(imageOnly(f));
        CHECK(id.body == Body::Saturn);
        CHECK(id.summary.find("rings") != std::string::npos);
    }
}

TEST_CASE("measureTarget: a crescent is elongated but lopsided, so the Moon, not Saturn") {
    const auto px = bayerFrame(400, 400, [](double x, double y) -> std::array<double, 3> {
        const bool lit = std::hypot(x - 200, y - 200) <= 150 && std::hypot(x - 270, y - 200) > 150;
        return lit ? std::array<double, 3>{0.5, 0.5, 0.35} : std::array<double, 3>{0, 0, 0};
    });
    const auto f = measureTarget(px, 400, 400, 8, Bayer::RGGB);
    REQUIRE(f.found);
    CHECK(f.elongation >= 1.6);
    CHECK(f.symmetry < 0.8);
    const auto id = identifyTarget(imageOnly(f));
    CHECK(id.body == Body::Moon);
}

TEST_CASE("identify: a round neutral disc needs the image scale or the mount") {
    // Jupiter-coloured, 340 px across: round, so not Saturn (rings 7° open); neutral, so not Mars, Uranus or Neptune.
    const auto px = bayerFrame(600, 500, [](double x, double y) -> std::array<double, 3> {
        return std::hypot(x - 300, y - 250) <= 170 ? std::array<double, 3>{0.6, 0.45, 0.25} : std::array<double, 3>{0, 0, 0};
    });
    const auto f = measureTarget(px, 600, 500, 8, Bayer::RGGB);
    REQUIRE(f.found);
    CHECK(f.elongation < 1.3);
    CHECK(f.diameterPx == Catch::Approx(340).epsilon(0.03));

    auto in = imageOnly(f);
    auto id = identifyTarget(in);
    CHECK(!id.body);
    CHECK(id.candidates == std::vector<Body>{Body::Moon, Body::Jupiter});
    CHECK(candidatesText(id.candidates) == "Moon or Jupiter");

    in.arcsecPerPixel = 0.1;   // 34″: Jupiter's size today
    id = identifyTarget(in);
    CHECK(id.body == Body::Jupiter);

    in.arcsecPerPixel = 5.8;   // ~1970″: the Moon
    id = identifyTarget(in);
    CHECK(id.body == Body::Moon);

    in.arcsecPerPixel.reset();   // or the mount, pointed at Jupiter's position
    const auto j = bodyPosition(Body::Jupiter, kOct2026);
    in.pointing = std::array<double, 2>{j.ra + 0.05, j.dec - 1.0};   // alignment a degree or so off
    id = identifyTarget(in);
    CHECK(id.body == Body::Jupiter);
    CHECK(id.summary.find("from mount") != std::string::npos);
}

TEST_CASE("identify: colour picks out Mars and the ice giants; the mount tells Uranus from Neptune") {
    auto disc = [](double r, std::array<double, 3> c) {
        return bayerFrame(200, 200, [=](double x, double y) -> std::array<double, 3> {
            return std::hypot(x - 100, y - 100) <= r ? c : std::array<double, 3>{0, 0, 0};
        });
    };
    const auto mars = measureTarget(disc(40, {0.7, 0.3, 0.12}), 200, 200, 8, Bayer::RGGB);   // ln R/B = 1.76
    CHECK(identifyTarget(imageOnly(mars)).body == Body::Mars);

    const auto blue = measureTarget(disc(18, {0.2, 0.35, 0.45}), 200, 200, 8, Bayer::RGGB);   // ln R/B = -0.81
    auto in = imageOnly(blue);
    auto id = identifyTarget(in);
    CHECK(candidatesText(id.candidates) == "Uranus or Neptune");
    const auto u = bodyPosition(Body::Uranus, kOct2026);
    in.pointing = std::array<double, 2>{u.ra, u.dec};
    CHECK(identifyTarget(in).body == Body::Uranus);
    const auto n = bodyPosition(Body::Neptune, kOct2026);
    in.pointing = std::array<double, 2>{n.ra, n.dec};
    CHECK(identifyTarget(in).body == Body::Neptune);
}

TEST_CASE("identify: conflicting evidence is ignored and reported, not obeyed") {
    // The mount points at Saturn (Neptune is 8° away this season), but the image is a small blue disc.
    const auto blue = measureTarget(bayerFrame(200, 200, [](double x, double y) -> std::array<double, 3> {
        return std::hypot(x - 100, y - 100) <= 18 ? std::array<double, 3>{0.2, 0.35, 0.45} : std::array<double, 3>{0, 0, 0};
    }), 200, 200, 8, Bayer::RGGB);
    auto in = imageOnly(blue);
    const auto s = bodyPosition(Body::Saturn, kOct2026);
    in.pointing = std::array<double, 2>{s.ra, s.dec};
    const auto id = identifyTarget(in);
    CHECK(id.body == Body::Saturn);   // pointing came first and left only Saturn...
    const bool reported = std::any_of(id.notes.begin(), id.notes.end(), [](const std::string& n) { return n.find("ignored") != std::string::npos; });
    CHECK(reported);                  // ...and the image's disagreement is in the notes

    // Far from everything: the pointing isn't used at all.
    in.pointing = std::array<double, 2>{18.0, -60.0};
    const auto far = identifyTarget(in);
    CHECK(candidatesText(far.candidates) == "Uranus or Neptune");
}

TEST_CASE("identify: what's below the horizon can't be the target") {
    const auto px = bayerFrame(200, 200, [](double x, double y) -> std::array<double, 3> {
        return std::hypot(x - 100, y - 100) <= 18 ? std::array<double, 3>{0.2, 0.35, 0.45} : std::array<double, 3>{0, 0, 0};
    });
    auto in = imageOnly(measureTarget(px, 200, 200, 8, Bayer::RGGB));
    // Find a time when one of Uranus and Neptune is well up and the other well down, at Greenwich.
    for (double hour = 0; hour < 24; hour += 0.5) {
        in.jdUtc = kOct2026 + hour / 24;
        in.site = Site{51.5, 0};
        const auto u = bodyPosition(Body::Uranus, in.jdUtc), n = bodyPosition(Body::Neptune, in.jdUtc);
        const double au = altitudeDeg(u.ra, u.dec, in.jdUtc, 51.5, 0), an = altitudeDeg(n.ra, n.dec, in.jdUtc, 51.5, 0);
        if (!((au > 5 && an < -5) || (an > 5 && au < -5))) continue;
        const auto id = identifyTarget(in);
        CHECK(id.body == (au > 0 ? Body::Uranus : Body::Neptune));
        return;
    }
    FAIL("no hour with only one of Uranus and Neptune up");
}

TEST_CASE("identify: a disc too many pixels across for a small planet at any image scale") {
    // A mono 1100 px disc (like a whole full Moon): no colour, no scale, no mount. At the finest amateur image scale
    // Mars, Uranus and Neptune would still be a few hundred pixels at most; the Moon and Jupiter could be this big.
    const int w = 1400, h = 1300;
    std::vector<std::byte> mono(static_cast<std::size_t>(w * h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) mono[static_cast<std::size_t>(y * w + x)] = std::byte(std::hypot(x - 700, y - 650) <= 550 ? 180 : 8);
    const auto f = measureTarget(mono, w, h, 8, Bayer::None);
    REQUIRE(f.found);
    const auto id = identifyTarget(imageOnly(f));
    CHECK(candidatesText(id.candidates) == "Moon or Jupiter");

    // A small disc keeps them all (Saturn aside: round, with its rings open).
    std::vector<std::byte> small(200 * 200);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x) small[static_cast<std::size_t>(y * 200 + x)] = std::byte(std::hypot(x - 100, y - 100) <= 30 ? 180 : 8);
    CHECK(identifyTarget(imageOnly(measureTarget(small, 200, 200, 8, Bayer::None))).candidates.size() == 5);
}

TEST_CASE("measureTarget: nothing there, and mono frames") {
    std::vector<std::byte> flat(200 * 200, std::byte{20});
    CHECK(!measureTarget(flat, 200, 200, 8, Bayer::None).found);
    CHECK(!measureTarget(flat, 200, 400, 8, Bayer::None).found);   // buffer shorter than the frame

    std::vector<std::byte> mono(200 * 200);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x) mono[static_cast<std::size_t>(y * 200 + x)] = std::byte(std::hypot(x - 100, y - 100) <= 50 ? 200 : 10);
    const auto f = measureTarget(mono, 200, 200, 8, Bayer::None);
    REQUIRE(f.found);
    CHECK(f.channels == 1);
    CHECK(!colourIndex(f));
    CHECK(f.diameterPx == Catch::Approx(100).epsilon(0.04));
    CHECK(f.cx == Catch::Approx(100).margin(1.5));
}

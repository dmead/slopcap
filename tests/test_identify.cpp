// Target identification: ephemerides against libnova (values computed with libnova 0.16, precessed to date), the
// image measurements on drawn targets, and the decision rules. The thresholds were calibrated on real recordings
// (ASI585MC Jupiter and Saturn in 2022 and 2026, lunar captures), quoted in analysis/identify.cpp. No mount: the
// identifier works from the image and the clock (and the site, when it's set).
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
const double kOct2026 = 2461321.5 + 5.5 / 24;       // 2026-10-08 05:30 UTC: Saturn's rings 7.4° open, the Moon 6.7% lit
const double kFullMoon2026 = 2461043.5 + 7.2 / 24;  // 2026-01-03 07:12 UTC: the Moon 99.8% lit
const double kGibbous2022 = 2459808.5 + 8.9 / 24;   // 2022-08-17 08:54 UTC: the Moon 68% lit
const double kFullMoon2022 = 2459803.5 + 1.6 / 24;  // 2022-08-12 01:36 UTC: full Moon, Saturn's rings 14° open

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

/// A sphere of radius r at (cx, cy), lit from +x so that `lit` of its disc is lit (terminator: a half-ellipse).
bool litSphere(double x, double y, double cx, double cy, double r, double lit) {
    const double dx = x - cx, dy = y - cy, cosPhase = 2 * lit - 1;
    return dx * dx + dy * dy <= r * r && dx > -cosPhase * std::sqrt(std::max(0.0, r * r - dy * dy));
}

/// A round disc of colour c (and limb darkening: brightness × (1 - k + k·cos θ), θ from the centre to the limb).
std::vector<std::byte> disc(int w, int h, double r, std::array<double, 3> c, double limbDarkening = 0) {
    return bayerFrame(w, h, [=](double x, double y) -> std::array<double, 3> {
        const double d = std::hypot(x - w / 2.0, y - h / 2.0);
        if (d > r) return {0, 0, 0};
        const double mu = std::sqrt(1 - (d / r) * (d / r)), k = 1 - limbDarkening + limbDarkening * mu;
        return {c[0] * k, c[1] * k, c[2] * k};
    });
}

IdentifyInput at(double jd, const TargetFeatures& f) {
    IdentifyInput in;
    in.image = f;
    in.jdUtc = jd;
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

    const auto m = bodyPosition(Body::Moon, kFullMoon2026);
    CHECK(separationDeg(m.ra, m.dec, 6.8431, 27.3050) < 0.4);
    CHECK(m.distanceAu * 149597870.7 == Catch::Approx(362014).epsilon(0.005));
    CHECK(m.diameterArcsec == Catch::Approx(1976).margin(15));
}

TEST_CASE("ephemeris: the Moon's phase and Mars's, as libnova has them") {
    CHECK(bodyPosition(Body::Moon, kFullMoon2026).illuminated == Catch::Approx(0.998).margin(0.005));
    CHECK(bodyPosition(Body::Moon, kOct2026).illuminated == Catch::Approx(0.067).margin(0.005));
    CHECK(bodyPosition(Body::Moon, kGibbous2022).illuminated == Catch::Approx(0.680).margin(0.005));
    const double mars = bodyPosition(Body::Mars, kOct2026).illuminated;
    CHECK(mars > 0.84);   // Mars never shows less than ~84% lit
    CHECK(mars < 1.0);
    CHECK(bodyPosition(Body::Neptune, kOct2026).illuminated > 0.9999);
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
        CHECK(f.clipped == 0);
        CHECK(colourIndex(f).value() == Catch::Approx(std::log(0.55 / 0.27)).margin(0.05));
        const auto id = identifyTarget(at(kOct2026, f));
        CHECK(id.body == Body::Saturn);
        CHECK(id.summary.find("rings") != std::string::npos);
    }
}

TEST_CASE("identify: the Moon at tonight's phase, whether a crescent or gibbous") {
    for (double jd : {kOct2026, kGibbous2022, kFullMoon2026}) {
        const double lit = bodyPosition(Body::Moon, jd).illuminated;
        if (lit > 0.97) continue;   // a full Moon is a round disc: the next test
        const auto px = bayerFrame(400, 400, [=](double x, double y) -> std::array<double, 3> {
            return litSphere(x, y, 200, 200, 150, lit) ? std::array<double, 3>{0.5, 0.5, 0.35} : std::array<double, 3>{0, 0, 0};
        });
        const auto f = measureTarget(px, 400, 400, 8, Bayer::RGGB);
        INFO("Moon " << lit * 100 << "% lit");
        REQUIRE(f.found);
        const auto id = identifyTarget(at(jd, f));
        CHECK(id.body == Body::Moon);
    }
}

TEST_CASE("identify: a round neutral disc, by the Moon's phase, the limb and the image scale") {
    // Jupiter-coloured, 340 px across: round, so not Saturn (rings open); neutral, so not Mars, Uranus or Neptune.
    const auto flat = measureTarget(disc(600, 500, 170, {0.6, 0.45, 0.25}), 600, 500, 8, Bayer::RGGB);
    REQUIRE(flat.found);
    CHECK(flat.elongation < 1.3);
    CHECK(flat.symmetry > 0.97);
    CHECK(flat.diameterPx == Catch::Approx(340).epsilon(0.03));

    // A thin crescent Moon tonight: a whole disc isn't the Moon. Mars and Jupiter look alike (round, limb-darkened,
    // and the same raw colour on the IMX585), so without the image scale it's one of those two.
    auto id = identifyTarget(at(kOct2026, flat));
    CHECK(candidatesText(id.candidates) == "Mars or Jupiter");

    // Under a full Moon it could be the Moon too...
    id = identifyTarget(at(kFullMoon2022, flat));
    CHECK(!id.body);
    CHECK(candidatesText(id.candidates) == "Moon, Mars or Jupiter");

    // ...unless the limb is darkened, as Jupiter's and Mars's are (0.51-0.76 measured on unclipped captures)...
    const auto darkLimb = measureTarget(disc(600, 500, 170, {0.6, 0.45, 0.25}, 0.9), 600, 500, 8, Bayer::RGGB);
    CHECK(darkLimb.limbRatio < 0.75);
    CHECK(candidatesText(identifyTarget(at(kFullMoon2022, darkLimb)).candidates) == "Mars or Jupiter");

    // ...and the image scale says which, by size.
    auto in = at(kFullMoon2022, flat);
    in.arcsecPerPixel = bodyPosition(Body::Jupiter, kFullMoon2022).diameterArcsec / 340;
    CHECK(identifyTarget(in).body == Body::Jupiter);
    in.arcsecPerPixel = bodyPosition(Body::Mars, kFullMoon2022).diameterArcsec / 340;
    CHECK(identifyTarget(in).body == Body::Mars);
    in.arcsecPerPixel = bodyPosition(Body::Moon, kFullMoon2022).diameterArcsec / 340;
    CHECK(identifyTarget(in).body == Body::Moon);

    // With Saturn's rings nearly edge-on (early 2026), a round disc could also be Saturn's globe.
    REQUIRE(std::abs(bodyPosition(Body::Saturn, kFullMoon2026).ringTiltDeg) < 3);
    CHECK(candidatesText(identifyTarget(at(kFullMoon2026, flat)).candidates) == "Moon, Mars, Jupiter or Saturn");
}

TEST_CASE("identify: clipping silences colour and limb") {
    // A Jupiter overexposed until its channels clip: limb flattened, colour skewed. Neither may decide anything.
    // Drawn red-heavy (ln R/B 1.4 before clipping, near Mars's): clipped, its colour must not make it Mars.
    const auto f = measureTarget(disc(600, 500, 170, {1.6, 1.1, 0.4}, 0.9), 600, 500, 8, Bayer::RGGB);
    REQUIRE(f.found);
    CHECK(f.clipped > 0.5);
    const auto id = identifyTarget(at(kFullMoon2022, f));
    CHECK(!id.body);
    CHECK(std::find(id.candidates.begin(), id.candidates.end(), Body::Moon) != id.candidates.end());   // limb not used
    const bool noted = std::any_of(id.notes.begin(), id.notes.end(), [](const std::string& n) { return n.find("clipped") != std::string::npos; });
    CHECK(noted);
}

TEST_CASE("identify: colour picks out the ice giants, never Mars; the site tells Uranus from Neptune") {
    // However red, colour doesn't make it Mars (real Mars, raw: ln R/B 0.71-1.45, like Jupiter and Saturn).
    const auto red = measureTarget(disc(200, 200, 40, {0.7, 0.3, 0.12}), 200, 200, 8, Bayer::RGGB);   // ln R/B = 1.76
    CHECK(candidatesText(identifyTarget(at(kOct2026, red)).candidates) == "Mars or Jupiter");

    const auto blue = measureTarget(disc(200, 200, 18, {0.2, 0.35, 0.45}), 200, 200, 8, Bayer::RGGB);   // ln R/B = -0.81
    auto in = at(kOct2026, blue);
    CHECK(candidatesText(identifyTarget(in).candidates) == "Uranus or Neptune");
    // With the site: at some hour one of them is well up and the other well down, at Greenwich.
    for (double hour = 0; hour < 24; hour += 0.5) {
        in.jdUtc = kOct2026 + hour / 24;
        in.site = Site{51.5, 0};
        const auto u = bodyPosition(Body::Uranus, in.jdUtc), n = bodyPosition(Body::Neptune, in.jdUtc);
        const double au = altitudeDeg(u.ra, u.dec, in.jdUtc, 51.5, 0), an = altitudeDeg(n.ra, n.dec, in.jdUtc, 51.5, 0);
        if (!((au > 5 && an < -5) || (an > 5 && au < -5))) continue;
        CHECK(identifyTarget(in).body == (au > 0 ? Body::Uranus : Body::Neptune));
        return;
    }
    FAIL("no hour with only one of Uranus and Neptune up");
}

TEST_CASE("identify: conflicting evidence is ignored and reported, not obeyed") {
    // A crescent shape on a night of full Moon: nothing tonight looks like that. The shape is set aside.
    const auto px = bayerFrame(400, 400, [](double x, double y) -> std::array<double, 3> {
        return litSphere(x, y, 200, 200, 150, 0.2) ? std::array<double, 3>{0.5, 0.5, 0.35} : std::array<double, 3>{0, 0, 0};
    });
    const auto id = identifyTarget(at(kFullMoon2026, measureTarget(px, 400, 400, 8, Bayer::RGGB)));
    const bool ignored = std::any_of(id.notes.begin(), id.notes.end(), [](const std::string& n) { return n.find("ignored") != std::string::npos; });
    CHECK(ignored);
    CHECK(!id.candidates.empty());
}

TEST_CASE("identify: a disc too many pixels across for a small planet at any image scale") {
    // A mono 1200 px disc (like a whole full Moon): no colour, no scale. At the finest amateur image scale Mars,
    // Uranus and Neptune would still be a few hundred pixels at most; the Moon and Jupiter could be this big.
    const int w = 1400, h = 1300;
    std::vector<std::byte> mono(static_cast<std::size_t>(w * h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) mono[static_cast<std::size_t>(y * w + x)] = std::byte(std::hypot(x - 700, y - 650) <= 600 ? 180 : 8);
    const auto f = measureTarget(mono, w, h, 8, Bayer::None);
    REQUIRE(f.found);
    CHECK(candidatesText(identifyTarget(at(kFullMoon2022, f)).candidates) == "Moon or Jupiter");

    // A small disc keeps them all but Saturn (round, with its rings open) and, tonight, the thin crescent Moon.
    std::vector<std::byte> small(200 * 200);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x) small[static_cast<std::size_t>(y * 200 + x)] = std::byte(std::hypot(x - 100, y - 100) <= 30 ? 180 : 8);
    CHECK(candidatesText(identifyTarget(at(kOct2026, measureTarget(small, 200, 200, 8, Bayer::None))).candidates) ==
          "Mars, Jupiter, Uranus or Neptune");
}

TEST_CASE("identify: no Moon near new Moon") {
    // The new Moon after 2026-10-08: whatever the image, it isn't the Moon.
    double newMoon = kOct2026;
    for (double jd = kOct2026; jd < kOct2026 + 5; jd += 1.0 / 24)
        if (bodyPosition(Body::Moon, jd).illuminated < bodyPosition(Body::Moon, newMoon).illuminated) newMoon = jd;
    REQUIRE(bodyPosition(Body::Moon, newMoon).illuminated < 0.01);
    const auto f = measureTarget(disc(600, 500, 170, {0.6, 0.45, 0.25}), 600, 500, 8, Bayer::RGGB);
    const auto id = identifyTarget(at(newMoon, f));
    CHECK(std::find(id.candidates.begin(), id.candidates.end(), Body::Moon) == id.candidates.end());
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

#include "analysis/sweep.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>

using namespace slopcap::analysis;

namespace {

/// A focus curve with peak at `best`, plus seeing noise (frame-to-frame).
double score(double pos, double best, double width, std::mt19937& rng) {
    std::normal_distribution<double> seeing(1.0, 0.08);
    return std::max(0.0, seeing(rng)) / (1 + std::pow((pos - best) / width, 2));
}

}  // namespace

TEST_CASE("sweep finds each channel's best focus and the spread") {
    std::mt19937 rng(7);
    SweepAnalysis sw;
    // R best at 64950, G at 64700, B at 64420 (like the Oct 8 recordings suggest)
    for (int pos = 63900; pos <= 65700; pos += 100)
        for (int f = 0; f < 40; ++f) {
            FocusScores s;
            s.channels = 3;
            s.ch[0].sharpness = score(pos, 64950, 400, rng);
            s.ch[1].sharpness = score(pos, 64700, 400, rng);
            s.ch[2].sharpness = score(pos, 64420, 400, rng);
            sw.add(pos, s, Metric::Gradient);
        }
    const auto r = sw.result(Metric::Gradient);
    REQUIRE(r.channels == 3);
    REQUIRE(r.ch[0].best);
    REQUIRE(r.ch[1].best);
    REQUIRE(r.ch[2].best);
    CHECK(*r.ch[0].best == Catch::Approx(64950).margin(40));
    CHECK(*r.ch[1].best == Catch::Approx(64700).margin(40));
    CHECK(*r.ch[2].best == Catch::Approx(64420).margin(40));
    CHECK(r.ch[1].bestError > 0);
    CHECK(r.ch[1].bestError < 60);
    REQUIRE(r.spread);
    CHECK(*r.spread == Catch::Approx(530).margin(60));
    CHECK_FALSE(r.ch[1].atEdge);

    CHECK(*r.position(FocusTarget::Green) == *r.ch[1].best);
    const double lum = *r.position(FocusTarget::Luminance);
    CHECK(lum == Catch::Approx(0.30 * *r.ch[0].best + 0.59 * *r.ch[1].best + 0.11 * *r.ch[2].best));
    // balanced sits between the extremes, near the middle for symmetric curves
    const double bal = *r.position(FocusTarget::Balanced);
    CHECK(bal > 64500);
    CHECK(bal < 64900);
}

TEST_CASE("sweep flags a peak at the end of the range") {
    std::mt19937 rng(3);
    SweepAnalysis sw;
    for (int pos = 0; pos <= 1000; pos += 100)
        for (int f = 0; f < 20; ++f) {
            FocusScores s;
            s.channels = 1;
            s.ch[0].sharpness = score(pos, 1200, 300, rng);
            sw.add(pos, s, Metric::Laplacian);
        }
    const auto r = sw.result(Metric::Laplacian);
    CHECK(r.ch[0].atEdge);
    CHECK(sw.metrics().size() == 1);
}

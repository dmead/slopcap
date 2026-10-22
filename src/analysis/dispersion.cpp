#include "analysis/dispersion.hpp"

#include "analysis/focus.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace slopcap::analysis {

double DispersionMeasure::magnitude() const { return std::hypot(dx(), dy()); }

DispersionMeasure measureDispersion(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer) {
    DispersionMeasure d;
    if (bayer == Bayer::None) {
        d.problem = "A mono camera has no colour channels to compare: the ADC tool needs a colour camera.";
        return d;
    }
    // The focus scorer finds the target and each channel's background-subtracted centroid: in a window wide enough
    // for a large planet (in plane pixels: 1024 frame pixels on a colour sensor), and down into the limb's faint edge,
    // where cutting off at a higher level would bias channels sampled at different Bayer sites differently.
    FocusOptions opt;
    opt.window = 512;
    opt.centroidLevel = 0.04;
    const auto s = scoreFrame(pixels, width, height, bitDepth, bayer, opt);
    if (s.channels != 3) {
        d.problem = "No planet in the frame.";
        return d;
    }
    d.found = true;
    d.rx = s.ch[0].cx, d.ry = s.ch[0].cy;
    d.gx = s.ch[1].cx, d.gy = s.ch[1].cy;
    d.bx = s.ch[2].cx, d.by = s.ch[2].cy;
    d.atEdge = s.targetX0 <= 0 || s.targetY0 <= 0 || s.targetX1 >= width || s.targetY1 >= height;
    return d;
}

namespace {

/// n − 1 of dry air at a wavelength (µm).
double refractivity(double wavelengthUm, const Atmosphere& a) {
    const double s2 = 1 / (wavelengthUm * wavelengthUm);
    const double standard = (64.328 + 29498.1 / (146 - s2) + 255.4 / (41 - s2)) * 1e-6;   // 15 °C, 760 mmHg
    const double p = a.pressureHpa * 0.750062, t = a.temperatureC;                        // mmHg, °C
    return standard * p * (1 + (1.049 - 0.0157 * t) * 1e-6 * p) / (720.883 * (1 + 0.003661 * t));
}

/// Refraction at an altitude as a multiple of n − 1: Bennett's refraction (arcminutes, for 10 °C and 1010 hPa) over
/// n − 1 in arcminutes for those conditions at 0.55 µm, so it follows tan(zenith distance) high up.
double bend(double altitudeDeg) {
    const double h = std::min(altitudeDeg, 90.0);
    return std::max(0.0, 1 / std::tan((h + 7.31 / (h + 4.4)) * std::numbers::pi / 180) / 0.9691);
}

}  // namespace

double refractionArcsec(double wavelengthUm, double altitudeDeg, const Atmosphere& a) {
    if (altitudeDeg <= 0) return 0;
    return refractivity(wavelengthUm, a) * bend(altitudeDeg) * (180 * 3600 / std::numbers::pi);
}

double dispersionArcsec(double altitudeDeg, const Atmosphere& a, double blueUm, double redUm) {
    return refractionArcsec(blueUm, altitudeDeg, a) - refractionArcsec(redUm, altitudeDeg, a);
}

}  // namespace slopcap::analysis

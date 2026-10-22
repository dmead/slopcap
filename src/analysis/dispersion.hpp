// Atmospheric dispersion, measured and predicted. The air bends blue light more than red, so low in the sky a planet's
// blue image sits above its red one (toward the zenith): a blue fringe on top, a red one below. An ADC (atmospheric
// dispersion corrector: two counter-rotating prisms) cancels it. The ADC tool measures what's left between the colour
// channels and compares the uncorrected dispersion with what the atmosphere should be doing at the target's altitude.
#pragma once

#include "camera/camera.hpp"

#include <cstddef>
#include <span>
#include <string>

namespace slopcap::analysis {

struct DispersionMeasure {
    bool found = false;
    std::string problem;                                     ///< why not, when !found
    double rx = 0, ry = 0, gx = 0, gy = 0, bx = 0, by = 0;   ///< each channel's centroid on the target, full-frame px
    bool atEdge = false;                                     ///< the target touches the frame's edge (centroids biased)
    double dx() const { return rx - bx; }                    ///< red − blue: points away from the zenith, uncorrected
    double dy() const { return ry - by; }
    double magnitude() const;
};

/// The colour channels' centroids on the planet in a raw Bayer frame (8-bit, or 16-bit little-endian).
DispersionMeasure measureDispersion(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer);

struct Atmosphere {
    double pressureHpa = 1013.25;   ///< at the telescope (sea-level standard; about 11 % less per 1000 m up)
    double temperatureC = 10;
};

/// Refraction of light of `wavelengthUm` at `altitudeDeg`, arcseconds: n − 1 from Edlén's dispersion formula for dry
/// air with Filippenko's pressure and temperature terms (PASP 94, 715, 1982), times how refraction grows toward the
/// horizon: tan(zenith distance) high up, Bennett's formula (J. Navigation 35, 255, 1982) lower down, which stays
/// finite at the horizon (about 35′) where tan z doesn't. Rough below ~10°; 0 at or below the horizon.
double refractionArcsec(double wavelengthUm, double altitudeDeg, const Atmosphere& a = {});

/// How far the blue image sits above the red one at `altitudeDeg`, arcseconds: what an ADC has to cancel. By default
/// between the effective wavelengths of a colour camera's blue and red pixels.
double dispersionArcsec(double altitudeDeg, const Atmosphere& a = {}, double blueUm = 0.46, double redUm = 0.62);

}  // namespace slopcap::analysis

// What is the camera looking at? The Moon, Mars or an outer planet (the Target menu's "Auto"). Two independent kinds
// of evidence, because neither is enough alone:
//   the sky    positions (analysis/ephemeris) against the mount's pointing, and what is above the horizon at the site
//   the image  the target's shape and symmetry (Saturn's rings: elongated but point-symmetric, at any camera rotation;
//              a crescent Moon: elongated and lopsided), limb darkening (Jupiter's disc dims to its edge, the full
//              Moon doesn't), its size in arcseconds when the image scale is known, and its raw colour (Mars red,
//              Uranus / Neptune blue-green)
// Each piece of evidence rules candidates out; one that would rule out every candidate is reported as a conflict and
// ignored. One candidate left is the answer; several are reported as such ("Uranus or Neptune"), never guessed.
// Thresholds are calibrated on recordings of Jupiter, Saturn and the Moon (tests/test_identify.cpp).
#pragma once

#include "analysis/ephemeris.hpp"
#include "camera/camera.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace slopcap::analysis {

/// The brightest extended object in a raw frame: size, position and colour on its half-maximum region, shape on its
/// outline at 15% of the peak (where Saturn's fainter rings still count).
struct TargetFeatures {
    bool found = false;
    int channels = 0;                 ///< 3 colour, 1 mono
    double cx = 0, cy = 0;            ///< centroid, full-frame pixels
    double diameterPx = 0;            ///< equivalent diameter (same area) at half maximum, full-frame pixels
    double elongation = 1;            ///< outline's long / short axis from second moments: 1 round, ringed Saturn ~2.4
    double angleDeg = 0;              ///< long axis, from the +x axis
    double symmetry = 1;              ///< share of the outline that is also outline when turned 180° about its centre
    double limbRatio = 0;             ///< brightness near the edge / near the centre of the disc (0 if too small to say)
    std::array<double, 3> signal{};   ///< mean above the sky, per channel (R, G, B; mono: [0]), 0..1 of full scale
    bool touchesEdge = false;         ///< cut off by the frame edge: size and shape are lower bounds
};

TargetFeatures measureTarget(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer);

/// ln(R / B) of the raw signal: camera-dependent, but Mars is far redder and Uranus / Neptune far bluer than
/// Jupiter / Saturn on any one camera. nullopt for mono or no signal.
std::optional<double> colourIndex(const TargetFeatures& f);

struct Site {
    double latitude = 0, longitude = 0;   ///< degrees, east positive
};

struct IdentifyInput {
    TargetFeatures image;
    std::optional<double> arcsecPerPixel;            ///< image scale, if the focal length and pixel size are known
    double jdUtc = 0;
    std::optional<std::array<double, 2>> pointing;   ///< mount RA (h) / Dec (°), equinox of date
    std::optional<Site> site;
};

struct Identification {
    std::optional<Body> body;              ///< the answer, when only one candidate is left
    std::vector<Body> candidates;          ///< what's left (one when identified)
    std::string summary;                   ///< short, for the UI: "rings 2.4:1 · 0.6° from mount"
    std::vector<std::string> notes;        ///< every step: what each piece of evidence did
};

Identification identifyTarget(const IdentifyInput& in);

/// Display name of a result: "Saturn", "Uranus or Neptune", "Moon, Jupiter or Saturn", "" for none.
std::string candidatesText(const std::vector<Body>& c);

}  // namespace slopcap::analysis

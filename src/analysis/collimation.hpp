// Collimation from a defocused star (SCT / Mak / Newtonian): the out-of-focus star is a ring with the secondary's
// shadow in it. With good collimation the shadow is centred in the ring. Circles are fitted to the ring's outer edge
// and to the shadow's edge from radial scans; their offset (as a fraction of the ring radius) is the number to drive to
// zero. On an SCT: turn the secondary screw that moves the star toward the ring's thin side (the side the shadow is
// shifted to), then re-centre the star and repeat.
#pragma once

#include "camera/camera.hpp"

#include <span>
#include <string>

namespace slopcap::analysis {

struct DonutResult {
    bool found = false;
    std::string problem;              ///< why not found: "no star", "defocus more (no shadow)", "star touches the edge"
    double outerX = 0, outerY = 0, outerR = 0;   ///< full-frame pixels
    double innerX = 0, innerY = 0, innerR = 0;
    double offsetX = 0, offsetY = 0;  ///< shadow centre minus ring centre (points to the thin side)
    double offsetFraction = 0;        ///< |offset| / outer radius
    double fromCentreX = 0, fromCentreY = 0;   ///< ring centre minus frame centre (re-centre the star after each tweak)
    double outerRms = 0, innerRms = 0;         ///< circle-fit residuals: large = not round (pinched optics, seeing, tilt)
};

DonutResult analyzeDonut(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer);

}  // namespace slopcap::analysis

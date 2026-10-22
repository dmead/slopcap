// Live focus scores, one per colour channel (PLAN.md §7a). The raw Bayer frame is split into its R, G (two sites
// averaged) and B planes with no interpolation, so no channel's sharpness leaks into another's. The target is found on
// a sparse sample of the frame, and only a window around it is extracted and scored, so cost doesn't grow with the
// frame size.
//
// Every metric is scale-invariant (doesn't change with exposure or gain), so each channel is compared with its own
// best, never with the other channels:
//   gradient   squared central differences / squared signal (Tenengrad-like; the default)
//   laplacian  squared 4-neighbour Laplacian / squared signal (more weight on fine detail; more noise-sensitive)
//   variance   variance / mean² of the signal (normalised contrast; robust, broad peak)
//   fourier    share of the window's spectral power in a mid band of spatial frequencies (skips the noisiest
//              highest frequencies and the seeing-dominated lowest ones)
#pragma once

#include "camera/camera.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace slopcap::analysis {

enum class Metric { Gradient, Laplacian, Variance, Fourier };
const char* metricName(Metric m);
Metric metricFromName(std::string_view name);   ///< throws std::invalid_argument

struct FocusOptions {
    Metric metric = Metric::Gradient;
    int window = 384;           ///< scored region, in plane pixels (colour planes are half resolution); fourier uses
                                ///< the largest power of two that fits
    double bandLow = 0.10;      ///< fourier band, as fractions of the Nyquist frequency
    double bandHigh = 0.50;
    double centroidLevel = 0.15;   ///< the centroids weigh pixels above this share of the channel's range over the sky
};

struct ChannelScore {
    double sharpness = 0;   ///< the metric; only meaningful relative to the same channel's other frames
    double cx = 0, cy = 0;  ///< brightness centroid in full-frame pixels (R vs B offset = atmospheric dispersion)
    double peak = 0;        ///< brightest value in the window, 0..1 of full scale
    double background = 0;  ///< sky level, 0..1
    double clipped = 0;     ///< fraction of window pixels at full scale (a clipped channel can't be scored well)
};

struct FocusScores {
    int channels = 0;                     ///< 3 (R, G, B) for colour, 1 for mono, 0 if there was no target
    std::array<ChannelScore, 3> ch{};     ///< R, G, B (mono: ch[0] only)
    int windowX = 0, windowY = 0, windowW = 0, windowH = 0;   ///< scored region in full-frame pixels
    int targetX0 = 0, targetY0 = 0, targetX1 = 0, targetY1 = 0;   ///< the target's bounding box (full-frame pixels,
                                                                  ///< from the locate sample: within a sample step)
};

/// Score one raw frame (8-bit, or 16-bit little-endian).
FocusScores scoreFrame(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer,
                       const FocusOptions& options = {});

}  // namespace slopcap::analysis

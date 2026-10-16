// CPU reference for what the preview shader does: bilinear Bayer demosaic, levels stretch (black/white point,
// gamma), plus the histogram and auto-stretch the preview uses. The GPU path must match this; it's also the fallback.
// Also the per-channel readout histogram the Capture sidebar shows.
#pragma once

#include "camera/camera.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace slopcap::preview {

struct Levels {
    double black = 0.0;   ///< input level mapped to 0 (0..1 of full scale)
    double white = 1.0;   ///< input level mapped to 1
    double gamma = 1.0;   ///< applied after the black/white mapping (output = x^(1/gamma))
    std::array<double, 3> wb{1.0, 1.0, 1.0};   ///< display white balance: R, G, B gains applied before the levels
};

/// Gray-world white balance on the target: gains that make the bright part of the frame neutral (G stays 1).
/// Display only; recordings stay raw.
std::array<double, 3> autoWhiteBalance(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer);

/// 256-bin histogram of one frame (8-bit samples directly, 16-bit by their high byte), optionally on every Nth pixel.
std::array<uint32_t, 256> histogram(std::span<const std::byte> pixels, int bitDepth, int stride = 1);

/// The raw readout per colour channel (R, G, B by Bayer site; one channel for mono), in the sensor's own units:
/// 16-bit samples are MSB-aligned, so with a known ADC depth they are shifted down to 0..2^adcBits-1.
struct RawHistogram {
    int channels = 0;                            ///< 3 colour, 1 mono, 0 = no frame
    int fullScale = 255;                         ///< the top of the range, in ADU (255, 4095, 65535, ...)
    int adcBits = 8;                             ///< bits of the range above (fullScale = 2^adcBits - 1)
    std::array<std::vector<uint32_t>, 3> bins;   ///< `bins` equal-width bins over 0..fullScale, per channel
    std::array<int, 3> median{}, max{};          ///< ADU
    std::array<double, 3> clipped{};             ///< fraction of samples at full scale
    std::array<uint64_t, 3> samples{};
};

/// Every channel of a raw frame, on at most about `maxSamples` pixels (whole 2x2 Bayer blocks, evenly spaced).
RawHistogram rawHistogram(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer,
                          std::optional<int> adcBits, int bins = 128, std::size_t maxSamples = std::size_t{1} << 18);

/// Auto-stretch: black just above the background, white at the brightest `whitePercentile` of the histogram.
Levels autoLevels(const std::array<uint32_t, 256>& hist, double blackPercentile = 0.5, double whitePercentile = 0.9995);

/// Demosaic + stretch one raw frame to 8-bit RGB (3 bytes per pixel). Mono frames are replicated to grey.
/// Bayer: bilinear interpolation, edges mirrored. 16-bit input is little-endian, full-scale 65535.
std::vector<uint8_t> renderRgb(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer, const Levels& lv);

}  // namespace slopcap::preview

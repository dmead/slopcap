// Squeezes raw frames for the remote stream, keeping them raw: the client gets back a frame of the same size, bit
// depth and Bayer layout, so its preview, histograms and analysis work exactly as on the server. Two codecs:
//
// Lossless (quality kLossless): the exact frame, all 16 bits. Each CFA site's plane is predicted pixel by pixel from
//   its neighbours (the median edge predictor of JPEG-LS), and the residuals are deflated, in parallel chunks.
//   Padding bits that are zero in every sample (a 12- or 14-bit ADC in 16-bit frames) are shifted out first.
// JPEG (quality 1-99), lossy:
//   1. 16-bit frames are mapped to 8 bits through a square-root curve between the frame's min and max (keeps faint
//      detail); the header carries the range, so the client maps them back to 16 bits. 8-bit frames go as they are.
//   2. A colour (Bayer) frame is split into its four CFA sites, tiled as the quadrants of one grey image: each
//      quadrant is a smooth half-resolution plane, which JPEG handles well (JPEG on a raw mosaic smears the colours).
//   3. Optionally shrunk by 2 or 4 (block average) when the link is slow; the client scales back up.
//   4. Grey JPEG at the given quality.
#pragma once

#include "camera/camera.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <span>

namespace slopcap::net {

inline constexpr int kLossless = 100;   ///< the "quality" that means lossless

struct EncodeParams {
    int quality = 85;   ///< JPEG quality 1-99, or kLossless
    int scale = 1;      ///< 1, 2 or 4: shrink factor (JPEG only)
};

struct EncodedFrame {
    QJsonObject header;   ///< size, bit depth, layout, codec and its parameters (everything decodeFrame needs)
    QByteArray data;      ///< the compressed pixels
};

EncodedFrame encodeFrame(std::span<const std::byte> pixels, const FrameInfo& info, Bayer bayer, EncodeParams params);

/// The lossless codec's prediction stage on its own (its deflate step aside): zigzagged residual bytes into `out`
/// (width * height bytes, twice that for 16-bit). Returns the padding shift. For tools/kernelbench.
int losslessResiduals(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer, uint8_t* out);

/// The frame's metadata from a header (roi, bit depth, sequence, timestamp, bytes).
FrameInfo frameInfoFrom(const QJsonObject& header);

/// Rebuild the raw frame into dst (at least frameInfoFrom(header).bytes long). False if the data can't be decoded.
bool decodeFrame(const QJsonObject& header, const QByteArray& data, std::span<std::byte> dst);

/// "lossless" or "quality 85", for the UI and the log.
QString qualityName(int quality);

/// True if this build can write and read JPEG (Qt's image plugin is present).
bool jpegAvailable();

}  // namespace slopcap::net

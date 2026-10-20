// Squeezes raw frames for the remote stream, keeping them raw: the client gets back a frame of the same size, bit
// depth and Bayer layout, so its preview, histograms and analysis work exactly as on the server, just on a lossy copy.
//
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

#include <span>

namespace slopcap::net {

struct EncodeParams {
    int quality = 75;   ///< JPEG quality, 1-100
    int scale = 1;      ///< 1, 2 or 4: shrink factor
};

struct EncodedFrame {
    QJsonObject header;   ///< size, bit depth, layout, range, ... (everything decodeFrame needs)
    QByteArray data;      ///< the JPEG
};

EncodedFrame encodeFrame(std::span<const std::byte> pixels, const FrameInfo& info, Bayer bayer, EncodeParams params);

/// The frame's metadata from a header (roi, bit depth, sequence, timestamp, bytes).
FrameInfo frameInfoFrom(const QJsonObject& header);

/// Rebuild the raw frame into dst (at least frameInfoFrom(header).bytes long). False if the data can't be decoded.
bool decodeFrame(const QJsonObject& header, const QByteArray& data, std::span<std::byte> dst);

/// True if this build can write and read JPEG (Qt's image plugin is present).
bool jpegAvailable();

}  // namespace slopcap::net

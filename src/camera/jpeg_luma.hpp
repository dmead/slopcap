// JPEG to its luminance, for the stills camera's test shots: the focus tools score one channel, so only the first
// component (Y) is decoded, at full resolution; the colour components are read past. Baseline and extended-sequential
// Huffman JPEGs with 8-bit samples (what cameras write): any sampling factors, interleaved or one scan per component,
// restart markers, EXIF thumbnails skipped. Progressive, arithmetic-coded, 12-bit and lossless JPEGs are refused.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace slopcap::jpeg {

struct Size {
    int width = 0, height = 0;
};

/// The picture's size, from its frame header. Throws CameraError.
Size size(std::span<const uint8_t> data);
/// The luminance into `out` (width * height bytes, row-major). Throws CameraError for what it can't decode.
Size decodeLuma(std::span<const uint8_t> data, std::vector<uint8_t>& out);

}  // namespace slopcap::jpeg

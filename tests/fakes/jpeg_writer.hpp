// A small baseline JPEG encoder for the tests: what the fake Sony camera sends, and the decoder's test pictures. Unlike
// a library's, every choice a decoder must cope with is open: sampling factors, restart markers, one scan per
// component, an EXIF segment holding a thumbnail JPEG. Its own Huffman tables use codes up to 16 bits long.
#pragma once

#include <cstdint>
#include <vector>

namespace fake {

struct JpegOptions {
    int quality = 90;
    bool color = true;           ///< YCbCr (the chroma carries a pattern of its own); false: one grey component
    int yh = 2, yv = 1;          ///< luminance sampling factors; chroma's below (2x1 / 1x1 = 4:2:2, Sony's)
    int ch = 1, cv = 1;
    int restart = 0;             ///< restart interval in MCUs (0: none)
    bool separateScans = false;  ///< one scan per component instead of one interleaved scan
    bool exifThumbnail = false;  ///< an APP1 segment with a small JPEG inside, as cameras write
};

/// The luminance `gray` (width x height, row-major) as a JPEG.
std::vector<uint8_t> encodeJpeg(const uint8_t* gray, int width, int height, const JpegOptions& o = {});

}  // namespace fake

#include "net/frame_codec.hpp"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace slopcap::net {

namespace {

uint16_t load16(const std::byte* p) { return static_cast<uint16_t>(std::to_integer<unsigned>(p[0]) | (std::to_integer<unsigned>(p[1]) << 8)); }

}  // namespace

EncodedFrame encodeFrame(std::span<const std::byte> pixels, const FrameInfo& info, Bayer bayer, EncodeParams params) {
    const int w = info.roi.width, h = info.roi.height;
    const bool wide = info.bitDepth > 8;
    const int bps = wide ? 2 : 1;
    const int s = params.scale >= 4 ? 4 : params.scale >= 2 ? 2 : 1;
    const bool planes = bayer != Bayer::None && w % 2 == 0 && h % 2 == 0;
    const int step = planes ? 2 : 1, sites = planes ? 2 : 1;
    const int pw = w / step, ph = h / step;                          // one CFA site's plane
    const int sw = std::max(1, pw / s), sh = std::max(1, ph / s);    // ... after shrinking

    // 16 -> 8 bits: square root between the frame's extremes
    int black = 0, white = 255;
    std::vector<uint8_t> lut;
    if (wide) {
        uint16_t lo = 0xffff, hi = 0;
        const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        for (std::size_t i = 0; i < n; ++i) {
            const uint16_t v = load16(pixels.data() + 2 * i);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        black = lo, white = std::max<int>(hi, lo + 1);
        lut.resize(static_cast<std::size_t>(white - black + 1));
        for (std::size_t i = 0; i < lut.size(); ++i)
            lut[i] = static_cast<uint8_t>(std::lround(255 * std::sqrt(static_cast<double>(i) / (white - black))));
    }
    // Row by row: each output row (CFA row phase dy, row oy) sums its s source rows into acc, both column phases at
    // once (column x lands in acc[col[x]]: quadrant dx, block ox), then maps the block means to 8 bits.
    const int used = sw * s * step;   // source columns that land in the image (the rest of a ragged edge is dropped)
    std::vector<uint32_t> col(static_cast<std::size_t>(used));
    for (int x = 0; x < used; ++x) col[static_cast<std::size_t>(x)] = static_cast<uint32_t>((planes ? x & 1 : 0) * sw + x / step / s);
    std::vector<uint32_t> acc(static_cast<std::size_t>(sites * sw));
    const uint32_t blocks = static_cast<uint32_t>(s * s);
    QImage img(sw * sites, sh * sites, QImage::Format_Grayscale8);
    for (int dy = 0; dy < sites; ++dy)
        for (int oy = 0; oy < sh; ++oy) {
            std::fill(acc.begin(), acc.end(), 0u);
            for (int j = 0; j < s; ++j) {
                const std::byte* row = pixels.data() + static_cast<std::size_t>((oy * s + j) * step + dy) * static_cast<std::size_t>(w) * bps;
                if (wide)
                    for (std::size_t x = 0; x < col.size(); ++x) acc[col[x]] += load16(row + 2 * x);
                else
                    for (std::size_t x = 0; x < col.size(); ++x) acc[col[x]] += std::to_integer<uint32_t>(row[x]);
            }
            uchar* line = img.scanLine(dy * sh + oy);
            for (std::size_t i = 0; i < acc.size(); ++i) {
                const uint32_t v = (acc[i] + blocks / 2) / blocks;
                line[i] = wide ? lut[std::clamp<uint32_t>(v, black, white) - black] : static_cast<uchar>(v);
            }
        }

    EncodedFrame out;
    QBuffer buf(&out.data);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPG", std::clamp(params.quality, 1, 100));
    out.header = QJsonObject{{"seq", static_cast<double>(info.sequence)},
                             {"t", static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(info.timestamp.time_since_epoch()).count())},
                             {"x", info.roi.x},
                             {"y", info.roi.y},
                             {"w", w},
                             {"h", h},
                             {"bin", info.roi.bin},
                             {"bits", info.bitDepth},
                             {"layout", planes ? "planes" : "mono"},
                             {"sw", sw},
                             {"sh", sh},
                             {"scale", s},
                             {"q", params.quality},
                             {"black", black},
                             {"white", white}};
    return out;
}

FrameInfo frameInfoFrom(const QJsonObject& h) {
    FrameInfo fi;
    fi.sequence = static_cast<uint64_t>(h["seq"].toDouble());
    fi.timestamp = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::microseconds(static_cast<int64_t>(h["t"].toDouble()))));
    fi.roi = {h["x"].toInt(), h["y"].toInt(), h["w"].toInt(), h["h"].toInt(), std::max(1, h["bin"].toInt(1))};
    fi.bitDepth = h["bits"].toInt(8) > 8 ? 16 : 8;
    fi.bytes = static_cast<std::size_t>(fi.roi.width) * static_cast<std::size_t>(fi.roi.height) * (fi.bitDepth > 8 ? 2 : 1);
    return fi;
}

bool decodeFrame(const QJsonObject& h, const QByteArray& data, std::span<std::byte> dst) {
    const FrameInfo fi = frameInfoFrom(h);
    const int w = fi.roi.width, ht = fi.roi.height;
    if (w <= 0 || ht <= 0 || dst.size() < fi.bytes) return false;
    const bool planes = h["layout"].toString() == "planes";
    const int sites = planes ? 2 : 1;
    const int sw = h["sw"].toInt(), sh = h["sh"].toInt(), s = std::max(1, h["scale"].toInt(1));
    QImage img = QImage::fromData(data, "JPG");
    if (img.isNull()) return false;
    if (img.format() != QImage::Format_Grayscale8) img = img.convertToFormat(QImage::Format_Grayscale8);
    if (sw <= 0 || sh <= 0 || img.width() != sw * sites || img.height() != sh * sites) return false;

    const bool wide = fi.bitDepth > 8;
    std::array<uint16_t, 256> inv{};
    if (wide) {
        const int black = h["black"].toInt(), white = h["white"].toInt(black + 1);
        for (int v = 0; v < 256; ++v) {
            const double f = v / 255.0;
            inv[static_cast<std::size_t>(v)] = static_cast<uint16_t>(std::clamp<long>(std::lround(black + f * f * (white - black)), 0, 65535));
        }
    }
    std::vector<int> col(static_cast<std::size_t>(w));   // image column for each output column
    for (int x = 0; x < w; ++x) {
        const int dx = planes ? x & 1 : 0, px = planes ? x >> 1 : x;
        col[static_cast<std::size_t>(x)] = dx * sw + std::min(px / s, sw - 1);
    }
    for (int y = 0; y < ht; ++y) {
        const int dy = planes ? y & 1 : 0, py = planes ? y >> 1 : y;
        const uchar* line = img.constScanLine(dy * sh + std::min(py / s, sh - 1));
        std::byte* out = dst.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * (wide ? 2 : 1);
        if (wide) {
            for (int x = 0; x < w; ++x) {
                const uint16_t v = inv[line[col[static_cast<std::size_t>(x)]]];
                out[2 * x] = static_cast<std::byte>(v & 0xff);
                out[2 * x + 1] = static_cast<std::byte>(v >> 8);
            }
        } else {
            for (int x = 0; x < w; ++x) out[x] = static_cast<std::byte>(line[col[static_cast<std::size_t>(x)]]);
        }
    }
    return true;
}

bool jpegAvailable() {
    return QImageWriter::supportedImageFormats().contains("jpeg") && QImageReader::supportedImageFormats().contains("jpeg");
}

}  // namespace slopcap::net

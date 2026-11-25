#include "net/frame_codec.hpp"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <vector>

namespace slopcap::net {

namespace {

uint16_t load16(const std::byte* p) { return static_cast<uint16_t>(std::to_integer<unsigned>(p[0]) | (std::to_integer<unsigned>(p[1]) << 8)); }

/// What every header carries, whatever the codec.
QJsonObject baseHeader(const FrameInfo& info, bool planes) {
    return {{"seq", static_cast<double>(info.sequence)},
            {"t", static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(info.timestamp.time_since_epoch()).count())},
            {"x", info.roi.x},
            {"y", info.roi.y},
            {"w", info.roi.width},
            {"h", info.roi.height},
            {"bin", info.roi.bin},
            {"bits", info.bitDepth},
            {"layout", planes ? "planes" : "mono"}};
}

// ---------------------------------------------------------------- lossless
//
// Each CFA site's plane in turn (one plane for mono), row by row. Every sample is predicted from its reconstructed
// neighbours with the median edge detector of LOCO-I / JPEG-LS (left a, above b, upper-left c), and the residual stored
// zigzagged (small magnitudes → small numbers). 16-bit residuals go as two blocks, all low bytes then all high bytes
// (the high bytes are mostly zero). Bits that are zero in every sample (a 12- or 14-bit ADC's padding) are shifted out
// first. The residual bytes are deflated in up to 8 chunks at once.
//
// A row's samples sit in a buffer from index 1; index 0 holds the sample above the row's first, so the first sample
// predicts from above like the others predict from their neighbours (a = b gives b, whatever c is). Before a plane's
// first row the row above is zeros, so that row predicts from the left (b = c = 0 gives a) and its first sample is 0.
// The encoder knows every sample, so its predictions don't depend on each other and the loop vectorizes; the decoder
// needs each sample before the next.

/// JPEG-LS's median edge detector without branches: c at or above both neighbours gives the smaller, at or below both
/// the larger, else a + b - c, which is a + b - c clamped to [min(a, b), max(a, b)]. Min and max become conditional
/// moves or vector instructions; LLVM kept the three-way branch, which mispredicts on noisy pixels.
int predict(int a, int b, int c) { return std::max(std::min(a, b), std::min(std::max(a, b), a + b - c)); }

/// One row of a plane: its samples (every `step`th from `src`, padding shifted out) into cur[1..pw], then their
/// zigzagged residuals into lo (and hi: the high bytes of 16-bit ones).
template <bool Wide>
void residualRow(const std::byte* src, int step, int pw, int shift, const int* prev, int* cur, uint8_t* lo, uint8_t* hi) {
    cur[0] = prev[1];
    for (int x = 0; x < pw; ++x) {
        const std::byte* s = src + static_cast<std::size_t>(x) * static_cast<std::size_t>(step) * (Wide ? 2 : 1);
        cur[x + 1] = (Wide ? load16(s) : std::to_integer<int>(*s)) >> shift;
    }
    for (int x = 1; x <= pw; ++x) {
        const int d = cur[x] - predict(cur[x - 1], prev[x], prev[x - 1]);
        if constexpr (Wide) {
            const auto r = static_cast<int16_t>(d);
            const auto z = static_cast<uint16_t>((static_cast<uint16_t>(r) << 1) ^ static_cast<uint16_t>(r >> 15));
            lo[x - 1] = static_cast<uint8_t>(z & 0xff);
            hi[x - 1] = static_cast<uint8_t>(z >> 8);
        } else {
            const auto r = static_cast<int8_t>(d);
            lo[x - 1] = static_cast<uint8_t>((static_cast<uint8_t>(r) << 1) ^ static_cast<uint8_t>(r >> 7));
        }
    }
}

/// The decoder's side of residualRow: the samples back into cur[1..pw].
template <bool Wide>
void reconstructRow(const uint8_t* lo, const uint8_t* hi, int pw, const int* prev, int* cur) {
    cur[0] = prev[1];
    for (int x = 1; x <= pw; ++x) {
        const unsigned z = Wide ? lo[x - 1] | (static_cast<unsigned>(hi[x - 1]) << 8) : lo[x - 1];
        const int r = static_cast<int>(z >> 1) ^ -static_cast<int>(z & 1);
        cur[x] = (predict(cur[x - 1], prev[x], prev[x - 1]) + r) & (Wide ? 0xffff : 0xff);
    }
}

constexpr std::size_t kChunkTarget = 1 << 20;   ///< bytes of residuals per deflate chunk (at least)

EncodedFrame encodeLossless(std::span<const std::byte> pixels, const FrameInfo& info, Bayer bayer) {
    const int w = info.roi.width, h = info.roi.height;
    const bool wide = info.bitDepth > 8;
    const int bps = wide ? 2 : 1;
    const bool planes = bayer != Bayer::None && w % 2 == 0 && h % 2 == 0;
    const int step = planes ? 2 : 1, sites = planes ? 2 : 1;
    const int pw = w / step, ph = h / step;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);

    unsigned used = 0;   // bits set in any sample
    if (wide)
        for (std::size_t i = 0; i < n; ++i) used |= load16(pixels.data() + 2 * i);
    const int shift = wide && used ? std::countr_zero(used) : 0;

    QByteArray residuals(static_cast<qsizetype>(n * bps), Qt::Uninitialized);
    auto* lo = reinterpret_cast<uint8_t*>(residuals.data());
    uint8_t* hi = wide ? lo + n : nullptr;
    std::size_t k = 0;
    std::vector<int> prev(static_cast<std::size_t>(pw) + 2), cur(static_cast<std::size_t>(pw) + 2);   // + 2: a 0-wide row still reads prev[1]
    for (int dy = 0; dy < sites; ++dy)
        for (int dx = 0; dx < sites; ++dx) {
            std::fill(prev.begin(), prev.end(), 0);
            for (int y = 0; y < ph; ++y) {
                const std::byte* src = pixels.data() + (static_cast<std::size_t>(y * step + dy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(dx)) * bps;
                if (wide) residualRow<true>(src, step, pw, shift, prev.data(), cur.data(), lo + k, hi + k);
                else residualRow<false>(src, step, pw, shift, prev.data(), cur.data(), lo + k, nullptr);
                k += static_cast<std::size_t>(pw);
                std::swap(prev, cur);
            }
        }

    // deflate in chunks, in parallel
    const std::size_t total = static_cast<std::size_t>(residuals.size());
    const std::size_t chunks = std::clamp<std::size_t>(total / kChunkTarget, 1, 8);
    std::vector<std::future<QByteArray>> jobs;
    for (std::size_t c = 0; c < chunks; ++c) {
        const std::size_t from = total * c / chunks, to = total * (c + 1) / chunks;
        jobs.push_back(std::async(std::launch::async, [&residuals, from, to] {
            return qCompress(reinterpret_cast<const uchar*>(residuals.constData()) + from, static_cast<qsizetype>(to - from), 1);
        }));
    }
    EncodedFrame out;
    QJsonArray sizes;
    for (auto& j : jobs) {
        const QByteArray z = j.get();
        sizes.append(static_cast<double>(z.size()));
        out.data.append(z);
    }
    out.header = baseHeader(info, planes);
    out.header["codec"] = "lossless";
    out.header["shift"] = shift;
    out.header["chunks"] = sizes;
    out.header["q"] = kLossless;
    out.header["scale"] = 1;
    return out;
}

bool decodeLossless(const QJsonObject& h, const QByteArray& data, std::span<std::byte> dst, const FrameInfo& fi) {
    const int w = fi.roi.width, ht = fi.roi.height;
    const bool wide = fi.bitDepth > 8;
    const int bps = wide ? 2 : 1;
    const bool planes = h["layout"].toString() == "planes" && w % 2 == 0 && ht % 2 == 0;
    const int step = planes ? 2 : 1, sites = planes ? 2 : 1;
    const int pw = w / step, ph = ht / step;
    const int shift = h["shift"].toInt();
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(ht);
    if (shift < 0 || shift > 15) return false;

    // inflate the chunks, in parallel
    std::vector<std::future<QByteArray>> jobs;
    qsizetype pos = 0;
    for (const auto& v : h["chunks"].toArray()) {
        const auto len = static_cast<qsizetype>(v.toDouble());
        if (len <= 0 || pos + len > data.size()) return false;
        jobs.push_back(std::async(std::launch::async, [&data, pos, len] { return qUncompress(reinterpret_cast<const uchar*>(data.constData()) + pos, len); }));
        pos += len;
    }
    QByteArray residuals;
    for (auto& j : jobs) residuals.append(j.get());
    if (static_cast<std::size_t>(residuals.size()) != n * bps) return false;
    const auto* lo = reinterpret_cast<const uint8_t*>(residuals.constData());
    const uint8_t* hi = wide ? lo + n : nullptr;

    std::size_t k = 0;
    std::vector<int> prev(static_cast<std::size_t>(pw) + 2), cur(static_cast<std::size_t>(pw) + 2);   // + 2: a 0-wide row still reads prev[1]
    for (int dy = 0; dy < sites; ++dy)
        for (int dx = 0; dx < sites; ++dx) {
            std::fill(prev.begin(), prev.end(), 0);
            for (int y = 0; y < ph; ++y) {
                if (wide) reconstructRow<true>(lo + k, hi + k, pw, prev.data(), cur.data());
                else reconstructRow<false>(lo + k, nullptr, pw, prev.data(), cur.data());
                k += static_cast<std::size_t>(pw);
                std::byte* row = dst.data() + (static_cast<std::size_t>(y * step + dy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(dx)) * bps;
                for (int x = 0; x < pw; ++x) {
                    const std::size_t col = static_cast<std::size_t>(x) * static_cast<std::size_t>(step);
                    if (wide) {
                        const unsigned s = static_cast<unsigned>(cur[static_cast<std::size_t>(x) + 1]) << shift;
                        row[2 * col] = static_cast<std::byte>(s & 0xff);
                        row[2 * col + 1] = static_cast<std::byte>((s >> 8) & 0xff);
                    } else {
                        row[col] = static_cast<std::byte>(cur[static_cast<std::size_t>(x) + 1]);
                    }
                }
                std::swap(prev, cur);
            }
        }
    return true;
}

// ---------------------------------------------------------------- JPEG

EncodedFrame encodeJpeg(std::span<const std::byte> pixels, const FrameInfo& info, Bayer bayer, EncodeParams params) {
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
    img.save(&buf, "JPG", std::clamp(params.quality, 1, 99));
    out.header = baseHeader(info, planes);
    out.header["codec"] = "jpeg";
    out.header["sw"] = sw;
    out.header["sh"] = sh;
    out.header["scale"] = s;
    out.header["q"] = params.quality;
    out.header["black"] = black;
    out.header["white"] = white;
    return out;
}

bool decodeJpeg(const QJsonObject& h, const QByteArray& data, std::span<std::byte> dst, const FrameInfo& fi) {
    const int w = fi.roi.width, ht = fi.roi.height;
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

}  // namespace

EncodedFrame encodeFrame(std::span<const std::byte> pixels, const FrameInfo& info, Bayer bayer, EncodeParams params) {
    return params.quality >= kLossless ? encodeLossless(pixels, info, bayer) : encodeJpeg(pixels, info, bayer, params);
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
    if (fi.roi.width <= 0 || fi.roi.height <= 0 || dst.size() < fi.bytes) return false;
    return h["codec"].toString() == "lossless" ? decodeLossless(h, data, dst, fi) : decodeJpeg(h, data, dst, fi);
}

QString qualityName(int quality) { return quality >= kLossless ? QString("lossless") : QString("quality %1").arg(quality); }

bool jpegAvailable() {
    return QImageWriter::supportedImageFormats().contains("jpeg") && QImageReader::supportedImageFormats().contains("jpeg");
}

}  // namespace slopcap::net

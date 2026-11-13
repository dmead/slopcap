#include "camera/jpeg_luma.hpp"

#include "camera/camera.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>

namespace slopcap::jpeg {

namespace {

constexpr uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
constexpr int kFastBits = 9;

[[noreturn]] void bad(const std::string& why) { throw CameraError("JPEG: " + why); }

struct Huffman {
    bool defined = false;
    std::array<int, 18> maxcode{};   ///< per code length, the largest code (-1: none of that length)
    std::array<int, 17> mincode{}, valptr{};
    std::array<uint8_t, 256> symbols{};
    std::array<uint16_t, 1 << kFastBits> fast{};   ///< codes up to kFastBits long: length << 8 | symbol (0: longer)

    void build(const uint8_t* counts, const uint8_t* syms, int nsyms) {
        std::copy(syms, syms + nsyms, symbols.begin());
        fast.fill(0);
        int code = 0, k = 0;
        for (int len = 1; len <= 16; ++len) {
            const int n = counts[len - 1];
            if (code + n > (1 << len)) bad("Huffman table with more codes than bits");
            valptr[static_cast<std::size_t>(len)] = k;
            mincode[static_cast<std::size_t>(len)] = code;
            maxcode[static_cast<std::size_t>(len)] = n ? code + n - 1 : -1;
            if (len <= kFastBits)
                for (int i = 0; i < n; ++i) {
                    const int shift = kFastBits - len, first = (code + i) << shift;
                    for (int j = 0; j < 1 << shift; ++j)
                        fast[static_cast<std::size_t>(first | j)] = static_cast<uint16_t>(len << 8 | symbols[static_cast<std::size_t>(k + i)]);
                }
            code = (code + n) << 1;
            k += n;
        }
        maxcode[17] = INT_MAX;
        defined = true;
    }
};

/// The entropy-coded data, a bit at a time: byte stuffing undone, zeros fed once a marker (or the end) is reached.
struct Bits {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf = 0;   ///< the next bits, from the top
    int n = 0;          ///< how many
    int padded = 0;     ///< zero bytes fed past a marker or the end
    bool atMarker = false;

    void fill() {
        while (n <= 56) {
            uint8_t b = 0;
            if (!atMarker && p < end && *p == 0xFF && (p + 1 >= end || p[1] != 0x00)) atMarker = true;   // p stays on it
            if (!atMarker && p < end) {
                b = *p;
                p += b == 0xFF ? 2 : 1;   // FF 00 is a data FF
            } else if (++padded > 64) {
                bad("the data ends inside the picture");
            }
            buf |= static_cast<uint64_t>(b) << (56 - n);
            n += 8;
        }
    }
    int peek(int k) {
        if (n < k) fill();
        return static_cast<int>(buf >> (64 - k));
    }
    void drop(int k) {
        buf <<= k;
        n -= k;
    }
    int get(int k) {
        if (k == 0) return 0;
        const int v = peek(k);
        drop(k);
        return v;
    }
    /// At a restart marker: on past it, with an empty buffer.
    void restart() {
        buf = 0;
        n = 0;
        padded = 0;
        atMarker = false;
        while (p + 1 < end && !(p[0] == 0xFF && (p[1] & 0xF8) == 0xD0)) ++p;   // (where the data was cut: on to the marker)
        if (p + 1 >= end) bad("restart marker missing");
        p += 2;
    }
};

int decode(Bits& b, const Huffman& h) {
    const int look = b.peek(kFastBits);
    if (const uint16_t f = h.fast[static_cast<std::size_t>(look)]) {
        b.drop(f >> 8);
        return f & 0xFF;
    }
    const int code16 = b.peek(16);
    for (int len = kFastBits + 1; len <= 16; ++len) {
        const int c = code16 >> (16 - len);
        if (c <= h.maxcode[static_cast<std::size_t>(len)]) {
            b.drop(len);
            const int i = h.valptr[static_cast<std::size_t>(len)] + c - h.mincode[static_cast<std::size_t>(len)];
            return h.symbols[static_cast<std::size_t>(std::clamp(i, 0, 255))];
        }
    }
    bad("damaged data (no such Huffman code)");
}

int extend(int v, int s) { return v < 1 << (s - 1) ? v - (1 << s) + 1 : v; }

struct Component {
    int id = 0, h = 1, v = 1, tq = 0;
    int td = 0, ta = 0;   ///< the current scan's tables
    int pred = 0;
};

/// Inverse DCT factors: kIdct[x][u] = C(u) / 2 * cos((2x + 1) u pi / 16).
const std::array<std::array<float, 8>, 8>& idctTable() {
    static const auto t = [] {
        std::array<std::array<float, 8>, 8> m{};
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u)
                m[static_cast<std::size_t>(x)][static_cast<std::size_t>(u)] =
                    static_cast<float>((u == 0 ? std::sqrt(0.5) : 1.0) / 2 * std::cos((2 * x + 1) * u * std::numbers::pi / 16));
        return m;
    }();
    return t;
}

/// One 8x8 block of coefficients (natural order, dequantised) into pixels.
void idct(const float* f, bool acZero, uint8_t* out, int stride) {
    if (acZero) {
        const auto v = static_cast<uint8_t>(std::clamp(std::lround(f[0] / 8 + 128), 0L, 255L));
        for (int y = 0; y < 8; ++y) std::memset(out + y * stride, v, 8);
        return;
    }
    const auto& c = idctTable();
    float tmp[64];
    for (int v = 0; v < 8; ++v)   // rows: each vertical frequency's horizontal transform
        for (int x = 0; x < 8; ++x) {
            float s = 0;
            for (int u = 0; u < 8; ++u) s += c[static_cast<std::size_t>(x)][static_cast<std::size_t>(u)] * f[v * 8 + u];
            tmp[v * 8 + x] = s;
        }
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            float s = 0;
            for (int v = 0; v < 8; ++v) s += c[static_cast<std::size_t>(y)][static_cast<std::size_t>(v)] * tmp[v * 8 + x];
            out[y * stride + x] = static_cast<uint8_t>(std::clamp(std::lround(s + 128), 0L, 255L));
        }
}

class Decoder {
public:
    explicit Decoder(std::span<const uint8_t> d) : d_(d) {}

    Size run(std::vector<uint8_t>* out) {
        if (d_.size() < 4 || d_[0] != 0xFF || d_[1] != 0xD8) bad("not a JPEG (no start marker)");
        pos_ = 2;
        for (;;) {
            const uint8_t m = nextMarker();
            if (m == 0xD9) break;   // end of image
            if (m >= 0xD0 && m <= 0xD7) continue;   // a stray restart marker
            const std::size_t len = u16At(pos_);
            if (len < 2 || pos_ + len > d_.size()) bad("a segment runs past the end");
            const std::span<const uint8_t> seg = d_.subspan(pos_ + 2, len - 2);
            pos_ += len;
            switch (m) {
                case 0xC0:
                case 0xC1: frame(seg); if (!out) return {width_, height_}; break;
                case 0xC2: bad("progressive JPEG (not supported)");
                case 0xC4: tables(seg); break;
                case 0xDB: quant(seg); break;
                case 0xDD:
                    if (seg.size() < 2) bad("short restart interval segment");
                    restart_ = seg[0] << 8 | seg[1];
                    break;
                case 0xDA: scan(seg); break;
                default:
                    if ((m >= 0xC3 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC))
                        bad(std::format("unsupported JPEG type (SOF 0x{:02X})", m));
                    break;   // APPn (EXIF and its thumbnail), COM, DNL, ...: skipped
            }
        }
        if (!width_) bad("no frame header");
        if (!sawY_) bad("no luminance data");
        // the luminance plane, upsampled if Y itself is subsampled (rare)
        const auto& y = comps_[0];
        out->resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_));
        for (int r = 0; r < height_; ++r) {
            const uint8_t* src = plane_.data() + static_cast<std::size_t>(r * y.v / vmax_) * static_cast<std::size_t>(planeW_);
            uint8_t* dst = out->data() + static_cast<std::size_t>(r) * static_cast<std::size_t>(width_);
            if (y.h == hmax_) std::memcpy(dst, src, static_cast<std::size_t>(width_));
            else
                for (int x = 0; x < width_; ++x) dst[x] = src[x * y.h / hmax_];
        }
        return {width_, height_};
    }

private:
    std::size_t u16At(std::size_t at) const {
        if (at + 2 > d_.size()) bad("the data ends early");
        return static_cast<std::size_t>(d_[at] << 8 | d_[at + 1]);
    }

    uint8_t nextMarker() {
        while (pos_ < d_.size() && d_[pos_] != 0xFF) ++pos_;   // (entropy-coded data after a scan, or junk)
        while (pos_ < d_.size() && d_[pos_] == 0xFF) ++pos_;   // fill bytes
        if (pos_ >= d_.size()) bad("the data ends before the end marker");
        const uint8_t m = d_[pos_++];
        if (m == 0x00) return nextMarker();   // a stuffed FF in data we are skipping
        return m;
    }

    void frame(std::span<const uint8_t> s) {
        if (s.size() < 6) bad("short frame header");
        if (s[0] != 8) bad(std::format("{}-bit samples (only 8-bit supported)", s[0]));
        height_ = s[1] << 8 | s[2];
        width_ = s[3] << 8 | s[4];
        const int nc = s[5];
        if (!width_ || !height_) bad("no size in the frame header");
        if (nc < 1 || s.size() < 6 + 3 * static_cast<std::size_t>(nc)) bad("bad frame header");
        comps_.clear();
        for (int i = 0; i < nc; ++i) {
            Component c;
            c.id = s[6 + 3 * i];
            c.h = s[7 + 3 * i] >> 4;
            c.v = s[7 + 3 * i] & 15;
            c.tq = s[8 + 3 * i] & 3;
            if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4) bad("bad sampling factors");
            comps_.push_back(c);
        }
        hmax_ = vmax_ = 1;
        for (const auto& c : comps_) hmax_ = std::max(hmax_, c.h), vmax_ = std::max(vmax_, c.v);
        mcusX_ = (width_ + 8 * hmax_ - 1) / (8 * hmax_);
        mcusY_ = (height_ + 8 * vmax_ - 1) / (8 * vmax_);
        planeW_ = mcusX_ * comps_[0].h * 8;
        plane_.assign(static_cast<std::size_t>(planeW_) * static_cast<std::size_t>(mcusY_ * comps_[0].v * 8), 0);
    }

    void tables(std::span<const uint8_t> s) {
        std::size_t i = 0;
        while (i + 17 <= s.size()) {
            const int tc = s[i] >> 4, th = s[i] & 15;
            if (tc > 1 || th > 3) bad("bad Huffman table id");
            int total = 0;
            for (int k = 0; k < 16; ++k) total += s[i + 1 + static_cast<std::size_t>(k)];
            if (total > 256 || i + 17 + static_cast<std::size_t>(total) > s.size()) bad("bad Huffman table");
            (tc ? ac_ : dc_)[static_cast<std::size_t>(th)].build(&s[i + 1], &s[i + 17], total);
            i += 17 + static_cast<std::size_t>(total);
        }
    }

    void quant(std::span<const uint8_t> s) {
        std::size_t i = 0;
        while (i < s.size()) {
            const int pq = s[i] >> 4, tq = s[i] & 15;
            if (tq > 3) bad("bad quantisation table id");
            const std::size_t n = pq ? 128 : 64;
            if (i + 1 + n > s.size()) bad("short quantisation table");
            auto& q = quant_[static_cast<std::size_t>(tq)];
            for (int k = 0; k < 64; ++k)
                q[kZigzag[k]] = pq ? static_cast<float>(s[i + 1 + 2 * static_cast<std::size_t>(k)] << 8 | s[i + 2 + 2 * static_cast<std::size_t>(k)])
                                   : static_cast<float>(s[i + 1 + static_cast<std::size_t>(k)]);
            i += 1 + n;
        }
    }

    /// One block: Huffman decoded always; dequantised and transformed into the plane when `dst` is given.
    void block(Bits& b, Component& c, uint8_t* dst) {
        const Huffman& dc = dc_[static_cast<std::size_t>(c.td)];
        const Huffman& ac = ac_[static_cast<std::size_t>(c.ta)];
        int s = decode(b, dc);
        if (s > 11) bad("damaged data (DC)");
        c.pred += s ? extend(b.get(s), s) : 0;
        float f[64];
        if (dst) std::fill(f, f + 64, 0.0f);
        bool acZero = true;
        for (int k = 1; k < 64;) {
            const int rs = decode(b, ac), r = rs >> 4;
            s = rs & 15;
            if (s == 0) {
                if (r != 15) break;   // end of block
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) bad("damaged data (AC)");
            const int v = extend(b.get(s), s);
            if (dst) {
                const int z = kZigzag[k];
                f[z] = static_cast<float>(v) * quant_[static_cast<std::size_t>(c.tq)][static_cast<std::size_t>(z)];
                acZero = false;
            }
            ++k;
        }
        if (!dst) return;
        f[0] = static_cast<float>(c.pred) * quant_[static_cast<std::size_t>(c.tq)][0];
        idct(f, acZero, dst, planeW_);
    }

    void scan(std::span<const uint8_t> s) {
        if (!width_) bad("scan before the frame header");
        if (s.empty()) bad("short scan header");
        const int ns = s[0];
        if (ns < 1 || ns > 4 || s.size() < 1 + 2 * static_cast<std::size_t>(ns) + 3) bad("bad scan header");
        std::vector<int> in;   // indices into comps_
        for (int i = 0; i < ns; ++i) {
            const int id = s[1 + 2 * i];
            const auto it = std::ranges::find(comps_, id, &Component::id);
            if (it == comps_.end()) bad("scan of a component the frame doesn't have");
            it->td = s[2 + 2 * i] >> 4;
            it->ta = s[2 + 2 * i] & 15;
            if (it->td > 3 || it->ta > 3) bad("bad table id in a scan");
            in.push_back(static_cast<int>(it - comps_.begin()));
        }
        const std::size_t p = 1 + 2 * static_cast<std::size_t>(ns);
        if (s[p] != 0 || s[p + 1] != 63 || s[p + 2] != 0) bad("progressive scan (not supported)");
        const bool hasY = std::ranges::find(in, 0) != in.end();
        if (!hasY) return;   // colour only: nextMarker() skips its data
        for (int i : in) {
            const auto& c = comps_[static_cast<std::size_t>(i)];
            if (!dc_[static_cast<std::size_t>(c.td)].defined || !ac_[static_cast<std::size_t>(c.ta)].defined) bad("scan without its Huffman tables");
        }
        sawY_ = true;
        for (auto& c : comps_) c.pred = 0;
        Bits b{d_.data() + pos_, d_.data() + d_.size()};
        auto& y = comps_[0];
        int sinceRestart = 0;
        auto maybeRestart = [&](bool last) {
            if (!restart_ || last || ++sinceRestart < restart_) return;
            sinceRestart = 0;
            b.restart();
            for (auto& c : comps_) c.pred = 0;
        };
        if (ns == 1) {   // Y on its own: its blocks in raster order, as many as cover its own (subsampled) size
            const int cw = (width_ * y.h + hmax_ - 1) / hmax_, ch = (height_ * y.v + vmax_ - 1) / vmax_;
            const int bw = (cw + 7) / 8, bh = (ch + 7) / 8;
            for (int by = 0; by < bh; ++by)
                for (int bx = 0; bx < bw; ++bx) {
                    block(b, y, plane_.data() + static_cast<std::size_t>(by * 8) * static_cast<std::size_t>(planeW_) + static_cast<std::size_t>(bx * 8));
                    maybeRestart(by == bh - 1 && bx == bw - 1);
                }
        } else {
            for (int my = 0; my < mcusY_; ++my)
                for (int mx = 0; mx < mcusX_; ++mx) {
                    for (int i : in) {
                        auto& c = comps_[static_cast<std::size_t>(i)];
                        for (int v = 0; v < c.v; ++v)
                            for (int h = 0; h < c.h; ++h) {
                                uint8_t* dst = nullptr;
                                if (i == 0)
                                    dst = plane_.data() + static_cast<std::size_t>((my * c.v + v) * 8) * static_cast<std::size_t>(planeW_) +
                                          static_cast<std::size_t>((mx * c.h + h) * 8);
                                block(b, c, dst);
                            }
                    }
                    maybeRestart(my == mcusY_ - 1 && mx == mcusX_ - 1);
                }
        }
        pos_ = static_cast<std::size_t>(b.p - d_.data());   // nextMarker() finds what follows
    }

    std::span<const uint8_t> d_;
    std::size_t pos_ = 0;
    int width_ = 0, height_ = 0, hmax_ = 1, vmax_ = 1, mcusX_ = 0, mcusY_ = 0, planeW_ = 0, restart_ = 0;
    bool sawY_ = false;
    std::vector<Component> comps_;
    std::array<Huffman, 4> dc_, ac_;
    std::array<std::array<float, 64>, 4> quant_{};
    std::vector<uint8_t> plane_;
};

}  // namespace

Size size(std::span<const uint8_t> data) { return Decoder(data).run(nullptr); }

Size decodeLuma(std::span<const uint8_t> data, std::vector<uint8_t>& out) { return Decoder(data).run(&out); }

}  // namespace slopcap::jpeg

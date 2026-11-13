#include "fakes/jpeg_writer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace fake {

namespace {

constexpr uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

constexpr uint8_t kLumaQ[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,  14, 13, 16, 24,  40,  57,
                                69, 56, 14, 17, 22,  29,  51,  87,  80, 62, 18, 22, 37,  56,  68,  109, 103, 77, 24, 35, 55, 64,
                                81, 104, 113, 92, 49, 64,  78,  87,  103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr uint8_t kChromaQ[8] = {17, 18, 24, 47, 99, 99, 99, 99};   // first row; the table is symmetric-ish, rest 99

struct Code {
    uint16_t bits = 0;
    uint8_t len = 0;
};

struct Table {
    std::array<uint8_t, 16> counts{};
    std::vector<uint8_t> symbols;
    std::array<Code, 256> codes{};
};

Table makeTable(std::array<uint8_t, 16> counts, std::vector<uint8_t> symbols) {
    Table t{counts, std::move(symbols), {}};
    int code = 0;
    std::size_t k = 0;
    for (int len = 1; len <= 16; ++len) {
        for (int i = 0; i < counts[static_cast<std::size_t>(len - 1)]; ++i)
            t.codes[t.symbols[k++]] = {static_cast<uint16_t>(code++), static_cast<uint8_t>(len)};
        code <<= 1;
    }
    return t;
}

const Table& dcTable() {
    static const Table t = makeTable({0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
    return t;
}

/// Every run/size symbol, the likely ones first; codes from 2 to 16 bits.
const Table& acTable() {
    static const Table t = [] {
        std::vector<uint8_t> syms{0x00};   // end of block
        std::vector<std::pair<int, uint8_t>> rs;
        for (int r = 0; r < 16; ++r)
            for (int s = 1; s <= 10; ++s) rs.emplace_back(r * 3 + s, static_cast<uint8_t>(r << 4 | s));
        std::ranges::stable_sort(rs, {}, &std::pair<int, uint8_t>::first);
        for (const auto& [rank, sym] : rs) {
            if (syms.size() == 20) syms.push_back(0xF0);   // sixteen zeros
            syms.push_back(sym);
        }
        return makeTable({0, 1, 1, 2, 4, 6, 8, 12, 16, 24, 30, 30, 10, 0, 0, 18}, syms);
    }();
    return t;
}

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int n = 0;
    void put(uint32_t bits, int len) {
        for (int i = len - 1; i >= 0; --i) {
            acc = acc << 1 | (bits >> i & 1);
            if (++n == 8) {
                out.push_back(static_cast<uint8_t>(acc));
                if (acc == 0xFF) out.push_back(0);   // byte stuffing
                acc = 0;
                n = 0;
            }
        }
    }
    void flush() {
        if (n) put((1u << (8 - n)) - 1, 8 - n);   // padded with ones
    }
};

int category(int v) {
    int a = std::abs(v), s = 0;
    while (a) ++s, a >>= 1;
    return s;
}

void putValue(BitWriter& w, int v, int s) {
    if (s) w.put(static_cast<uint32_t>(v < 0 ? v + (1 << s) - 1 : v), s);
}

struct Comp {
    int id, h, v, tq;
    int pred = 0;
};

class Encoder {
public:
    Encoder(const uint8_t* gray, int w, int h, const JpegOptions& o) : g_(gray), w_(w), h_(h), o_(o) {
        const int scale = o.quality < 50 ? 5000 / std::max(1, o.quality) : 200 - 2 * o.quality;
        for (int i = 0; i < 64; ++i) {
            q_[0][static_cast<std::size_t>(i)] = std::clamp((kLumaQ[i] * scale + 50) / 100, 1, 255);
            const int c = i / 8 < 4 && i % 8 < 4 ? kChromaQ[std::max(i / 8, i % 8)] : 99;
            q_[1][static_cast<std::size_t>(i)] = std::clamp((c * scale + 50) / 100, 1, 255);
        }
        if (o.color) comps_ = {{1, o.yh, o.yv, 0}, {2, o.ch, o.cv, 1}, {3, o.ch, o.cv, 1}};
        else comps_ = {{1, 1, 1, 0}};
        for (const auto& c : comps_) hmax_ = std::max(hmax_, c.h), vmax_ = std::max(vmax_, c.v);
    }

    std::vector<uint8_t> run() {
        marker(0xD8);
        if (o_.exifThumbnail) {
            std::vector<uint8_t> thumb(16 * 16, 90);
            JpegOptions t;
            t.color = false;
            const auto small = Encoder(thumb.data(), 16, 16, t).run();
            const uint8_t head[] = {'E', 'x', 'i', 'f', 0, 0, 'M', 'M', 0, 42, 0, 0, 0, 8};
            std::vector<uint8_t> seg(sizeof head + small.size());
            std::copy(std::begin(head), std::end(head), seg.begin());
            std::copy(small.begin(), small.end(), seg.begin() + sizeof head);   // the thumbnail, markers and all
            segment(0xE1, seg);
        }
        std::vector<uint8_t> dqt;
        for (int t = 0; t < (o_.color ? 2 : 1); ++t) {
            dqt.push_back(static_cast<uint8_t>(t));
            for (int k = 0; k < 64; ++k) dqt.push_back(static_cast<uint8_t>(q_[static_cast<std::size_t>(t)][kZigzag[k]]));
        }
        segment(0xDB, dqt);
        std::vector<uint8_t> sof = {8, static_cast<uint8_t>(h_ >> 8), static_cast<uint8_t>(h_), static_cast<uint8_t>(w_ >> 8),
                                    static_cast<uint8_t>(w_), static_cast<uint8_t>(comps_.size())};
        for (const auto& c : comps_)
            sof.insert(sof.end(), {static_cast<uint8_t>(c.id), static_cast<uint8_t>(c.h << 4 | c.v), static_cast<uint8_t>(c.tq)});
        segment(0xC0, sof);
        std::vector<uint8_t> dht;
        for (int id = 0; id < 2; ++id)
            for (const Table* t : {&dcTable(), &acTable()}) {
                dht.push_back(static_cast<uint8_t>((t == &acTable() ? 0x10 : 0) | id));
                dht.insert(dht.end(), t->counts.begin(), t->counts.end());
                dht.insert(dht.end(), t->symbols.begin(), t->symbols.end());
            }
        segment(0xC4, dht);
        if (o_.restart) segment(0xDD, {static_cast<uint8_t>(o_.restart >> 8), static_cast<uint8_t>(o_.restart)});
        if (o_.separateScans) {
            for (std::size_t i = 0; i < comps_.size(); ++i) scan({i});
        } else {
            std::vector<std::size_t> all(comps_.size());
            for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
            scan(all);
        }
        marker(0xD9);
        return std::move(out_);
    }

private:
    void marker(uint8_t m) { out_.insert(out_.end(), {0xFF, m}); }
    void segment(uint8_t m, const std::vector<uint8_t>& body) {
        marker(m);
        const auto n = body.size() + 2;
        out_.insert(out_.end(), {static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)});
        out_.insert(out_.end(), body.begin(), body.end());
    }

    /// Component c's sample at (x, y) in its own (possibly subsampled) coordinates.
    double sample(std::size_t c, int x, int y) const {
        const auto& k = comps_[c];
        const int sx = std::clamp(x * hmax_ / k.h, 0, w_ - 1), sy = std::clamp(y * vmax_ / k.v, 0, h_ - 1);
        if (c == 0) return g_[static_cast<std::size_t>(sy) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(sx)];
        return 128 + (c == 1 ? 50 * std::sin(sx / 9.0) : 40 * std::cos(sy / 7.0));
    }

    void block(BitWriter& w, std::size_t c, int bx, int by) {
        static const auto cosines = [] {
            std::array<std::array<double, 8>, 8> m{};
            for (int x = 0; x < 8; ++x)
                for (int u = 0; u < 8; ++u)
                    m[static_cast<std::size_t>(x)][static_cast<std::size_t>(u)] =
                        (u == 0 ? std::sqrt(0.5) : 1.0) / 2 * std::cos((2 * x + 1) * u * std::numbers::pi / 16);
            return m;
        }();
        double f[8][8], tmp[8][8];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) f[y][x] = sample(c, bx * 8 + x, by * 8 + y) - 128;
        for (int y = 0; y < 8; ++y)
            for (int u = 0; u < 8; ++u) {
                double s = 0;
                for (int x = 0; x < 8; ++x) s += cosines[static_cast<std::size_t>(x)][static_cast<std::size_t>(u)] * f[y][x];
                tmp[y][u] = s;
            }
        int q[64];
        const auto& qt = q_[static_cast<std::size_t>(comps_[c].tq)];
        for (int v = 0; v < 8; ++v)
            for (int u = 0; u < 8; ++u) {
                double s = 0;
                for (int y = 0; y < 8; ++y) s += cosines[static_cast<std::size_t>(y)][static_cast<std::size_t>(v)] * tmp[y][u];
                q[v * 8 + u] = static_cast<int>(std::lround(s / qt[static_cast<std::size_t>(v * 8 + u)]));
            }
        const int diff = q[0] - comps_[c].pred;
        comps_[c].pred = q[0];
        const int s = category(diff);
        w.put(dcTable().codes[static_cast<std::size_t>(s)].bits, dcTable().codes[static_cast<std::size_t>(s)].len);
        putValue(w, diff, s);
        int run = 0;
        for (int k = 1; k < 64; ++k) {
            const int v = q[kZigzag[k]];
            if (v == 0) {
                ++run;
                continue;
            }
            while (run > 15) {
                w.put(acTable().codes[0xF0].bits, acTable().codes[0xF0].len);
                run -= 16;
            }
            const int sz = category(v);
            const auto sym = static_cast<std::size_t>(run << 4 | sz);
            w.put(acTable().codes[sym].bits, acTable().codes[sym].len);
            putValue(w, v, sz);
            run = 0;
        }
        if (run) w.put(acTable().codes[0].bits, acTable().codes[0].len);
    }

    void scan(const std::vector<std::size_t>& in) {
        std::vector<uint8_t> sos = {static_cast<uint8_t>(in.size())};
        for (auto c : in) sos.insert(sos.end(), {static_cast<uint8_t>(comps_[c].id), static_cast<uint8_t>(comps_[c].tq ? 0x11 : 0x00)});
        sos.insert(sos.end(), {0, 63, 0});
        segment(0xDA, sos);
        for (auto& c : comps_) c.pred = 0;
        BitWriter w{out_};
        int units = 0, rst = 0, total = 0;
        auto unitDone = [&](bool last) {
            if (!o_.restart || last || ++units < o_.restart) return;
            units = 0;
            w.flush();
            marker(static_cast<uint8_t>(0xD0 + (rst++ & 7)));
            for (auto& c : comps_) c.pred = 0;
        };
        if (in.size() == 1) {
            const auto c = in[0];
            const int cw = (w_ * comps_[c].h + hmax_ - 1) / hmax_, chh = (h_ * comps_[c].v + vmax_ - 1) / vmax_;
            const int bw = (cw + 7) / 8, bh = (chh + 7) / 8;
            total = bw * bh;
            for (int by = 0, n = 0; by < bh; ++by)
                for (int bx = 0; bx < bw; ++bx, ++n) {
                    block(w, c, bx, by);
                    unitDone(n == total - 1);
                }
        } else {
            const int mx = (w_ + 8 * hmax_ - 1) / (8 * hmax_), my = (h_ + 8 * vmax_ - 1) / (8 * vmax_);
            total = mx * my;
            for (int y = 0, n = 0; y < my; ++y)
                for (int x = 0; x < mx; ++x, ++n) {
                    for (auto c : in)
                        for (int v = 0; v < comps_[c].v; ++v)
                            for (int h = 0; h < comps_[c].h; ++h) block(w, c, x * comps_[c].h + h, y * comps_[c].v + v);
                    unitDone(n == total - 1);
                }
        }
        w.flush();
    }

    const uint8_t* g_;
    int w_, h_;
    JpegOptions o_;
    std::array<std::array<int, 64>, 2> q_{};
    std::vector<Comp> comps_;
    int hmax_ = 1, vmax_ = 1;
    std::vector<uint8_t> out_;
};

}  // namespace

std::vector<uint8_t> encodeJpeg(const uint8_t* gray, int width, int height, const JpegOptions& o) { return Encoder(gray, width, height, o).run(); }

}  // namespace fake

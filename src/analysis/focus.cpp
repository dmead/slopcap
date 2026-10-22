#include "analysis/focus.hpp"

#include "util/simd.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace slopcap::analysis {

const char* metricName(Metric m) {
    switch (m) {
        case Metric::Gradient: return "gradient";
        case Metric::Laplacian: return "laplacian";
        case Metric::Variance: return "variance";
        case Metric::Fourier: return "fourier";
    }
    return "?";
}

Metric metricFromName(std::string_view n) {
    for (Metric m : {Metric::Gradient, Metric::Laplacian, Metric::Variance, Metric::Fourier})
        if (n == metricName(m)) return m;
    throw std::invalid_argument("unknown focus metric: " + std::string(n));
}

namespace {

/// Where each colour sits in the 2x2 Bayer cell. Mono: one channel, a 1-pixel cell.
struct Layout {
    int cell = 2;
    int channels = 3;
    int sx[3][2] = {}, sy[3][2] = {};   // per channel, one or two sites (G has two)
    int sites[3] = {1, 2, 1};
};

Layout layoutOf(Bayer b) {
    Layout l;
    if (b == Bayer::None) {
        l.cell = 1;
        l.channels = 1;
        l.sites[0] = 1;
        return l;
    }
    int rx = 0, ry = 0;   // red site; blue is diagonally opposite, greens are the other two
    switch (b) {
        case Bayer::GRBG: rx = 1, ry = 0; break;
        case Bayer::GBRG: rx = 0, ry = 1; break;
        case Bayer::BGGR: rx = 1, ry = 1; break;
        default: break;
    }
    l.sx[0][0] = rx, l.sy[0][0] = ry;
    l.sx[2][0] = 1 - rx, l.sy[2][0] = 1 - ry;
    l.sx[1][0] = 1 - rx, l.sy[1][0] = ry;
    l.sx[1][1] = rx, l.sy[1][1] = 1 - ry;
    return l;
}

/// One raw sample, 0..1 (only used by the sparse locate pass).
inline float sampleAt(const std::byte* p, int width, int bits, int x, int y) {
    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
    if (bits > 8) {
        uint16_t v;
        std::memcpy(&v, p + 2 * i, 2);
        return static_cast<float>(v) * (1.0f / 65535.0f);
    }
    return static_cast<float>(std::to_integer<uint8_t>(p[i])) * (1.0f / 255.0f);
}

// Vectorized: runs over every pixel of the window, for every channel and frame. A plain strided loop over a typed
// pointer (no per-pixel branch on bit depth, no per-pixel call) lets GCC turn it into wide loads + shuffles + converts.
template <typename T>
void extractRow(const T* row, int stride, int count, float scale, float* out) {
    for (int i = 0; i < count; ++i) out[i] = static_cast<float>(row[i * stride]) * scale;
}
template <typename T>
void extractRowAvg(const T* a, const T* b, int stride, int count, float scale, float* out) {
    const float half = 0.5f * scale;
    for (int i = 0; i < count; ++i) out[i] = (static_cast<float>(a[i * stride]) + static_cast<float>(b[i * stride])) * half;
}

/// The q-quantile of values in 0..1, to 1/4096 of full scale. A histogram instead of std::nth_element: profiling showed
/// the selection sorts taking ~40% of scoring time, and the values are bounded, so one linear pass does it.
float quantile01(const std::vector<float>& v, double q) {
    constexpr int kBins = 4096;
    std::vector<uint32_t> h(kBins, 0);
    for (float x : v) ++h[static_cast<std::size_t>(std::clamp(static_cast<int>(x * (kBins - 1) + 0.5f), 0, kBins - 1))];
    const auto target = static_cast<uint64_t>(q * static_cast<double>(v.size()));
    uint64_t seen = 0;
    for (int b = 0; b < kBins; ++b) {
        seen += h[static_cast<std::size_t>(b)];
        if (seen > target) return static_cast<float>(b) / (kBins - 1);
    }
    return 1.0f;
}

/// A window of one channel, row-major floats.
struct Plane {
    int w = 0, h = 0;
    std::vector<float> v;
    const float* row(int y) const { return v.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w); }
    float* row(int y) { return v.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w); }
};

template <typename T>
Plane extract(const std::byte* pixels, int width, const Layout& l, int c, int px0, int py0, int pw, int ph, float scale) {
    Plane p{pw, ph, std::vector<float>(static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph))};
    const T* base = reinterpret_cast<const T*>(pixels);   // 16-bit samples are little-endian, like the host
    for (int y = 0; y < ph; ++y) {
        const int fy = (py0 + y) * l.cell;
        auto src = [&](int site) {
            return base + static_cast<std::size_t>(fy + l.sy[c][site]) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(px0 * l.cell + l.sx[c][site]);
        };
        if (l.sites[c] == 2) extractRowAvg(src(0), src(1), l.cell, pw, scale, p.row(y));
        else extractRow(src(0), l.cell, pw, scale, p.row(y));
    }
    return p;
}

/// Sky level (low percentile of a sparse sample), peak and clipped fraction of a plane.
void planeLevels(const Plane& p, float& bg, float& peak, double& clipped) {
    std::vector<float> s;
    const int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(p.w) * p.h / 16384.0)));
    for (int y = 0; y < p.h; y += step)
        for (int x = 0; x < p.w; x += step) s.push_back(p.row(y)[x]);
    bg = quantile01(s, 0.25);   // planets cover a small part of the frame, so a low quantile is the sky
    peak = 0;
    std::size_t clip = 0;
    for (int y = 0; y < p.h; ++y) {
        const float* r = p.row(y);
        int32_t m = 0;
        int c = 0;
        // Vectorized: max and a select-count over the whole window. The max compares bit patterns: a plane holds no
        // negative values, and non-negative floats order like their bits. An integer max vectorizes on every compiler;
        // clang won't vectorize a float max (a NaN could make the order of the lanes matter).
        SLOPCAP_SIMD_SUM_MAX(c, m)
        for (int x = 0; x < p.w; ++x) {
            m = std::max(m, std::bit_cast<int32_t>(r[x]));
            c += r[x] >= 0.995f ? 1 : 0;
        }
        peak = std::max(peak, std::bit_cast<float>(m));
        clip += static_cast<std::size_t>(c);
    }
    clipped = static_cast<double>(clip) / (static_cast<double>(p.w) * p.h);
}

/// Squared signal above the sky, and the brightness centroid of the target (pixels above `thresh`), plane coordinates.
/// Vectorized: branch-free (the threshold is a select), float sums per row via SLOPCAP_SIMD_SUM, rows summed in double
/// so precision doesn't suffer.
void signalAndCentroid(const Plane& p, float bg, float thresh, double& signal2, double& cx, double& cy) {
    double s2 = 0, m = 0, mx = 0, my = 0;
    for (int y = 0; y < p.h; ++y) {
        const float* r = p.row(y);
        float rs2 = 0, rm = 0, rmx = 0;
        SLOPCAP_SIMD_SUM(rs2, rm, rmx)
        for (int x = 0; x < p.w; ++x) {
            const float s = r[x] - bg;
            rs2 += s * s;
            const float w = r[x] > thresh ? s : 0.0f;
            rm += w;
            rmx += w * static_cast<float>(x);
        }
        s2 += rs2;
        m += rm;
        mx += rmx;
        my += static_cast<double>(rm) * y;
    }
    signal2 = s2;
    cx = m > 0 ? mx / m : p.w / 2.0;
    cy = m > 0 ? my / m : p.h / 2.0;
}

// Vectorized: contiguous float arithmetic on three rows with a SIMD sum per row.
double gradientEnergy(const Plane& p) {
    double g = 0;
    for (int y = 1; y + 1 < p.h; ++y) {
        const float *up = p.row(y - 1), *r = p.row(y), *dn = p.row(y + 1);
        float rg = 0;
        SLOPCAP_SIMD_SUM(rg)
        for (int x = 1; x < p.w - 1; ++x) {
            const float dx = r[x + 1] - r[x - 1], dy = dn[x] - up[x];
            rg += dx * dx + dy * dy;
        }
        g += rg;
    }
    return g;
}

// Vectorized: as gradientEnergy.
double laplacianEnergy(const Plane& p) {
    double g = 0;
    for (int y = 1; y + 1 < p.h; ++y) {
        const float *up = p.row(y - 1), *r = p.row(y), *dn = p.row(y + 1);
        float rg = 0;
        SLOPCAP_SIMD_SUM(rg)
        for (int x = 1; x < p.w - 1; ++x) {
            const float l = r[x - 1] + r[x + 1] + up[x] + dn[x] - 4.0f * r[x];
            rg += l * l;
        }
        g += rg;
    }
    return g;
}

// Vectorized: two SIMD sums per row.
double normalisedVariance(const Plane& p, float bg) {
    double s = 0, s2 = 0;
    for (int y = 0; y < p.h; ++y) {
        const float* r = p.row(y);
        float rs = 0, rs2 = 0;
        SLOPCAP_SIMD_SUM(rs, rs2)
        for (int x = 0; x < p.w; ++x) {
            const float v = r[x] - bg;
            rs += v;
            rs2 += v * v;
        }
        s += rs;
        s2 += rs2;
    }
    const double n = static_cast<double>(p.w) * p.h, mean = s / n;
    return mean > 0 ? (s2 / n - mean * mean) / (mean * mean) : 0;
}

/// In-place radix-2 FFT of n = 2^k complex values.
void fft(std::complex<float>* a, int n, const std::vector<std::complex<float>>& tw) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int step = n / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * tw[static_cast<std::size_t>(k * step)];
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
    }
}

double bandPowerShare(const Plane& p, float bg, double lo, double hi) {
    const int n = p.w;   // square, power of two
    std::vector<std::complex<float>> tw(static_cast<std::size_t>(n / 2));
    for (int k = 0; k < n / 2; ++k) tw[static_cast<std::size_t>(k)] = std::polar(1.0f, static_cast<float>(-2 * std::numbers::pi * k / n));
    std::vector<float> hann(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) hann[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / (n - 1)));
    std::vector<std::complex<float>> f(static_cast<std::size_t>(n) * static_cast<std::size_t>(n)), col(static_cast<std::size_t>(n));
    for (int y = 0; y < n; ++y) {
        const float* r = p.row(y);
        auto* o = f.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(n);
        for (int x = 0; x < n; ++x) o[x] = (r[x] - bg) * hann[static_cast<std::size_t>(x)] * hann[static_cast<std::size_t>(y)];
        fft(o, n, tw);
    }
    double band = 0, total = 0;
    for (int x = 0; x < n; ++x) {
        for (int y = 0; y < n; ++y) col[static_cast<std::size_t>(y)] = f[static_cast<std::size_t>(y) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x)];
        fft(col.data(), n, tw);
        const int u = x <= n / 2 ? x : x - n;
        for (int y = 0; y < n; ++y) {
            const int v = y <= n / 2 ? y : y - n;
            if (u == 0 && v == 0) continue;   // DC: overall brightness, not detail
            const double r = std::sqrt(static_cast<double>(u * u + v * v)) / (n / 2.0);
            const double pw = std::norm(col[static_cast<std::size_t>(y)]);
            if (r <= 1.0) total += pw;
            if (r >= lo && r < hi) band += pw;
        }
    }
    return total > 0 ? band / total : 0;
}

}  // namespace

FocusScores scoreFrame(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer, const FocusOptions& opt) {
    FocusScores out;
    const std::size_t bpp = bitDepth > 8 ? 2 : 1;
    if (width < 8 || height < 8 || pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bpp) return out;
    const Layout l = layoutOf(bayer);
    const int pw = width / l.cell, ph = height / l.cell;   // plane size
    const int refC = l.channels == 3 ? 1 : 0;              // locate on green (or mono)

    // 1. Locate the target on a sparse sample (~16k points) of the reference channel. It only has to place the window,
    //    and this pass (scattered reads + a percentile) doesn't vectorize, so it's kept small.
    const int step = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(pw) * ph / 16384.0))));
    std::vector<float> s;
    s.reserve(static_cast<std::size_t>((pw / step + 1) * (ph / step + 1)));
    auto refAt = [&](int x, int y) {
        float v = 0;
        for (int k = 0; k < l.sites[refC]; ++k)
            v += sampleAt(pixels.data(), width, bitDepth, x * l.cell + l.sx[refC][k], y * l.cell + l.sy[refC][k]);
        return v / static_cast<float>(l.sites[refC]);
    };
    // Sparse grid of the reference channel, then a 3x3 box blur on it: single noisy or hot samples must not set the
    // peak, the threshold or the target's bounding box.
    const int gw = (pw + step - 1) / step, gh = (ph + step - 1) / step;
    for (int y = 0; y < ph; y += step)
        for (int x = 0; x < pw; x += step) s.push_back(refAt(x, y));
    std::vector<float> g(s.size());
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            float sum = 0;
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= gw || yy >= gh) continue;
                    sum += s[static_cast<std::size_t>(yy) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(xx)];
                    ++n;
                }
            g[static_cast<std::size_t>(y) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(x)] = sum / static_cast<float>(n);
        }
    const float bg = quantile01(g, 0.25);
    const float peak = *std::max_element(g.begin(), g.end());
    if (peak - bg < 0.02f) return out;   // nothing there (dark frame, cloud)
    const float thresh = bg + 0.15f * (peak - bg);
    double m = 0, mx = 0, my = 0;
    int bx0 = gw, by0 = gh, bx1 = 0, by1 = 0;
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x)
            if (const float v = g[static_cast<std::size_t>(y) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(x)]; v > thresh) {
                const double w = v - bg;
                m += w, mx += w * x * step, my += w * y * step;
                bx0 = std::min(bx0, x), by0 = std::min(by0, y), bx1 = std::max(bx1, x), by1 = std::max(by1, y);
            }
    out.targetX0 = bx0 * step * l.cell, out.targetY0 = by0 * step * l.cell;
    out.targetX1 = std::min(width, (bx1 + 1) * step * l.cell), out.targetY1 = std::min(height, (by1 + 1) * step * l.cell);
    const int cx = static_cast<int>(mx / m), cy = static_cast<int>(my / m);

    // 2. Window around it (a square power of two for the FFT), clamped to the frame.
    int win = std::min({opt.window, pw, ph});
    if (opt.metric == Metric::Fourier) win = 1 << static_cast<int>(std::floor(std::log2(std::max(8, win))));
    const int ww = std::min(win, pw), wh = std::min(win, ph);
    const int x0 = std::clamp(cx - ww / 2, 0, pw - ww), y0 = std::clamp(cy - wh / 2, 0, ph - wh);
    out.windowX = x0 * l.cell, out.windowY = y0 * l.cell, out.windowW = ww * l.cell, out.windowH = wh * l.cell;

    // 3. Extract and score each channel on the same window.
    out.channels = l.channels;
    for (int c = 0; c < l.channels; ++c) {
        const Plane p = bpp == 2 ? extract<uint16_t>(pixels.data(), width, l, c, x0, y0, ww, wh, 1.0f / 65535.0f)
                                 : extract<uint8_t>(pixels.data(), width, l, c, x0, y0, ww, wh, 1.0f / 255.0f);
        ChannelScore& sc = out.ch[static_cast<std::size_t>(c)];
        float pbg = 0, ppeak = 0;
        planeLevels(p, pbg, ppeak, sc.clipped);
        sc.background = pbg;
        sc.peak = ppeak;
        double signal2 = 0, pcx = 0, pcy = 0;
        signalAndCentroid(p, pbg, pbg + static_cast<float>(opt.centroidLevel) * (ppeak - pbg), signal2, pcx, pcy);
        double offX = 0, offY = 0;   // centroid back to full-frame pixels (centre of this channel's site(s))
        for (int k = 0; k < l.sites[c]; ++k) offX += l.sx[c][k], offY += l.sy[c][k];
        sc.cx = (x0 + pcx) * l.cell + offX / l.sites[c];
        sc.cy = (y0 + pcy) * l.cell + offY / l.sites[c];
        switch (opt.metric) {
            case Metric::Gradient: sc.sharpness = signal2 > 0 ? gradientEnergy(p) / signal2 : 0; break;
            case Metric::Laplacian: sc.sharpness = signal2 > 0 ? laplacianEnergy(p) / signal2 : 0; break;
            case Metric::Variance: sc.sharpness = normalisedVariance(p, pbg); break;
            case Metric::Fourier: sc.sharpness = bandPowerShare(p, pbg, opt.bandLow, opt.bandHigh); break;
        }
    }
    return out;
}

}  // namespace slopcap::analysis

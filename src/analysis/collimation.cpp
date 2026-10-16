#include "analysis/collimation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <vector>

namespace slopcap::analysis {

namespace {

/// Luminance image: 2x2 cell sums for Bayer (half resolution, all colours), the frame itself for mono.
struct Lum {
    int w = 0, h = 0, cell = 1;
    std::vector<float> v;
    float at(double x, double y) const {
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        if (x0 < 0 || y0 < 0 || x0 + 1 >= w || y0 + 1 >= h) return 0;
        const float fx = static_cast<float>(x - x0), fy = static_cast<float>(y - y0);
        auto p = [&](int xx, int yy) { return v[static_cast<std::size_t>(yy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(xx)]; };
        return (p(x0, y0) * (1 - fx) + p(x0 + 1, y0) * fx) * (1 - fy) + (p(x0, y0 + 1) * (1 - fx) + p(x0 + 1, y0 + 1) * fx) * fy;
    }
};

template <typename T>
Lum luminance(const std::byte* px, int width, int height, int cell) {
    Lum l;
    l.cell = cell;
    l.w = width / cell;
    l.h = height / cell;
    l.v.assign(static_cast<std::size_t>(l.w) * static_cast<std::size_t>(l.h), 0.0f);
    const T* p = reinterpret_cast<const T*>(px);
    for (int y = 0; y < l.h; ++y)
        for (int dy = 0; dy < cell; ++dy) {
            const T* row = p + static_cast<std::size_t>(y * cell + dy) * static_cast<std::size_t>(width);
            float* out = l.v.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(l.w);
            for (int x = 0; x < l.w; ++x)
                for (int dx = 0; dx < cell; ++dx) out[x] += static_cast<float>(row[x * cell + dx]);
        }
    return l;
}

struct Circle {
    double x = 0, y = 0, r = 0, rms = 0;
    bool ok = false;
};

/// Algebraic (Kasa) circle fit: least squares on x² + y² + D x + E y + F = 0.
Circle fitCircle(const std::vector<std::pair<double, double>>& pts) {
    Circle c;
    if (pts.size() < 6) return c;
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, sz = 0, sxz = 0, syz = 0;
    const double n = static_cast<double>(pts.size());
    for (auto [x, y] : pts) {
        const double z = x * x + y * y;
        sx += x, sy += y, sxx += x * x, syy += y * y, sxy += x * y, sz += z, sxz += x * z, syz += y * z;
    }
    // normal equations for [D E F]
    const double M[3][3] = {{sxx, sxy, sx}, {sxy, syy, sy}, {sx, sy, n}};
    const double v[3] = {-sxz, -syz, -sz};
    const double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                       M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    if (std::abs(det) < 1e-12) return c;
    auto det3 = [&](int col) {
        double A[3][3];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) A[r][k] = k == col ? v[r] : M[r][k];
        return A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
               A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    };
    const double D = det3(0) / det, E = det3(1) / det, F = det3(2) / det;
    c.x = -D / 2, c.y = -E / 2;
    const double r2 = c.x * c.x + c.y * c.y - F;
    if (r2 <= 0) return c;
    c.r = std::sqrt(r2);
    double ss = 0;
    for (auto [x, y] : pts) ss += std::pow(std::hypot(x - c.x, y - c.y) - c.r, 2);
    c.rms = std::sqrt(ss / n);
    c.ok = true;
    return c;
}

}  // namespace

DonutResult analyzeDonut(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer) {
    DonutResult out;
    const int cell = bayer == Bayer::None ? 1 : 2;
    if (width < 16 || height < 16) return out;
    const Lum L = bitDepth > 8 ? luminance<uint16_t>(pixels.data(), width, height, cell) : luminance<uint8_t>(pixels.data(), width, height, cell);

    // Sky and star levels; the star is the bright blob.
    std::vector<float> s;
    const int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(L.w) * L.h / 40000.0)));
    for (int y = 0; y < L.h; y += step)
        for (int x = 0; x < L.w; x += step) s.push_back(L.v[static_cast<std::size_t>(y) * static_cast<std::size_t>(L.w) + static_cast<std::size_t>(x)]);
    std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(s.size() / 2), s.end());
    const double sky = s[s.size() / 2];
    const double peak = *std::max_element(L.v.begin(), L.v.end());
    if (peak - sky < 1e-6 || peak < sky * 1.5 + 4 * cell * cell) {
        out.problem = "no star";
        return out;
    }
    const double t = sky + 0.25 * (peak - sky);
    double m = 0, mx = 0, my = 0;
    for (int y = 0; y < L.h; ++y)
        for (int x = 0; x < L.w; ++x)
            if (const double v = L.v[static_cast<std::size_t>(y) * static_cast<std::size_t>(L.w) + static_cast<std::size_t>(x)]; v > t) m += v - sky, mx += (v - sky) * x, my += (v - sky) * y;
    double cx = mx / m, cy = my / m;

    // Radial scans from the centre estimate; twice, the second from the fitted ring centre.
    Circle outer, inner;
    constexpr int kRays = 90;
    const double maxR = std::min(L.w, L.h) / 2.0;
    bool touches = false;
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<std::pair<double, double>> op, ip;
        int holes = 0;
        for (int k = 0; k < kRays; ++k) {
            const double a = 2 * std::numbers::pi * k / kRays, ca = std::cos(a), sa = std::sin(a);
            std::vector<double> prof;
            for (double r = 0; r < maxR; r += 0.5) prof.push_back(L.at(cx + r * ca, cy + r * sa));
            const double top = *std::max_element(prof.begin(), prof.end());
            const double lvl = sky + 0.5 * (top - sky);
            // outer edge: last fall through lvl; inner edge: first rise through lvl
            std::size_t last = 0, first = prof.size();
            for (std::size_t i = 0; i + 1 < prof.size(); ++i) {
                if (prof[i] >= lvl && prof[i + 1] < lvl) last = i;
                if (first == prof.size() && prof[i] < lvl && prof[i + 1] >= lvl) first = i;
            }
            if (last == 0) continue;
            if (last + 4 >= prof.size()) touches = true;
            const double ro = (last + (prof[last] - lvl) / (prof[last] - prof[last + 1])) * 0.5;
            op.emplace_back(cx + ro * ca, cy + ro * sa);
            if (prof[0] < lvl && first < last) {
                ++holes;
                const double ri = (first + (lvl - prof[first]) / (prof[first + 1] - prof[first])) * 0.5;
                ip.emplace_back(cx + ri * ca, cy + ri * sa);
            }
        }
        outer = fitCircle(op);
        if (!outer.ok) {
            out.problem = "no star";
            return out;
        }
        inner = holes >= kRays * 2 / 3 ? fitCircle(ip) : Circle{};
        cx = outer.x, cy = outer.y;
    }
    if (touches) {
        out.problem = "star touches the edge (centre it or defocus less)";
        return out;
    }
    if (!inner.ok) {
        out.problem = "defocus more (no shadow visible)";
        return out;
    }
    auto full = [&](double v, int off) { return v * cell + off * 0.5 * (cell - 1); };   // plane -> full-frame pixels
    out.found = true;
    out.outerX = full(outer.x, 1), out.outerY = full(outer.y, 1), out.outerR = outer.r * cell;
    out.innerX = full(inner.x, 1), out.innerY = full(inner.y, 1), out.innerR = inner.r * cell;
    out.offsetX = out.innerX - out.outerX, out.offsetY = out.innerY - out.outerY;
    out.offsetFraction = std::hypot(out.offsetX, out.offsetY) / out.outerR;
    out.fromCentreX = out.outerX - width / 2.0, out.fromCentreY = out.outerY - height / 2.0;
    out.outerRms = outer.rms * cell, out.innerRms = inner.rms * cell;
    return out;
}

}  // namespace slopcap::analysis

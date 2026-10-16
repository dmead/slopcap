// slopcap-diag: optical diagnostics from planetary recordings.
//   slopcap-diag [--png DIR] [--best PERCENT] FILE.ser...
// Per recording: stacks the sharpest frames (per-channel planes, aligned on the green centroid), then measures
//   dispersion   R vs B centroid offset (atmosphere; an ADC removes it)
//   limb width   FWHM of the brightness fall at the globe's limb (line-spread function), per channel, on both sides of the ring/equator axis. A side that is
//                always softer, in the same place on the sensor across recordings, points at the instrument
//                (collimation, tilted optics/corrector); a channel that is always softer points at focus differing by
//                colour (chromatic focus shift). Seeing softens everything evenly.
//   halo         light just off the limb relative to the globe (scatter: dew on the corrector, dirty optics)
//   jitter       frame-to-frame centroid RMS (seeing, wind, mount)
#include "analysis/focus.hpp"
#include "io/ser.hpp"

#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

using namespace slopcap;
namespace fs = std::filesystem;

namespace {

Bayer bayerOf(ser::ColorId c) {
    switch (c) {
        case ser::ColorId::BayerRGGB: return Bayer::RGGB;
        case ser::ColorId::BayerGRBG: return Bayer::GRBG;
        case ser::ColorId::BayerGBRG: return Bayer::GBRG;
        case ser::ColorId::BayerBGGR: return Bayer::BGGR;
        default: return Bayer::None;
    }
}

struct Plane {
    int w = 0, h = 0;
    std::vector<double> v;
    double at(double x, double y) const {   // bilinear, 0 outside
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        if (x0 < 0 || y0 < 0 || x0 + 1 >= w || y0 + 1 >= h) return 0;
        const double fx = x - x0, fy = y - y0;
        auto p = [&](int xx, int yy) { return v[static_cast<std::size_t>(yy) * w + xx]; };
        return (p(x0, y0) * (1 - fx) + p(x0 + 1, y0) * fx) * (1 - fy) + (p(x0, y0 + 1) * (1 - fx) + p(x0 + 1, y0 + 1) * fx) * fy;
    }
};

struct Geometry {
    double cx = 0, cy = 0;     // centroid, plane px
    double axis = 0;           // ring (major) axis angle, radians
    double sky = 0, globe = 0; // levels
};

double percentile(std::vector<double> v, double q) {
    auto it = v.begin() + static_cast<std::ptrdiff_t>(q * (v.size() - 1));
    std::nth_element(v.begin(), it, v.end());
    return *it;
}

Geometry geometry(const Plane& p) {
    Geometry g;
    g.sky = percentile(p.v, 0.10);
    g.globe = percentile(p.v, 0.995);
    const double t = g.sky + 0.4 * (g.globe - g.sky);
    double m = 0, mx = 0, my = 0;
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x)
            if (const double v = p.v[static_cast<std::size_t>(y) * p.w + x]; v > t) m += 1, mx += x, my += y;
    g.cx = mx / m, g.cy = my / m;
    double sxx = 0, syy = 0, sxy = 0;
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x)
            if (p.v[static_cast<std::size_t>(y) * p.w + x] > t) {
                const double dx = x - g.cx, dy = y - g.cy;
                sxx += dx * dx, syy += dy * dy, sxy += dx * dy;
            }
    g.axis = 0.5 * std::atan2(2 * sxy, sxx - syy);
    return g;
}

/// Limb along angle a from (cx, cy): the edge is where brightness falls fastest (limb darkening makes the inner
/// profile slope gently, so fixed-percentage levels would measure that instead). `width` is the FWHM of that fall
/// (the line-spread function: blur at the limb, in plane px); `radius` is where it peaks.
bool edge(const Plane& p, const Geometry& g, double a, double& width, double& radius) {
    const double maxR = std::min(p.w, p.h) * 0.5, step = 0.25;
    std::vector<double> prof;
    for (double r = 0; r < maxR; r += step) prof.push_back(p.at(g.cx + r * std::cos(a), g.cy + r * std::sin(a)));
    if (prof.size() < 20) return false;
    std::vector<double> d(prof.size(), 0.0);   // -d(profile)/dr, smoothed over +-1 px
    for (std::size_t i = 4; i + 4 < prof.size(); ++i) d[i] = (prof[i - 4] - prof[i + 4]) / (8 * step);
    std::size_t ipk = 0;
    for (std::size_t i = 0; i < d.size(); ++i)
        if (d[i] > d[ipk]) ipk = i;
    if (d[ipk] <= 0) return false;
    const double half = d[ipk] / 2;
    std::size_t lo = ipk, hi = ipk;
    while (lo > 0 && d[lo] > half) --lo;
    while (hi + 1 < d.size() && d[hi] > half) ++hi;
    if (lo == 0 || hi + 1 == d.size()) return false;
    auto interp = [&](std::size_t i0, std::size_t i1) { return i0 + (d[i0] - half) / (d[i0] - d[i1]) * (static_cast<double>(i1) - i0); };
    width = (interp(hi - 1, hi) - interp(lo + 1, lo)) * step;
    radius = ipk * step;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string pngDir;
    double bestPct = 10;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--png" && i + 1 < argc) pngDir = argv[++i];
        else if (a == "--best" && i + 1 < argc) bestPct = std::stod(argv[++i]);
        else files.push_back(a);
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: slopcap-diag [--png DIR] [--best PERCENT] FILE.ser...\n");
        return 2;
    }
    std::printf("%-30s %6s %5s | %-14s | %-23s %-23s | %5s | %6s\n", "file", "frames", "used", "disp R-B px", "limb FWHM N side R/G/B",
                "limb S side R/G/B", "halo", "jitter");
    for (const auto& f : files) {
        ser::Reader rd(f);
        const auto& h = rd.header();
        const Bayer bayer = bayerOf(h.colorId);
        if (bayer == Bayer::None) continue;
        const int bits = h.bitDepth > 8 ? 16 : 8;
        std::vector<std::byte> buf(h.frameBytes());
        const auto n = static_cast<int>(rd.frameCount());
        // pass 1: score every frame (cap at 6000 evenly spread)
        const int stride = std::max(1, n / 6000);
        struct Fs { int i; double s, cx, cy; };
        std::vector<Fs> sc;
        for (int i = 0; i < n; i += stride) {
            rd.readFrame(i, buf);
            const auto s = analysis::scoreFrame(buf, h.width, h.height, bits, bayer);
            if (s.channels == 3) sc.push_back({i, s.ch[1].sharpness, s.ch[1].cx, s.ch[1].cy});
        }
        if (sc.size() < 10) continue;
        double mx = 0, my = 0;
        for (auto& s : sc) mx += s.cx, my += s.cy;
        mx /= sc.size(), my /= sc.size();
        double jit = 0;
        for (std::size_t i = 1; i < sc.size(); ++i) jit += std::pow(sc[i].cx - sc[i - 1].cx, 2) + std::pow(sc[i].cy - sc[i - 1].cy, 2);
        jit = std::sqrt(jit / (2.0 * (sc.size() - 1)));
        std::sort(sc.begin(), sc.end(), [](auto& a, auto& b) { return a.s > b.s; });
        sc.resize(std::max<std::size_t>(10, static_cast<std::size_t>(sc.size() * bestPct / 100)));
        // pass 2: stack per-channel planes, shifted so the green centroid lands on the mean position
        const int pw = h.width / 2, ph = h.height / 2;
        Plane P[3];
        for (auto& p : P) p = {pw, ph, std::vector<double>(static_cast<std::size_t>(pw) * ph, 0.0)};
        int rx = 0, ry = 0;
        if (bayer == Bayer::GRBG) rx = 1;
        if (bayer == Bayer::GBRG) ry = 1;
        if (bayer == Bayer::BGGR) rx = 1, ry = 1;
        const double scale = bits > 8 ? 1.0 / 65535 : 1.0 / 255;
        auto raw = [&](int x, int y) {
            const std::size_t i = static_cast<std::size_t>(y) * h.width + x;
            return bits > 8 ? (std::to_integer<unsigned>(buf[2 * i]) | std::to_integer<unsigned>(buf[2 * i + 1]) << 8) * scale
                            : std::to_integer<unsigned>(buf[i]) * scale;
        };
        for (const auto& s : sc) {
            rd.readFrame(s.i, buf);
            const int dx = static_cast<int>(std::lround((mx - s.cx) / 2)), dy = static_cast<int>(std::lround((my - s.cy) / 2));
            for (int y = 0; y < ph; ++y) {
                const int sy = y - dy;
                if (sy < 0 || sy >= ph) continue;
                for (int x = 0; x < pw; ++x) {
                    const int sx = x - dx;
                    if (sx < 0 || sx >= pw) continue;
                    const std::size_t o = static_cast<std::size_t>(y) * pw + x;
                    P[0].v[o] += raw(2 * sx + rx, 2 * sy + ry);
                    P[1].v[o] += 0.5 * (raw(2 * sx + 1 - rx, 2 * sy + ry) + raw(2 * sx + rx, 2 * sy + 1 - ry));
                    P[2].v[o] += raw(2 * sx + 1 - rx, 2 * sy + 1 - ry);
                }
            }
        }
        for (auto& p : P)
            for (auto& v : p.v) v /= static_cast<double>(sc.size());
        Geometry G[3];
        for (int c = 0; c < 3; ++c) G[c] = geometry(P[c]);
        // limb edges around the minor axis (perpendicular to the rings), +-40 deg, on each side
        double wN[3] = {}, wS[3] = {}, rN = 0;
        for (int c = 0; c < 3; ++c) {
            Geometry g = G[1];   // same centre for all channels so widths compare
            g.sky = G[c].sky;
            for (int side = 0; side < 2; ++side) {
                double sum = 0;
                int cnt = 0;
                for (int k = -4; k <= 4; ++k) {
                    const double a = G[1].axis + (side ? -1 : 1) * std::numbers::pi / 2 + k * std::numbers::pi / 180 * 10;
                    double w = 0, r = 0;
                    if (edge(P[c], g, a, w, r)) {
                        sum += w, ++cnt;
                        if (c == 1 && side == 0) rN += r / 9;
                    }
                }
                (side ? wS : wN)[c] = cnt ? sum / cnt : std::nan("");
            }
        }
        // halo: mean level from 1.6R to 2.2R along the minor axis (both sides, +-25 deg), relative to globe
        double halo = 0;
        int hc = 0;
        for (int side = 0; side < 2; ++side)
            for (int k = -5; k <= 5; ++k)
                for (double fr = 1.6; fr <= 2.2; fr += 0.1) {
                    const double a = G[1].axis + (side ? -1 : 1) * std::numbers::pi / 2 + k * std::numbers::pi / 180 * 5;
                    halo += P[1].at(G[1].cx + fr * rN * std::cos(a), G[1].cy + fr * rN * std::sin(a)) - G[1].sky;
                    ++hc;
                }
        halo = hc ? halo / hc / (G[1].globe - G[1].sky) : 0;
        std::printf("%-30s %6d %5zu | %+5.2f,%+5.2f    | %4.1f %4.1f %4.1f  (r%3.0f) | %4.1f %4.1f %4.1f        | %4.1f%% | %5.2f\n",
                    fs::path(f).filename().string().c_str(), n, sc.size(), 2 * (G[0].cx - G[2].cx), 2 * (G[0].cy - G[2].cy), wN[0],
                    wN[1], wN[2], rN, wS[0], wS[1], wS[2], 100 * halo, jit);
        if (!pngDir.empty()) {
            QImage img(pw, ph, QImage::Format_RGB888);
            for (int y = 0; y < ph; ++y)
                for (int x = 0; x < pw; ++x)
                    for (int c = 0; c < 3; ++c) {
                        const std::size_t o = static_cast<std::size_t>(y) * pw + x;
                        const double v = std::clamp((P[c].v[o] - G[c].sky) / (G[c].globe - G[c].sky), 0.0, 1.0);
                        img.scanLine(y)[3 * x + c] = static_cast<uchar>(std::lround(std::pow(v, 1 / 1.6) * 255));
                    }
            img.save(QString::fromStdString((fs::path(pngDir) / (fs::path(f).stem().string() + "-stack.png")).string()));
        }
        std::fflush(stdout);
    }
}

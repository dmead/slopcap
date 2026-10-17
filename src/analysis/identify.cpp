#include "analysis/identify.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <iterator>

namespace slopcap::analysis {

namespace {

/// Colour of the Bayer site at (x, y): 0 = R, 1 = G, 2 = B (the pattern names the top-left 2x2 block in reading order).
int siteColor(Bayer b, int x, int y) {
    const int idx = (y & 1) * 2 + (x & 1);
    static constexpr int kRGGB[4] = {0, 1, 1, 2}, kGRBG[4] = {1, 0, 2, 1}, kGBRG[4] = {1, 2, 0, 1}, kBGGR[4] = {2, 1, 1, 0};
    switch (b) {
        case Bayer::RGGB: return kRGGB[idx];
        case Bayer::GRBG: return kGRBG[idx];
        case Bayer::GBRG: return kGBRG[idx];
        case Bayer::BGGR: return kBGGR[idx];
        case Bayer::None: break;
    }
    return 1;
}

double percentile(std::vector<float> v, double p) {
    if (v.empty()) return 0;
    const auto k = static_cast<std::size_t>(std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

constexpr double kPi = 3.14159265358979323846;

// Thresholds, calibrated on real recordings (ASI585MC: Jupiter and Saturn in 2022 and 2026; lunar captures) and on
// drawn phases of a sphere. Measured: Jupiter 1.06-1.07:1, symmetry 0.98-0.99; Saturn 2.3-2.7:1, symmetry 0.92-0.98
// (rings 7-14° open); the full Moon 1.00 / 1.00; a 5-day crescent 2.15:1 / 0.71.
constexpr double kRoundElongation = 1.3;    ///< below: round enough to measure limb darkening
constexpr double kRinged = 1.6;             ///< Saturn with its rings: at least this elongated...
constexpr double kRingSymmetry = 0.86;      ///< ...and at least this point-symmetric (rings don't have a phase)
constexpr double kEdgeOnTiltDeg = 3.0;      ///< rings this close to edge-on may not show
constexpr double kHorizonDeg = -1.0;        ///< below this altitude it can't be the target
constexpr double kNewMoonLit = 0.01;        ///< less lit than this, the Moon isn't a target
constexpr double kClippedShare = 0.02;      ///< more of the disc clipped than this: colour and limb aren't used
constexpr double kSeeingArcsec = 2.0;       ///< blur added to small discs before comparing sizes

/// A lit sphere's outline at each lit fraction (elongation, point symmetry), measured by measureTarget on drawn
/// phases (terminator a half-ellipse): crescents long and lopsided, gibbous phases nearly round, full round.
struct PhaseShape {
    double lit, elongation, symmetry;
};
constexpr PhaseShape kPhaseShapes[] = {{0.00, 3.00, 0.00}, {0.05, 3.00, 0.082}, {0.15, 2.99, 0.329}, {0.30, 2.58, 0.647},
                                       {0.45, 2.04, 0.785}, {0.55, 1.75, 0.844}, {0.70, 1.41, 0.912}, {0.80, 1.24, 0.947},
                                       {0.85, 1.17, 0.963}, {0.90, 1.11, 0.976}, {0.95, 1.05, 0.988}, {0.98, 1.02, 0.994},
                                       {1.00, 1.00, 1.000}};

PhaseShape phaseShape(double lit) {
    lit = std::clamp(lit, 0.0, 1.0);
    for (std::size_t i = 1; i < std::size(kPhaseShapes); ++i) {
        const auto& a = kPhaseShapes[i - 1];
        const auto& b = kPhaseShapes[i];
        if (lit <= b.lit) {
            const double t = (lit - a.lit) / (b.lit - a.lit);
            return {lit, a.elongation + t * (b.elongation - a.elongation), a.symmetry + t * (b.symmetry - a.symmetry)};
        }
    }
    return kPhaseShapes[std::size(kPhaseShapes) - 1];
}

/// Could this target be body `p` tonight, by its shape? `lit`: p's lit fraction; `tilt`: Saturn's ring opening.
bool shapeMatches(Body p, const TargetFeatures& im, double lit, double tilt) {
    const double e = im.elongation, s = im.symmetry;
    const bool ringed = e >= kRinged && s >= kRingSymmetry;
    switch (p) {
        case Body::Moon: {   // generous: real terminators are soft, and the Moon's limb is rough
            const auto m = phaseShape(lit);
            return e >= m.elongation / 1.35 - 0.05 && e <= m.elongation * 1.35 + 0.05 && std::abs(s - m.symmetry) <= 0.15;
        }
        case Body::Mars: {   // at most a mild gibbous phase
            const auto m = phaseShape(lit);
            return e <= m.elongation + 0.15 && s >= m.symmetry - 0.08;
        }
        case Body::Jupiter: return e <= 1.2 && s >= 0.93;   // round (6.5% oblate)
        case Body::Uranus:
        case Body::Neptune: return e <= 1.25 && s >= 0.9;   // small round discs
        case Body::Saturn: return ringed || (tilt < kEdgeOnTiltDeg && e <= kRoundElongation && s >= 0.9);   // edge-on: the globe
    }
    return true;
}

/// A few words for the shape, for the summary.
std::string shapeWords(const TargetFeatures& im) {
    if (im.elongation >= kRinged && im.symmetry >= kRingSymmetry) return std::format("rings {:.1f}:1", im.elongation);
    if (im.elongation >= kRinged) return "crescent";
    if (im.symmetry < 0.95) return "gibbous";
    return "round";
}
constexpr double kJupiterSaturnColour = 0.75;   ///< ln(R/B) on the raw IMX585: Saturn 0.5-0.74, Jupiter 1.03
constexpr double kRedMargin = 0.8, kBlueMargin = 0.8;   ///< Mars is redder, Uranus / Neptune bluer, by at least this
/// Limb / centre brightness below this: limb-darkened, so not the Moon (full Moon 0.95). Only that way round: Jupiter,
/// red-heavy and near clipping on the IMX585, measured 0.92, as flat as the Moon.
constexpr double kDarkLimb = 0.75;
/// The finest image scale amateur optics reach (~2 µm pixels at 25 m effective focal length). Without the real scale,
/// a disc more pixels across than a body could cover even at this scale can't be that body.
constexpr double kFinestArcsecPerPixel = 0.015;

/// The largest a body can measure (arcseconds), blurred, with Saturn's rings, or the Moon's whole disc.
double largestArcsec(Body p, double diameter) {
    if (p == Body::Moon) return 1.3 * diameter;
    if (p == Body::Saturn) return 2.6 * diameter;
    return 1.6 * std::hypot(diameter, kSeeingArcsec);
}

}  // namespace

TargetFeatures measureTarget(std::span<const std::byte> pixels, int width, int height, int bitDepth, Bayer bayer) {
    TargetFeatures f;
    const std::size_t bpp = bitDepth > 8 ? 2 : 1;
    if (width < 8 || height < 8 || pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bpp) return f;
    const bool colour = bayer != Bayer::None;
    f.channels = colour ? 3 : 1;
    auto at = [&](int x, int y) -> float {
        const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
        if (bpp == 2) return static_cast<float>((std::to_integer<unsigned>(pixels[2 * i]) | (std::to_integer<unsigned>(pixels[2 * i + 1]) << 8)) / 65535.0);
        return static_cast<float>(std::to_integer<unsigned>(pixels[i]) / 255.0);
    };

    // A grid of 2x2 blocks (one R, two G, one B each), every `s`-th block, at most ~300k cells.
    const int bw = width / 2, bh = height / 2;
    const int s = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(bw) * bh / 300000.0))));
    const int gw = bw / s, gh = bh / s;
    const std::size_t n = static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh);
    std::array<std::vector<float>, 3> ch;
    for (auto& c : ch) c.resize(n);
    std::vector<float> lum(n);
    for (int gy = 0; gy < gh; ++gy) {
        for (int gx = 0; gx < gw; ++gx) {
            const int x0 = gx * s * 2, y0 = gy * s * 2;
            std::array<float, 3> sum{};
            std::array<int, 3> cnt{};
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int c = colour ? siteColor(bayer, x0 + dx, y0 + dy) : 0;
                    sum[static_cast<std::size_t>(c)] += at(x0 + dx, y0 + dy);
                    ++cnt[static_cast<std::size_t>(c)];
                }
            const std::size_t i = static_cast<std::size_t>(gy) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(gx);
            float l = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                ch[c][i] = cnt[c] ? sum[c] / static_cast<float>(cnt[c]) : 0.f;
                l += ch[c][i];
            }
            lum[i] = l;
        }
    }
    // Sky: a low percentile (the planet can fill more than half the frame). Bright end: a high one (hot pixels).
    const double bg = percentile(lum, 0.05), peak = percentile(lum, 0.999);
    if (peak - bg < 0.02 * f.channels) return f;   // nothing stands out

    // The largest 4-connected region above a level (not a moon, a star or a hot pixel).
    auto largestRegion = [&](double level) {
        const float thr = static_cast<float>(bg + level * (peak - bg));
        std::vector<char> seen(n, 0);
        std::vector<std::size_t> best, cur, stack;
        for (std::size_t i0 = 0; i0 < n; ++i0) {
            if (lum[i0] < thr || seen[i0]) continue;
            cur.clear();
            stack.assign(1, i0);
            seen[i0] = 1;
            while (!stack.empty()) {
                const std::size_t i = stack.back();
                stack.pop_back();
                cur.push_back(i);
                const int x = static_cast<int>(i % static_cast<std::size_t>(gw)), y = static_cast<int>(i / static_cast<std::size_t>(gw));
                const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= gw || ny[k] >= gh) continue;
                    const std::size_t j = static_cast<std::size_t>(ny[k]) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(nx[k]);
                    if (lum[j] >= thr && !seen[j]) {
                        seen[j] = 1;
                        stack.push_back(j);
                    }
                }
            }
            if (cur.size() > best.size()) best.swap(cur);
        }
        return best;
    };
    // Size, position and colour at half maximum (the disc's edge, whatever the exposure). Shape lower down, at 15%:
    // Saturn's rings are fainter than the globe and mostly fall below half maximum.
    const auto disc = largestRegion(0.5);
    if (disc.size() < 4) return f;
    const auto outline = largestRegion(0.15);

    const double cell = 2.0 * s;   // full-frame pixels per grid cell
    auto xOf = [&](std::size_t i) { return static_cast<double>(i % static_cast<std::size_t>(gw)); };
    auto yOf = [&](std::size_t i) { return static_cast<double>(i / static_cast<std::size_t>(gw)); };
    const double cnt = static_cast<double>(disc.size());
    double mx = 0, my = 0;
    std::array<double, 3> sig{}, sky{};
    for (std::size_t c = 0; c < 3; ++c) sky[c] = percentile(ch[c], 0.05);
    std::size_t clipped = 0;
    for (auto i : disc) {
        mx += xOf(i), my += yOf(i);
        for (std::size_t c = 0; c < 3; ++c) sig[c] += ch[c][i] - sky[c];
        if (std::max({ch[0][i], ch[1][i], ch[2][i]}) >= 0.98f) ++clipped;
    }
    mx /= cnt, my /= cnt;
    f.clipped = static_cast<double>(clipped) / cnt;

    // Shape from the outline's second moments (+ a cell's own spread).
    const auto& shape = outline.size() >= disc.size() ? outline : disc;
    const double sc = static_cast<double>(shape.size());
    double ox = 0, oy = 0;
    for (auto i : shape) ox += xOf(i), oy += yOf(i);
    ox /= sc, oy /= sc;
    double sxx = 0, syy = 0, sxy = 0;
    for (auto i : shape) {
        const double dx = xOf(i) - ox, dy = yOf(i) - oy;
        sxx += dx * dx, syy += dy * dy, sxy += dx * dy;
        const int x = static_cast<int>(xOf(i)), y = static_cast<int>(yOf(i));
        if (x == 0 || y == 0 || x == gw - 1 || y == gh - 1) f.touchesEdge = true;
    }
    sxx = sxx / sc + 1.0 / 12, syy = syy / sc + 1.0 / 12, sxy /= sc;
    const double tr = sxx + syy, det = sxx * syy - sxy * sxy;
    const double root = std::sqrt(std::max(0.0, tr * tr / 4 - det));
    const double l1 = tr / 2 + root, l2 = std::max(1e-9, tr / 2 - root);

    f.found = true;
    f.cx = (mx + 0.5) * cell;
    f.cy = (my + 0.5) * cell;
    f.diameterPx = std::sqrt(4.0 * cnt / kPi) * cell;
    f.elongation = std::sqrt(l1 / l2);
    f.angleDeg = 0.5 * std::atan2(2 * sxy, sxx - syy) * 180.0 / kPi;
    for (std::size_t c = 0; c < (colour ? 3u : 1u); ++c) f.signal[c] = sig[c] / cnt;

    // Point symmetry: turned 180° about its centre, how much of the outline lands on itself. Saturn and a full disc
    // ~1; a crescent or a gibbous Moon well below.
    std::vector<char> in(n, 0);
    for (auto i : shape) in[i] = 1;
    std::size_t same = 0;
    for (auto i : shape) {
        const long x = std::lround(2 * ox - xOf(i)), y = std::lround(2 * oy - yOf(i));
        if (x >= 0 && y >= 0 && x < gw && y < gh && in[static_cast<std::size_t>(y) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(x)]) ++same;
    }
    f.symmetry = static_cast<double>(same) / sc;

    // Limb darkening, on round, whole, well-resolved targets: mean brightness at 70-85% of the radius over the mean
    // inside 40%. Jupiter dims towards its edge; the full Moon stays bright right to the limb.
    const double radius = std::sqrt(sc / kPi);
    if (!f.touchesEdge && f.elongation < kRoundElongation && radius >= 12) {
        double inner = 0, limb = 0;
        int ni = 0, nl = 0;
        for (auto i : shape) {
            const double r = std::hypot(xOf(i) - ox, yOf(i) - oy) / radius;
            const double v = lum[i] - bg;
            if (r < 0.4) inner += v, ++ni;
            else if (r >= 0.7 && r <= 0.85) limb += v, ++nl;
        }
        if (ni && nl && inner > 0) f.limbRatio = (limb / nl) / (inner / ni);
    }
    return f;
}

std::optional<double> colourIndex(const TargetFeatures& f) {
    if (!f.found || f.channels != 3 || f.signal[0] <= 1e-4 || f.signal[2] <= 1e-4) return std::nullopt;
    return std::log(f.signal[0] / f.signal[2]);
}

std::string candidatesText(const std::vector<Body>& c) {
    std::string s;
    for (std::size_t i = 0; i < c.size(); ++i) {
        if (i) s += i + 1 == c.size() ? " or " : ", ";
        s += bodyName(c[i]);
    }
    return s;
}

Identification identifyTarget(const IdentifyInput& in) {
    Identification out;
    std::vector<Body> cand(std::begin(kBodies), std::end(kBodies));
    std::array<BodyPosition, 6> pos{};
    for (auto p : kBodies) pos[static_cast<std::size_t>(p)] = bodyPosition(p, in.jdUtc);
    std::vector<std::string> evidence;   // what decided it, for the summary

    // Drop the candidates `keep` rejects, unless that would leave none (then it's a conflict, and ignored).
    auto apply = [&](const std::function<bool(Body)>& keep, const std::string& what, const std::string& shortWhat) {
        std::vector<Body> next;
        for (auto p : cand)
            if (keep(p)) next.push_back(p);
        if (next.empty()) {
            out.notes.push_back(what + ": rules out every candidate, ignored");
            return;
        }
        if (next.size() < cand.size()) {
            std::string gone;
            for (auto p : cand)
                if (std::find(next.begin(), next.end(), p) == next.end()) gone += std::string(gone.empty() ? "" : ", ") + bodyName(p);
            out.notes.push_back(what + ": not " + gone);
            if (!shortWhat.empty()) evidence.push_back(shortWhat);
            cand.swap(next);
        } else {
            out.notes.push_back(what + ": rules nothing out");
        }
    };

    // ---- the sky
    if (in.site) {
        std::array<double, 6> alt{};
        for (auto p : kBodies) {
            const auto& q = pos[static_cast<std::size_t>(p)];
            alt[static_cast<std::size_t>(p)] = altitudeDeg(q.ra, q.dec, in.jdUtc, in.site->latitude, in.site->longitude);
        }
        apply([&](Body p) { return alt[static_cast<std::size_t>(p)] > kHorizonDeg; }, "above the horizon", "");
        if (cand.size() == 1) evidence.push_back("the only one up");
    }
    // The Moon's phase, from the clock alone: tonight's Moon has one shape, and near new it can't be the target.
    const double lit = pos[static_cast<std::size_t>(Body::Moon)].illuminated;
    if (lit < kNewMoonLit) apply([](Body p) { return p != Body::Moon; }, std::format("new Moon ({:.0f}% lit)", lit * 100), "");

    // ---- the image
    const auto& im = in.image;
    if (im.found) {
        // Shape and symmetry against what each body looks like tonight: the Moon at its phase, Mars at its (mild) phase,
        // Saturn with its rings as open as they are, the rest round. Meaningless when the frame edge cuts the target off.
        if (!im.touchesEdge) {
            const double tilt = std::abs(pos[static_cast<std::size_t>(Body::Saturn)].ringTiltDeg);
            apply([&](Body p) { return shapeMatches(p, im, pos[static_cast<std::size_t>(p)].illuminated, tilt); },
                  std::format("shape {:.2f}:1, symmetry {:.2f} (the Moon is {:.0f}% lit)", im.elongation, im.symmetry, lit * 100),
                  shapeWords(im));
        }
        // Clipping flattens the disc and skews its colour: then limb and colour say nothing.
        const bool clipped = im.clipped > kClippedShare;
        if (clipped) out.notes.push_back(std::format("{:.0f}% of the disc clipped: colour and limb not used", im.clipped * 100));
        // Limb darkening: the Moon stays bright to its edge, so a clearly darkened limb isn't the Moon.
        if (!clipped && im.limbRatio > 0 && im.limbRatio < kDarkLimb)
            apply([](Body p) { return p != Body::Moon; }, std::format("limb-darkened ({:.2f})", im.limbRatio), "limb-darkened");
        // Size, when the image scale is known. Cut off by the frame edge, the size is only a lower bound.
        if (in.arcsecPerPixel) {
            const double measured = im.diameterPx * *in.arcsecPerPixel;
            apply(
                [&](Body p) {
                    const double d = pos[static_cast<std::size_t>(p)].diameterArcsec;
                    const double low = im.touchesEdge ? 0.0 : 0.6 * d;
                    return measured >= low && measured <= largestArcsec(p, d);
                },
                std::format("{} {:.1f}″", im.touchesEdge ? "at least" : "disc", measured), std::format("{:.0f}″", measured));
        } else {
            // No scale: still, a disc can be too many pixels across for a small body at any amateur image scale.
            apply(
                [&](Body p) { return im.diameterPx * kFinestArcsecPerPixel <= largestArcsec(p, pos[static_cast<std::size_t>(p)].diameterArcsec); },
                std::format("{:.0f} px across: too big at any image scale", im.diameterPx), std::format("{:.0f} px", im.diameterPx));
        }
        // Raw colour.
        if (auto ci = colourIndex(im); ci && !clipped) {
            if (*ci >= kJupiterSaturnColour + kRedMargin)
                apply([](Body p) { return p == Body::Mars; }, std::format("red (ln R/B {:+.2f})", *ci), "red");
            else if (*ci <= kJupiterSaturnColour - kBlueMargin)
                apply([](Body p) { return p == Body::Uranus || p == Body::Neptune; }, std::format("blue (ln R/B {:+.2f})", *ci), "blue");
            else
                apply([](Body p) { return p == Body::Moon || p == Body::Jupiter || p == Body::Saturn; },
                      std::format("neutral colour (ln R/B {:+.2f})", *ci), "");
        }
    } else {
        out.notes.push_back("no target in the frame");
    }

    out.candidates = cand;
    if (cand.size() == 1) out.body = cand.front();
    for (std::size_t i = 0; i < evidence.size(); ++i) out.summary += (i ? " · " : "") + evidence[i];
    return out;
}

}  // namespace slopcap::analysis

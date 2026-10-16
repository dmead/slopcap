#include "analysis/sweep.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace slopcap::analysis {

const char* focusTargetName(FocusTarget t) {
    switch (t) {
        case FocusTarget::Green: return "Green";
        case FocusTarget::Luminance: return "Luminance-weighted";
        case FocusTarget::Balanced: return "Balanced";
        case FocusTarget::Red: return "Red";
        case FocusTarget::Blue: return "Blue";
    }
    return "?";
}

double ChannelCurve::fitted(double position) const {
    const double x = (position - x0) / xs;
    return a * x * x + b * x + c;
}

void SweepAnalysis::add(int position, const FocusScores& s, Metric m) {
    if (s.channels == 0) return;
    channels_ = s.channels;
    auto& at = samples_[m][position];
    for (int c = 0; c < s.channels; ++c) at[static_cast<std::size_t>(c)].push_back(s.ch[static_cast<std::size_t>(c)].sharpness);
}

std::vector<Metric> SweepAnalysis::metrics() const {
    std::vector<Metric> out;
    for (const auto& [m, _] : samples_) out.push_back(m);
    return out;
}

namespace {

/// Solve the 3x3 system M p = v (Gaussian elimination); false if singular.
bool solve3(double M[3][3], double v[3], double p[3]) {
    for (int i = 0; i < 3; ++i) {
        int piv = i;
        for (int r = i + 1; r < 3; ++r)
            if (std::abs(M[r][i]) > std::abs(M[piv][i])) piv = r;
        if (std::abs(M[piv][i]) < 1e-15) return false;
        std::swap(M[i], M[piv]);
        std::swap(v[i], v[piv]);
        for (int r = i + 1; r < 3; ++r) {
            const double f = M[r][i] / M[i][i];
            for (int k = i; k < 3; ++k) M[r][k] -= f * M[i][k];
            v[r] -= f * v[i];
        }
    }
    for (int i = 2; i >= 0; --i) {
        double s = v[i];
        for (int k = i + 1; k < 3; ++k) s -= M[i][k] * p[k];
        p[i] = s / M[i][i];
    }
    return true;
}

bool invert3(const double A[3][3], double inv[3][3]) {
    for (int col = 0; col < 3; ++col) {
        double M[3][3], e[3] = {0, 0, 0}, x[3];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) M[r][k] = A[r][k];
        e[col] = 1;
        if (!solve3(M, e, x)) return false;
        for (int r = 0; r < 3; ++r) inv[r][col] = x[r];
    }
    return true;
}

}  // namespace

void fitPeak(ChannelCurve& cv) {
    cv.best.reset();
    const auto& pts = cv.points;
    if (pts.size() < 3) return;
    std::size_t ipk = 0;
    for (std::size_t i = 0; i < pts.size(); ++i)
        if (pts[i].bestQuarter > pts[ipk].bestQuarter) ipk = i;
    cv.atEdge = ipk == 0 || ipk + 1 == pts.size();
    // Points around the peak: those above half the way from the curve's floor to its peak, contiguous with it, at
    // least 2 each side when available (a parabola only describes the top of a focus curve).
    double lo = pts[0].bestQuarter;
    for (const auto& p : pts) lo = std::min(lo, p.bestQuarter);
    const double cut = lo + 0.5 * (pts[ipk].bestQuarter - lo);
    std::size_t i0 = ipk, i1 = ipk;
    while (i0 > 0 && (pts[i0 - 1].bestQuarter >= cut || ipk - i0 < 2)) --i0;
    while (i1 + 1 < pts.size() && (pts[i1 + 1].bestQuarter >= cut || i1 - ipk < 2)) ++i1;
    const std::size_t n = i1 - i0 + 1;
    if (n < 3) return;
    double mean = 0;
    for (std::size_t i = i0; i <= i1; ++i) mean += pts[i].position;
    cv.x0 = mean / static_cast<double>(n);
    cv.xs = std::max(1.0, (pts[i1].position - pts[i0].position) / 2.0);
    double A[3][3] = {}, v[3] = {};
    for (std::size_t i = i0; i <= i1; ++i) {
        const double x = (pts[i].position - cv.x0) / cv.xs, y = pts[i].bestQuarter;
        const double row[3] = {x * x, x, 1};
        for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k) A[r][k] += row[r] * row[k];
            v[r] += row[r] * y;
        }
    }
    double AA[3][3], vv[3], p[3];
    std::copy(&A[0][0], &A[0][0] + 9, &AA[0][0]);
    std::copy(v, v + 3, vv);
    if (!solve3(AA, vv, p)) return;
    cv.a = p[0], cv.b = p[1], cv.c = p[2];
    if (cv.a >= 0) return;   // not a peak
    const double xv = -cv.b / (2 * cv.a);
    cv.best = cv.x0 + xv * cv.xs;
    // Uncertainty: parameter covariance from the residuals, propagated to the vertex -b/2a.
    double rss = 0;
    for (std::size_t i = i0; i <= i1; ++i) {
        const double r = pts[i].bestQuarter - cv.fitted(pts[i].position);
        rss += r * r;
    }
    const double dof = static_cast<double>(n) - 3;
    double inv[3][3];
    if (dof > 0 && invert3(A, inv)) {
        const double s2 = rss / dof;
        const double ga = cv.b / (2 * cv.a * cv.a), gb = -1 / (2 * cv.a);   // d(xv)/da, d(xv)/db
        const double var = s2 * (ga * ga * inv[0][0] + 2 * ga * gb * inv[0][1] + gb * gb * inv[1][1]);
        cv.bestError = std::sqrt(std::max(0.0, var)) * cv.xs;
    }
}

SweepResult SweepAnalysis::result(Metric m) const {
    SweepResult r;
    r.channels = channels_;
    auto it = samples_.find(m);
    if (it == samples_.end()) return r;
    for (int c = 0; c < channels_; ++c) {
        auto& cv = r.ch[static_cast<std::size_t>(c)];
        for (const auto& [pos, chans] : it->second) {
            auto v = chans[static_cast<std::size_t>(c)];
            if (v.empty()) continue;
            std::sort(v.begin(), v.end(), std::greater<>());
            CurvePoint p;
            p.position = pos;
            p.frames = static_cast<int>(v.size());
            p.median = v[v.size() / 2];
            const std::size_t q = std::max<std::size_t>(1, v.size() / 4);
            p.bestQuarter = std::accumulate(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(q), 0.0) / static_cast<double>(q);
            cv.points.push_back(p);
        }
        fitPeak(cv);
    }
    double mn = 1e18, mx = -1e18;
    int have = 0;
    for (int c = 0; c < channels_; ++c)
        if (const auto& b = r.ch[static_cast<std::size_t>(c)].best) mn = std::min(mn, *b), mx = std::max(mx, *b), ++have;
    if (have == channels_ && have > 1) r.spread = mx - mn;
    return r;
}

std::optional<double> SweepResult::position(FocusTarget t) const {
    if (channels == 1) return ch[0].best;
    const auto &R = ch[0].best, &G = ch[1].best, &B = ch[2].best;
    switch (t) {
        case FocusTarget::Green: return G;
        case FocusTarget::Red: return R;
        case FocusTarget::Blue: return B;
        case FocusTarget::Luminance:
            if (R && G && B) return 0.30 * *R + 0.59 * *G + 0.11 * *B;
            return G;
        case FocusTarget::Balanced: {
            if (!R || !G || !B) return G;
            // the position where the worst channel loses the least of its own peak score
            const double lo = std::min({*R, *G, *B}), hi = std::max({*R, *G, *B});
            double bestPos = *G, bestWorst = -1e18;
            for (int i = 0; i <= 200; ++i) {
                const double p = lo + (hi - lo) * i / 200.0;
                double worst = 1e18;
                for (int c = 0; c < 3; ++c) {
                    const auto& cv = ch[static_cast<std::size_t>(c)];
                    const double peak = cv.fitted(*cv.best);
                    if (peak > 0) worst = std::min(worst, cv.fitted(p) / peak);
                }
                if (worst > bestWorst) bestWorst = worst, bestPos = p;
            }
            return bestPos;
        }
    }
    return G;
}

}  // namespace slopcap::analysis

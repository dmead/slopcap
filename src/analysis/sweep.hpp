// Focus sweep analysis (PLAN.md §7a): scores collected at a series of focuser positions become one curve per colour
// channel and metric; each curve's peak is located by a parabola fit with an uncertainty, and the channels' bests give
// the chromatic focus spread and the position to use for a chosen strategy.
#pragma once

#include "analysis/focus.hpp"

#include <array>
#include <map>
#include <optional>
#include <vector>

namespace slopcap::analysis {

struct CurvePoint {
    int position = 0;
    double median = 0;       ///< median score of the frames at this position
    double bestQuarter = 0;  ///< mean of the best 25 % (what's fitted: robust to seeing)
    int frames = 0;
};

struct ChannelCurve {
    std::vector<CurvePoint> points;   ///< sorted by position
    std::optional<double> best;       ///< fitted peak position
    double bestError = 0;             ///< 1-sigma uncertainty of `best`, in steps
    double a = 0, b = 0, c = 0;       ///< fit: score = a x² + b x + c with x = (position - x0) / xs
    double x0 = 0, xs = 1;
    bool atEdge = false;              ///< the peak is at the end of the sweep: extend it that way
    double fitted(double position) const;
};

enum class FocusTarget { Green, Luminance, Balanced, Red, Blue };
const char* focusTargetName(FocusTarget t);

struct SweepResult {
    int channels = 0;
    std::array<ChannelCurve, 3> ch;   ///< R, G, B (mono: ch[0])
    std::optional<double> spread;     ///< largest minus smallest best, steps
    std::optional<double> position(FocusTarget t) const;
};

class SweepAnalysis {
public:
    void add(int position, const FocusScores& s, Metric m);   ///< one frame's scores at a focuser position
    SweepResult result(Metric m) const;
    std::vector<Metric> metrics() const;
    void clear() { samples_.clear(); }

private:
    // metric -> position -> channel -> scores
    std::map<Metric, std::map<int, std::array<std::vector<double>, 3>>> samples_;
    int channels_ = 0;
};

/// Least-squares parabola through the points around the highest one (fills a, b, c, best, bestError, atEdge).
void fitPeak(ChannelCurve& curve);

}  // namespace slopcap::analysis

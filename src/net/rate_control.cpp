#include "net/rate_control.hpp"

#include "net/frame_codec.hpp"
#include "net/protocol.hpp"

#include <algorithm>
#include <array>

namespace slopcap::net {

namespace {

struct Rung {
    int quality, scale;
};
constexpr std::array<Rung, 11> kLadder{
    {{kLossless, 1}, {85, 1}, {75, 1}, {65, 1}, {75, 2}, {65, 2}, {75, 4}, {65, 4}, {55, 4}, {45, 4}, {35, 4}}};
constexpr int kLast = static_cast<int>(kLadder.size()) - 1;
constexpr int kForgetCapacity = 120;   ///< seconds without a full link before its measured capacity is forgotten
constexpr double kMargin = 1.25;       ///< a step up by an estimated capacity must make the floor by this much

const Rung& rung(int r) { return kLadder[static_cast<std::size_t>(r)]; }

/// Roughly how a JPEG's size goes with its quality (relative; only ratios are used).
double jpegFactor(int q) { return 0.3 + 0.9 * q / 100.0; }

}  // namespace

RateControl::RateControl() { setRung(0); }

void RateControl::setRung(int r) {
    rung_ = std::clamp(r, 0, kLast);
    quality = rung(rung_).quality;
    scale = rung(rung_).scale;
}

double RateControl::bytesRatio(int from, int to) const {
    auto size = [this](int r) {
        const auto& g = rung(r);
        const double q = g.quality >= kLossless ? losslessBytes_ * jpegFactor(85) : jpegFactor(g.quality);
        return q / (g.scale * g.scale);
    };
    return size(to) / size(from);
}

double RateControl::encodeRatio(int from, int to) const {
    auto cost = [this](int r) {
        const auto& g = rung(r);
        return (g.quality >= kLossless ? losslessEncode_ : 1.0) / (g.scale * g.scale);
    };
    return cost(to) / cost(from);
}

const char* RateControl::update(const StreamSecond& s, double availableFps) {
    const double secs = s.seconds > 0 ? s.seconds : 1;
    const double fps = s.sent / secs;
    const double perFrameMs = s.sent ? s.encodeMs / s.sent : 0;
    const double frameBytes = s.sent ? s.bytes / s.sent : 0;
    // Just moved between lossless and quality 85 (either way)? Then the two seconds measure how much bigger and slower
    // lossless frames really are on this target.
    if (s.sent && lastSent_ && ((lastRung_ == 0 && rung_ == 1) || (lastRung_ == 1 && rung_ == 0))) {
        const bool nowLossless = rung_ == 0;
        const double lb = nowLossless ? frameBytes : lastFrameBytes_, jb = nowLossless ? lastFrameBytes_ : frameBytes;
        const double le = nowLossless ? perFrameMs : lastFrameMs_, je = nowLossless ? lastFrameMs_ : perFrameMs;
        if (lb > 0 && jb > 0) losslessBytes_ = std::clamp(lb / jb, 1.0, 50.0);
        if (le > 0 && je > 0) losslessEncode_ = std::clamp(le / je, 0.2, 20.0);
    }
    lastRung_ = rung_, lastSent_ = s.sent, lastFrameBytes_ = frameBytes, lastFrameMs_ = perFrameMs;

    ++sinceUpgrade_;
    if (++capacityAge_ > kForgetCapacity) capacity_ = 0;
    if (availableFps < 0.5) return nullptr;   // long exposures: nothing to judge by
    const double floor = std::min(kMinStreamFps, availableFps);

    const bool full = s.skipped > 0;   // frames went by unsent: the link (or the viewer) set the pace
    if (full && s.sent > 0) capacity_ = s.bytes / secs, capacityAge_ = 0;
    else if (capacity_ > 0) capacity_ = std::max(capacity_, s.bytes / secs);   // not full: it carries at least this
    const bool tooFew = full && fps < 0.8 * floor;
    const bool encodeBound = perFrameMs > 1000 / floor;
    const int from = rung_;
    const char* why = nullptr;

    if (tooFew || encodeBound) {
        goodSeconds_ = 0;
        if (sinceUpgrade_ <= 2) {   // the last step up was too far: undo it, and wait longer before the next
            why = "the step up didn't fit";
            upgradeWait_ = std::min(upgradeWait_ * 2, 60);
            setRung(rung_ + 1);
            sinceUpgrade_ = 100;
        } else if (encodeBound) {
            why = "encoding too slow";
            int r = rung_ + 1;   // from lossless: JPEG at full size; else the next size down
            if (rung_ > 0)
                while (r < kLast && rung(r).scale == scale) ++r;
            setRung(r);
        } else {
            why = "too few frames get through";
            setRung(rung_ + 1);
        }
    } else if (rung_ > 0 && ++goodSeconds_ >= upgradeWait_) {
        goodSeconds_ = 0;
        // Predicted frame rate one rung up, by the link and by the encoding time. While the link is full, its capacity
        // is a measurement and the prediction an estimate (so it needs a margin); when it isn't, it's a lower bound.
        const int to = rung_ - 1;
        const double byLink = capacity_ > 0 && frameBytes > 0 ? capacity_ / (frameBytes * bytesRatio(from, to)) : 1e9;
        const double byEncoder = perFrameMs > 0 ? 1000 / (perFrameMs * encodeRatio(from, to)) : 1e9;
        if (byLink >= (full ? kMargin : 1.0) * floor && byEncoder >= kMargin * floor) {
            why = "room for more detail";
            setRung(to);
            sinceUpgrade_ = 0;
        }
    }
    return rung_ != from ? why : nullptr;
}

}  // namespace slopcap::net

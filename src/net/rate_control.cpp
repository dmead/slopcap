#include "net/rate_control.hpp"

#include "net/protocol.hpp"

#include <algorithm>
#include <array>

namespace slopcap::net {

namespace {

struct Rung {
    int quality, scale;
};
constexpr std::array<Rung, 10> kLadder{{{85, 1}, {75, 1}, {65, 1}, {75, 2}, {65, 2}, {75, 4}, {65, 4}, {55, 4}, {45, 4}, {35, 4}}};
constexpr int kLast = static_cast<int>(kLadder.size()) - 1;
constexpr int kForgetCapacity = 120;   ///< seconds without a full link before its measured capacity is forgotten
constexpr double kMargin = 1.25;       ///< a step up must be predicted to make the floor by this much

/// Roughly how a frame's size goes with JPEG quality (relative; only ratios are used).
double qualityFactor(int q) { return 0.3 + 0.9 * q / 100.0; }

/// Bytes and encoding time of rung `to` relative to rung `from`.
double bytesRatio(int from, int to) {
    const auto& a = kLadder[static_cast<std::size_t>(from)];
    const auto& b = kLadder[static_cast<std::size_t>(to)];
    const double area = static_cast<double>(a.scale * a.scale) / (b.scale * b.scale);
    return area * qualityFactor(b.quality) / qualityFactor(a.quality);
}
double encodeRatio(int from, int to) {
    const auto& a = kLadder[static_cast<std::size_t>(from)];
    const auto& b = kLadder[static_cast<std::size_t>(to)];
    return static_cast<double>(a.scale * a.scale) / (b.scale * b.scale);
}

}  // namespace

RateControl::RateControl() { setRung(0); }

void RateControl::setRung(int r) {
    rung_ = std::clamp(r, 0, kLast);
    quality = kLadder[static_cast<std::size_t>(rung_)].quality;
    scale = kLadder[static_cast<std::size_t>(rung_)].scale;
}

const char* RateControl::update(const StreamSecond& s, double availableFps) {
    const double secs = s.seconds > 0 ? s.seconds : 1;
    const double fps = s.sent / secs;
    const double perFrameMs = s.sent ? s.encodeMs / s.sent : 0;
    const double frameBytes = s.sent ? s.bytes / s.sent : 0;
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
            int r = rung_;
            while (r < kLast && kLadder[static_cast<std::size_t>(r)].scale == scale) ++r;   // the next size down
            setRung(r);
        } else {
            why = "too few frames get through";
            setRung(rung_ + 1);
        }
    } else if (rung_ > 0 && ++goodSeconds_ >= upgradeWait_) {
        goodSeconds_ = 0;
        // predicted frame rate one rung up: by the link's capacity (if it's been full) and by the encoding time
        const int to = rung_ - 1;
        const double byLink = capacity_ > 0 && frameBytes > 0 ? capacity_ / (frameBytes * bytesRatio(from, to)) : 1e9;
        const double byEncoder = perFrameMs > 0 ? 1000 / (perFrameMs * encodeRatio(from, to)) : 1e9;
        if (std::min(byLink, byEncoder) >= kMargin * floor) {
            why = "room for more detail";
            setRung(to);
            sinceUpgrade_ = 0;
        }
    }
    return rung_ != from ? why : nullptr;
}

}  // namespace slopcap::net

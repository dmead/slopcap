#include "net/rate_control.hpp"

#include <algorithm>

namespace slopcap::net {

namespace {
constexpr int kForgetCapacity = 120;   ///< seconds without a full link before its measured capacity is forgotten
}

const char* RateControl::update(const StreamSecond& s, double availableFps) {
    const double secs = s.seconds > 0 ? s.seconds : 1;
    const double target = availableFps;
    const double fps = s.sent / secs;
    const double perFrameMs = s.sent ? s.encodeMs / s.sent : 0;
    const double frameBytes = s.sent ? s.bytes / s.sent : 0;
    ++sinceUpgrade_;
    ++sinceDegrade_;
    if (hold_ > 0) --hold_;
    if (++capacityAge_ > kForgetCapacity) capacity_ = 0;
    if (target < 1) return nullptr;   // long exposures: nothing to judge by

    const int q0 = quality, s0 = scale;
    const char* why = nullptr;
    const bool linkBound = s.skipped > 0 && fps < 0.8 * target;
    const bool cpuBound = perFrameMs > 0.8 * 1000 / target;
    // On a full link, frames come faster as they shrink (fps × bytes stays put); a slow viewer acks at its own pace
    // whatever the size. Less than half the speed-up the link would give: it's the viewer. (Only when that speed-up is
    // big enough to see in whole frames.)
    const double expected = frameBytes > 0 ? fpsBefore_ * frameBytesBefore_ / frameBytes : 0;
    if (linkBound && sinceDegrade_ == 1 && expected - fpsBefore_ >= 2 && fps < fpsBefore_ + 0.5 * (expected - fpsBefore_)) {
        why = "smaller frames came no faster: the viewer is slow, not the link";
        quality = qualityBefore_, scale = scaleBefore_;
        hold_ = 20;
        goodSeconds_ = 0;
        capacity_ = 0;   // that wasn't the link's limit
    } else if ((linkBound && hold_ == 0) || cpuBound) {
        if (linkBound) capacity_ = fps * frameBytes, capacityAge_ = 0;
        qualityBefore_ = quality, scaleBefore_ = scale, fpsBefore_ = fps, frameBytesBefore_ = frameBytes;
        if (sinceUpgrade_ <= 2) {   // the last step up was too far: undo it, and wait longer before the next
            why = "the step up didn't fit";
            upgradeWait_ = std::min(upgradeWait_ * 2, 60);
            quality = qualityUp_, scale = scaleUp_;
            sinceUpgrade_ = 100;
        } else {
            why = linkBound ? "frames skipped waiting for acks" : "encoding too slow";
            if (linkBound && quality > 45) quality -= 10;
            else if (scale < 4) scale *= 2, quality = std::max(quality, 65);
            else if (linkBound && quality > 25) quality -= 10;
        }
        if (quality != qualityBefore_ || scale != scaleBefore_) sinceDegrade_ = 0;
        goodSeconds_ = 0;
    } else if (s.skipped == 0 && ++goodSeconds_ >= upgradeWait_) {
        goodSeconds_ = 0;
        // The steps up in order, each with roughly how much bigger it makes a frame; the first one the link's measured
        // capacity (if known) says will fit.
        struct Step {
            bool possible;
            int q, sc;
            double growth;
        };
        const Step steps[] = {{quality < 70, quality + 10, scale, 1.3}, {scale > 1, quality, scale / 2, 4}, {quality < 85, quality + 5, scale, 1.2}};
        for (const auto& st : steps) {
            if (!st.possible) continue;
            if (capacity_ > 0 && frameBytes > 0 && capacity_ / (frameBytes * st.growth) < 0.85 * target) continue;
            why = "clean";
            qualityUp_ = quality, scaleUp_ = scale;
            quality = st.q, scale = st.sc;
            sinceUpgrade_ = 0;
            break;
        }
    }
    return quality != q0 || scale != s0 ? why : nullptr;
}

}  // namespace slopcap::net

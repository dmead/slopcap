#include "preview/latest_frame.hpp"

#include <cstring>

namespace slopcap {

void LatestFrame::publish(std::span<const std::byte> pixels, const FrameInfo& info) {
    // Clear the request first: a request() that lands while we copy then asks for the frame after this one.
    wanted_.store(false, std::memory_order_relaxed);
    Frame& f = buffers_[write_];
    if (f.pixels.size() < pixels.size()) f.pixels.resize(pixels.size());
    std::memcpy(f.pixels.data(), pixels.data(), pixels.size());
    f.info = info;
    f.info.bytes = pixels.size();
    // Swap our filled buffer into "ready" (marked fresh); take back whatever was there to write into next time.
    const uint32_t prev = ready_.exchange(write_ | kFresh, std::memory_order_acq_rel);
    write_ = prev & ~kFresh;
}

const LatestFrame::Frame* LatestFrame::take() {
    request();
    if (!(ready_.load(std::memory_order_acquire) & kFresh)) return nullptr;
    const uint32_t prev = ready_.exchange(read_, std::memory_order_acq_rel);
    read_ = prev & ~kFresh;
    return &buffers_[read_];
}

}  // namespace slopcap

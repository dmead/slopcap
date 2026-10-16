// Hands the newest frame from the capture thread to the preview without ever making capture wait.
//
// Triple buffer: the producer writes into its own buffer and swaps it into the "ready" slot; the consumer swaps the
// ready slot out when it wants a frame. Neither side blocks. The producer only copies when the consumer has asked
// (wanted()), so a preview drawing at 30-60 Hz costs one memcpy per displayed frame, not per captured frame.
#pragma once

#include "camera/camera.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace slopcap {

class LatestFrame {
public:
    struct Frame {
        std::vector<std::byte> pixels;
        FrameInfo info;
    };

    // ---- producer (capture thread)
    /// True if the consumer has asked for a frame since the last publish (cheap atomic load).
    bool wanted() const { return wanted_.load(std::memory_order_acquire); }
    /// Copy a frame in and make it the newest. Call only when wanted() (or always; extra copies just cost time).
    void publish(std::span<const std::byte> pixels, const FrameInfo& info);

    // ---- consumer (preview / UI thread)
    /// Ask for a new frame (the producer will publish its next one).
    void request() { wanted_.store(true, std::memory_order_release); }
    /// The newest frame if one arrived since the last take(), else nullptr. The pointer stays valid until the next
    /// take(). Also requests the next frame.
    const Frame* take();

private:
    // index of the buffer in each role; bit 2 of ready_ marks "fresh"
    static constexpr uint32_t kFresh = 4;
    std::array<Frame, 3> buffers_;
    uint32_t write_ = 0;                 // producer-owned
    uint32_t read_ = 1;                  // consumer-owned
    std::atomic<uint32_t> ready_{2};     // shared: index | kFresh
    std::atomic<bool> wanted_{true};
};

}  // namespace slopcap

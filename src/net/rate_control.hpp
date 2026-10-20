// How one viewer's stream adapts to what it can take (remote_server.hpp), once a second:
//   - a fifth of the frames skipped waiting for acks → lower JPEG quality (down to 45), then shrink 2× / 4×
//   - encoding slower than the frame rate needs → shrink (quality hardly changes the encoding time)
//   - several clean seconds → back up: quality to 70, then full size, then quality to 85. A step up that fails at once
//     is undone, and the next waits twice as long (up to a minute). While the link's capacity is known (bytes a second
//     when it last filled, forgotten after two minutes), steps up that wouldn't fit are passed over.
//   - a step down that doesn't bring frames faster means the viewer (busy drawing), not the link, is slow: on a full
//     link frames come faster as they shrink. It's undone, and steps down for skipped frames wait 20 s.
#pragma once

namespace slopcap::net {

/// One viewer's last second (or so).
struct StreamSecond {
    double seconds = 1;
    int sent = 0;           ///< frames sent
    int skipped = 0;        ///< frames that went by while the viewer's window was full
    double encodeMs = 0;    ///< total encoding time of the frames sent
    double bytes = 0;       ///< bytes sent
};

class RateControl {
public:
    int quality = 75;   ///< JPEG quality
    int scale = 1;      ///< 1, 2 or 4

    /// Feed one second; `availableFps` is how many frames the stream had to offer (camera, capped at kMaxStreamFps).
    /// Returns why the setting changed, or nullptr if it didn't.
    const char* update(const StreamSecond& s, double availableFps);
    int upgradeWait() const { return upgradeWait_; }

private:
    int goodSeconds_ = 0, upgradeWait_ = 3, sinceUpgrade_ = 100, sinceDegrade_ = 100, hold_ = 0;
    int qualityBefore_ = 75, scaleBefore_ = 1;   ///< before the last step down
    int qualityUp_ = 75, scaleUp_ = 1;           ///< before the last step up
    double fpsBefore_ = 0, frameBytesBefore_ = 0;
    double capacity_ = 0;   ///< bytes a second the link carried when it was last full (0 = unknown)
    int capacityAge_ = 0;
};

}  // namespace slopcap::net

// How one viewer's stream adapts to what it can take (remote_server.hpp). Detail comes first, smoothness second: this is
// astronomy, not TV. Each frame goes at the best setting that still makes kMinStreamFps; the ack window fills the pipe
// and the frame rate is whatever fits through it (up to kMaxStreamFps).
//
// The settings form one ladder, best first: lossless, then JPEG quality 85, 75, 65 at full size, then 75 and 65 at half
// size, then quarter size down to quality 35. Once a second:
//   - fewer than 0.8 × kMinStreamFps frames got through a full window (the link or the viewer is the limit), or
//     encoding alone can't make kMinStreamFps → one step down (a slow JPEG encoder steps to the next size)
//   - several good seconds → one step up, when it's predicted to still make kMinStreamFps: by the link's measured
//     capacity (bytes a second while it was full, forgotten after two minutes of not being full) and the encoding
//     time. How much bigger and slower lossless frames are than JPEG is measured each time the stream moves between
//     them. A step up that fails at once is undone, and the next waits twice as long (up to a minute).
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
    RateControl();

    // the current rung (read only)
    int quality = 100;   ///< kLossless, or a JPEG quality
    int scale = 1;       ///< 1, 2 or 4

    /// Feed one second; `availableFps` is how many frames the stream had to offer (the camera's rate, capped at
    /// kMaxStreamFps). Returns why the setting changed, or nullptr if it didn't.
    const char* update(const StreamSecond& s, double availableFps);
    int upgradeWait() const { return upgradeWait_; }

private:
    void setRung(int r);
    double bytesRatio(int from, int to) const;    ///< frame size of rung `to` relative to `from`
    double encodeRatio(int from, int to) const;   ///< encoding time, likewise
    int rung_ = 0;
    int goodSeconds_ = 0, upgradeWait_ = 3, sinceUpgrade_ = 100;
    double capacity_ = 0;   ///< bytes a second the link carried when it was last full (0 = unknown)
    int capacityAge_ = 0;
    // lossless relative to JPEG quality 85, measured when the stream moves between them (until then, guesses)
    double losslessBytes_ = 5, losslessEncode_ = 2;
    int lastRung_ = 0, lastSent_ = 0;
    double lastFrameBytes_ = 0, lastFrameMs_ = 0;
};

}  // namespace slopcap::net

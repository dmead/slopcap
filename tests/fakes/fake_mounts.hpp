// Fake mounts for the built-in drivers' tests: each speaks one protocol, built from the same vendor documents as its
// driver (src/mounts/*.hpp), on top of a SimMount (or, for Sky-Watcher's motor controller, two simulated motor axes),
// over an in-memory link. A driver's test runs its real wire code end to end, as tests/fakes does for the camera SDKs.
#pragma once

#include "mounts/lx200.hpp"
#include "mounts/nexstar.hpp"
#include "mounts/sim_mount.hpp"
#include "mounts/transport.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace slopcap::test {

/// What a fake device makes of one write: its reply ("" for none).
using Device = std::function<std::string(std::string_view)>;

/// The link's side the test keeps: everything written, and a switch to pull the plug.
struct LinkState {
    std::mutex m;
    std::vector<std::string> written;
    std::atomic<bool> dead{false};
    bool wrote(const std::string& s) {
        std::lock_guard lock(m);
        for (const auto& w : written)
            if (w == s) return true;
        return false;
    }
};

/// A transport to a device in memory: a write goes to the device at once; its reply waits to be read.
class FakeLink : public mounts::ITransport {
public:
    FakeLink(Device device, std::shared_ptr<LinkState> state, bool datagrams = false) : device_(std::move(device)), state_(std::move(state)), datagrams_(datagrams) {}
    void write(std::string_view bytes) override;
    std::string read(int ms) override;
    void drain() override { buffer_.clear(); }
    bool datagrams() const override { return datagrams_; }
    std::string name() const override { return "a fake link"; }

private:
    Device device_;
    std::shared_ptr<LinkState> state_;
    bool datagrams_;
    std::string buffer_;
};

/// A factory for drivers: a FakeLink to `device`, recording into `state`.
mounts::TransportFactory fakeLink(Device device, std::shared_ptr<LinkState> state, bool datagrams = false);

/// LX200: Meade Classic (no :GVP#, no pulses), Meade Autostar, OnStep. Starts in low precision.
Device fakeLx200(std::shared_ptr<mounts::SimMount> sim, mounts::Lx200Dialect dialect);
/// OnStepX as the firmware's command handlers answer (OnStepX src/telescope/mount/*.command.cpp; replies checked against
/// an OnStepX 10.25a): set commands answer a bare 0 or 1, unknown ones a bare 0, :GU# ends with the pier side, the
/// pulse-guide and guide rate indices and the error code. The fields are the mount's settings and quirks; set them
/// before the driver connects, read them under `m`.
class FakeOnStep {
public:
    explicit FakeOnStep(std::shared_ptr<mounts::SimMount> sim) : sim_(std::move(sim)) {}
    Device device();
    std::string handle(std::string_view in);

    std::mutex m;
    std::string firmware = "10.25a";
    bool lostRate = false;           ///< tracking rate 0, as after :ERESET#: :Te# says 1 but nothing tracks until :TQ#
    int error = 0;                   ///< the general error in :GU#
    bool parkFailed = false, atHome = false, noParkPosition = false;
    int pulseRate = 0, guideRate = 10;   ///< :R indices (10: custom)
    bool separatePulseRate = true;   ///< GUIDE_SEPARATE_PULSE_RATE: :R0# .. :R2# also set the pulse-guide rate
    bool rateOffsets = true;         ///< :GXTR# / :SXTR,n# (arcsec per sidereal second)
    double raOffset = 0, decOffset = 0;
    bool highPrecision = true;       ///< :GRH# / :GDH#
    bool longFormat = true;          ///< :GR# as HH:MM:SS (false: HH:MM.T until :U#)
    double clockOffset = 0;          ///< seconds the mount's clock is ahead
    std::string utcOffset = "+05:00";
    std::string latitude = "+51*30", longitude = "+000*06";   ///< :Gt#, :Gg# (west positive)
    double speedup = 20;             ///< moves this many times faster than OnStep's rates (so tests can measure them)
    int stillMoving = 0;             ///< the next this many :MS# say 8 (still slowing down after a move)
    // the settings OnStepX's web server sets
    std::string elevation = "45.0";
    int horizon = -10, overhead = 80, meridianEast = 60, meridianWest = 60;
    bool autoFlip = true, buzzer = true, pauseAtHome = false, singleAxis = false;
    char pierSide = 'E', compensation = 0;   ///< compensation: 0, 'r' or 't'
    int backlashRa = 0, backlashDec = 70, gotoPreset = 3;
    bool parkSet = false;
    std::string setDate, setTime;    ///< what :SC# / :SL# last said
    int lastError = 0;

private:
    std::shared_ptr<mounts::SimMount> sim_;
    double ra_ = 0, dec_ = 0;
    bool trackingClaimed_ = false;   ///< :Te# with the rate lost
};

/// Celestron NexStar or SynScan hand controller.
Device fakeNexStar(std::shared_ptr<mounts::SimMount> sim, mounts::HandController kind, bool aligned = true);
/// The SynScan app; `j2000`: it says its coordinates are J2000.
Device fakeSynScanApp(std::shared_ptr<mounts::SimMount> sim, bool j2000 = false);

/// Sky-Watcher's motor controller: two axes counting steps, in speed (tracking) or goto mode.
class FakeSkyWatcherMc {
public:
    FakeSkyWatcherMc();
    std::string handle(std::string_view cmd);
    Device device();
    struct Axis {
        double pos = 0x800000;   ///< counts
        bool init = false, running = false, gotoMode = false, ccw = false, fast = false;
        uint32_t period = 0;
        double target = 0;
    };
    Axis axis(int i);   ///< 0 RA, 1 Dec
    static constexpr uint32_t kCpr = 9024000, kTimer = 64935, kHighSpeed = 32;
    double gotoDegPerSec = 20;

private:
    void advance();
    std::mutex m_;
    Axis a_[2];
    double last_ = 0;
};

}  // namespace slopcap::test

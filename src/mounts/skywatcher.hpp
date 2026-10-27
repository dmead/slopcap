// Sky-Watcher's motor controllers spoken to directly, without a hand controller (EQ6-R, HEQ5, EQ8, AZ-EQ5 / 6 in EQ
// mode, the GTi mounts): over the mount's own USB port, an EQDIR cable, or the mount's Wi-Fi (UDP port 11880). From
// Sky-Watcher's "Motor Controller Command Set" (2021-08): ":" + command letter + axis ('1' RA, '2' Dec) + hex data
// (24-bit values low byte first) + CR; replies "=" + data + CR, or "!" + error code + CR.
//
// The controller only counts motor steps. Where the telescope points comes from the axes, the site's sidereal time
// and a German equatorial model whose home (pointing at the pole, counterweights down) is count 0x800000 on both axes,
// the convention EQMOD and INDI's EQMod driver share; a sync shifts the model to the sky. Without a site there are no
// coordinates and no gotos, but tracking and moves work. Counts are taken to rise clockwise and the RA axis to track
// clockwise in the north; both to be checked on a mount. Equatorial mounts only, for now. Moves and emulated guide
// pulses change the axes' speeds (speed mode); gotos use the controller's goto mode, twice, to catch the sky's turn.
#pragma once

#include "mounts/command_channel.hpp"
#include "mounts/mount_driver.hpp"

#include <cstdint>

namespace slopcap::mounts {

class SkyWatcherDriver : public IMountDriver {
public:
    /// `opens`: the links to try in turn (the mount's USB runs at 115200 baud, EQDIR cables at 9600).
    SkyWatcherDriver(std::vector<TransportFactory> opens, std::optional<Site> site, std::function<double()> jdNow = {});
    MountInfo connect() override;
    void disconnect() override;
    MountStatus poll() override;
    void slewTo(double ra, double dec) override;
    void abort() override;
    void move(Dir d, int rate) override;
    void stop(Dir d) override;
    void setTracking(bool on) override;
    void setTrackRate(TrackRate r) override;
    void park() override;
    void unpark() override;
    void sync(double ra, double dec) override;

private:
    struct AxisStatus {
        bool tracking = false, ccw = false, fast = false, running = false, blocked = false, init = false;
    };
    std::string command(char c, int axis, std::string_view data = {});   ///< the reply's data; throws on "!"
    AxisStatus status(int axis);
    double angle(int axis);              ///< degrees from home
    void stopAxis(int axis, bool wait);
    void axisSpeed(int axis, double degPerSec);   ///< speed mode, signed (counts rising); 0 stops
    void raSpeed();                      ///< tracking (if on) plus the east / west move
    void gotoAxes(double a1, double a2);
    bool haDecToAxes(double ra, double dec, double& a1, double& a2) const;   ///< false: no site
    double lstHours() const;

    std::vector<TransportFactory> opens_;
    std::unique_ptr<CommandChannel> ch_;
    std::optional<Site> site_;
    std::function<double()> jdNow_;
    uint32_t cpr_[2] = {0, 0};
    uint32_t timerFreq_ = 0;
    int highSpeed_[2] = {16, 16};
    double trackArcsec_ = 15.041067;   ///< sidereal
    bool trackingOn_ = false;          ///< asked for (tracks once no goto runs)
    double moveRa_ = 0, moveDec_ = 0;  ///< east / west and north / south moves, degrees per second against the sky
    bool gotoActive_ = false, parking_ = false, parked_ = false;
    int gotoPasses_ = 0;
    double targetRa_ = 0, targetDec_ = 0;
    double syncHa_ = 0, syncDec_ = 0;  ///< the sync's shift of the model, hours and degrees
};

/// 0x123456 <-> "563412" (low byte first, as the controller sends it).
std::string skyWatcherHex24(uint32_t v);
uint32_t skyWatcherValue(std::string_view hex);

}  // namespace slopcap::mounts

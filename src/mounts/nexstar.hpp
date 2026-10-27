// Mounts through their hand controller's serial port (or a SkyFi-style Wi-Fi bridge on it): Celestron NexStar / CPC /
// CGE / SLT / Advanced GT and Sky-Watcher SynScan, which speak nearly the same protocol. From the vendors' documents:
//   Celestron "NexStar Communication Protocol" (hand control versions 1.2 to 4.x)
//   Sky-Watcher "SynScan Serial Communication Protocol" version 3.3 (SynScan V3 3.38+, V4 4.38.06+)
// Positions are fractions of a turn in hex ("e" / "r" / "s"). SynScan's RA/Dec are J2000 (its document says so);
// Celestron's document doesn't say, so they're taken as of date. Moves are the fixed hand-controller rates 1-9 through
// the motor pass-through ("P"); there are no guide pulses, so the worker times rate-1 moves, which Celestron says
// don't upset equatorial tracking. No park in this protocol. Which way the RA motor's "positive" turns the sky follows
// INDI's NexStar driver (west); to be checked on a mount.
#pragma once

#include "mounts/command_channel.hpp"
#include "mounts/mount_driver.hpp"

namespace slopcap::mounts {

enum class HandController { Auto, Celestron, SynScan };

class NexStarDriver : public IMountDriver {
public:
    NexStarDriver(TransportFactory open, HandController kind);
    MountInfo connect() override;
    void disconnect() override { ch_.reset(); }
    MountStatus poll() override;
    void slewTo(double ra, double dec) override;
    void abort() override;
    void move(Dir d, int rate) override;
    void stop(Dir d) override;
    void setTracking(bool on) override;
    void sync(double ra, double dec) override;
    HandController kind() const { return kind_; }

private:
    CommandChannel& ch();
    void motor(Dir d, int rate);   ///< fixed-rate move (0 stops that motor)
    TransportFactory open_;
    std::unique_ptr<CommandChannel> ch_;
    HandController kind_;
    bool aligned_ = false;
    int trackMode_ = 2;   ///< the mode tracking goes back on in (what the mount was in, else EQ north / EQ)
};

/// "34AB0500,12CE0500" <-> RA hours, Dec degrees (fractions of a turn, 32 bits of which the top 24 count).
std::optional<std::pair<double, double>> parseNexStarRaDec(std::string_view s);
std::string nexStarRaDec(double ra, double dec);

}  // namespace slopcap::mounts

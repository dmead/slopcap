// The LX200 family, one engine with dialects (docs/design/mounts.md §3.5), from the vendors' own command sets:
//   Meade Telescope Serial Command Protocol, revision 2007.10 (LX200 Classic, Autostar / Audiostar, Autostar II,
//   LX200GPS); OnStepX's docs/COMMAND_REFERENCE.md and command handlers (2026-10).
// Coordinates are of date (JNow) in all of them; the engine asks for high precision (:U#) when the mount talks low.
//
//               Meade Classic        Meade (Autostar, II, GPS)     OnStep / OnStepX
//   identify    no :GVP# reply       :GVP# product name            :GVP# "On-Step"
//   pulses      none: the worker     :MgnDDDD# (4 digits)          :MgnD...#
//               times :Mn# / :Qn#
//   tracking    always on            :AP# / :AA# on, :AL# off      :Te# / :Td#, state in :GU#
//   rates       :TQ# :TL#            :TQ# :TL#                     :TQ# :TL# :TS# :TK#
//   park        none                 :hP#                          :hP# / :hR#, state in :GU#
//   pier side   -                    -                             :Gm#
#pragma once

#include "mounts/command_channel.hpp"
#include "mounts/mount_driver.hpp"

namespace slopcap::mounts {

enum class Lx200Dialect { Auto, MeadeClassic, Meade, OnStep };

class Lx200Driver : public IMountDriver {
public:
    Lx200Driver(TransportFactory open, Lx200Dialect dialect);
    MountInfo connect() override;
    void disconnect() override { ch_.reset(); }
    MountStatus poll() override;
    void slewTo(double ra, double dec) override;
    void abort() override;
    void move(Dir d, int rate) override;
    void stop(Dir d) override;
    void pulse(Dir d, int ms) override;
    void setTracking(bool on) override;
    void setTrackRate(TrackRate r) override;
    void park() override;
    void unpark() override;
    void sync(double ra, double dec) override;
    Lx200Dialect dialect() const { return dialect_; }

private:
    CommandChannel& ch();
    void setTarget(double ra, double dec);
    TransportFactory open_;
    std::unique_ptr<CommandChannel> ch_;
    Lx200Dialect dialect_;
    char mounting_ = 'P';   ///< the ACK reply: A alt-az, P polar, G German polar, L land (tracking off)
    int rate_ = -1;         ///< the slew rate last chosen (:RG# ... :RS#)
};

/// "HH:MM:SS" or "HH:MM.T" -> hours; "sDD*MM:SS", "sDD*MM'SS", "sDD*MM" (any separators) -> degrees. nullopt: not one.
std::optional<double> parseLx200Ra(std::string_view s);
std::optional<double> parseLx200Dec(std::string_view s);

}  // namespace slopcap::mounts

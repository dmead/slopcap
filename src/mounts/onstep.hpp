// OnStep and OnStepX, on their own driver rather than as an LX200 dialect (docs/design/mounts.md §3.7), from
// OnStepX's command handlers (github.com/hjd1964/OnStepX, src/telescope/mount/*/*.command.cpp and
// src/libApp/commands/ProcessCmds.cpp, 2026-11) and replies read from an OnStepX 10.25a (a JTW Trident P75).
//
// What it does beyond the LX200 basics:
//   link         serial without a speed tries 9600, 230400, 115200, 57600, 38400 and 19200 baud (OnStepX's default
//                is 9600, but ESP32 builds often run faster); TCP to its Wi-Fi (port 9999). DTR and RTS stay low (an
//                ESP32 or Arduino resets when they rise). A port someone else has open (the ASCOM driver) is named.
//   framing      OnStepX answers set commands with a bare 0 or 1, no '#', and unknown commands with a bare 0:
//                every reply is read the way OnStepX sends it, so a stray byte never shifts the replies after it.
//   status       one :GU# per poll: tracking, goto, parked / parking / park failed, at home, homing, guiding, pier
//                side, mount type, the pulse-guide rate and the mount's own error (a limit, a motor fault), in words.
//   rates        the ten OnStep rates (:R0# to :R9#, 0.25x to max), not Meade's four. The rate a move sets is also
//                the speed of everyone's guide pulses (PHD2's), and a move at 1x or slower is stored as the
//                pulse-guide rate (GUIDE_SEPARATE_PULSE_RATE): when the moves end the driver puts back the rates
//                :GU# gave before them.
//   gotos        right after a move the mount is still slowing down and refuses a goto ("already moving"): the
//                driver asks again for a few seconds.
//   tracking     :Te# is checked: a mount whose tracking rate was lost (after :ERESET#, :GT# says 0) gets its rate
//                back (:TQ# etc.) and tracking checked again. Sidereal, lunar, solar and King, and rate offsets
//                (:SXTR / :SXTD, arcseconds per sidereal second) for a planet's own motion where the firmware has them.
//   errors       a refused goto, sync, park or tracking change says why (:MS# / :CM# codes, :GE#).
//   on connect   the mount's clock against this computer's (an hour off: the UTC offset and daylight saving), its
//                site against Settings', and its limits and guide rate for the log. Nothing is written to the mount
//                that the user didn't ask for.
#pragma once

#include "mounts/command_channel.hpp"
#include "mounts/mount_driver.hpp"

#include <chrono>

namespace slopcap::mounts {

/// What :GU# says (OnStepX's Status.command.cpp).
struct OnStepStatus {
    bool tracking = false, slewing = false;
    char park = 'p';              ///< p unparked, I parking, P parked, F park failed
    bool atHome = false, homing = false, pulseGuiding = false, guiding = false;
    char mountType = 0;           ///< E German, K fork, A alt-az, L alt-alt
    PierSide pier = PierSide::Unknown;
    int pulseRate = -1;           ///< :R index (0 = 0.25x ... 9 = max, 10 custom); -1: not said
    int guideRate = -1;
    int error = 0;                ///< the general error code (onStepFault())
    std::optional<TrackRate> rate;   ///< lunar, solar or King when :GU# says so; nullopt: sidereal or not said
    char compensation = 0;        ///< r refraction, t full (OnTrack); 0 none
    bool singleAxis = false;      ///< (with compensation) RA only
    bool buzzer = false, autoFlip = false, pauseAtHome = false;
};
std::optional<OnStepStatus> parseOnStepStatus(std::string_view gu);
/// The general error code in words ("" for 0).
std::string onStepFault(int code);
/// A command error code (:GE#) in words.
std::string onStepCommandError(int code);

class OnStepDriver : public IMountDriver {
public:
    using SystemClock = std::function<std::chrono::system_clock::time_point()>;
    /// `opens`: the links to try in turn (one per baud rate); `site`: SlopCap's, to compare with the mount's.
    explicit OnStepDriver(std::vector<TransportFactory> opens, std::optional<Site> site = std::nullopt, SystemClock now = {});
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
    void setRateOffsets(double raArcsecPerSec, double decArcsecPerSec) override;
    std::vector<MountSetting> settings() override;
    void changeSetting(const std::string& key, const std::string& value) override;

    /// Go to the home position (:hC#, no reply; :GU# says H when it's there).
    void home();

    static constexpr int kRates = 10;   ///< :R0# .. :R9#
    static inline int kGotoRetries = 12, kGotoRetryMs = 500;   ///< asking again while the mount slows down (tests shorten)
    const std::string& firmware() const { return firmware_; }

private:
    CommandChannel& ch();
    bool numeric(std::string_view cmd, int ms = 1500);   ///< a 0 / 1 reply
    std::string lastError();                             ///< :GE# in words
    void setTarget(double ra, double dec);
    void restoreRates();
    void checkClock(MountInfo& info);
    void checkSite(MountInfo& info);
    void setNumeric(const std::string& cmd, const std::string& what);   ///< a 1, or the mount's reason
    void syncTime();
    char mountType_ = 0;
    std::vector<TransportFactory> opens_;
    std::optional<Site> site_;
    SystemClock now_;
    std::unique_ptr<CommandChannel> ch_;
    std::string firmware_;
    bool highPrecision_ = false;         ///< :GRH# / :GDH# answer (HH:MM:SS.SSSS)
    int pulseRate_ = -1, guideRate_ = -1;   ///< the mount's own :R indices (:GU#), put back after our moves
    int moveRate_ = -1;                  ///< the :R# rate last sent
    bool moving_[2] = {false, false};    ///< N/S, E/W
    bool ratesChanged_ = false;          ///< moves of ours have changed them
    TrackRate trackRate_ = TrackRate::Sidereal;
};

}  // namespace slopcap::mounts

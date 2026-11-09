// The built-in mount drivers' common interface (docs/design/mounts.md §2.1, §3.2): a few synchronous calls that may
// block, made only on the mount's own thread (MountWorker), never the GUI's. A driver only translates between these
// calls and its mount's protocol; polling, emulated guide pulses, the move deadman, J2000 <-> of date, refusing what a
// mount can't do and reconnecting are the worker's, written once for every driver.
//
// The core is what every mount's protocol has: identify, where it points and whether it slews, goto, abort, move each
// way at the mount's own speeds and stop, sync. The rest is optional, declared in MountInfo (caps, trackRates), and the
// worker calls an optional one only when the driver declared it.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace slopcap::mounts {

/// Anything a mount or its link can't do: the message is for the user ("the mount refused the goto: below the horizon").
struct MountError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class Dir { North, South, East, West };
enum class TrackRate { Sidereal, Lunar, Solar, King };
enum class PierSide { Unknown, East, West };

inline bool isNorthSouth(Dir d) { return d == Dir::North || d == Dir::South; }
/// "sidereal", "lunar", "solar", "King"
inline const char* trackRateName(TrackRate r) {
    return r == TrackRate::Lunar ? "lunar" : r == TrackRate::Solar ? "solar" : r == TrackRate::King ? "King" : "sidereal";
}

struct Site {
    double latitude = 0, longitude = 0;   ///< degrees, north and east positive
};

/// One of a mount's own settings (OnStep's site, limits, backlash, ...), as the driver reads it from the mount, for
/// Settings and log... in the mount panel. Values travel as text; a driver writes one with changeSetting().
struct MountSetting {
    enum class Kind { Number, Choice, Toggle, Text, Info, Action };
    std::string key;     ///< the driver's name for it ("horizon")
    std::string group;   ///< "Site and time", "Limits", ...
    std::string label;
    Kind kind = Kind::Info;
    std::string value;   ///< Number: "12.5"; Choice: one of choices' values; Toggle: "1" / "0"; Info: the text
    std::vector<std::pair<std::string, std::string>> choices;   ///< value, label
    double min = 0, max = 0;
    int decimals = 0;
    std::string unit;    ///< "°", "min", "″", "m"
    std::string help;    ///< a tooltip; an Action's is asked as a question first when it says "?"
};

/// What a mount can do besides the core.
struct Caps {
    bool pulse = false;        ///< timed guide pulses (without: the worker times a move at the slowest rate)
    bool park = false, unpark = false;
    bool tracking = false;     ///< tracking on / off
    bool rateOffsets = false;  ///< tracking a little faster or slower on each axis (a planet's own motion)
    bool settings = false;     ///< settings() / changeSetting(): the mount's own settings
};

struct MountInfo {
    std::string backend;              ///< the spec's name: "lx200", "nexstar", ...
    std::string model;                ///< what the mount says it is ("Autostar", "On-Step 10.24", "SynScan 4.39.05")
    bool j2000 = false;               ///< reads and writes J2000 coordinates (else of date, JNow)
    std::vector<std::string> rates;   ///< move rates, slowest first; move() takes an index into this
    std::vector<TrackRate> trackRates;   ///< what setTrackRate() takes (empty: the mount tracks at its own rate)
    Caps caps;
    std::string details;              ///< for the log: link, limits, guide rate ("" when the driver has nothing to add)
    std::vector<std::string> notes;   ///< things found on connecting that the user should hear (a clock that's off)
};

struct MountStatus {
    bool haveCoords = false;   ///< false: the mount can't say (not aligned, no site for an axis-only mount)
    double ra = 0, dec = 0;    ///< hours, degrees: the driver's epoch (MountInfo::j2000); the worker reports JNow
    bool slewing = false;      ///< a goto (or park) under way
    std::optional<bool> tracking, parked;   ///< nullopt: the mount doesn't say
    std::optional<TrackRate> trackRate;     ///< nullopt: the mount doesn't say
    PierSide pier = PierSide::Unknown;
    bool pulsing = false;      ///< (the worker's: a guide pulse is running)
    bool atHome = false;       ///< at the home position (where an OnStep starts: counterweights down, at the pole)
    std::string fault;         ///< what the mount itself says is wrong ("" when nothing): a limit, a motor fault
};

/// Opens the link (on the worker thread: network sockets belong to the thread that makes them).
class ITransport;
using TransportFactory = std::function<std::unique_ptr<ITransport>()>;

class IMountDriver {
public:
    virtual ~IMountDriver() = default;
    // the core
    virtual MountInfo connect() = 0;       ///< open the link, identify the mount (throws MountError)
    virtual void disconnect() {}
    virtual MountStatus poll() = 0;
    virtual void slewTo(double ra, double dec) = 0;   ///< then track
    virtual void abort() = 0;              ///< stop gotos and moves (tracking carries on)
    virtual void move(Dir d, int rate) = 0;   ///< that way at rates[rate] until stop(d) or abort()
    virtual void stop(Dir d) = 0;          ///< end the move on d's axis
    virtual void sync(double ra, double dec) = 0;   ///< "the mount points here"
    // optional: called only when MountInfo declares it
    virtual void pulse(Dir, int /*ms*/) {} ///< caps.pulse
    virtual void setTracking(bool) {}      ///< caps.tracking
    virtual void setTrackRate(TrackRate) {}   ///< one of trackRates
    virtual void park() {}                 ///< caps.park
    virtual void unpark() {}               ///< caps.unpark
    /// caps.rateOffsets: track this much faster than the chosen rate, arcseconds per SI second, RA (as an angle,
    /// 15" = 1 s of time) and Dec; 0, 0 is the plain rate again.
    virtual void setRateOffsets(double /*raArcsecPerSec*/, double /*decArcsecPerSec*/) {}
    /// caps.settings: the mount's own settings as it has them now (a setting the firmware lacks is left out) ...
    virtual std::vector<MountSetting> settings() { return {}; }
    /// ... and one written (an Action's value is ignored). Throws MountError with the mount's reason.
    virtual void changeSetting(const std::string& /*key*/, const std::string& /*value*/) {}
};

}  // namespace slopcap::mounts

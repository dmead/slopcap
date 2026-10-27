// The built-in mount drivers' common interface (docs/design/mounts.md §3.2): a few synchronous calls that may block,
// made only on the mount's own thread (MountWorker), never the GUI's. A driver only translates between these calls and
// its mount's protocol; polling, emulated guide pulses, the move deadman, J2000 <-> of date and reconnecting are the
// worker's, written once for every driver.
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

struct Site {
    double latitude = 0, longitude = 0;   ///< degrees, north and east positive
};

struct Caps {
    bool pulse = false;        ///< timed guide pulses (without: the worker times a move at the slowest rate)
    bool park = false, unpark = false;
    bool tracking = false;     ///< tracking on / off
    bool trackRates = false;   ///< lunar, solar (and King where there is one)
    bool sync = false;         ///< "the mount points here"
};

struct MountInfo {
    std::string backend;              ///< the spec's name: "lx200", "nexstar", ...
    std::string model;                ///< what the mount says it is ("Autostar", "On-Step 10.24", "SynScan 4.39.05")
    bool j2000 = false;               ///< reads and writes J2000 coordinates (else of date, JNow)
    std::vector<std::string> rates;   ///< move rates, slowest first; move() takes an index into this
    Caps caps;
};

struct MountStatus {
    bool haveCoords = false;   ///< false: the mount can't say (not aligned, no site for an axis-only mount)
    double ra = 0, dec = 0;    ///< hours, degrees: the driver's epoch (MountInfo::j2000); the worker reports JNow
    bool slewing = false;      ///< a goto (or park) under way
    std::optional<bool> tracking, parked;   ///< nullopt: the mount doesn't say
    PierSide pier = PierSide::Unknown;
    bool pulsing = false;      ///< (the worker's: a guide pulse is running)
};

/// Opens the link (on the worker thread: network sockets belong to the thread that makes them).
class ITransport;
using TransportFactory = std::function<std::unique_ptr<ITransport>()>;

class IMountDriver {
public:
    virtual ~IMountDriver() = default;
    virtual MountInfo connect() = 0;       ///< open the link, identify the mount (throws MountError)
    virtual void disconnect() {}
    virtual MountStatus poll() = 0;
    virtual void slewTo(double ra, double dec) = 0;   ///< then track
    virtual void abort() = 0;              ///< stop gotos and moves (tracking carries on)
    virtual void move(Dir d, int rate) = 0;   ///< that way at rates[rate] until stop(d) or abort()
    virtual void stop(Dir d) = 0;          ///< end the move on d's axis
    virtual void pulse(Dir, int /*ms*/) {} ///< caps.pulse
    virtual void setTracking(bool) {}      ///< caps.tracking
    virtual void setTrackRate(TrackRate) {}   ///< caps.trackRates
    virtual void park() {}                 ///< caps.park
    virtual void unpark() {}               ///< caps.unpark
    virtual void sync(double /*ra*/, double /*dec*/) {}   ///< caps.sync
};

}  // namespace slopcap::mounts

// A mount in software: the `sim` backend (SlopCap's mount controls with nothing plugged in) and the mount behind every
// protocol fake in the tests. Equatorial, coordinates of date; slews each axis at a fixed speed; tracks the sky, or
// lets it drift past when tracking is off; moves and guide pulses push it at their rates. Thread-safe: a test's fake
// device and the driver's worker thread may both use one.
#pragma once

#include "mounts/mount_driver.hpp"

#include <functional>
#include <mutex>

namespace slopcap::mounts {

class SimMount {
public:
    using Clock = std::function<double()>;   ///< seconds; a test's clock, or the steady clock
    explicit SimMount(Clock clock = {});

    struct State {
        double ra = 0, dec = 0;   ///< hours, degrees
        bool tracking = true, slewing = false, parked = false, pulsing = false;
        TrackRate rate = TrackRate::Sidereal;
        PierSide pier = PierSide::Unknown;
    };
    State state();   ///< as of now

    bool goTo(double ra, double dec);   ///< false: parked
    void abort();
    void move(Dir d, double degPerSec);
    void stop(Dir d);
    void pulse(Dir d, int ms);          ///< at guideRate
    void setTracking(bool on);
    void setTrackRate(TrackRate r);
    void park();                        ///< slews to the pole, then stops tracking
    void unpark();
    void sync(double ra, double dec);
    void place(double ra, double dec);  ///< (tests) put it there

    double slewRate = 8.0;              ///< degrees per second, each axis
    double guideRate = 0.5 * 15.041067 / 3600.0;   ///< degrees per second (a test makes it bigger, to measure)

private:
    void advance();   ///< up to now (holding the lock)
    std::mutex m_;
    Clock clock_;
    double last_ = 0;
    State s_;
    double targetRa_ = 0, targetDec_ = 0;
    bool parking_ = false;
    double moveRa_ = 0, moveDec_ = 0;    ///< degrees per second, east and north positive
    double pulseRa_ = 0, pulseDec_ = 0, pulseRaUntil_ = 0, pulseDecUntil_ = 0;
};

/// The `sim` backend: a SimMount behind the driver interface, with every capability.
class SimDriver : public IMountDriver {
public:
    explicit SimDriver(std::shared_ptr<SimMount> mount = {});
    MountInfo connect() override;
    MountStatus poll() override;
    void slewTo(double ra, double dec) override;
    void abort() override { m_->abort(); }
    void move(Dir d, int rate) override;
    void stop(Dir d) override { m_->stop(d); }
    void pulse(Dir d, int ms) override { m_->pulse(d, ms); }
    void setTracking(bool on) override { m_->setTracking(on); }
    void setTrackRate(TrackRate r) override { m_->setTrackRate(r); }
    void park() override { m_->park(); }
    void unpark() override { m_->unpark(); }
    void sync(double ra, double dec) override { m_->sync(ra, dec); }

private:
    std::shared_ptr<SimMount> m_;
};

}  // namespace slopcap::mounts

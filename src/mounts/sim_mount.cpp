#include "mounts/sim_mount.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace slopcap::mounts {

namespace {

constexpr double kSiderealHoursPerSecond = 1.00273790935 / 3600.0;   // how fast RA passes a still telescope
constexpr double kSidereal = 360.0 / 86164.0905;                      // degrees per second
constexpr double kRates[] = {0.5 * kSidereal, 8 * kSidereal, 64 * kSidereal, 4.0};   // Guide, Centre, Find, Max

double wrap24(double h) {
    h = std::fmod(h, 24.0);
    return h < 0 ? h + 24.0 : h;
}

/// The shorter way round from a to b, hours.
double raStep(double a, double b) {
    double d = std::fmod(b - a, 24.0);
    if (d > 12) d -= 24;
    if (d < -12) d += 24;
    return d;
}

}  // namespace

SimMount::SimMount(Clock clock) : clock_(std::move(clock)) {
    if (!clock_) {
        const auto start = std::chrono::steady_clock::now();
        clock_ = [start] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    }
    last_ = clock_();
    s_.ra = 6.0, s_.dec = 20.0;
}

void SimMount::advance() {
    const double now = clock_(), dt = now - last_;
    if (dt <= 0) return;
    // a still telescope sees RA go by; tracking at another rate than sidereal does that slowly too
    const double drift = !s_.tracking || s_.parked ? 1.0
                         : s_.rate == TrackRate::Lunar ? 1.0 - 14.685 / 15.041067
                         : s_.rate == TrackRate::Solar ? 1.0 - 15.0 / 15.041067
                         : s_.rate == TrackRate::King  ? 1.0 - 15.0369 / 15.041067
                                                       : 0.0;
    s_.ra += drift * kSiderealHoursPerSecond * dt;
    if (s_.slewing) {
        const double step = slewRate * dt;
        const double dRa = raStep(s_.ra, targetRa_) * 15.0, dDec = targetDec_ - s_.dec;
        s_.ra += std::clamp(dRa, -step, step) / 15.0;
        s_.dec += std::clamp(dDec, -step, step);
        if (std::abs(dRa) <= step && std::abs(dDec) <= step) {
            s_.ra = targetRa_, s_.dec = targetDec_;
            s_.slewing = false;
            if (parking_) parking_ = false, s_.parked = true, s_.tracking = false;
        }
    }
    auto active = [&](double until) { return std::max(0.0, std::min(now, until) - last_); };
    s_.ra += (moveRa_ * dt + pulseRa_ * active(pulseRaUntil_)) / 15.0;
    s_.dec = std::clamp(s_.dec + moveDec_ * dt + pulseDec_ * active(pulseDecUntil_), -90.0, 90.0);
    s_.ra = wrap24(s_.ra);
    s_.pulsing = now < pulseRaUntil_ || now < pulseDecUntil_;
    last_ = now;
}

SimMount::State SimMount::state() {
    std::lock_guard lock(m_);
    advance();
    s_.pier = s_.dec < 90 ? PierSide::East : PierSide::Unknown;
    return s_;
}

bool SimMount::goTo(double ra, double dec) {
    std::lock_guard lock(m_);
    advance();
    if (s_.parked) return false;
    targetRa_ = wrap24(ra), targetDec_ = std::clamp(dec, -90.0, 90.0);
    s_.slewing = true;
    s_.tracking = true;
    parking_ = false;
    return true;
}

void SimMount::abort() {
    std::lock_guard lock(m_);
    advance();
    s_.slewing = parking_ = false;
    moveRa_ = moveDec_ = 0;
}

void SimMount::move(Dir d, double v) {
    std::lock_guard lock(m_);
    advance();
    if (isNorthSouth(d)) moveDec_ = d == Dir::North ? v : -v;
    else moveRa_ = d == Dir::East ? v : -v;
}

void SimMount::stop(Dir d) {
    std::lock_guard lock(m_);
    advance();
    (isNorthSouth(d) ? moveDec_ : moveRa_) = 0;
}

void SimMount::pulse(Dir d, int ms) {
    std::lock_guard lock(m_);
    advance();
    const double until = last_ + ms / 1000.0;
    if (isNorthSouth(d)) pulseDec_ = d == Dir::North ? guideRate : -guideRate, pulseDecUntil_ = until;
    else pulseRa_ = d == Dir::East ? guideRate : -guideRate, pulseRaUntil_ = until;
    s_.pulsing = true;
}

void SimMount::setTracking(bool on) {
    std::lock_guard lock(m_);
    advance();
    if (!s_.parked) s_.tracking = on;
}

void SimMount::setTrackRate(TrackRate r) {
    std::lock_guard lock(m_);
    advance();
    s_.rate = r;
}

void SimMount::park() {
    std::lock_guard lock(m_);
    advance();
    targetRa_ = s_.ra, targetDec_ = 90.0;
    s_.slewing = parking_ = true;
}

void SimMount::unpark() {
    std::lock_guard lock(m_);
    advance();
    s_.parked = false;
    s_.tracking = true;
}

void SimMount::sync(double ra, double dec) { place(ra, dec); }

void SimMount::place(double ra, double dec) {
    std::lock_guard lock(m_);
    advance();
    s_.ra = wrap24(ra), s_.dec = std::clamp(dec, -90.0, 90.0);
}

// ---------------------------------------------------------------- SimDriver

SimDriver::SimDriver(std::shared_ptr<SimMount> mount) : m_(mount ? std::move(mount) : std::make_shared<SimMount>()) {}

MountInfo SimDriver::connect() {
    MountInfo info;
    info.backend = "sim";
    info.model = "Simulator";
    info.rates = {"Guide", "Centre", "Find", "Max"};
    info.caps = {true, true, true, true, true, true};
    return info;
}

MountStatus SimDriver::poll() {
    const auto s = m_->state();
    MountStatus st;
    st.haveCoords = true;
    st.ra = s.ra, st.dec = s.dec;
    st.slewing = s.slewing;
    st.tracking = s.tracking;
    st.parked = s.parked;
    st.pier = s.pier;
    return st;
}

void SimDriver::slewTo(double ra, double dec) {
    if (!m_->goTo(ra, dec)) throw MountError("the mount is parked: unpark it first");
}

void SimDriver::move(Dir d, int rate) { m_->move(d, kRates[std::clamp(rate, 0, 3)]); }

}  // namespace slopcap::mounts

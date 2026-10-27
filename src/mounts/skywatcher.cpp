#include "mounts/skywatcher.hpp"

#include "analysis/ephemeris.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <thread>

namespace slopcap::mounts {

namespace {

constexpr uint32_t kHome = 0x800000;                     // both axes' count at home (pointing at the pole)
constexpr double kSidereal = 360.0 / 86164.0905;         // degrees per second
constexpr double kRates[] = {0.5, 1, 8, 32, 128, 400, 800};   // move rates, times sidereal
constexpr int kReplyMs = 1000;

double norm180(double a) {
    a = std::fmod(a + 180.0, 360.0);
    return (a < 0 ? a + 360.0 : a) - 180.0;
}

double norm12(double h) {
    h = std::fmod(h + 12.0, 24.0);
    return (h < 0 ? h + 24.0 : h) - 12.0;
}

const char* errorText(std::string_view code) {
    switch (std::strtol(std::string(code).c_str(), nullptr, 16)) {
        case 0: return "unknown command";
        case 1: return "command length error";
        case 2: return "motor not stopped";
        case 3: return "invalid character";
        case 4: return "not initialised";
        case 5: return "driver sleeping";
        case 7: return "PEC training is running";
        case 8: return "no valid PEC data";
    }
    return "error";
}

/// Axis angles (degrees from home) -> hour angle (hours) and Dec, German equatorial, northern hemisphere: on one side
/// of the pier the Dec axis turns from the pole one way, on the other side the other way, and the RA axis is a quarter
/// turn from the meridian either way.
void axesToHaDec(double a1, double a2, double& ha, double& dec) {
    a2 = norm180(a2);
    dec = 90.0 - std::abs(a2);
    ha = norm12(a1 / 15.0 + (a2 >= 0 ? -6.0 : 6.0));
}

}  // namespace

std::string skyWatcherHex24(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02X%02X%02X", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
    return buf;
}

uint32_t skyWatcherValue(std::string_view hex) {
    uint32_t v = 0;
    for (std::size_t i = 0; i + 2 <= hex.size() && i < 6; i += 2)
        v |= static_cast<uint32_t>(std::strtoul(std::string(hex.substr(i, 2)).c_str(), nullptr, 16)) << (4 * i);
    return v;
}

SkyWatcherDriver::SkyWatcherDriver(std::vector<TransportFactory> opens, std::optional<Site> site, std::function<double()> jdNow)
    : opens_(std::move(opens)), site_(site), jdNow_(std::move(jdNow)) {
    if (!jdNow_) jdNow_ = [] { return analysis::julianDate(std::chrono::system_clock::now()); };
}

std::string SkyWatcherDriver::command(char c, int axis, std::string_view data) {
    if (!ch_) throw MountError("the mount isn't connected");
    std::string cmd = {':', c, static_cast<char>('0' + axis)};
    cmd += data;
    cmd += '\r';
    // inquiries (lower case) only read: those may go twice
    const auto r = ch_->until(cmd, '\r', kReplyMs, c >= 'a' && c <= 'z');
    if (!r.empty() && r[0] == '!') throw MountError(std::string("the motor controller said: ") + errorText(r.substr(1)));
    if (r.empty() || r[0] != '=') throw MountError("the motor controller sent " + printable(r));
    return r.substr(1);
}

MountInfo SkyWatcherDriver::connect() {
    std::string version, failure = "no Sky-Watcher motor controller answers";
    for (const auto& open : opens_) {
        try {
            ch_ = std::make_unique<CommandChannel>(open(), "skywatcher");
            version = command('e', 1);
            break;
        } catch (const MountError& e) {
            failure = e.what();
            ch_.reset();
        }
    }
    if (!ch_) throw MountError(failure);
    for (int i = 0; i < 2; ++i) {
        cpr_[i] = skyWatcherValue(command('a', i + 1));
        highSpeed_[i] = std::max(1, static_cast<int>(skyWatcherValue(command('g', i + 1))));
    }
    timerFreq_ = skyWatcherValue(command('b', 1));
    if (!cpr_[0] || !cpr_[1] || !timerFreq_) throw MountError("the motor controller gave no step counts");
    // Just switched on, the axes aren't initialised: they're at home, so they count from there
    if (!status(1).init || !status(2).init) {
        command('E', 1, skyWatcherHex24(kHome));
        command('E', 2, skyWatcherHex24(kHome));
        command('F', 3);
    }
    gotoActive_ = parking_ = parked_ = false;
    moveRa_ = moveDec_ = 0;
    trackingOn_ = status(1).running && status(1).tracking;

    MountInfo info;
    info.backend = "skywatcher";
    info.model = "Sky-Watcher motor controller " + version + (site_ ? "" : " (no site in Settings: no coordinates or gotos)");
    info.rates = {"0.5\xC3\x97", "1\xC3\x97", "8\xC3\x97", "32\xC3\x97", "128\xC3\x97", "400\xC3\x97", "800\xC3\x97"};   // kRates

    info.caps.park = info.caps.unpark = info.caps.tracking = info.caps.trackRates = info.caps.sync = true;
    return info;
}

void SkyWatcherDriver::disconnect() {
    try {
        if (ch_ && (gotoActive_ || moveRa_ || moveDec_)) command('K', 3);   // a move or goto doesn't outlive the link
    } catch (const MountError&) {
    }
    ch_.reset();
}

SkyWatcherDriver::AxisStatus SkyWatcherDriver::status(int axis) {
    const auto s = command('f', axis);
    if (s.size() < 3) throw MountError("the motor controller sent a short status");
    auto nibble = [&](std::size_t i) { return static_cast<int>(std::strtol(std::string(1, s[i]).c_str(), nullptr, 16)); };
    AxisStatus a;
    a.tracking = nibble(0) & 1, a.ccw = nibble(0) & 2, a.fast = nibble(0) & 4;
    a.running = nibble(1) & 1, a.blocked = nibble(1) & 2;
    a.init = nibble(2) & 1;
    return a;
}

double SkyWatcherDriver::angle(int axis) {
    const auto steps = static_cast<int64_t>(skyWatcherValue(command('j', axis))) - kHome;
    return static_cast<double>(steps) * 360.0 / cpr_[axis - 1];
}

void SkyWatcherDriver::stopAxis(int axis, bool wait) {
    command('K', axis);
    if (!wait) return;
    for (int i = 0; i < 100; ++i) {   // decelerating from a fast slew takes a few seconds
        if (!status(axis).running) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    command('L', axis);   // instant stop
}

void SkyWatcherDriver::axisSpeed(int axis, double v) {
    if (std::abs(v) < 1e-9) {
        stopAxis(axis, false);
        return;
    }
    const bool ccw = v < 0, fast = std::abs(v) > 128 * kSidereal;   // the controller's high-speed mode, above 128x
    const double preset = timerFreq_ * 360.0 / std::abs(v) / cpr_[axis - 1] * (fast ? highSpeed_[axis - 1] : 1);
    const auto t1 = static_cast<uint32_t>(std::clamp<long long>(std::llround(preset), 1, 0xFFFFFF));
    auto s = status(axis);
    // A running axis takes a new step period only at slow speed, in the same direction and mode
    if (s.running && (!s.tracking || s.ccw != ccw || s.fast || fast)) {
        stopAxis(axis, true);
        s.running = false;
    }
    if (!s.running) command('G', axis, std::string{fast ? '3' : '1', ccw ? '1' : '0'});   // speed mode, slow / fast
    command('I', axis, skyWatcherHex24(t1));
    if (!s.running) command('J', axis);
}

void SkyWatcherDriver::raSpeed() {
    if (gotoActive_) return;   // the goto has the axis; tracking resumes when it ends
    const double sign = site_ && site_->latitude < 0 ? -1.0 : 1.0;
    const double track = trackingOn_ && !parked_ ? trackArcsec_ / 3600.0 : 0.0;
    axisSpeed(1, sign * (track + moveRa_));   // west: ahead of the sky; east: behind it, or backwards
}

double SkyWatcherDriver::lstHours() const { return analysis::localSiderealDeg(jdNow_(), site_->longitude) / 15.0; }

bool SkyWatcherDriver::haDecToAxes(double ra, double dec, double& a1, double& a2) const {
    if (!site_) return false;
    const bool south = site_->latitude < 0;
    const double ha = norm12(lstHours() - ra - syncHa_);
    double d = dec - syncDec_;
    if (south) d = -d;
    // the two ways of pointing there; the one with the counterweights below the RA axis
    const double a1e = norm180((ha + 6.0) * 15.0), a1w = norm180((ha - 6.0) * 15.0);
    const bool east = std::abs(a1e) <= 90.0;
    a1 = east ? a1e : a1w;
    a2 = east ? 90.0 - d : -(90.0 - d);
    if (south) a1 = -a1;
    return true;
}

void SkyWatcherDriver::gotoAxes(double a1, double a2) {
    const double target[2] = {a1, a2};
    for (int axis = 1; axis <= 2; ++axis) {
        const auto to = static_cast<uint32_t>(static_cast<int64_t>(kHome) + std::llround(target[axis - 1] / 360.0 * cpr_[axis - 1]));
        if (status(axis).running) stopAxis(axis, true);
        const auto at = skyWatcherValue(command('j', axis));
        command('G', axis, std::string{'0', to >= at ? '0' : '1'});   // goto mode, fast, which way
        command('S', axis, skyWatcherHex24(to));
        command('J', axis);
    }
    gotoActive_ = true;
}

MountStatus SkyWatcherDriver::poll() {
    MountStatus st;
    const double a1 = angle(1), a2 = angle(2);
    if (gotoActive_ && !status(1).running && !status(2).running) {
        double b1 = 0, b2 = 0;
        if (--gotoPasses_ > 0 && haDecToAxes(targetRa_, targetDec_, b1, b2)) {
            gotoAxes(b1, b2);   // again: the sky turned while it slewed
        } else {
            gotoActive_ = false;
            if (parking_) parking_ = false, parked_ = true, trackingOn_ = false;
            raSpeed();
        }
    }
    st.slewing = gotoActive_;
    st.tracking = trackingOn_ && !gotoActive_ && !parked_;
    st.parked = parked_;
    if (site_) {
        const bool south = site_->latitude < 0;
        double ha = 0, dec = 0;
        axesToHaDec(south ? -a1 : a1, a2, ha, dec);
        if (south) dec = -dec;
        st.haveCoords = true;
        st.ra = std::fmod(lstHours() - (ha + syncHa_) + 48.0, 24.0);
        st.dec = dec + syncDec_;
    }
    return st;
}

void SkyWatcherDriver::slewTo(double ra, double dec) {
    if (parked_) throw MountError("the mount is parked: unpark it first");
    double a1 = 0, a2 = 0;
    if (!haDecToAxes(ra, dec, a1, a2))
        throw MountError("set the site in Settings first: on its own motors the mount can't know where anything is without it");
    targetRa_ = ra, targetDec_ = dec;
    gotoPasses_ = 2;
    parking_ = false;
    moveRa_ = moveDec_ = 0;
    trackingOn_ = true;   // tracks on arrival
    gotoAxes(a1, a2);
}

void SkyWatcherDriver::abort() {
    gotoActive_ = parking_ = false;
    moveRa_ = moveDec_ = 0;
    command('K', 3);
    raSpeed();   // tracking carries on
}

void SkyWatcherDriver::move(Dir d, int rate) {
    if (parked_) throw MountError("the mount is parked");
    if (gotoActive_) throw MountError("a goto is under way");
    const double v = kRates[std::clamp(rate, 0, static_cast<int>(std::size(kRates)) - 1)] * kSidereal;
    if (isNorthSouth(d)) {
        moveDec_ = d == Dir::North ? v : -v;
        // northward the Dec axis turns towards the pole: one way on one side of the pier, the other on the other
        const double side = norm180(angle(2)) >= 0 ? -1.0 : 1.0, sign = site_ && site_->latitude < 0 ? -1.0 : 1.0;
        axisSpeed(2, moveDec_ * side * sign);
    } else {
        moveRa_ = d == Dir::West ? v : -v;
        raSpeed();
    }
}

void SkyWatcherDriver::stop(Dir d) {
    if (isNorthSouth(d)) {
        moveDec_ = 0;
        if (!gotoActive_) stopAxis(2, false);
    } else {
        moveRa_ = 0;
        raSpeed();
    }
}

void SkyWatcherDriver::setTracking(bool on) {
    trackingOn_ = on;
    raSpeed();
}

void SkyWatcherDriver::setTrackRate(TrackRate r) {
    trackArcsec_ = r == TrackRate::Lunar ? 14.685 : r == TrackRate::Solar ? 15.0 : r == TrackRate::King ? 15.0369 : 15.041067;
    if (trackingOn_) raSpeed();
}

void SkyWatcherDriver::park() {
    parking_ = true;
    gotoPasses_ = 1;
    moveRa_ = moveDec_ = 0;
    gotoAxes(0, 0);   // home: at the pole, counterweights down
}

void SkyWatcherDriver::unpark() {
    parked_ = false;
    trackingOn_ = true;
    raSpeed();
}

void SkyWatcherDriver::sync(double ra, double dec) {
    if (!site_) throw MountError("set the site in Settings first");
    const bool south = site_->latitude < 0;
    double ha = 0, d = 0;
    axesToHaDec(south ? -angle(1) : angle(1), angle(2), ha, d);
    if (south) d = -d;
    syncHa_ = norm12(lstHours() - ra - ha);
    syncDec_ = dec - d;
}

}  // namespace slopcap::mounts

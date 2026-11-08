#include "fakes/fake_mounts.hpp"

#include "mounts/skywatcher.hpp"

#include <QByteArray>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <thread>

namespace slopcap::test {

using mounts::Dir;
using mounts::Lx200Dialect;

void FakeLink::write(std::string_view bytes) {
    if (state_->dead) throw mounts::MountError("the link went away (a fake link)");
    {
        std::lock_guard lock(state_->m);
        state_->written.emplace_back(bytes);
    }
    buffer_ += device_(bytes);
}

std::string FakeLink::read(int ms) {
    if (state_->dead) throw mounts::MountError("the link went away (a fake link)");
    if (buffer_.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(ms, 5)));   // nothing will come: a timeout
        return {};
    }
    return std::exchange(buffer_, {});
}

mounts::TransportFactory fakeLink(Device device, std::shared_ptr<LinkState> state, bool datagrams) {
    return [device, state, datagrams] { return std::make_unique<FakeLink>(device, state, datagrams); };
}

// ---------------------------------------------------------------- LX200

namespace {

double rateOf(char r) { return r == 'G' ? 0.2 : r == 'C' ? 1.0 : r == 'M' ? 3.0 : 6.0; }   // degrees per second

Dir dirOf(char c) { return c == 'n' ? Dir::North : c == 's' ? Dir::South : c == 'e' ? Dir::East : Dir::West; }

std::string lx200Ra(double h, bool longFormat) {
    char buf[48];
    if (longFormat) {
        const long s = std::lround(h * 3600) % 86400;
        std::snprintf(buf, sizeof buf, "%02ld:%02ld:%02ld#", s / 3600, s / 60 % 60, s % 60);
    } else {
        const long t = std::lround(h * 600) % 14400;   // tenths of a minute
        std::snprintf(buf, sizeof buf, "%02ld:%02ld.%ld#", t / 600, t / 10 % 60, t % 10);
    }
    return buf;
}

std::string lx200Dec(double d, bool longFormat, char secondsSep) {
    char buf[48];
    const long s = std::lround(std::abs(d) * 3600);
    if (longFormat) std::snprintf(buf, sizeof buf, "%c%02ld*%02ld%c%02ld#", d < 0 ? '-' : '+', s / 3600, s / 60 % 60, secondsSep, s % 60);
    else std::snprintf(buf, sizeof buf, "%c%02ld*%02ld#", d < 0 ? '-' : '+', s / 3600, s / 60 % 60);
    return buf;
}

}  // namespace

Device fakeLx200(std::shared_ptr<mounts::SimMount> sim, Lx200Dialect dialect) {
    struct State {
        bool longFormat = false;
        double ra = 0, dec = 0;
        char rate = 'C';
    };
    auto st = std::make_shared<State>();
    return [sim, dialect, st](std::string_view in) -> std::string {
        auto& s = *st;
        const bool onStep = dialect == Lx200Dialect::OnStep, classic = dialect == Lx200Dialect::MeadeClassic;
        const auto x = sim->state();
        if (in == "\x06") return std::string(1, !onStep && !x.tracking ? 'L' : 'P');
        if (in.size() < 3 || in.front() != ':' || in.back() != '#') return {};
        const std::string c(in.substr(1, in.size() - 2));
        auto is = [&](const char* p) { return c.rfind(p, 0) == 0; };
        if (c == "GVP") return classic ? "" : onStep ? "On-Step#" : "Autostar#";
        if (c == "GVN") return classic ? "" : onStep ? "10.29a#" : "43Eg#";
        if (c == "GR") return lx200Ra(x.ra, s.longFormat);
        if (c == "GD") return lx200Dec(x.dec, s.longFormat, onStep ? ':' : '\'');
        if (c == "U") return s.longFormat = !s.longFormat, "";
        if (c == "D") return x.slewing ? "\x7f#" : "#";
        if (is("Sr")) return s.ra = mounts::parseLx200Ra(c.substr(2)).value_or(-1), s.ra >= 0 ? "1" : "0";
        if (is("Sd")) {
            const auto d = mounts::parseLx200Dec(c.substr(2));
            s.dec = d.value_or(0);
            return d ? "1" : "0";
        }
        if (c == "MS") return sim->goTo(s.ra, s.dec) ? "0" : onStep ? "4" : "1Object below horizon#";
        if (c == "Q") return sim->abort(), "";
        if (c.size() == 2 && c[0] == 'Q') return sim->stop(dirOf(c[1])), "";
        if (c.size() == 2 && c[0] == 'M' && std::string("nsew").find(c[1]) != std::string::npos) return sim->move(dirOf(c[1]), rateOf(s.rate)), "";
        if (c.size() == 2 && c[0] == 'R') return s.rate = c[1], "";
        if (is("Mg") && !classic && c.size() > 3) return sim->pulse(dirOf(c[2]), std::atoi(c.c_str() + 3)), "";
        if ((c == "TQ" || c == "TL" || c == "TS" || c == "TK") && !classic)
            return sim->setTrackRate(c == "TL" ? mounts::TrackRate::Lunar : c == "TS" ? mounts::TrackRate::Solar : c == "TK" ? mounts::TrackRate::King : mounts::TrackRate::Sidereal), "";
        if (c == "CM") return sim->sync(s.ra, s.dec), onStep ? "N/A#" : " M31 EX GAL MAG 3.5 SZ178.0'#";
        if (onStep) {
            if (c == "Te" || c == "Td") return sim->setTracking(c == "Te"), "1";
            if (c == "GU") return std::string(x.tracking ? "" : "n") + (x.slewing ? "" : "N") + (x.parked ? "P" : "p") + "ET#";
            if (c == "Gm") return "E#";
            if (c == "hP") return sim->park(), "1";
            if (c == "hR") return sim->unpark(), "1";
        } else if (!classic) {
            if (c == "AP" || c == "AA" || c == "AL") return sim->setTracking(c != "AL"), "";
            if (c == "hP") return sim->park(), "";
        }
        return {};   // LX200s say nothing to what they don't know
    };
}

// ---------------------------------------------------------------- OnStepX

Device FakeOnStep::device() {
    return [this](std::string_view in) { return handle(in); };
}

std::string FakeOnStep::handle(std::string_view in) {
    std::lock_guard lock(m);
    if (in.size() < 3 || in.front() != ':' || in.back() != '#') return {};
    const std::string c(in.substr(1, in.size() - 2));
    auto is = [&](const char* p) { return c.rfind(p, 0) == 0; };
    const auto x = sim_->state();
    constexpr double kSidereal = 15.041067 / 3600;   // degrees per second
    static const double kRateX[] = {0.25, 0.5, 1, 2, 4, 8, 20, 48, 240, 480, 1};
    auto ok = [&](bool good, int why) {
        lastError = good ? 0 : why;
        return std::string(good ? "1" : "0");   // numeric: no '#'
    };
    if (c == "GE") {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%02d#", lastError);
        return buf;
    }
    lastError = 0;
    if (c == "GVP") return "On-Step#";
    if (c == "GVN") return firmware + "#";
    if (c == "GR" || (c == "GRH" && highPrecision)) {
        if (c == "GRH") {
            char buf[32];
            const double s = x.ra * 3600;
            std::snprintf(buf, sizeof buf, "%02d:%02d:%07.4f#", static_cast<int>(s / 3600), static_cast<int>(s / 60) % 60, std::fmod(s, 60.0));
            return buf;
        }
        return lx200Ra(x.ra, longFormat);
    }
    if (c == "GD" || (c == "GDH" && highPrecision)) {
        if (c == "GDH") {
            char buf[32];
            const double s = std::abs(x.dec) * 3600;
            std::snprintf(buf, sizeof buf, "%c%02d*%02d:%06.3f#", x.dec < 0 ? '-' : '+', static_cast<int>(s / 3600), static_cast<int>(s / 60) % 60, std::fmod(s, 60.0));
            return buf;
        }
        return lx200Dec(x.dec, longFormat, ':');
    }
    if (c == "U") return longFormat = !longFormat, "";
    if (c == "GU") {
        std::string s;
        if (!x.tracking && !trackingClaimed_) s += 'n';
        if (!x.slewing) s += 'N';
        s += parkFailed ? 'F' : x.parked ? 'P' : 'p';
        if (atHome) s += 'H';
        if (x.rate == mounts::TrackRate::Lunar) s += '(';
        if (x.rate == mounts::TrackRate::Solar) s += 'O';
        if (x.rate == mounts::TrackRate::King) s += 'k';
        if (compensation) s += compensation;
        if (compensation && singleAxis) s += 's';
        if (pauseAtHome) s += 'u';
        if (buzzer) s += 'z';
        if (autoFlip) s += 'a';
        s += 'E';
        s += x.pier == mounts::PierSide::East ? 'T' : x.pier == mounts::PierSide::West ? 'W' : 'o';
        s += static_cast<char>('0' + pulseRate);
        s += static_cast<char>('0' + guideRate);
        s += static_cast<char>('0' + error);
        return s + "#";
    }
    if (c == "Gm") return x.pier == mounts::PierSide::East ? "E#" : x.pier == mounts::PierSide::West ? "W#" : "N#";
    if (c == "GT") return lostRate ? "0.00000#" : x.tracking ? "60.16427#" : "0#";
    if (c == "GX90") {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.2f#", kRateX[pulseRate]);
        return buf;
    }
    if ((c == "GXTR" || c == "GXTD") && rateOffsets) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.8f#", c == "GXTR" ? raOffset : decOffset);
        return buf;
    }
    if ((is("SXTR,") || is("SXTD,")) && rateOffsets) {
        (c[3] == 'R' ? raOffset : decOffset) = std::atof(c.c_str() + 5);
        return ok(true, 0);
    }
    char out[48];
    if (c == "Gh") return std::snprintf(out, sizeof out, "%+03d*#", horizon), out;
    if (c == "Go") return std::snprintf(out, sizeof out, "%02d*#", overhead), out;
    if (c == "GXE9" || c == "GXEA") return std::snprintf(out, sizeof out, "%d#", c == "GXE9" ? meridianEast : meridianWest), out;
    if (c == "Gv") return elevation + "#";
    if (c == "GX95") return autoFlip ? "1#" : "0#";
    if (c == "GX96") return std::string(1, pierSide) + "#";
    if (c == "GX97") return "5.0#";
    if (c == "GXE4" || c == "GXE5") return "25600#";
    if (c == "%BR" || c == "%BD") return std::snprintf(out, sizeof out, "%d#", c == "%BR" ? backlashRa : backlashDec), out;
    auto intAfter = [&](std::size_t at, int lo, int hi, int& into) {
        char* end = nullptr;
        const long v = std::strtol(c.c_str() + at, &end, 10);
        if (end == c.c_str() + at || *end || v < lo || v > hi) return ok(false, 4);
        into = static_cast<int>(v);
        return ok(true, 0);
    };
    if (is("Sh")) return intAfter(2, -30, 30, horizon);
    if (is("So")) return intAfter(2, 60, 90, overhead);
    if (is("SXE9,")) return intAfter(5, -720, 720, meridianEast);
    if (is("SXEA,")) return intAfter(5, -720, 720, meridianWest);
    if (is("$BR")) return intAfter(3, 0, 3600, backlashRa);
    if (is("$BD")) return intAfter(3, 0, 3600, backlashDec);
    if (is("SX95,") || is("SX97,") || is("SX98,")) {
        if (c.size() != 6 || (c[5] != '0' && c[5] != '1')) return ok(false, 4);
        (c[3] == '5' ? autoFlip : c[3] == '7' ? buzzer : pauseAtHome) = c[5] == '1';
        return ok(true, 0);
    }
    if (is("SX96,")) {
        if (c.size() != 6 || std::string("EWBA").find(c[5]) == std::string::npos) return ok(false, 4);
        pierSide = c[5];
        return ok(true, 0);
    }
    if (is("SX93,")) return gotoPreset = std::atoi(c.c_str() + 5), "";
    if (is("Sv")) return elevation = c.substr(2), ok(true, 0);
    if (is("SG")) return utcOffset = c.substr(2), ok(c.size() == 8 && (c[2] == '+' || c[2] == '-'), 5);
    if (is("St")) return latitude = c.substr(2), ok(mounts::parseLx200Dec(c.substr(2)).has_value(), 5);
    if (is("Sg")) return longitude = c.substr(2), ok(c.size() >= 9, 5);
    if (is("SC")) return setDate = c.substr(2), ok(c.size() == 10, 5);
    if (is("SL")) return setTime = c.substr(2), ok(c.size() == 10, 5);
    if (c == "Tn" || c == "Tr" || c == "To") return compensation = c == "Tn" ? 0 : c[1] == 'r' ? 'r' : 't', ok(true, 0);
    if (c == "T1" || c == "T2") return singleAxis = c == "T1", ok(true, 0);
    if (c == "hQ") return parkSet = true, ok(true, 0);
    if (c == "hC") return sim_->goTo(sim_->state().ra, 90), atHome = true, "";
    if (c == "Gt") return latitude + "#";
    if (c == "Gg") return longitude + "#";
    if (c == "GG") return utcOffset + "#";
    if (c == "GC" || c == "GL") {
        // local time = UTC - offset (OnStep's offset is what to add to local time for UTC)
        const int sign = utcOffset[0] == '-' ? -1 : 1;
        const double offH = sign * (std::atoi(utcOffset.c_str() + 1) + std::atoi(utcOffset.c_str() + 4) / 60.0);
        const auto t = static_cast<std::time_t>(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() + clockOffset - offH * 3600);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[32];
        if (c == "GC") std::snprintf(buf, sizeof buf, "%02d/%02d/%02d#", tm.tm_mon + 1, tm.tm_mday, tm.tm_year % 100);
        else std::snprintf(buf, sizeof buf, "%02d:%02d:%02d#", tm.tm_hour, tm.tm_min, tm.tm_sec);
        return buf;
    }
    if (c == "D") return x.slewing ? "\x7f#" : "#";
    if (is("Sr")) return ra_ = mounts::parseLx200Ra(c.substr(2)).value_or(-1), ok(ra_ >= 0, 5);
    if (is("Sd")) {
        const auto d = mounts::parseLx200Dec(c.substr(2));
        dec_ = d.value_or(0);
        return ok(d.has_value(), 5);
    }
    if (c == "MS" && stillMoving > 0) return --stillMoving, lastError = 22, "8";
    if (c == "MS") return sim_->goTo(ra_, dec_) ? "0" : (lastError = 18, "4");   // a digit, no '#'
    if (c == "CM") {
        if (x.parked) return lastError = 18, "E4#";
        return sim_->sync(ra_, dec_), "N/A#";
    }
    if (c == "Q") return sim_->abort(), "";
    if (c.size() == 2 && c[0] == 'Q' && std::string("nsew").find(c[1]) != std::string::npos) return sim_->stop(dirOf(c[1])), "";
    if (c.size() == 2 && c[0] == 'R') {
        const auto at = std::string("0123456789").find(c[1]);
        const int r = c[1] == 'G' ? 2 : c[1] == 'C' ? 5 : c[1] == 'M' ? 6 : c[1] == 'F' ? 7 : c[1] == 'S' ? 8 : at != std::string::npos ? static_cast<int>(at) : -1;
        if (r < 0) return "0";
        guideRate = r;
        if (separatePulseRate && r <= 2) pulseRate = r;   // and into NV, as the firmware does
        return "";
    }
    if (c.size() == 2 && c[0] == 'M' && std::string("nsew").find(c[1]) != std::string::npos)
        return sim_->move(dirOf(c[1]), kRateX[guideRate] * kSidereal * speedup), "";
    if (is("Mg") && c.size() > 3) return sim_->pulse(dirOf(c[2]), std::atoi(c.c_str() + 3)), "";
    if (c == "TQ" || c == "TL" || c == "TS" || c == "TK") {
        lostRate = false;
        if (trackingClaimed_) sim_->setTracking(true), trackingClaimed_ = false;
        sim_->setTrackRate(c == "TL" ? mounts::TrackRate::Lunar : c == "TS" ? mounts::TrackRate::Solar : c == "TK" ? mounts::TrackRate::King : mounts::TrackRate::Sidereal);
        return "";
    }
    if (c == "Te") {
        if (x.parked) return ok(false, 9);
        if (lostRate) return trackingClaimed_ = true, ok(true, 0);
        return sim_->setTracking(true), ok(true, 0);
    }
    if (c == "Td") return trackingClaimed_ = false, sim_->setTracking(false), ok(true, 0);
    if (c == "hP") {
        if (noParkPosition) return ok(false, 12);
        return sim_->park(), ok(true, 0);
    }
    if (c == "hR") {
        if (!x.parked) return ok(false, 11);
        return sim_->unpark(), ok(true, 0);
    }
    lastError = 2;
    return "0";   // OnStepX: a bare 0 to what it doesn't know
}

// ---------------------------------------------------------------- NexStar / SynScan

Device fakeNexStar(std::shared_ptr<mounts::SimMount> sim, mounts::HandController kind, bool aligned) {
    auto mode = std::make_shared<int>(2);
    return [sim, kind, aligned, mode](std::string_view in) -> std::string {
        if (in.empty()) return {};
        const bool synScan = kind == mounts::HandController::SynScan;
        switch (in[0]) {
            case 'K': return std::string(in.substr(1, 1)) + "#";
            case 'V': return synScan ? std::string("042507#") : std::string("\x04\x15#", 3);
            case 'm': return std::string("\x00#", 2);
            case 'J': return std::string(1, aligned ? '\x01' : '\x00') + "#";
            case 't': return std::string(1, sim->state().tracking ? static_cast<char>(*mode) : '\0') + "#";
            case 'T': {
                const int m = in.size() > 1 ? in[1] : 0;
                if (m) *mode = m;
                sim->setTracking(m != 0);
                return "#";
            }
            case 'e': {
                const auto x = sim->state();
                return mounts::nexStarRaDec(x.ra, x.dec) + "#";
            }
            case 'r':
            case 's':
                if (const auto c = mounts::parseNexStarRaDec(in.substr(1))) {
                    if (in[0] == 'r') sim->goTo(c->first, c->second);
                    else sim->sync(c->first, c->second);
                }
                return "#";
            case 'L': return sim->state().slewing ? "1#" : "0#";
            case 'M': return sim->abort(), "#";
            case 'p': return synScan ? "E#" : "";
            case 'P': {
                if (in.size() < 8) return {};
                const int dev = static_cast<unsigned char>(in[2]), cmd = static_cast<unsigned char>(in[3]), rate = static_cast<unsigned char>(in[4]);
                if ((dev != 16 && dev != 17) || (cmd != 36 && cmd != 37)) return std::string("\x00#", 2);   // the extra byte: no device
                const Dir d = dev == 17 ? (cmd == 36 ? Dir::North : Dir::South) : (cmd == 36 ? Dir::West : Dir::East);
                if (rate == 0) sim->stop(d);
                else sim->move(d, rate * 0.5);
                return "#";
            }
        }
        return {};
    };
}

// ---------------------------------------------------------------- the SynScan app

Device fakeSynScanApp(std::shared_ptr<mounts::SimMount> sim, bool j2000) {
    return [sim, j2000](std::string_view in) -> std::string {
        std::vector<std::string> p(1);
        for (const char c : in) {
            if (c == ',') p.emplace_back();
            else p.back() += c;
        }
        const std::string& n = p[0];
        auto ok = [&](const std::string& v = {}) { return "Ok," + n + (v.empty() ? "" : "," + v); };
        auto num = [](double v) { return QByteArray::number(v, 'f', 8).toStdString(); };
        auto arg = [&](std::size_t i) { return i < p.size() ? QByteArray::fromStdString(p[i]).toDouble() : 0.0; };
        const auto x = sim->state();
        if (n == "ServerVersion") return ok("1,0,0");
        if (n == "Action" && p.size() > 1 && p[1] == "AppVersionGet") return "Ok,Action,AppVersionGet,2.5.30";
        if (n == "CanPulseGuideGet" || n == "CanParkGet" || n == "CanUnparkGet" || n == "CanSetTrackingGet" || n == "CanSyncGet") return ok("1");
        if (n == "EquatorialSystemGet") return ok(j2000 ? "2" : "1");
        if (n == "RightAscensionDeclinationGet") return ok(num(x.ra) + "," + num(x.dec));
        if (n == "SlewingGet") return ok(x.slewing ? "1" : "0");
        if (n == "TrackingGet") return ok(x.tracking ? "1" : "0");
        if (n == "AtParkGet") return ok(x.parked ? "1" : "0");
        if (n == "SideOfPierGet") return ok("0");
        if (n == "SlewToCoordinatesAsync") return sim->goTo(arg(1), arg(2)) ? ok() : "InvalidOperation," + n;
        if (n == "AbortSlew") return sim->abort(), ok();
        if (n == "MoveAxis") {
            const double r = arg(2);
            const Dir d = p.size() > 1 && p[1] == "1" ? (r >= 0 ? Dir::North : Dir::South) : (r >= 0 ? Dir::East : Dir::West);
            if (r == 0) sim->stop(d);
            else sim->move(d, std::abs(r) * 100);   // (a test's mount: far quicker than the sky, to measure)
            return ok();
        }
        if (n == "PulseGuide") return sim->pulse(static_cast<Dir>(static_cast<int>(arg(1))), static_cast<int>(arg(2))), ok();
        if (n == "TrackingSet") return sim->setTracking(arg(1) != 0), ok();
        if (n == "TrackingRateSet") return sim->setTrackRate(static_cast<mounts::TrackRate>(static_cast<int>(arg(1)))), ok();
        if (n == "Park") return sim->park(), ok();
        if (n == "Unpark") return sim->unpark(), ok();
        if (n == "SyncToCoordinates") return sim->sync(arg(1), arg(2)), ok();
        return "Unknown," + n;
    };
}

// ---------------------------------------------------------------- Sky-Watcher motor controller

namespace {
double seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

FakeSkyWatcherMc::FakeSkyWatcherMc() : last_(seconds()) {}

void FakeSkyWatcherMc::advance() {
    const double now = seconds(), dt = now - last_;
    last_ = now;
    for (auto& a : a_) {
        if (!a.running) continue;
        if (a.gotoMode) {
            const double step = gotoDegPerSec / 360.0 * kCpr * dt, d = a.target - a.pos;
            if (std::abs(d) <= step) {
                a.pos = a.target;
                a.running = a.gotoMode = false;   // stopped: back in speed mode (the document)
            } else {
                a.pos += d > 0 ? step : -step;
            }
        } else if (a.period > 0) {
            a.pos += (a.ccw ? -1.0 : 1.0) * kTimer / a.period * (a.fast ? kHighSpeed : 1) * dt;
        }
    }
}

FakeSkyWatcherMc::Axis FakeSkyWatcherMc::axis(int i) {
    std::lock_guard lock(m_);
    advance();
    return a_[i];
}

Device FakeSkyWatcherMc::device() {
    return [this](std::string_view cmd) { return handle(cmd); };
}

std::string FakeSkyWatcherMc::handle(std::string_view cmd) {
    std::lock_guard lock(m_);
    advance();
    if (cmd.size() < 4 || cmd[0] != ':' || cmd.back() != '\r') return "!3\r";
    const char c = cmd[1];
    const int which = cmd[2] - '0';
    const std::string data(cmd.substr(3, cmd.size() - 4));
    std::vector<Axis*> axes;
    if (which == 1 || which == 3) axes.push_back(&a_[0]);
    if (which == 2 || which == 3) axes.push_back(&a_[1]);
    if (axes.empty()) return "!3\r";
    auto& a = *axes.front();
    auto hex = [](int v) { return "0123456789ABCDEF"[v & 15]; };
    switch (c) {
        case 'e': return "=030201\r";
        case 'a': return "=" + mounts::skyWatcherHex24(kCpr) + "\r";
        case 'b': return "=" + mounts::skyWatcherHex24(kTimer) + "\r";
        case 'g': return "=20\r";   // 32
        case 'f': return std::string("=") + hex((a.gotoMode ? 0 : 1) | (a.ccw ? 2 : 0) | (a.fast ? 4 : 0)) + hex(a.running ? 1 : 0) + hex(a.init ? 1 : 0) + "\r";
        case 'j': return "=" + mounts::skyWatcherHex24(static_cast<uint32_t>(std::llround(a.pos))) + "\r";
        case 'E':
            for (auto* x : axes) {
                if (x->running) return "!2\r";
                x->pos = mounts::skyWatcherValue(data);
            }
            return "=\r";
        case 'F':
            for (auto* x : axes) x->init = true;
            return "=\r";
        case 'G':
            for (auto* x : axes) {
                if (x->running || data.size() < 2) return "!2\r";
                const int m = std::stoi(data.substr(0, 1), nullptr, 16), d = std::stoi(data.substr(1, 1), nullptr, 16);
                x->gotoMode = !(m & 1);
                x->fast = !x->gotoMode && (m & 2);
                x->ccw = d & 1;
            }
            return "=\r";
        case 'I':
            for (auto* x : axes) x->period = mounts::skyWatcherValue(data);
            return "=\r";
        case 'S':
            for (auto* x : axes) x->target = mounts::skyWatcherValue(data);
            return "=\r";
        case 'J':
            for (auto* x : axes) x->running = !x->gotoMode || x->pos != x->target;
            return "=\r";
        case 'K':
        case 'L':
            for (auto* x : axes) x->running = x->gotoMode = false;
            return "=\r";
    }
    return "!0\r";
}

}  // namespace slopcap::test

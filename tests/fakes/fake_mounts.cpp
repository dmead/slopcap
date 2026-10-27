#include "fakes/fake_mounts.hpp"

#include "mounts/skywatcher.hpp"

#include <QByteArray>

#include <chrono>
#include <cmath>
#include <cstdio>
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

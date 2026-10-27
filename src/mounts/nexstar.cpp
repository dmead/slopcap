#include "mounts/nexstar.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace slopcap::mounts {

namespace {

constexpr int kReplyMs = 3500;   // Celestron: be ready to wait 3.5 s (the hand controller retries its motors)
constexpr double kTurn = 4294967296.0;

bool isHex(std::string_view s) {
    for (const char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return !s.empty();
}

}  // namespace

std::optional<std::pair<double, double>> parseNexStarRaDec(std::string_view s) {
    const auto comma = s.find(',');
    if (comma == std::string_view::npos) return std::nullopt;
    const auto a = s.substr(0, comma), b = s.substr(comma + 1);
    if (!isHex(a) || !isHex(b) || a.size() != b.size() || (a.size() != 8 && a.size() != 4)) return std::nullopt;
    const double scale = a.size() == 8 ? kTurn : 65536.0;
    const double ra = static_cast<double>(std::strtoul(std::string(a).c_str(), nullptr, 16)) / scale * 24.0;
    double dec = static_cast<double>(std::strtoul(std::string(b).c_str(), nullptr, 16)) / scale * 360.0;
    if (dec > 180) dec -= 360;   // south of the equator wraps to just under a whole turn
    return std::pair{ra, dec};
}

std::string nexStarRaDec(double ra, double dec) {
    auto turn = [](double fraction) {
        fraction -= std::floor(fraction);
        return static_cast<unsigned long>(std::llround(fraction * kTurn)) & 0xFFFFFF00UL;   // the low byte is ignored
    };
    char buf[48];
    std::snprintf(buf, sizeof buf, "%08lX,%08lX", turn(ra / 24.0), turn(dec / 360.0));
    return buf;
}

NexStarDriver::NexStarDriver(TransportFactory open, HandController kind) : open_(std::move(open)), kind_(kind) {}

CommandChannel& NexStarDriver::ch() {
    if (!ch_) throw MountError("the mount isn't connected");
    return *ch_;
}

MountInfo NexStarDriver::connect() {
    ch_ = std::make_unique<CommandChannel>(open_(), "nexstar");
    if (ch_->tryUntil("Kx", '#', kReplyMs, true) != std::optional<std::string>("x"))
        throw MountError("no hand controller answers on " + ch_->transport().name());
    // Version: Celestron sends two bytes (major, minor), SynScan six hex digits (04 25 07 = 4.37.07)
    const auto v = ch_->until("V", '#', kReplyMs, true);
    if (kind_ == HandController::Auto) kind_ = v.size() == 6 && isHex(v) ? HandController::SynScan : HandController::Celestron;
    MountInfo info;
    char model[96];
    if (kind_ == HandController::SynScan && v.size() == 6) {
        auto pair = [&](int i) { return std::strtol(v.substr(static_cast<std::size_t>(i), 2).c_str(), nullptr, 16); };
        std::snprintf(model, sizeof model, "SynScan %ld.%02ld.%02ld", pair(0), pair(2), pair(4));
    } else if (v.size() >= 2) {
        std::snprintf(model, sizeof model, "NexStar hand control %d.%d", static_cast<unsigned char>(v[0]), static_cast<unsigned char>(v[1]));
    } else {
        std::snprintf(model, sizeof model, "%s", kind_ == HandController::SynScan ? "SynScan" : "NexStar");
    }
    info.model = model;
    aligned_ = ch_->until("J", '#', kReplyMs, true) == std::string(1, '\x01');
    if (const auto t = ch_->tryUntil("t", '#', kReplyMs, true); t && t->size() == 1 && (*t)[0] != 0) trackMode_ = (*t)[0];
    info.backend = kind_ == HandController::SynScan ? "synscan" : "nexstar";
    info.j2000 = kind_ == HandController::SynScan;
    info.rates = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
    info.caps.tracking = true;
    info.caps.sync = true;
    if (!aligned_) info.model += " (not aligned: no coordinates until it is)";
    return info;
}

MountStatus NexStarDriver::poll() {
    MountStatus st;
    if (!aligned_) aligned_ = ch().until("J", '#', kReplyMs, true) == std::string(1, '\x01');
    if (const auto c = parseNexStarRaDec(ch().until("e", '#', kReplyMs, true)); c && aligned_) {
        st.haveCoords = true;
        st.ra = c->first, st.dec = c->second;
    }
    st.slewing = ch().until("L", '#', kReplyMs, true) == "1";
    if (const auto t = ch().until("t", '#', kReplyMs, true); t.size() == 1) st.tracking = t[0] != 0;
    if (kind_ == HandController::SynScan)
        if (const auto p = ch().tryUntil("p", '#', kReplyMs, true))
            st.pier = *p == "E" ? PierSide::East : *p == "W" ? PierSide::West : PierSide::Unknown;
    return st;
}

void NexStarDriver::slewTo(double ra, double dec) {
    if (!aligned_) throw MountError("the hand controller isn't aligned: it can't go to coordinates yet");
    ch().until("r" + nexStarRaDec(ra, dec), '#', kReplyMs);
}

void NexStarDriver::abort() {
    ch().until("M", '#', kReplyMs);
    motor(Dir::North, 0);
    motor(Dir::East, 0);
}

void NexStarDriver::move(Dir d, int rate) { motor(d, std::clamp(rate, 0, 8) + 1); }

void NexStarDriver::stop(Dir d) { motor(d, 0); }

void NexStarDriver::motor(Dir d, int rate) {
    // P, 2 bytes follow, motor (16 azimuth / RA, 17 altitude / Dec), 36 positive / 37 negative, rate, 0, 0, 0
    const bool ns = isNorthSouth(d), positive = d == Dir::North || d == Dir::West;
    const std::string cmd{'P', '\x02', static_cast<char>(ns ? 17 : 16), static_cast<char>(positive ? 36 : 37), static_cast<char>(rate), '\0', '\0', '\0'};
    // a pass-through the motor didn't answer comes back with one extra byte before the '#'
    if (!ch().until(cmd, '#', kReplyMs).empty()) throw MountError("the hand controller couldn't reach the motor");
}

void NexStarDriver::setTracking(bool on) { ch().until(std::string{'T', static_cast<char>(on ? trackMode_ : 0)}, '#', kReplyMs); }

void NexStarDriver::sync(double ra, double dec) { ch().until("s" + nexStarRaDec(ra, dec), '#', kReplyMs); }

}  // namespace slopcap::mounts

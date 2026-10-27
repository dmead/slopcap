#include "mounts/lx200.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace slopcap::mounts {

namespace {

constexpr int kReplyMs = 1500;   // Meade: "tens to hundreds of milliseconds"; a busy Autostar takes longer

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    s.erase(0, s.find_first_not_of(' '));
    return s;
}

/// Up to three numeric fields ("12", "34.5") and whether a '-' came first.
std::vector<double> fields(std::string_view s, bool& negative) {
    std::vector<double> out;
    std::string tok;
    negative = false;
    bool seenDigit = false;
    for (const char c : s) {
        if ((c >= '0' && c <= '9') || (c == '.' && !tok.empty())) {
            tok += c;
            seenDigit = true;
            continue;
        }
        if (c == '-' && !seenDigit) negative = true;
        if (!tok.empty()) out.push_back(std::atof(tok.c_str())), tok.clear();
    }
    if (!tok.empty()) out.push_back(std::atof(tok.c_str()));
    return out;
}

std::string raText(double hours) {
    const long s = std::lround(std::fmod(std::fmod(hours, 24.0) + 24.0, 24.0) * 3600.0) % 86400;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%02ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    return buf;
}

std::string decText(double deg) {
    const long s = std::min(90L * 3600, std::lround(std::abs(deg) * 3600.0));
    char buf[40];
    std::snprintf(buf, sizeof buf, "%c%02ld*%02ld:%02ld", deg < 0 ? '-' : '+', s / 3600, s / 60 % 60, s % 60);
    return buf;
}

/// OnStepX's :MS# codes (Goto.command.cpp)
const char* onStepGotoError(char c) {
    switch (c) {
        case '1': return "below the horizon limit";
        case '2': return "above the overhead limit";
        case '3': return "the controller is in standby";
        case '4': return "the mount is parked";
        case '5': return "a goto is already under way";
        case '6': return "outside the mount's limits";
        case '7': return "hardware fault";
        case '8': return "the mount is already moving";
    }
    return "unspecified error";
}

}  // namespace

std::optional<double> parseLx200Ra(std::string_view s) {
    bool neg = false;
    const auto f = fields(s, neg);
    if (f.size() < 2 || f[0] >= 24 || f[1] >= 60 || (f.size() > 2 && f[2] >= 60)) return std::nullopt;
    return f[0] + f[1] / 60.0 + (f.size() > 2 ? f[2] / 3600.0 : 0.0);
}

std::optional<double> parseLx200Dec(std::string_view s) {
    bool neg = false;
    const auto f = fields(s, neg);
    if (f.size() < 2 || f[0] > 90 || f[1] >= 60 || (f.size() > 2 && f[2] >= 60)) return std::nullopt;
    const double d = f[0] + f[1] / 60.0 + (f.size() > 2 ? f[2] / 3600.0 : 0.0);
    return neg ? -d : d;
}

Lx200Driver::Lx200Driver(TransportFactory open, Lx200Dialect dialect) : open_(std::move(open)), dialect_(dialect) {}

CommandChannel& Lx200Driver::ch() {
    if (!ch_) throw MountError("the mount isn't connected");
    return *ch_;
}

MountInfo Lx200Driver::connect() {
    ch_ = std::make_unique<CommandChannel>(open_(), "lx200");
    // ACK asks how it's mounted: every LX200 answers it with one letter. (Without an answer, :GR# still tells.)
    if (const auto ack = ch_->tryCount("\x06", 1, kReplyMs, true)) mounting_ = (*ack)[0];
    else if (!ch_->tryUntil(":GR#", '#', kReplyMs, true))
        throw MountError("no LX200 mount answers on " + ch_->transport().name());
    std::string product, firmware;
    if (dialect_ != Lx200Dialect::MeadeClassic)
        if (const auto p = ch_->tryUntil(":GVP#", '#', 800)) product = trim(*p);
    if (dialect_ == Lx200Dialect::Auto) {
        std::string lower = product;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        dialect_ = product.empty() ? Lx200Dialect::MeadeClassic
                   : lower.find("on-step") != std::string::npos || lower.find("onstep") != std::string::npos ? Lx200Dialect::OnStep
                                                                                                              : Lx200Dialect::Meade;
    }
    if (dialect_ != Lx200Dialect::MeadeClassic)
        if (const auto v = ch_->tryUntil(":GVN#", '#', 800)) firmware = trim(*v);
    // high precision (HH:MM:SS, sDD*MM:SS): :U# toggles, so only when the mount answers in the short form
    if (ch_->until(":GR#", '#', kReplyMs, true).find('.') != std::string::npos) {
        ch_->send(":U#");
        ch_->until(":GR#", '#', kReplyMs, true);
    }
    rate_ = -1;

    MountInfo info;
    info.backend = "lx200";
    info.model = product.empty() ? std::string("LX200 Classic") : product + (firmware.empty() ? "" : " " + firmware);
    info.rates = {"Guide", "Centre", "Find", "Max"};
    const bool onStep = dialect_ == Lx200Dialect::OnStep, classic = dialect_ == Lx200Dialect::MeadeClassic;
    info.caps.pulse = !classic;
    info.caps.park = !classic;
    info.caps.unpark = onStep;
    info.caps.tracking = !classic;
    info.caps.trackRates = !classic;
    info.caps.sync = true;
    return info;
}

MountStatus Lx200Driver::poll() {
    MountStatus st;
    const auto ra = parseLx200Ra(ch().until(":GR#", '#', kReplyMs, true));
    const auto dec = parseLx200Dec(ch().until(":GD#", '#', kReplyMs, true));
    st.haveCoords = ra && dec;
    st.ra = ra.value_or(0), st.dec = dec.value_or(0);
    // distance bars: a string of them until the goto ends, then nothing
    if (const auto bars = ch().tryUntil(":D#", '#', kReplyMs, true)) st.slewing = !trim(*bars).empty();
    if (dialect_ == Lx200Dialect::OnStep) {
        if (const auto u = ch().tryUntil(":GU#", '#', kReplyMs, true)) {
            st.tracking = u->find('n') == std::string::npos;   // n: not tracking
            st.parked = u->find('P') != std::string::npos;     // P: parked (I: parking)
            st.slewing = st.slewing || u->find('N') == std::string::npos;   // N: no goto
        }
        if (const auto m = ch().tryUntil(":Gm#", '#', kReplyMs, true))
            st.pier = *m == "E" ? PierSide::East : *m == "W" ? PierSide::West : PierSide::Unknown;
    } else if (dialect_ == Lx200Dialect::Meade) {
        if (const auto a = ch().tryCount("\x06", 1, kReplyMs, true)) {
            mounting_ = (*a)[0];
            st.tracking = mounting_ != 'L';   // land mode doesn't track
        }
    }
    return st;
}

void Lx200Driver::setTarget(double ra, double dec) {
    if (ch().count(":Sr" + raText(ra) + "#", 1, kReplyMs) != "1") throw MountError("the mount refused the target's RA");
    if (ch().count(":Sd" + decText(dec) + "#", 1, kReplyMs) != "1") throw MountError("the mount refused the target's Dec");
}

void Lx200Driver::slewTo(double ra, double dec) {
    setTarget(ra, dec);
    const auto r = ch().count(":MS#", 1, 3000);
    if (r == "0") return;
    std::string why = dialect_ == Lx200Dialect::OnStep ? onStepGotoError(r[0]) : r == "1" ? "below the horizon" : "above the high limit";
    if (dialect_ != Lx200Dialect::OnStep)   // Meade follows the digit with its own message
        if (const auto more = ch().readUntil('#', 500); more && !trim(*more).empty()) why = trim(*more);
    throw MountError("the mount refused the goto: " + why);
}

void Lx200Driver::abort() { ch().send(":Q#"); }

void Lx200Driver::move(Dir d, int rate) {
    rate = std::clamp(rate, 0, 3);
    if (rate != rate_) ch().send(std::string(":R") + "GCMS"[rate] + "#");
    rate_ = rate;
    ch().send(std::string(":M") + "nsew"[static_cast<int>(d)] + "#");
}

void Lx200Driver::stop(Dir d) { ch().send(std::string(":Q") + "nsew"[static_cast<int>(d)] + "#"); }

void Lx200Driver::pulse(Dir d, int ms) {
    char buf[40];
    const char dir = "nsew"[static_cast<int>(d)];
    if (dialect_ == Lx200Dialect::OnStep) std::snprintf(buf, sizeof buf, ":Mg%c%d#", dir, std::clamp(ms, 1, 16399));
    else std::snprintf(buf, sizeof buf, ":Mg%c%04d#", dir, std::clamp(ms, 1, 9999));
    ch().send(buf);
}

void Lx200Driver::setTracking(bool on) {
    if (dialect_ == Lx200Dialect::OnStep) {
        if (ch().count(on ? ":Te#" : ":Td#", 1, kReplyMs) != "1") throw MountError(on ? "the mount couldn't start tracking" : "the mount couldn't stop tracking");
        return;
    }
    // Autostar: land mode stops tracking; back to the way it's mounted starts it
    ch().send(!on ? ":AL#" : mounting_ == 'A' ? ":AA#" : ":AP#");
}

void Lx200Driver::setTrackRate(TrackRate r) {
    const char* cmd = r == TrackRate::Lunar ? ":TL#" : r == TrackRate::Solar ? ":TS#" : r == TrackRate::King ? ":TK#" : ":TQ#";
    if (dialect_ != Lx200Dialect::OnStep && (r == TrackRate::Solar || r == TrackRate::King))
        throw MountError("this mount has only sidereal and lunar tracking");
    ch().send(cmd);
}

void Lx200Driver::park() {
    if (dialect_ == Lx200Dialect::OnStep) {
        if (ch().count(":hP#", 1, kReplyMs) != "1") throw MountError("the mount couldn't park (is a park position set?)");
        return;
    }
    ch().send(":hP#");
}

void Lx200Driver::unpark() {
    if (ch().count(":hR#", 1, kReplyMs) != "1") throw MountError("the mount couldn't unpark");
}

void Lx200Driver::sync(double ra, double dec) {
    setTarget(ra, dec);
    ch().until(":CM#", '#', 3000);
}

}  // namespace slopcap::mounts

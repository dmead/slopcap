#include "mounts/synscan_app.hpp"

#include <QByteArray>

#include <algorithm>
#include <cctype>
#include <iterator>

namespace slopcap::mounts {

namespace {

constexpr int kReplyMs = 2000;   // the document: up to ~800 ms over poor Wi-Fi, app and mount both
constexpr double kSidereal = 360.0 / 86164.0905;
constexpr double kRates[] = {0.5, 1, 8, 32, 128, 400, 800};   // MoveAxis rates, times sidereal

std::string num(double v) { return QByteArray::number(v, 'f', 6).toStdString(); }   // '.', whatever the locale

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out(1);
    for (const char c : s) {
        if (c == ',') out.emplace_back();
        else out.back() += c;
    }
    return out;
}

double toDouble(const std::string& s) {
    bool ok = false;
    const double v = QByteArray::fromStdString(s).trimmed().toDouble(&ok);
    if (!ok) throw MountError("the SynScan app sent " + printable(s) + " for a number");
    return v;
}

}  // namespace

SynScanAppDriver::SynScanAppDriver(TransportFactory open) : open_(std::move(open)) {}

std::optional<std::vector<std::string>> SynScanAppDriver::tryCall(const std::string& name) {
    try {
        return call(name);
    } catch (const MountError& e) {
        const std::string why = e.what();
        if (why.find("Unimplemented") != std::string::npos || why.find("Unknown") != std::string::npos) return std::nullopt;
        throw;
    }
}

std::vector<std::string> SynScanAppDriver::call(const std::string& name, const std::string& args) {
    if (!ch_) throw MountError("the mount isn't connected");
    // one datagram each way, no terminator: '\n' never comes, so the whole datagram is the reply
    const bool get = name.size() > 3 && name.compare(name.size() - 3, 3, "Get") == 0;
    const auto reply = ch_->until(name + (args.empty() ? "" : "," + args), '\n', kReplyMs, get);
    auto parts = split(reply);
    const std::string head = name.substr(0, name.find(','));
    if (parts.size() < 2 || parts[1] != head) throw MountError("the SynScan app sent " + printable(reply));
    if (parts[0] != "Ok") throw MountError("the SynScan app said " + parts[0] + " to " + head);
    parts.erase(parts.begin(), parts.begin() + 2);
    return parts;
}

bool SynScanAppDriver::flag(const std::string& name) {
    const auto r = tryCall(name);
    if (!r || r->empty()) return false;
    std::string v = r->back();
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v == "1" || v == "true";
}

MountInfo SynScanAppDriver::connect() {
    ch_ = std::make_unique<CommandChannel>(open_(), "synscan-app");
    if (!ch_->transport().datagrams()) throw MountError("the SynScan app protocol runs over UDP only: udp://HOST");
    try {
        call("ServerVersion");
    } catch (const MountError&) {
        throw MountError("no SynScan app answers on " + ch_->transport().name() + " (is the app running and connected to the mount?)");
    }
    MountInfo info;
    info.backend = "synscan-app";
    info.model = "SynScan app";
    if (const auto v = tryCall("Action,AppVersionGet"); v && !v->empty()) info.model += " " + v->back();
    caps_.pulse = flag("CanPulseGuideGet");
    caps_.park = flag("CanParkGet");
    caps_.unpark = flag("CanUnparkGet");
    caps_.tracking = caps_.trackRates = flag("CanSetTrackingGet");
    caps_.sync = flag("CanSyncGet");
    info.caps = caps_;
    // ASCOM's EquatorialCoordinateType: 1 of date (topocentric), 2 J2000
    if (const auto e = tryCall("EquatorialSystemGet"); e && !e->empty()) info.j2000 = e->front() == "2";
    for (const double r : kRates) info.rates.push_back(QByteArray::number(r, 'g', 6).toStdString() + "\xC3\x97");   // "8×"
    return info;
}

MountStatus SynScanAppDriver::poll() {
    MountStatus st;
    const auto c = call("RightAscensionDeclinationGet");
    if (c.size() >= 2) {
        st.haveCoords = true;
        st.ra = toDouble(c[0]), st.dec = toDouble(c[1]);
    }
    st.slewing = flag("SlewingGet");
    st.tracking = flag("TrackingGet");
    if (caps_.park) st.parked = flag("AtParkGet");
    if (const auto p = tryCall("SideOfPierGet"); p && !p->empty())   // ASCOM PierSide: 0 east, 1 west, -1 unknown
        st.pier = p->front() == "0" ? PierSide::East : p->front() == "1" ? PierSide::West : PierSide::Unknown;
    return st;
}

void SynScanAppDriver::slewTo(double ra, double dec) { call("SlewToCoordinatesAsync", num(ra) + "," + num(dec)); }

void SynScanAppDriver::abort() {
    call("AbortSlew");
    call("MoveAxis", "0,0");
    call("MoveAxis", "1,0");
}

void SynScanAppDriver::move(Dir d, int rate) {
    const double v = kRates[std::clamp(rate, 0, static_cast<int>(std::size(kRates)) - 1)] * kSidereal;
    const bool positive = d == Dir::North || d == Dir::East;
    call("MoveAxis", std::string(isNorthSouth(d) ? "1," : "0,") + num(positive ? v : -v));
}

void SynScanAppDriver::stop(Dir d) { call("MoveAxis", isNorthSouth(d) ? "1,0" : "0,0"); }

// ASCOM's GuideDirections are north, south, east, west: Dir's order
void SynScanAppDriver::pulse(Dir d, int ms) { call("PulseGuide", std::to_string(static_cast<int>(d)) + "," + std::to_string(ms)); }

void SynScanAppDriver::setTracking(bool on) { call("TrackingSet", on ? "1" : "0"); }

// ASCOM's DriveRates: sidereal, lunar, solar, King: TrackRate's order
void SynScanAppDriver::setTrackRate(TrackRate r) { call("TrackingRateSet", std::to_string(static_cast<int>(r))); }

void SynScanAppDriver::park() { call("Park"); }
void SynScanAppDriver::unpark() { call("Unpark"); }
void SynScanAppDriver::sync(double ra, double dec) { call("SyncToCoordinates", num(ra) + "," + num(dec)); }

}  // namespace slopcap::mounts

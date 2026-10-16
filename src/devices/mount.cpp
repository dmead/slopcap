#include "devices/mount.hpp"

#include <algorithm>

namespace slopcap::devices {

namespace {
constexpr int kTelescopeInterface = 1;   // INDI::BaseDevice::TELESCOPE_INTERFACE
const QString kCoords = "EQUATORIAL_EOD_COORD", kGuideNS = "TELESCOPE_TIMED_GUIDE_NS", kGuideWE = "TELESCOPE_TIMED_GUIDE_WE";
const QString kMotionNS = "TELESCOPE_MOTION_NS", kMotionWE = "TELESCOPE_MOTION_WE", kSlewRate = "TELESCOPE_SLEW_RATE";
const QString kPark = "TELESCOPE_PARK", kTrack = "TELESCOPE_TRACK_STATE", kAbort = "TELESCOPE_ABORT_MOTION";

bool isNS(Mount::Direction d) { return d == Mount::Direction::North || d == Mount::Direction::South; }
}  // namespace

Mount::Mount(IndiClient& client, QObject* parent) : QObject(parent), client_(client) {
    connect(&client_, &IndiClient::propertyDefined, this, &Mount::onDefined);
    connect(&client_, &IndiClient::propertyUpdated, this, &Mount::onUpdated);
    connect(&client_, &IndiClient::serverDisconnected, this, [this] {
        device_.clear();
        nsBusy_ = weBusy_ = slewing_ = false;
        nsTimeout_.stop();
        weTimeout_.stop();
        updatePulsing();
        emit readyChanged(false);
    });
    connect(&client_, &IndiClient::propertyDeleted, this, [this](const QString& dev, const QString& name) {
        if (dev == device_ && (name.isEmpty() || name == kCoords)) emit readyChanged(false);
    });
    // A driver that never reports a pulse's end: count it as done once its time has passed.
    for (auto* t : {&nsTimeout_, &weTimeout_}) {
        t->setSingleShot(true);
        connect(t, &QTimer::timeout, this, [this, t] {
            (t == &nsTimeout_ ? nsBusy_ : weBusy_) = false;
            updatePulsing();
        });
    }
}

bool Mount::ready() const { return !device_.isEmpty() && client_.property(device_, kCoords) != nullptr; }

std::optional<bool> Mount::tracking() const {
    const auto* p = client_.property(device_, kTrack);
    if (!p || !p->elements.count("TRACK_ON")) return std::nullopt;
    return p->elements.at("TRACK_ON").on();
}

std::optional<bool> Mount::parked() const {
    const auto* p = client_.property(device_, kPark);
    if (!p || !p->elements.count("PARK")) return std::nullopt;
    return p->elements.at("PARK").on() && !p->busy();   // PARK goes On (Busy) as soon as parking starts
}

bool Mount::canPulse() const { return client_.property(device_, kGuideNS) && client_.property(device_, kGuideWE); }

void Mount::onDefined(const QString& dev, const QString& name) {
    if (device_.isEmpty()) {
        bool isMount = false;
        if (name == "DRIVER_INFO") {   // drivers say what they are...
            if (const auto* p = client_.property(dev, name))
                if (auto it = p->elements.find("DRIVER_INTERFACE"); it != p->elements.end())
                    isMount = it->second.text.toInt() & kTelescopeInterface;
        }
        if (name == kCoords) isMount = true;   // ...or, for old drivers, have coordinates
        if (!isMount || (!preferred_.isEmpty() && preferred_ != dev)) return;
        device_ = dev;
        ensureConnected();
    }
    if (dev != device_) return;
    if (name == "CONNECTION") ensureConnected();
    if (name == kCoords) {
        readCoords();
        emit readyChanged(true);
    }
    if (name == kTrack || name == kPark || name == kSlewRate || name == kGuideNS) emit stateChanged();
}

void Mount::ensureConnected() {
    const auto* p = client_.property(device_, "CONNECTION");
    if (p && p->elements.count("CONNECT") && !p->elements.at("CONNECT").on() && !p->busy())
        client_.sendSwitch(device_, "CONNECTION", "CONNECT");
}

void Mount::readCoords() {
    const auto* p = client_.property(device_, kCoords);
    if (!p) return;
    const auto ra = client_.number(device_, kCoords, "RA"), dec = client_.number(device_, kCoords, "DEC");
    if (ra && dec && (*ra != ra_ || *dec != dec_)) {
        ra_ = *ra, dec_ = *dec;
        emit coordsChanged(ra_, dec_);
    }
    if (p->busy() != slewing_) {
        slewing_ = p->busy();
        emit stateChanged();
    }
}

void Mount::onUpdated(const QString& dev, const QString& name) {
    if (dev != device_) return;
    if (name == "CONNECTION") {
        ensureConnected();
        return;
    }
    if (name == kCoords) {
        readCoords();
        return;
    }
    if (name == kGuideNS || name == kGuideWE) {
        const bool ns = name == kGuideNS;
        const auto* p = client_.property(dev, name);
        if (p->state == "Alert") {
            (ns ? nsBusy_ : weBusy_) = false;
            (ns ? nsTimeout_ : weTimeout_).stop();
            updatePulsing();
            emit failed("mount refused the guide pulse");
        } else if (!p->busy()) {
            (ns ? nsBusy_ : weBusy_) = false;
            (ns ? nsTimeout_ : weTimeout_).stop();
            updatePulsing();
        }
        return;
    }
    if (name == kTrack || name == kPark || name == kSlewRate || name == kMotionNS || name == kMotionWE) {
        if (const auto* p = client_.property(dev, name); p && p->state == "Alert")
            emit failed(name == kPark ? "mount couldn't park / unpark" : name == kTrack ? "mount couldn't change tracking" : "mount refused the move");
        emit stateChanged();
    }
}

void Mount::updatePulsing() {
    const bool now = nsBusy_ || weBusy_;
    if (now == pulsing_) return;
    pulsing_ = now;
    emit stateChanged();
    if (!now) emit pulseDone();
}

void Mount::pulse(Direction d, int ms) {
    if (!ready() || !canPulse()) {
        emit failed(ready() ? "mount doesn't take guide pulses" : "no mount");
        return;
    }
    ms = std::max(1, ms);
    const bool ns = isNS(d);
    const QString elem = d == Direction::North ? "TIMED_GUIDE_N" : d == Direction::South ? "TIMED_GUIDE_S" : d == Direction::East ? "TIMED_GUIDE_E" : "TIMED_GUIDE_W";
    const QString other = d == Direction::North ? "TIMED_GUIDE_S" : d == Direction::South ? "TIMED_GUIDE_N" : d == Direction::East ? "TIMED_GUIDE_W" : "TIMED_GUIDE_E";
    (ns ? nsBusy_ : weBusy_) = true;
    (ns ? nsTimeout_ : weTimeout_).start(ms + 3000);
    updatePulsing();
    client_.sendNumbers(device_, ns ? kGuideNS : kGuideWE, {{elem, static_cast<double>(ms)}, {other, 0.0}});
}

void Mount::startMove(Direction d) {
    if (!ready()) {
        emit failed("no mount");
        return;
    }
    const QString elem = d == Direction::North ? "MOTION_NORTH" : d == Direction::South ? "MOTION_SOUTH" : d == Direction::East ? "MOTION_EAST" : "MOTION_WEST";
    client_.sendSwitch(device_, isNS(d) ? kMotionNS : kMotionWE, elem, true);
}

void Mount::stopMove() {
    if (!ready()) return;
    // Turning a motion switch off ends that motion.
    auto stop = [this](const QString& prop) {
        if (const auto* p = client_.property(device_, prop))
            for (const auto& [name, e] : p->elements)
                if (e.on()) client_.sendSwitch(device_, prop, name, false);
    };
    stop(kMotionNS);
    stop(kMotionWE);
}

void Mount::goTo(double raHours, double decDegrees) {
    if (!ready()) {
        emit failed("no mount");
        return;
    }
    client_.sendSwitch(device_, "ON_COORD_SET", "TRACK");
    client_.sendNumbers(device_, kCoords, {{"RA", raHours}, {"DEC", decDegrees}});
}

void Mount::abort() {
    if (ready()) client_.sendSwitch(device_, kAbort, "ABORT");
}

void Mount::park() {
    if (ready()) client_.sendSwitch(device_, kPark, "PARK");
}

void Mount::unpark() {
    if (ready()) client_.sendSwitch(device_, kPark, "UNPARK");
}

void Mount::setTracking(bool on) {
    if (ready()) client_.sendSwitch(device_, kTrack, on ? "TRACK_ON" : "TRACK_OFF");
}

QStringList Mount::slewRates() const {
    const auto* p = client_.property(device_, kSlewRate);
    return p ? p->order : QStringList{};
}

QString Mount::slewRate() const {
    const auto* p = client_.property(device_, kSlewRate);
    if (!p) return {};
    for (const auto& [name, e] : p->elements)
        if (e.on()) return name;
    return {};
}

void Mount::setSlewRate(const QString& name) {
    if (ready() && slewRates().contains(name)) client_.sendSwitch(device_, kSlewRate, name);
}

}  // namespace slopcap::devices

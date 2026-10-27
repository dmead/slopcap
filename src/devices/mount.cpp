#include "devices/mount.hpp"

#include "mounts/mount_worker.hpp"
#include "mounts/open_mount.hpp"

#include <QDebug>

#include <algorithm>

namespace slopcap::devices {

namespace {
constexpr int kTelescopeInterface = 1;   // INDI::BaseDevice::TELESCOPE_INTERFACE
const QString kCoords = "EQUATORIAL_EOD_COORD", kGuideNS = "TELESCOPE_TIMED_GUIDE_NS", kGuideWE = "TELESCOPE_TIMED_GUIDE_WE";
const QString kMotionNS = "TELESCOPE_MOTION_NS", kMotionWE = "TELESCOPE_MOTION_WE", kSlewRate = "TELESCOPE_SLEW_RATE";
const QString kPark = "TELESCOPE_PARK", kTrack = "TELESCOPE_TRACK_STATE", kAbort = "TELESCOPE_ABORT_MOTION";

bool isNS(Mount::Direction d) { return d == Mount::Direction::North || d == Mount::Direction::South; }
mounts::Dir toDir(Mount::Direction d) { return static_cast<mounts::Dir>(static_cast<int>(d)); }   // same order
}  // namespace

Mount::Mount(IndiClient& client, QObject* parent) : QObject(parent), client_(client) {
    connect(&client_, &IndiClient::propertyDefined, this, &Mount::onDefined);
    connect(&client_, &IndiClient::propertyUpdated, this, &Mount::onUpdated);
    reconnect_.setSingleShot(true);
    connect(&reconnect_, &QTimer::timeout, this, [this] {
        const auto* p = client_.property(device_, "CONNECTION");
        if (!worker_ && p && p->elements.count("CONNECT") && !p->elements.at("CONNECT").on() && !p->busy())
            client_.sendSwitch(device_, "CONNECTION", "CONNECT");
    });
    connect(&client_, &IndiClient::serverDisconnected, this, [this] {
        reconnect_.stop();
        device_.clear();
        if (worker_) return;
        nsBusy_ = weBusy_ = slewing_ = false;
        nsTimeout_.stop();
        weTimeout_.stop();
        updatePulsing();
        emit readyChanged(false);
    });
    connect(&client_, &IndiClient::propertyDeleted, this, [this](const QString& dev, const QString& name) {
        if (!worker_ && dev == device_ && (name.isEmpty() || name == kCoords)) emit readyChanged(false);
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

Mount::~Mount() = default;   // (the worker stops what moves and lets go of the port)

// ---------------------------------------------------------------- which mount

void Mount::setSpec(const QString& spec, std::optional<mounts::Site> site) {
    const bool same = spec.trimmed() == spec_ && site.has_value() == site_.has_value() &&
                      (!site || (site->latitude == site_->latitude && site->longitude == site_->longitude));
    spec_ = spec.trimmed();
    site_ = site;
    if (worker_ && !same) close();
}

bool Mount::builtInSpec() const { return mounts::isBuiltInMount(spec_.toStdString()); }

void Mount::openBuiltIn() {
    if (worker_ || !builtInSpec()) return;
    std::function<std::unique_ptr<mounts::IMountDriver>()> make;
    try {
        make = mounts::mountFactory(spec_.toStdString(), site_);
    } catch (const mounts::MountError& e) {
        emit failed(QString::fromUtf8(e.what()));
        return;
    }
    qInfo().noquote() << "mount: opening" << spec_;
    const bool wasReady = ready();
    worker_ = std::make_unique<mounts::MountWorker>(std::move(make));
    connect(worker_.get(), &mounts::MountWorker::connectedChanged, this, [this](bool ok) {
        if (ok) qInfo().noquote() << "mount:" << spec_ << "is" << QString::fromStdString(worker_->info().model);
        emit readyChanged(ok);
        emit stateChanged();
    });
    connect(worker_.get(), &mounts::MountWorker::statusChanged, this, &Mount::onWorkerStatus);
    connect(worker_.get(), &mounts::MountWorker::failed, this, [this](const QString& why) { emit failed(why); });
    if (wasReady) emit readyChanged(false);   // INDI's mount steps aside
}

void Mount::close() {
    if (!worker_) return;
    qInfo().noquote() << "mount: closing" << spec_;
    worker_.reset();
    slewing_ = pulsing_ = false;
    emit readyChanged(ready());
    emit stateChanged();
    if (!device_.isEmpty()) ensureConnected();   // INDI's, if there is one
}

void Mount::onWorkerStatus() {
    const auto st = worker_->status();
    if (st.haveCoords && (st.ra != ra_ || st.dec != dec_)) {
        ra_ = st.ra, dec_ = st.dec;
        emit coordsChanged(ra_, dec_);
    }
    const bool wasPulsing = pulsing_;
    slewing_ = st.slewing;
    pulsing_ = st.pulsing;
    emit stateChanged();
    if (wasPulsing && !pulsing_) emit pulseDone();
}

int Mount::rateIndex() const { return std::max<qsizetype>(0, slewRates().indexOf(slewRate())); }

// ---------------------------------------------------------------- state

QString Mount::device() const { return worker_ ? QString::fromStdString(worker_->info().model) : device_; }

bool Mount::ready() const {
    if (worker_) return worker_->connected();
    return !device_.isEmpty() && client_.property(device_, kCoords) != nullptr;
}

bool Mount::hasCoords() const { return worker_ ? worker_->connected() && worker_->status().haveCoords : ready(); }

std::optional<bool> Mount::tracking() const {
    if (worker_) return worker_->status().tracking;
    const auto* p = client_.property(device_, kTrack);
    if (!p || !p->elements.count("TRACK_ON")) return std::nullopt;
    return p->elements.at("TRACK_ON").on();
}

std::optional<bool> Mount::parked() const {
    if (worker_) return worker_->status().parked;
    const auto* p = client_.property(device_, kPark);
    if (!p || !p->elements.count("PARK")) return std::nullopt;
    return p->elements.at("PARK").on() && !p->busy();   // PARK goes On (Busy) as soon as parking starts
}

// built-in mounts without pulses of their own get timed moves instead (MountWorker)
bool Mount::canPulse() const { return worker_ ? worker_->connected() : client_.property(device_, kGuideNS) && client_.property(device_, kGuideWE); }

// ---------------------------------------------------------------- INDI

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
    if (dev != device_ || worker_) return;   // (a built-in mount: INDI's telescope is left alone)
    if (name == "CONNECTION") ensureConnected();
    if (name == kCoords) {
        readCoords();
        emit readyChanged(true);
    }
    // canPulse() needs both guide properties, which drivers define one after the other: say so after either.
    if (name == kTrack || name == kPark || name == kSlewRate || name == kGuideNS || name == kGuideWE) emit stateChanged();
}

void Mount::ensureConnected() {
    if (worker_) return;
    const auto* p = client_.property(device_, "CONNECTION");
    if (!p || !p->elements.count("CONNECT") || p->elements.at("CONNECT").on() || p->busy()) return;
    // After a failed connect, try again every 10 s rather than at once (as the focuser does).
    if (p->state == "Alert") {
        if (!reconnect_.isActive()) reconnect_.start(10000);
        return;
    }
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
    if (dev != device_ || worker_) return;
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

// ---------------------------------------------------------------- commands

void Mount::pulse(Direction d, int ms) {
    if (!ready() || !canPulse()) {
        emit failed(ready() ? "mount doesn't take guide pulses" : "no mount");
        return;
    }
    ms = std::max(1, ms);
    const QString elem = d == Direction::North ? "TIMED_GUIDE_N" : d == Direction::South ? "TIMED_GUIDE_S" : d == Direction::East ? "TIMED_GUIDE_E" : "TIMED_GUIDE_W";
    qInfo().noquote() << "mount: pulse" << elem.right(1) << ms << "ms";
    if (worker_) {
        worker_->pulse(toDir(d), ms);
        pulsing_ = true;
        emit stateChanged();
        return;
    }
    const bool ns = isNS(d);
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
    if (worker_) {
        worker_->move(toDir(d), rateIndex());
        return;
    }
    const QString elem = d == Direction::North ? "MOTION_NORTH" : d == Direction::South ? "MOTION_SOUTH" : d == Direction::East ? "MOTION_EAST" : "MOTION_WEST";
    client_.sendSwitch(device_, isNS(d) ? kMotionNS : kMotionWE, elem, true);
}

void Mount::stopMove() {
    if (!ready()) return;
    if (worker_) {
        worker_->stopMoves();
        return;
    }
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
    if (worker_) {
        worker_->goTo(raHours, decDegrees);
        return;
    }
    client_.sendSwitch(device_, "ON_COORD_SET", "TRACK");
    client_.sendNumbers(device_, kCoords, {{"RA", raHours}, {"DEC", decDegrees}});
}

void Mount::abort() {
    qInfo() << "mount: abort";
    if (worker_) worker_->abort();
    else if (ready()) client_.sendSwitch(device_, kAbort, "ABORT");
}

void Mount::park() {
    qInfo() << "mount: park";
    if (worker_) worker_->park();
    else if (ready()) client_.sendSwitch(device_, kPark, "PARK");
}

void Mount::unpark() {
    qInfo() << "mount: unpark";
    if (worker_) worker_->unpark();
    else if (ready()) client_.sendSwitch(device_, kPark, "UNPARK");
}

void Mount::setTracking(bool on) {
    qInfo() << "mount: tracking" << on;
    if (worker_) worker_->setTracking(on);
    else if (ready()) client_.sendSwitch(device_, kTrack, on ? "TRACK_ON" : "TRACK_OFF");
}

QStringList Mount::slewRates() const {
    if (worker_) {
        QStringList out;
        for (const auto& r : worker_->info().rates) out << QString::fromStdString(r);
        return out;
    }
    const auto* p = client_.property(device_, kSlewRate);
    return p ? p->order : QStringList{};
}

QString Mount::slewRate() const {
    if (worker_) {
        // until one is picked, the second slowest (the slowest is for guiding)
        const auto rates = slewRates();
        return rates.contains(builtInRate_) ? builtInRate_ : rates.value(std::min<qsizetype>(1, rates.size() - 1));
    }
    const auto* p = client_.property(device_, kSlewRate);
    if (!p) return {};
    for (const auto& [name, e] : p->elements)
        if (e.on()) return name;
    return {};
}

void Mount::setSlewRate(const QString& name) {
    if (!ready() || !slewRates().contains(name)) return;
    if (worker_) {
        builtInRate_ = name;
        emit stateChanged();
        return;
    }
    client_.sendSwitch(device_, kSlewRate, name);
}

}  // namespace slopcap::devices

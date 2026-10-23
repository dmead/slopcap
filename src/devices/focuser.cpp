#include "devices/focuser.hpp"

#include <QDebug>

#include <algorithm>

namespace slopcap::devices {

namespace {
constexpr int kFocuserInterface = 8;   // INDI::BaseDevice::FOCUSER_INTERFACE
constexpr int kTelescopeInterface = 1;
const QString kAbs = "ABS_FOCUS_POSITION", kAbsElem = "FOCUS_ABSOLUTE_POSITION";
}  // namespace

Focuser::Focuser(IndiClient& client, QObject* parent) : QObject(parent), client_(client) {
    connect(&client_, &IndiClient::propertyDefined, this, &Focuser::onDefined);
    connect(&client_, &IndiClient::propertyUpdated, this, &Focuser::onUpdated);
    connect(&client_, &IndiClient::message, this, &Focuser::onMessage);
    reconnect_.setSingleShot(true);
    connect(&reconnect_, &QTimer::timeout, this, [this] {
        const auto* p = client_.property(device_, "CONNECTION");
        if (p && p->elements.count("CONNECT") && !p->elements.at("CONNECT").on() && !p->busy())
            client_.sendSwitch(device_, "CONNECTION", "CONNECT");
    });
    connect(&client_, &IndiClient::serverDisconnected, this, [this] {
        reconnect_.stop();
        device_.clear();
        legs_.clear();
        takenUp_ = 0;
        setMoving(false);
        emit readyChanged(false);
    });
    connect(&client_, &IndiClient::propertyDeleted, this, [this](const QString& dev, const QString& name) {
        if (dev == device_ && (name.isEmpty() || name == kAbs)) {
            takenUp_ = 0;
            emit readyChanged(false);
        }
    });
    legTimeout_.setSingleShot(true);
    connect(&legTimeout_, &QTimer::timeout, this, [this] {
        legs_.clear();
        setMoving(false);
        emit failed("focuser didn't finish the move in time");
    });
}

bool Focuser::ready() const { return !device_.isEmpty() && client_.property(device_, kAbs) != nullptr; }

void Focuser::onDefined(const QString& dev, const QString& name) {
    if (device_.isEmpty()) {
        int iface = 0;
        if (const auto* p = client_.property(dev, "DRIVER_INFO"))
            if (auto it = p->elements.find("DRIVER_INTERFACE"); it != p->elements.end()) iface = it->second.text.toInt();
        // Drivers say what they are, or, for old drivers, have a focuser position. A mount with a focuser port (every
        // LX200 driver says TELESCOPE | GUIDER | FOCUSER) is not the focuser to use unless it's asked for by name.
        bool isFocuser = (name == "DRIVER_INFO" && (iface & kFocuserInterface)) || name == kAbs;
        if ((iface & kTelescopeInterface) && preferred_ != dev) isFocuser = false;
        if (!isFocuser || (!preferred_.isEmpty() && preferred_ != dev)) return;
        device_ = dev;
        ensureConnected();
    }
    if (dev != device_) return;
    if (name == "CONNECTION") ensureConnected();
    if (name == kAbs) {
        const auto* p = client_.property(dev, name);
        if (const auto e = p->elements.find(kAbsElem); e != p->elements.end()) {
            position_ = static_cast<int>(e->second.number());
            if (!moving_) target_ = position_;   // any client's getProperties re-sends every definition, mid-move too
            // FOCUS_MAX is the user's limit; the position's own range can be wider (MyFocuserPro2: 200000 vs 100000)
            const auto limit = client_.number(dev, "FOCUS_MAX", "FOCUS_MAX_VALUE");
            max_ = limit && *limit > 0 ? static_cast<int>(*limit) : static_cast<int>(e->second.max);
        }
        emit positionChanged(position_);
        emit readyChanged(true);
    }
    if (name == "FOCUS_MAX")
        if (auto m = client_.number(dev, name, "FOCUS_MAX_VALUE")) max_ = static_cast<int>(*m);
}

void Focuser::ensureConnected() {
    const auto* p = client_.property(device_, "CONNECTION");
    if (!p || !p->elements.count("CONNECT") || p->elements.at("CONNECT").on() || p->busy()) return;
    // After a failed connect (EAF unplugged, wrong port), try again every 10 s rather than at once: an immediate retry
    // hammers the driver and floods the log, and a later try still picks the focuser up when it's plugged in.
    if (p->state == "Alert") {
        if (!reconnect_.isActive()) reconnect_.start(10000);
        return;
    }
    client_.sendSwitch(device_, "CONNECTION", "CONNECT");
}

void Focuser::onMessage(const QString& dev, const QString& text) {
    // A serial focuser whose USB adapter dropped off the bus: the driver keeps the dead port open, still says it's
    // connected, and fails every write with EIO (or ENODEV) until it's disconnected and connected again.
    if (dev == device_ && (text.contains("Input/output error") || text.contains("No such device"))) relink();
}

void Focuser::relink() {
    const auto* p = client_.property(device_, "CONNECTION");
    if (!p || !p->elements.count("CONNECT") || !p->elements.at("CONNECT").on()) return;   // not connected: ensureConnected's job
    if (lastRelink_.isValid() && lastRelink_.elapsed() < 10000) return;
    lastRelink_.start();
    legs_.clear();
    legTimeout_.stop();
    setMoving(false);
    takenUp_ = 0;   // the move was cut off somewhere
    emit failed("the focuser's serial port went away (USB unplugged, or a bad cable?); reconnecting");
    // Disconnecting closes the dead port. The driver then reports itself off and ensureConnected() connects it again,
    // which reopens the port by its path, and keeps trying every 10 s while the adapter is missing.
    client_.sendSwitch(device_, "CONNECTION", "DISCONNECT");
}

void Focuser::onUpdated(const QString& dev, const QString& name) {
    if (dev != device_) return;
    if (name == "CONNECTION") {
        ensureConnected();
        return;
    }
    if (name == "FOCUS_MAX") {
        if (auto m = client_.number(dev, name, "FOCUS_MAX_VALUE")) max_ = static_cast<int>(*m);
        return;
    }
    if (name != kAbs) return;
    const auto* p = client_.property(dev, name);
    if (auto v = client_.number(dev, name, kAbsElem)) {
        const int pos = static_cast<int>(*v);
        if (pos != position_) {
            const int ahead = (pos - position_) * (outward_ ? 1 : -1);
            takenUp_ = ahead > 0 ? std::min(backlash_, takenUp_ + ahead) : 0;
            position_ = pos;
            emit positionChanged(position_);
        }
    }
    if (p->state == "Alert") {
        legs_.clear();
        setMoving(false);
        emit failed("focuser reported an error");
        return;
    }
    if (!moving_ || legs_.empty()) return;
    if (p->busy()) {
        legSeenBusy_ = true;
        return;
    }
    // Idle/Ok: this leg is done once the driver has been busy, or it's already at the leg's position.
    if (legSeenBusy_ || position_ == legs_.front()) {
        legs_.pop_front();
        if (legs_.empty()) finishMove();
        else sendLeg();
    }
}

void Focuser::goTo(int position) {
    if (!ready()) {
        emit failed("no focuser");
        return;
    }
    if (max_ > 0) position = std::clamp(position, 0, max_);
    target_ = position;
    legs_.clear();
    // Final approach direction is fixed, and the move must end with the backlash taken up that way. Coming from the
    // wrong side, or too soon after running the other way (a click while it's still heading inward: the reported
    // position is above the new target, but the gears are loaded inward), overshoot past the target by the backlash.
    const int from = position_;
    const int dir = outward_ ? 1 : -1;
    const int ahead = (position - from) * dir;   // steps to go in the approach direction
    if (backlash_ > 0 && (ahead < 0 || takenUp_ + ahead < backlash_)) {
        int over = position - dir * backlash_;
        if (max_ > 0) over = std::clamp(over, 0, max_);
        legs_.push_back(over);
    }
    legs_.push_back(position);
    qInfo().noquote() << "focuser: go to" << position << "from" << from << (legs_.size() > 1 ? QString("via %1 (backlash)").arg(legs_.front()) : QString());
    setMoving(true);
    sendLeg();
}

void Focuser::moveBy(int steps) { goTo((moving_ ? target_ : position_) + steps); }

void Focuser::abort() {
    qInfo().noquote() << "focuser: stop at" << position_ << (moving_ ? QString("(was going to %1)").arg(target_) : QString());
    legs_.clear();
    legTimeout_.stop();
    if (ready()) client_.sendSwitch(device_, "FOCUS_ABORT_MOTION", "ABORT");
    setMoving(false);
}

void Focuser::sendLeg() {
    legSeenBusy_ = false;
    const int leg = legs_.front();
    // Running the other way unloads the gears from the moment the leg starts, well before position reports (about one
    // a second) show it, so a click in between still overshoots.
    if ((leg - position_) * (outward_ ? 1 : -1) < 0) takenUp_ = 0;
    // generous: the slowest focusers do ~100 steps/s
    legTimeout_.start(std::max(5000, std::abs(leg - position_) * 15 + 5000));
    client_.sendNumbers(device_, kAbs, {{kAbsElem, static_cast<double>(leg)}});
}

void Focuser::finishMove() {
    legTimeout_.stop();
    setMoving(false);
    emit arrived(position_);
}

void Focuser::setMoving(bool m) {
    if (m == moving_) return;
    moving_ = m;
    emit movingChanged(m);
}

}  // namespace slopcap::devices

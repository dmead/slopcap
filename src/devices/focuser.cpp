#include "devices/focuser.hpp"

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
    connect(&client_, &IndiClient::serverDisconnected, this, [this] {
        device_.clear();
        legs_.clear();
        setMoving(false);
        emit readyChanged(false);
    });
    connect(&client_, &IndiClient::propertyDeleted, this, [this](const QString& dev, const QString& name) {
        if (dev == device_ && (name.isEmpty() || name == kAbs)) emit readyChanged(false);
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
            position_ = target_ = static_cast<int>(e->second.number());
            max_ = static_cast<int>(e->second.max);
        }
        emit positionChanged(position_);
        emit readyChanged(true);
    }
    if (name == "FOCUS_MAX")
        if (auto m = client_.number(dev, name, "FOCUS_MAX_VALUE")) max_ = static_cast<int>(*m);
}

void Focuser::ensureConnected() {
    const auto* p = client_.property(device_, "CONNECTION");
    if (p && p->elements.count("CONNECT") && !p->elements.at("CONNECT").on() && !p->busy())
        client_.sendSwitch(device_, "CONNECTION", "CONNECT");
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
    // Final approach direction is fixed: coming from the wrong side means overshooting past the target by the backlash.
    const int from = position_;
    const bool needsOvershoot = outward_ ? position < from : position > from;
    if (needsOvershoot && backlash_ > 0) {
        int over = outward_ ? position - backlash_ : position + backlash_;
        if (max_ > 0) over = std::clamp(over, 0, max_);
        legs_.push_back(over);
    }
    legs_.push_back(position);
    setMoving(true);
    sendLeg();
}

void Focuser::moveBy(int steps) { goTo((moving_ ? target_ : position_) + steps); }

void Focuser::abort() {
    legs_.clear();
    legTimeout_.stop();
    if (ready()) client_.sendSwitch(device_, "FOCUS_ABORT_MOTION", "ABORT");
    setMoving(false);
}

void Focuser::sendLeg() {
    legSeenBusy_ = false;
    const int leg = legs_.front();
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

// An INDI focuser with backlash-aware moves: every move ends with an approach from the same direction (by default
// outward), overshooting by the backlash first unless the gears are known to be loaded that way already, so a position
// always means the same optical focus. The first INDI device whose DRIVER_INTERFACE includes FOCUSER_INTERFACE is used
// (or a named one), skipping mounts that also say FOCUSER (an LX200's focuser port), and it is connected if it isn't
// already, and connected again if its serial port goes away underneath the driver.
#pragma once

#include "devices/indi_client.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <deque>

namespace slopcap::devices {

class Focuser : public QObject {
    Q_OBJECT
public:
    explicit Focuser(IndiClient& client, QObject* parent = nullptr);

    void setPreferredDevice(const QString& name) { preferred_ = name; }
    QString device() const { return device_; }
    bool ready() const;                    ///< connected and reporting a position
    int position() const { return position_; }
    int maxPosition() const { return max_; }
    bool moving() const { return moving_; }
    int target() const { return target_; }

    void setBacklash(int steps) { backlash_ = std::max(0, steps); }
    int backlash() const { return backlash_; }
    void setApproachOutward(bool outward) {
        if (outward != outward_) takenUp_ = 0;
        outward_ = outward;
    }
    bool approachOutward() const { return outward_; }

    void goTo(int position);               ///< backlash-aware absolute move
    void moveBy(int steps);                ///< relative to the current target (so repeated clicks add up)
    void abort();

signals:
    void readyChanged(bool ready);
    void positionChanged(int position);
    void movingChanged(bool moving);
    void arrived(int position);            ///< the whole (possibly two-leg) move finished
    void failed(const QString& why);

private:
    void onDefined(const QString& dev, const QString& name);
    void onUpdated(const QString& dev, const QString& name);
    void onMessage(const QString& dev, const QString& text);
    void ensureConnected();
    void relink();
    void sendLeg();
    void finishMove();
    void setMoving(bool m);

    IndiClient& client_;
    QString preferred_, device_;
    int position_ = 0, max_ = 0, target_ = 0;
    int backlash_ = 600;
    bool outward_ = true;
    bool moving_ = false, legSeenBusy_ = false;
    // Steps run in the approach direction since the focuser last ran the other way, up to the backlash: at the
    // backlash, the gears are loaded on the approach side. 0 when unknown (just connected, after a lost link).
    int takenUp_ = 0;
    std::deque<int> legs_;
    QTimer legTimeout_;
    QTimer reconnect_;          // the next try after a failed connect
    QElapsedTimer lastRelink_;  // the driver repeats a dead port's error on every poll: reconnect once per 10 s
};

}  // namespace slopcap::devices

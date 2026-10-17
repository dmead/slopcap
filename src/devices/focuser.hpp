// An INDI focuser with backlash-aware moves: every move ends with an approach from the same direction (by default
// outward), overshooting by the backlash first when coming from the other side, so a position always means the same
// optical focus. The first INDI device whose DRIVER_INTERFACE includes FOCUSER_INTERFACE is used (or a named one),
// skipping mounts that also say FOCUSER (an LX200's focuser port), and it is connected if it isn't already.
#pragma once

#include "devices/indi_client.hpp"

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
    void setApproachOutward(bool outward) { outward_ = outward; }
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
    void ensureConnected();
    void sendLeg();
    void finishMove();
    void setMoving(bool m);

    IndiClient& client_;
    QString preferred_, device_;
    int position_ = 0, max_ = 0, target_ = 0;
    int backlash_ = 600;
    bool outward_ = true;
    bool moving_ = false, legSeenBusy_ = false;
    std::deque<int> legs_;
    QTimer legTimeout_;
    QTimer reconnect_;   // the next try after a failed connect
};

}  // namespace slopcap::devices

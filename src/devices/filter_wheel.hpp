// The filter wheel for the app: opens one (devices/wheel.hpp), names its slots (kept per wheel in QSettings, "1", "2", ...
// until named), turns it, and polls it while it turns and now and then while it doesn't (a wheel turned by another
// program, or unplugged, shows up within a couple of seconds). On the GUI thread: the SDK calls take a few ms.
#pragma once

#include "devices/wheel.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace slopcap::devices {

class FilterWheel : public QObject {
    Q_OBJECT
public:
    explicit FilterWheel(QObject* parent = nullptr);
    ~FilterWheel() override;

    bool open(const QString& spec);        ///< "efw", "sim:5"...; false (and failed()) when it can't
    void close();
    bool ready() const { return wheel_ != nullptr; }
    QString spec() const { return spec_; }
    QString model() const { return model_; }
    int slotCount() const { return names_.size(); }
    int position() const { return position_; }   ///< -1 while turning or not open
    int target() const { return target_; }
    bool moving() const { return target_ >= 0 && position_ != target_; }

    QStringList names() const { return names_; }
    void setNames(const QStringList& names);   ///< blanks become the slot's number; kept for this wheel
    QString name(int slot) const;
    int slotNamed(const QString& name) const;  ///< -1 if none (case-insensitive)

    void goTo(int slot);

signals:
    void readyChanged(bool ready);
    void positionChanged(int slot);        ///< -1 when it starts turning
    void arrived(int slot);                ///< a goTo finished
    void namesChanged();
    void failed(const QString& why);

private:
    void poll();
    void lost(const QString& why);

    std::unique_ptr<IFilterWheel> wheel_;
    QString spec_, model_, serial_;
    QStringList names_;
    int position_ = -1, target_ = -1;
    QTimer poll_;
    QElapsedTimer moveClock_;
};

}  // namespace slopcap::devices

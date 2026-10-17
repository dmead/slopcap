// An INDI mount, through the standard telescope properties so any driver works (LX200 Classic / Autostar / GPS, the
// simulator, ...): guide pulses (TELESCOPE_TIMED_GUIDE_NS/WE, at the mount's guide rate), axis moves at a slew rate
// until stopped (TELESCOPE_MOTION_NS/WE), goto, abort, park / unpark, tracking on / off, and the current RA / Dec.
// The first INDI device whose DRIVER_INTERFACE includes TELESCOPE_INTERFACE is used (or a named one), and it is
// connected if it isn't already.
#pragma once

#include "devices/indi_client.hpp"

#include <QObject>
#include <QStringList>
#include <QTimer>

#include <optional>

namespace slopcap::devices {

class Mount : public QObject {
    Q_OBJECT
public:
    enum class Direction { North, South, East, West };

    explicit Mount(IndiClient& client, QObject* parent = nullptr);

    void setPreferredDevice(const QString& name) { preferred_ = name; }
    QString device() const { return device_; }
    bool ready() const;                    ///< connected and reporting coordinates

    double ra() const { return ra_; }      ///< hours, JNow
    double dec() const { return dec_; }    ///< degrees, JNow
    bool slewing() const { return slewing_; }
    std::optional<bool> tracking() const;  ///< nullopt: the driver doesn't say
    std::optional<bool> parked() const;    ///< at the park position (not on the way); nullopt: the driver can't park
    bool canPulse() const;                 ///< the driver takes timed guide pulses
    bool pulsing() const { return pulsing_; }

    /// Guide pulse at the mount's guide rate. One axis at a time: N/S and E/W pulses may overlap.
    void pulse(Direction d, int ms);
    /// Move at the current slew rate until stop() / abort().
    void startMove(Direction d);
    void stopMove();
    void goTo(double raHours, double decDegrees);   ///< slew there (JNow), then track
    void abort();                          ///< stop every motion (slews, moves)
    void park();
    void unpark();
    void setTracking(bool on);

    QStringList slewRates() const;         ///< the driver's names, slowest first ("1x".. / "Guide", "Centering", ...)
    QString slewRate() const;
    void setSlewRate(const QString& name);

signals:
    void readyChanged(bool ready);
    void coordsChanged(double ra, double dec);
    void stateChanged();                   ///< tracking, park, slewing, pulsing or slew rate changed
    void pulseDone();                      ///< every pulse sent has finished
    void failed(const QString& why);

private:
    void onDefined(const QString& dev, const QString& name);
    void onUpdated(const QString& dev, const QString& name);
    void ensureConnected();
    void readCoords();
    void updatePulsing();

    IndiClient& client_;
    QString preferred_, device_;
    double ra_ = 0, dec_ = 0;
    bool slewing_ = false, pulsing_ = false;
    bool nsBusy_ = false, weBusy_ = false;   // a pulse sent on that axis hasn't finished
    QTimer nsTimeout_, weTimeout_;
};

}  // namespace slopcap::devices

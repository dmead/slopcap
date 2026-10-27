// The mount: guide pulses, axis moves at a slew rate until stopped, goto, abort, park / unpark, tracking on / off, and
// the current RA / Dec (JNow), the same whichever way it's reached:
//   INDI          the standard telescope properties, so any INDI driver works (TELESCOPE_TIMED_GUIDE_NS/WE,
//                 TELESCOPE_MOTION_NS/WE, ...): the first device whose DRIVER_INTERFACE includes TELESCOPE_INTERFACE
//                 (or a named one), connected if it isn't already
//   SlopCap's own drivers (src/mounts/: LX200 / OnStep, NexStar / SynScan, Sky-Watcher's motor controller, the SynScan
//                 app, a simulator), chosen by a spec such as "lx200:COM3", each on its own thread (MountWorker).
//                 INDI's telescope device is left alone meanwhile.
#pragma once

#include "devices/indi_client.hpp"
#include "mounts/mount_driver.hpp"

#include <QObject>
#include <QStringList>
#include <QTimer>

#include <memory>
#include <optional>

namespace slopcap::mounts {
class MountWorker;
}

namespace slopcap::devices {

class Mount : public QObject {
    Q_OBJECT
public:
    enum class Direction { North, South, East, West };

    explicit Mount(IndiClient& client, QObject* parent = nullptr);
    ~Mount() override;

    /// Which mount: one of SlopCap's own (src/mounts/open_mount.hpp), or anything else (empty) for INDI's. Remembered;
    /// a different built-in mount that is connected is closed. The site is for drivers that need it.
    void setSpec(const QString& spec, std::optional<mounts::Site> site = std::nullopt);
    QString spec() const { return spec_; }
    bool builtInSpec() const;              ///< the spec names one of SlopCap's own drivers
    void openBuiltIn();                    ///< connect it (retrying every 10 s until it answers)
    void close();                          ///< a built-in mount: stop what moves, let go of the port
    bool builtIn() const { return worker_ != nullptr; }

    void setPreferredDevice(const QString& name) { preferred_ = name; }
    QString device() const;                ///< INDI's device, or what a built-in mount says it is
    bool ready() const;                    ///< connected (INDI: and reporting coordinates)
    bool hasCoords() const;                ///< ra() / dec() mean something (a mount may not know: not aligned, no site)

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
    void onWorkerStatus();
    int rateIndex() const;

    IndiClient& client_;
    QString preferred_, device_;
    QString spec_;
    std::optional<mounts::Site> site_;
    std::unique_ptr<mounts::MountWorker> worker_;
    QString builtInRate_;   ///< the move rate picked for a built-in mount
    double ra_ = 0, dec_ = 0;
    bool slewing_ = false, pulsing_ = false;
    bool nsBusy_ = false, weBusy_ = false;   // a pulse sent on that axis hasn't finished
    QTimer nsTimeout_, weTimeout_;
    QTimer reconnect_;   // the next try after a failed connect
};

}  // namespace slopcap::devices

// Capture sidebar, under the focuser: the mount, as in the design canvas's "Mount · LX200 (INDI)" panel.
// A nudge pad (N / E / Stop / W / S) sends guide pulses of the chosen length, or, with one of the mount's speeds
// picked, moves the mount while a button is held; RA / Dec and the mount's state above it,
// with Unpark / Start tracking when the mount needs them. Through INDI the mount shares the focuser's server; with
// one of SlopCap's own drivers (Settings > Mount), Connect opens that.
#pragma once

#include "devices/mount.hpp"

#include <QWidget>

#include <optional>

class QComboBox;
class QTimer;
class QLabel;
class QPushButton;

namespace slopcap::app {

class FocusController;
class MountSettingsDialog;

class MountSection : public QWidget {
    Q_OBJECT
public:
    MountSection(devices::Mount& mount, FocusController& fc, QWidget* parent = nullptr);
    int pulseMs() const;
    void setPulseMs(int ms);
    /// The pad's speed: "" for guide pulses (a tap nudges), else one of the mount's rates (press and hold to move).
    QString speed() const;
    void setSpeed(const QString& rate);

signals:
    void pulseLengthChanged(int ms);
    void message(const QString& text);   ///< for the status footer

private:
    void refresh();
    void press(devices::Mount::Direction d);
    void release();
    devices::Mount& mount_;
    FocusController& fc_;
    QLabel* title_ = nullptr;
    QWidget* offline_ = nullptr;
    QLabel* offlineText_ = nullptr;
    QPushButton* connect_ = nullptr;
    QWidget* online_ = nullptr;
    QLabel* coords_ = nullptr;
    QLabel* state_ = nullptr;
    QPushButton* unpark_ = nullptr;
    QPushButton* track_ = nullptr;
    QWidget* pad_ = nullptr;
    QComboBox* pulse_ = nullptr;
    QWidget* pulseRow_ = nullptr;
    QComboBox* speed_ = nullptr;
    QStringList rates_;                                  ///< the mount's, as last put in speed_
    std::optional<devices::Mount::Direction> held_;      ///< a pad button held down, moving the mount
    QTimer* renew_ = nullptr;                            ///< keeps a held move going past the mount's 10 s deadman
    QLabel* lastError_ = nullptr;                        ///< the latest thing that went wrong, until the log is looked at
    QPushButton* more_ = nullptr;                        ///< Settings and log...
    MountSettingsDialog* dialog_ = nullptr;

public:
    MountSettingsDialog* openSettings(bool log = false);   ///< (also the tests')
};

}  // namespace slopcap::app

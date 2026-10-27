// Capture sidebar, under the focuser: the mount, as in the design canvas's "Mount · LX200 (INDI)" panel.
// A nudge pad (N / E / Stop / W / S) sends guide pulses of the chosen length; RA / Dec and the mount's state above it,
// with Unpark / Start tracking when the mount needs them. Through INDI the mount shares the focuser's server; with
// one of SlopCap's own drivers (Settings > Mount), Connect opens that.
#pragma once

#include "devices/mount.hpp"

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;

namespace slopcap::app {

class FocusController;

class MountSection : public QWidget {
    Q_OBJECT
public:
    MountSection(devices::Mount& mount, FocusController& fc, QWidget* parent = nullptr);
    int pulseMs() const;
    void setPulseMs(int ms);

signals:
    void pulseLengthChanged(int ms);
    void message(const QString& text);   ///< for the status footer

private:
    void refresh();
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
};

}  // namespace slopcap::app

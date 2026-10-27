#include "app/settings_dialog.hpp"

#include "app/theme.hpp"
#include "mounts/transport.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

namespace slopcap::app {

SettingsDialog::SettingsDialog(const Settings& s, QWidget* parent) : QDialog(parent) {
    setWindowTitle("SlopCap settings");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(20, 20, 20, 20);
    v->setSpacing(14);

    v->addWidget(theme::section("Focuser and mount (INDI)"));
    auto* f1 = new QFormLayout;
    server_ = new QLineEdit(s.indiServer);
    server_->setProperty("role", "mono");
    auto_ = new QCheckBox("Connect at start");
    auto_->setChecked(s.focuserAutoConnect);
    backlash_ = new QSpinBox;
    backlash_->setRange(0, 5000);
    backlash_->setValue(s.backlash);
    backlash_->setSuffix(" steps");
    approach_ = new QComboBox;
    approach_->addItems({"Outward (increasing position)", "Inward (decreasing position)"});
    approach_->setCurrentIndex(s.approachOutward ? 0 : 1);
    f1->addRow("Server", server_);
    f1->addRow("", auto_);
    f1->addRow("Backlash", backlash_);
    f1->addRow("Final approach", approach_);
    v->addLayout(f1);

    v->addWidget(theme::section("Optics"));
    auto* f2 = new QFormLayout;
    fRatio_ = new QDoubleSpinBox;
    fRatio_->setRange(0, 100);
    fRatio_->setDecimals(1);
    fRatio_->setSpecialValueText("unknown");
    fRatio_->setValue(s.focalRatio);
    fRatio_->setPrefix("f/");
    umStep_ = new QDoubleSpinBox;
    umStep_->setRange(0, 100);
    umStep_->setDecimals(2);
    umStep_->setSpecialValueText("unknown");
    umStep_->setValue(s.micronsPerStep);
    umStep_->setSuffix(" µm/step");
    focalLength_ = new QDoubleSpinBox;
    focalLength_->setRange(0, 50000);
    focalLength_->setDecimals(0);
    focalLength_->setSingleStep(100);
    focalLength_->setSpecialValueText("unknown");
    focalLength_->setValue(s.focalLengthMm);
    focalLength_->setSuffix(" mm");
    f2->addRow("Focal ratio", fRatio_);
    f2->addRow("Focuser step", umStep_);
    f2->addRow("Focal length", focalLength_);
    v->addLayout(f2);
    auto* note = theme::label("Focal ratio and step: the critical focus zone (±2.44 λN²) on the focus sweep. Focal length "
                              "(effective, with any Barlow): the image scale, so the automatic target can check the "
                              "target's size.", "dim");
    note->setWordWrap(true);
    v->addWidget(note);

    // The site, for which targets are above the horizon (the automatic target; the mount is never asked).
    v->addWidget(theme::section("Site"));
    auto* f3 = new QFormLayout;
    auto coordinate = [](double limit, std::optional<double> value, const QString& suffix) {
        auto* b = new QDoubleSpinBox;
        b->setRange(-limit - 1, limit);   // the extra step below the range means "unknown"
        b->setDecimals(4);
        b->setSpecialValueText("unknown");
        b->setValue(value.value_or(-limit - 1));
        b->setSuffix(suffix);
        return b;
    };
    latitude_ = coordinate(90, s.latitude, "° N");
    longitude_ = coordinate(180, s.longitude, "° E");
    f3->addRow("Latitude", latitude_);
    f3->addRow("Longitude", longitude_);
    v->addLayout(f3);
    auto* siteNote = theme::label("South and west are negative. With the time, it tells the automatic target what is above "
                                  "the horizon.", "dim");
    siteNote->setWordWrap(true);
    v->addWidget(siteNote);

    // (made after the focuser's fields: tests and readers find the INDI server's box first)
    v->addWidget(theme::section("Mount"));
    auto* f4 = new QFormLayout;
    mount_ = new QLineEdit(s.mountSpec);
    mount_->setProperty("role", "mono");
    mount_->setPlaceholderText("INDI, on the server above");
    mountAuto_ = new QCheckBox("Connect at start");
    mountAuto_->setChecked(s.mountAutoConnect);
    f4->addRow("Mount", mount_);
    f4->addRow("", mountAuto_);
    v->addLayout(f4);
    QStringList ports;
    for (const auto& p : mounts::listSerialPorts()) ports << QString::fromStdString(p);
    auto* mountNote = theme::label(QString("Empty for the INDI server's mount, or one SlopCap drives itself: lx200:PORT (Meade, "
                                           "OnStep), onstep:tcp://HOST (OnStep's Wi-Fi), nexstar:PORT (Celestron hand "
                                           "controller), synscan:PORT (Sky-Watcher hand controller), skywatcher:PORT or "
                                           "skywatcher:udp://192.168.4.1 (a Sky-Watcher's own USB or Wi-Fi; needs the site), "
                                           "synscan-app (the SynScan app), sim. Serial ports here: %1.")
                                       .arg(ports.isEmpty() ? QString("none") : ports.join(", ")),
                                   "dim");
    mountNote->setWordWrap(true);
    v->addWidget(mountNote);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);
}

Settings SettingsDialog::settings() const {
    Settings s;
    s.indiServer = server_->text().trimmed();
    s.focuserAutoConnect = auto_->isChecked();
    s.backlash = backlash_->value();
    s.approachOutward = approach_->currentIndex() == 0;
    s.focalRatio = fRatio_->value();
    s.micronsPerStep = umStep_->value();
    s.focalLengthMm = focalLength_->value();
    if (latitude_->value() >= -90 && longitude_->value() >= -180) s.latitude = latitude_->value(), s.longitude = longitude_->value();
    s.mountSpec = mount_->text().trimmed();
    s.mountAutoConnect = mountAuto_->isChecked();
    return s;
}

}  // namespace slopcap::app

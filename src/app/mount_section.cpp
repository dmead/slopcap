#include "app/mount_section.hpp"

#include "app/focus_controller.hpp"
#include "app/mount_settings_dialog.hpp"
#include "app/theme.hpp"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include <cmath>

namespace slopcap::app {

namespace {

using Direction = devices::Mount::Direction;

// 44 × 44 pad buttons. The size goes in the style sheet: a widget style sheet re-applies the theme's min-height and
// overrides setFixedSize. Sizes are of the contents, inside the 1 px border.
const QString kPadButton = "padding: 0; min-width: 42px; max-width: 42px; min-height: 42px; max-height: 42px; ";

/// 12h 34m 56s
QString formatRa(double hours) {
    long s = std::lround(std::fmod(hours + 24.0, 24.0) * 3600.0) % (24 * 3600);
    return QString("%1h %2m %3s").arg(s / 3600, 2, 10, QChar('0')).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

/// +20° 15′ 30″
QString formatDec(double degrees) {
    const long s = std::lround(std::abs(degrees) * 3600.0);
    return QString("%1%2° %3′ %4″").arg(degrees < 0 ? "−" : "+").arg(s / 3600, 2, 10, QChar('0')).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

}  // namespace

MountSection::MountSection(devices::Mount& mount, FocusController& fc, QWidget* parent) : QWidget(parent), mount_(mount), fc_(fc) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    title_ = theme::section("Mount");
    v->addWidget(title_);

    offline_ = new QWidget;
    auto* ol = new QHBoxLayout(offline_);
    ol->setContentsMargins(0, 0, 0, 0);
    offlineText_ = theme::label("Not connected", "muted");
    offlineText_->setWordWrap(true);
    connect_ = theme::button("Connect");
    ol->addWidget(offlineText_, 1);
    ol->addWidget(connect_);
    v->addWidget(offline_);
    connect(connect_, &QPushButton::clicked, this, [this] {
        if (mount_.builtInSpec()) mount_.openBuiltIn();   // SlopCap's own driver (Settings > Mount)
        else fc_.connectFocuser(fc_.server());            // INDI's, on the focuser's server
    });

    online_ = new QWidget;
    auto* on = new QVBoxLayout(online_);
    on->setContentsMargins(0, 0, 0, 0);
    on->setSpacing(4);
    coords_ = theme::label("—", "mono");
    state_ = theme::label("", "muted");
    state_->setWordWrap(true);
    on->addWidget(coords_);
    on->addWidget(state_);
    auto* fixes = new QHBoxLayout;
    fixes->setSpacing(6);
    unpark_ = theme::button("Unpark");
    track_ = theme::button("Start tracking");
    fixes->addWidget(unpark_);
    fixes->addWidget(track_);
    fixes->addStretch(1);
    on->addLayout(fixes);
    v->addWidget(online_);
    connect(unpark_, &QPushButton::clicked, this, [this] { mount_.unpark(); });
    connect(track_, &QPushButton::clicked, this, [this] { mount_.setTracking(true); });

    // The pad, laid out as seen on the sky: north up, east left.
    pad_ = new QWidget;
    auto* g = new QGridLayout(pad_);
    g->setContentsMargins(0, 0, 0, 0);
    g->setSpacing(4);
    auto nudge = [&](const QString& text, const QString& name, Direction d, int row, int col) {
        auto* b = theme::button(text);
        b->setStyleSheet(kPadButton + "font-weight: 600;");
        b->setAccessibleName(name);
        b->setToolTip(name + ": a guide pulse, or with a speed picked, moves while held");
        // guide pulses: a tap; a speed: moves from press to release
        connect(b, &QPushButton::pressed, this, [this, d] { press(d); });
        connect(b, &QPushButton::released, this, [this] { release(); });
        connect(b, &QPushButton::clicked, this, [this, d] {
            if (speed().isEmpty()) mount_.pulse(d, pulseMs());
        });
        g->addWidget(b, row, col);
    };
    nudge("N", "Nudge north", Direction::North, 0, 1);
    nudge("E", "Nudge east", Direction::East, 1, 0);
    nudge("W", "Nudge west", Direction::West, 1, 2);
    nudge("S", "Nudge south", Direction::South, 2, 1);
    auto* stop = theme::button("Stop");
    stop->setStyleSheet(kPadButton + "font-size: 11px;");
    stop->setAccessibleName("Stop mount");
    stop->setToolTip("Stop every mount motion (slews and moves)");
    connect(stop, &QPushButton::clicked, this, [this] {
        held_.reset();
        renew_->stop();
        mount_.stopMove();
        mount_.abort();
    });
    g->addWidget(stop, 1, 1);
    v->addWidget(pad_, 0, Qt::AlignHCenter);

    auto* speedRow = new QHBoxLayout;
    speedRow->addWidget(theme::label("Speed", "muted"));
    speedRow->addStretch(1);
    speed_ = new QComboBox;
    speed_->setAccessibleName("Mount speed");
    speed_->setToolTip("Guide pulse: each tap nudges the mount at its guide rate.\n"
                       "A speed: the mount moves while a pad button is held.");
    speed_->addItem("Guide pulse", QString());
    speedRow->addWidget(speed_);
    v->addLayout(speedRow);
    connect(speed_, &QComboBox::currentIndexChanged, this, [this] {
        release();
        if (!speed().isEmpty()) mount_.setSlewRate(speed());
        pulseRow_->setVisible(speed().isEmpty());
    });

    pulseRow_ = new QWidget;
    auto* pulseRow = new QHBoxLayout(pulseRow_);
    pulseRow->setContentsMargins(0, 0, 0, 0);
    pulseRow->addWidget(theme::label("Pulse length", "muted"));
    pulseRow->addStretch(1);
    pulse_ = new QComboBox;
    pulse_->setAccessibleName("Pulse length");
    pulse_->addItem("100 ms", 100);
    pulse_->addItem("300 ms", 300);
    pulse_->addItem("1 s", 1000);
    pulse_->setCurrentIndex(1);
    pulseRow->addWidget(pulse_);
    v->addWidget(pulseRow_);
    connect(pulse_, &QComboBox::currentIndexChanged, this, [this] { emit pulseLengthChanged(pulseMs()); });

    // a held button renews its move before the mount's own 10 s deadman stops it (MountWorker::kDeadmanMs)
    renew_ = new QTimer(this);
    renew_->setInterval(4000);
    connect(renew_, &QTimer::timeout, this, [this] {
        if (held_) mount_.startMove(*held_);
    });

    connect(&mount_, &devices::Mount::readyChanged, this, &MountSection::refresh);
    connect(&mount_, &devices::Mount::coordsChanged, this, &MountSection::refresh);
    connect(&mount_, &devices::Mount::stateChanged, this, &MountSection::refresh);
    // what went wrong last, in red until the log is opened; and the mount's own settings and the log, connected or not
    auto* moreRow = new QHBoxLayout;
    moreRow->setSpacing(6);
    lastError_ = theme::label("", "muted");
    lastError_->setWordWrap(true);
    lastError_->setStyleSheet("color: #ff6b6b;");
    lastError_->setVisible(false);
    more_ = theme::button("Settings and log…");
    more_->setToolTip("The mount's own settings, where SlopCap can change them, and every command, reply and error");
    moreRow->addWidget(lastError_, 1);
    moreRow->addWidget(more_, 0, Qt::AlignTop);
    v->addLayout(moreRow);
    connect(more_, &QPushButton::clicked, this, [this] { openSettings(lastError_->isVisible()); });

    connect(&mount_, &devices::Mount::failed, this, [this](const QString& why) {
        emit message("Mount: " + why);
        lastError_->setText(QTime::currentTime().toString("HH:mm") + "  " + why);
        lastError_->setToolTip("Settings and log… has the whole story");
        lastError_->setVisible(true);
    });
    // the INDI connection itself (shared with the focuser)
    connect(&fc_, &FocusController::focuserChanged, this, &MountSection::refresh);
    refresh();
}

int MountSection::pulseMs() const { return pulse_->currentData().toInt(); }

MountSettingsDialog* MountSection::openSettings(bool log) {
    if (!dialog_) dialog_ = new MountSettingsDialog(mount_, this);
    else if (mount_.hasSettings()) mount_.readSettings();
    if (log) dialog_->showLog();
    lastError_->setVisible(false);
    dialog_->show();
    dialog_->raise();
    dialog_->activateWindow();
    return dialog_;
}

QString MountSection::speed() const { return speed_->currentData().toString(); }

void MountSection::setSpeed(const QString& rate) {
    const int i = speed_->findData(rate);
    if (i >= 0) speed_->setCurrentIndex(i);
}

void MountSection::press(Direction d) {
    if (speed().isEmpty()) return;   // a guide pulse, on click
    mount_.setSlewRate(speed());
    held_ = d;
    mount_.startMove(d);
    renew_->start();
}

void MountSection::release() {
    if (!held_) return;
    held_.reset();
    renew_->stop();
    mount_.stopMove();
}

void MountSection::setPulseMs(int ms) {
    const int i = pulse_->findData(ms);
    if (i >= 0) pulse_->setCurrentIndex(i);
}

void MountSection::refresh() {
    const bool ok = mount_.ready();
    const bool indi = fc_.indi().isConnected();
    title_->setText(QString("MOUNT%1").arg(ok ? " · " + mount_.device().toUpper() : ""));
    offline_->setVisible(!ok);
    if (mount_.builtInSpec()) {
        connect_->setVisible(!mount_.builtIn());
        offlineText_->setText(mount_.builtIn() ? "Connecting to " + mount_.spec() + "… (again every 10 s until it answers)"
                                               : "Not connected · " + mount_.spec());
    } else {
        connect_->setVisible(!indi);
        offlineText_->setText(indi ? (mount_.device().isEmpty() ? "INDI connected · no mount yet" : "Connecting to " + mount_.device() + "…")
                                   : "Not connected · " + fc_.server());
    }
    online_->setVisible(ok);
    pad_->setVisible(ok);
    // the speeds the mount has, after "Guide pulse"
    if (const auto rates = ok ? mount_.slewRates() : QStringList{}; rates != rates_) {
        const QString was = speed();
        const QSignalBlocker block(speed_);
        rates_ = rates;
        while (speed_->count() > 1) speed_->removeItem(1);
        for (const auto& r : rates_) speed_->addItem(r, r);
        speed_->setCurrentIndex(std::max(0, speed_->findData(was)));
        pulseRow_->setVisible(speed().isEmpty());
    }
    speed_->setEnabled(ok && !rates_.isEmpty());
    if (!ok) {
        release();
        return;
    }

    coords_->setText(mount_.hasCoords() ? QString("RA %1   Dec %2").arg(formatRa(mount_.ra()), formatDec(mount_.dec())) : QString("RA —   Dec —"));
    const auto parked = mount_.parked();
    const auto tracking = mount_.tracking();
    QStringList state;
    if (parked == true) state << "Parked";
    else if (mount_.slewing()) state << "Slewing";
    else if (tracking == true) state << "Tracking";
    else if (tracking == false) state << "Not tracking";
    if (mount_.atHome() && parked != true) state << "at home";
    if (const auto pier = mount_.pierSide(); pier != mounts::PierSide::Unknown) state << (pier == mounts::PierSide::East ? "pier east" : "pier west");
    if (mount_.pulsing()) state << "pulsing";
    if (!mount_.canPulse()) state << "no guide pulses";
    if (const auto fault = mount_.fault(); !fault.isEmpty()) state << "⚠ " + fault;
    state_->setText(state.join(" · "));
    unpark_->setVisible(parked == true);
    track_->setVisible(parked != true && tracking == false);
    pad_->setEnabled(mount_.canPulse() && parked != true);
}

}  // namespace slopcap::app

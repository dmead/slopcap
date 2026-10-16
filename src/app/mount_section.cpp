#include "app/mount_section.hpp"

#include "app/focus_controller.hpp"
#include "app/theme.hpp"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

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
    connect(connect_, &QPushButton::clicked, this, [this] { fc_.connectFocuser(fc_.server()); });

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
        b->setToolTip(name + " (guide pulse)");
        connect(b, &QPushButton::clicked, this, [this, d] { mount_.pulse(d, pulseMs()); });
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
        mount_.stopMove();
        mount_.abort();
    });
    g->addWidget(stop, 1, 1);
    v->addWidget(pad_, 0, Qt::AlignHCenter);

    auto* pulseRow = new QHBoxLayout;
    pulseRow->addWidget(theme::label("Pulse length", "muted"));
    pulseRow->addStretch(1);
    pulse_ = new QComboBox;
    pulse_->addItem("100 ms", 100);
    pulse_->addItem("300 ms", 300);
    pulse_->addItem("1 s", 1000);
    pulse_->setCurrentIndex(1);
    pulseRow->addWidget(pulse_);
    v->addLayout(pulseRow);
    connect(pulse_, &QComboBox::currentIndexChanged, this, [this] { emit pulseLengthChanged(pulseMs()); });

    connect(&mount_, &devices::Mount::readyChanged, this, &MountSection::refresh);
    connect(&mount_, &devices::Mount::coordsChanged, this, &MountSection::refresh);
    connect(&mount_, &devices::Mount::stateChanged, this, &MountSection::refresh);
    connect(&mount_, &devices::Mount::failed, this, [this](const QString& why) { emit message("Mount: " + why); });
    // the INDI connection itself (shared with the focuser)
    connect(&fc_, &FocusController::focuserChanged, this, &MountSection::refresh);
    refresh();
}

int MountSection::pulseMs() const { return pulse_->currentData().toInt(); }

void MountSection::setPulseMs(int ms) {
    const int i = pulse_->findData(ms);
    if (i >= 0) pulse_->setCurrentIndex(i);
}

void MountSection::refresh() {
    const bool ok = mount_.ready();
    const bool indi = fc_.indi().isConnected();
    title_->setText(QString("MOUNT%1").arg(ok ? " · " + mount_.device().toUpper() : ""));
    offline_->setVisible(!ok);
    connect_->setVisible(!indi);
    offlineText_->setText(indi ? (mount_.device().isEmpty() ? "INDI connected · no mount yet" : "Connecting to " + mount_.device() + "…")
                               : "Not connected · " + fc_.server());
    online_->setVisible(ok);
    pad_->setVisible(ok);
    if (!ok) return;

    coords_->setText(QString("RA %1   Dec %2").arg(formatRa(mount_.ra()), formatDec(mount_.dec())));
    const auto parked = mount_.parked();
    const auto tracking = mount_.tracking();
    QStringList state;
    if (parked == true) state << "Parked";
    else if (mount_.slewing()) state << "Slewing";
    else if (tracking == true) state << "Tracking";
    else if (tracking == false) state << "Not tracking";
    if (mount_.pulsing()) state << "pulsing";
    if (!mount_.canPulse()) state << "no guide pulses";
    state_->setText(state.join(" · "));
    unpark_->setVisible(parked == true);
    track_->setVisible(parked != true && tracking == false);
    pad_->setEnabled(mount_.canPulse() && parked != true);
}

}  // namespace slopcap::app

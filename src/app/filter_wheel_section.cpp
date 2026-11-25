#include "app/filter_wheel_section.hpp"

#include "app/theme.hpp"
#include "devices/filter_wheel.hpp"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTime>
#include <QVBoxLayout>

namespace slopcap::app {

FilterWheelSection::FilterWheelSection(devices::FilterWheel& wheel, QWidget* parent) : QWidget(parent), wheel_(wheel) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    v->addWidget(theme::section("Filter wheel"));

    offline_ = new QWidget;
    auto* ol = new QHBoxLayout(offline_);
    ol->setContentsMargins(0, 0, 0, 0);
    offlineText_ = theme::label("Not connected", "muted");
    offlineText_->setWordWrap(true);
    connect_ = theme::button("Connect");
    connect_->setToolTip("Open the ZWO EFW (another program using it has to let go of it first)");
    ol->addWidget(offlineText_, 1);
    ol->addWidget(connect_);
    v->addWidget(offline_);
    connect(connect_, &QPushButton::clicked, this, [this] {
        QSettings st;
        if (wheel_.open(st.value("wheel/spec", "efw").toString())) st.setValue("wheel/autoConnect", true);
    });

    online_ = new QWidget;
    auto* on = new QVBoxLayout(online_);
    on->setContentsMargins(0, 0, 0, 0);
    on->setSpacing(8);
    grid_ = new QGridLayout;
    grid_->setSpacing(6);
    on->addLayout(grid_);
    state_ = theme::label("", "muted");
    state_->setWordWrap(true);
    on->addWidget(state_);
    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    auto* names = theme::button("Names…");
    names->setToolTip("Name the slots (L, R, G, B, IR, ...): the names go into file names, and the Filter box picks by them");
    auto* off = theme::button("Disconnect");
    off->setToolTip("Let go of the wheel (so another program can use it); SlopCap won't connect it at the next start");
    row->addWidget(names);
    row->addStretch(1);
    row->addWidget(off);
    on->addLayout(row);
    v->addWidget(online_);
    connect(names, &QPushButton::clicked, this, &FilterWheelSection::editNames);
    connect(off, &QPushButton::clicked, this, [this] {
        QSettings().setValue("wheel/autoConnect", false);
        wheel_.close();
    });

    connect(&wheel_, &devices::FilterWheel::readyChanged, this, &FilterWheelSection::rebuild);
    connect(&wheel_, &devices::FilterWheel::namesChanged, this, &FilterWheelSection::rebuild);
    connect(&wheel_, &devices::FilterWheel::positionChanged, this, &FilterWheelSection::refresh);
    connect(&wheel_, &devices::FilterWheel::arrived, this, [this](int slot) { emit message("Filter " + wheel_.name(slot)); });
    connect(&wheel_, &devices::FilterWheel::failed, this, [this](const QString& why) {
        offlineText_->setText(QTime::currentTime().toString("HH:mm") + "  " + why);
        emit message("Filter wheel: " + why);
        refresh();
    });
    rebuild();
}

void FilterWheelSection::rebuild() {
    for (auto* b : slots_) b->deleteLater();
    slots_.clear();
    if (wheel_.ready()) {
        const int n = wheel_.slotCount(), cols = n <= 5 ? n : (n + 1) / 2;
        for (int i = 0; i < n; ++i) {
            auto* b = theme::button(wheel_.name(i), "mono");
            b->setCheckable(true);
            b->setToolTip(QString("Slot %1").arg(i + 1));
            connect(b, &QPushButton::clicked, this, [this, i] { wheel_.goTo(i); refresh(); });
            grid_->addWidget(b, i / cols, i % cols);
            slots_.push_back(b);
        }
    }
    refresh();
}

void FilterWheelSection::refresh() {
    offline_->setVisible(!wheel_.ready());
    online_->setVisible(wheel_.ready());
    if (!wheel_.ready()) return;
    const int at = wheel_.position(), to = wheel_.target();
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        slots_[i]->setChecked(i == (at >= 0 ? at : to));
        slots_[i]->setEnabled(at >= 0);
    }
    if (at < 0 && to >= 0) state_->setText(wheel_.model() + " · turning to " + wheel_.name(to) + "…");
    else if (at < 0) state_->setText(wheel_.model() + " · turning…");
    else state_->setText(wheel_.model() + " · " + wheel_.name(at) + " in place");
}

void FilterWheelSection::editNames() {
    bool ok = false;
    const QString text = QInputDialog::getText(this, "Filter names", QString("Names of slots 1 to %1, separated by commas:").arg(wheel_.slotCount()),
                                               QLineEdit::Normal, wheel_.names().join(", "), &ok);
    if (ok) wheel_.setNames(text.split(','));
}

}  // namespace slopcap::app

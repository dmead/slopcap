#include "app/full_view_bar.hpp"

#include "app/theme.hpp"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace slopcap::app {

FullViewBar::FullViewBar(QWidget* parent) : QFrame(parent) {
    setObjectName("fullViewBar");
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QString("QFrame#fullViewBar { background: rgba(13,15,18,215); border: 1px solid %1; border-radius: 10px; }")
                      .arg(theme::kControlLine.name()));
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(16, 10, 16, 10);
    h->setSpacing(18);
    auto sliderBox = [&](const QString& name, QSlider*& slider, QLabel*& value) {
        auto* box = new QVBoxLayout;
        box->setSpacing(4);
        auto* top = new QHBoxLayout;
        top->addWidget(theme::label(name, "muted"));
        top->addStretch(1);
        value = theme::label("—", "mono");
        top->addWidget(value);
        box->addLayout(top);
        slider = new QSlider(Qt::Horizontal);
        slider->setMinimumWidth(220);
        box->addWidget(slider);
        h->addLayout(box);
    };
    sliderBox("Gain", gain, gainValue);
    sliderBox("Exposure", exposure, expValue);
    exposure->setRange(0, 1000);
    h->addWidget(theme::segmented({"Linear", "Auto stretch"}, stretch, true));
    record = theme::button("●  Record", "record");
    record->setMinimumWidth(150);
    h->addWidget(record);
    link = theme::label("", "monoMuted");
    h->addWidget(link);
    exit = theme::button("Exit full screen  (Esc)");
    h->addWidget(exit);
    for (auto* w : findChildren<QWidget*>()) w->setFocusPolicy(Qt::NoFocus);   // keys stay with the window (Esc, F11)
    hide();
}

void FullViewBar::place() {
    adjustSize();
    if (!parentWidget()) return;
    const QSize p = parentWidget()->size();
    move((p.width() - width()) / 2, p.height() - height() - 24);
    raise();
}

}  // namespace slopcap::app

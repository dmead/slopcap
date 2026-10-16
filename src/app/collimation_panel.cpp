#include "app/collimation_panel.hpp"

#include "app/theme.hpp"
#include "preview/preview_widget.hpp"

#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace slopcap::app {

void OffsetHistory::add(double pct) {
    v_.push_back(pct);
    while (v_.size() > 300) v_.pop_front();
    update();
}

void OffsetHistory::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::kControl);
    p.drawRoundedRect(rect(), 6, 6);
    const QRectF r = rect().adjusted(8, 8, -8, -8);
    const double top = std::max(6.0, v_.empty() ? 0.0 : *std::max_element(v_.begin(), v_.end()) * 1.1);
    auto Y = [&](double v) { return r.bottom() - v / top * r.height(); };
    p.setPen(QPen(theme::kGood, 1, Qt::DashLine));   // 2 %: good for most SCTs
    p.drawLine(QPointF(r.left(), Y(2)), QPointF(r.right(), Y(2)));
    if (v_.size() < 2) return;
    QPainterPath path;
    for (std::size_t i = 0; i < v_.size(); ++i) {
        const QPointF q(r.left() + r.width() * static_cast<double>(i) / 299.0, Y(v_[i]));
        if (i == 0) path.moveTo(q);
        else path.lineTo(q);
    }
    p.setPen(QPen(theme::kAccent, 1.8));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

CollimationPanel::CollimationPanel(preview::PreviewWidget* preview, QObject* parent) : QObject(parent), preview_(preview) {
    left_ = new QWidget;
    auto* l = new QVBoxLayout(left_);
    l->setContentsMargins(16, 16, 16, 16);
    l->setSpacing(14);
    l->addWidget(theme::section("Defocused star"));
    auto* how = new QLabel(
        "Put a bright star in the middle of the frame and defocus until it's a ring with the secondary's shadow inside, "
        "40–100 px across. The goal is the shadow centred in the ring.");
    how->setWordWrap(true);
    l->addWidget(how);
    l->addWidget(theme::section("Steps"));
    const char* steps[] = {"Tick <b>Analyse</b> below.", "Turn the secondary screw that moves the star <b>toward the arrow</b> (the ring's thin side), a little.",
                           "<b>Re-centre the star</b> with the mount, then look again.", "Repeat until the offset is under 2 %, then finish at smaller defocus."};
    for (int i = 0; i < 4; ++i) {
        auto* row = new QHBoxLayout;
        auto* num = new QLabel(QString::number(i + 1));
        num->setFixedSize(22, 22);
        num->setAlignment(Qt::AlignCenter);
        num->setStyleSheet(QString("background: %1; color: %2; border-radius: 11px; font-size: 12px; font-weight: 600;")
                               .arg(theme::kAccentBg.name(), theme::kAccent.name()));
        auto* t = new QLabel(steps[i]);
        t->setWordWrap(true);
        row->addWidget(num, 0, Qt::AlignTop);
        row->addWidget(t, 1);
        l->addLayout(row);
    }
    l->addStretch(1);
    enable_ = new QCheckBox("Analyse the live view");
    l->addWidget(enable_);

    right_ = new QWidget;
    auto* r = new QVBoxLayout(right_);
    r->setContentsMargins(16, 16, 16, 16);
    r->setSpacing(14);
    r->addWidget(theme::section("Shadow offset"));
    offset_ = theme::label("—", "monoHuge");
    r->addWidget(offset_);
    status_ = theme::label("Analysis off", "muted");
    status_->setWordWrap(true);
    r->addWidget(status_);
    auto* grid = new QGridLayout;
    grid->setSpacing(8);
    grid->addWidget(theme::stat("Thin side toward", thin_), 0, 0);
    grid->addWidget(theme::stat("Ring / shadow Ø", ring_), 0, 1);
    grid->addWidget(theme::stat("Roundness rms", round_), 1, 0);
    grid->addWidget(theme::stat("Star from centre", centre_), 1, 1);
    r->addLayout(grid);
    r->addWidget(theme::section("This session (% of ring radius)"));
    history_ = new OffsetHistory;
    r->addWidget(history_);
    r->addWidget(theme::label("Dashed line: 2 %, good for most SCTs.", "dim"));
    r->addStretch(1);

    connect(enable_, &QCheckBox::toggled, this, [this](bool on) {
        if (!on) clearOverlay();
        history_->clear();
        recent_.clear();
        preview_->setCrosshair(on);
        status_->setText(on ? "Looking for a star…" : "Analysis off");
        offset_->setText("—");
    });
}

void CollimationPanel::setAnalysing(bool on) { enable_->setChecked(on); }

void CollimationPanel::clearOverlay() { preview_->setOverlay("collimation", {}); }

void CollimationPanel::onFrame(const LatestFrame::Frame& frame, Bayer bayer) {
    if (!enable_->isChecked()) return;
    const auto& fi = frame.info;
    const auto d = analysis::analyzeDonut(std::span(frame.pixels).first(fi.bytes), fi.roi.width, fi.roi.height, fi.bitDepth, bayer);
    if (!d.found) {
        offset_->setText("—");
        status_->setText(QString::fromStdString(d.problem));
        clearOverlay();
        return;
    }
    // Average the last few results: seeing makes single frames jump.
    recent_.push_back(d);
    while (recent_.size() > 8) recent_.pop_front();
    analysis::DonutResult a = d;
    a.offsetX = a.offsetY = 0;
    for (const auto& x : recent_) a.offsetX += x.offsetX / recent_.size(), a.offsetY += x.offsetY / recent_.size();
    a.offsetFraction = std::hypot(a.offsetX, a.offsetY) / a.outerR;
    const double pct = 100 * a.offsetFraction;
    history_->add(pct);
    const QColor c = pct < 2 ? theme::kGood : pct < 5 ? theme::kAccent : theme::kRecText;
    offset_->setStyleSheet(QString("color: %1").arg(c.name()));
    offset_->setText(QString("%1 %").arg(pct, 0, 'f', 1));
    const bool off = std::hypot(a.fromCentreX, a.fromCentreY) > 0.1 * std::min(fi.roi.width, fi.roi.height);
    status_->setText(pct < 2 ? "Well collimated at this defocus." : off ? "Re-centre the star before judging." : "Turn toward the arrow, a little at a time.");
    thin_->setText(QString("%1°").arg(std::atan2(a.offsetY, a.offsetX) * 180 / std::numbers::pi, 0, 'f', 0));
    ring_->setText(QString("%1 / %2 px").arg(2 * a.outerR, 0, 'f', 0).arg(2 * a.innerR, 0, 'f', 0));
    round_->setText(QString("%1 / %2 px").arg(a.outerRms, 0, 'f', 1).arg(a.innerRms, 0, 'f', 1));
    centre_->setText(QString("%1, %2 px").arg(a.fromCentreX, 0, 'f', 0).arg(a.fromCentreY, 0, 'f', 0));
    preview_->setOverlay("collimation", [a, c](QPainter& p, double k) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(theme::kChannel[2], 1.5 / k));
        p.drawEllipse(QPointF(a.outerX, a.outerY), a.outerR, a.outerR);
        p.setPen(QPen(theme::kAccent, 1.5 / k));
        p.drawEllipse(QPointF(a.innerX, a.innerY), a.innerR, a.innerR);
        const double len = std::hypot(a.offsetX, a.offsetY);
        if (len > 1e-6) {   // arrow to the thin side, exaggerated so small offsets show
            const double ux = a.offsetX / len, uy = a.offsetY / len;
            const double L = std::min(a.outerR * 0.9, std::max(a.outerR * 0.25, len * 8));
            const QPointF s(a.outerX, a.outerY), e(a.outerX + ux * L, a.outerY + uy * L);
            p.setPen(QPen(c, 2.5 / k));
            p.drawLine(s, e);
            const double h = 10 / k;
            p.drawLine(e, e - QPointF(ux * h - uy * h * 0.5, uy * h + ux * h * 0.5));
            p.drawLine(e, e - QPointF(ux * h + uy * h * 0.5, uy * h - ux * h * 0.5));
        }
    });
}

}  // namespace slopcap::app

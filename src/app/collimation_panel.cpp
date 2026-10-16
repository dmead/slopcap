#include "app/collimation_panel.hpp"

#include "preview/preview_widget.hpp"

#include <QCheckBox>
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
    p.fillRect(rect(), QColor(18, 19, 22));
    if (v_.size() < 2) return;
    const double top = std::max(5.0, *std::max_element(v_.begin(), v_.end()) * 1.1);
    auto Y = [&](double v) { return height() - 4 - v / top * (height() - 8); };
    p.setPen(QPen(QColor(80, 160, 90, 120), 1, Qt::DashLine));   // 2 % guide: "good" for most SCTs
    p.drawLine(QPointF(0, Y(2)), QPointF(width(), Y(2)));
    QPainterPath path;
    for (std::size_t i = 0; i < v_.size(); ++i) {
        const QPointF q(width() * static_cast<double>(i) / 299.0, Y(v_[i]));
        if (i == 0) path.moveTo(q);
        else path.lineTo(q);
    }
    p.setPen(QPen(QColor(255, 190, 80), 1.5));
    p.drawPath(path);
}

CollimationPanel::CollimationPanel(preview::PreviewWidget* preview, QWidget* parent) : QWidget(parent), preview_(preview) {
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    auto* how = new QLabel(
        "<b>Defocused-star collimation</b><br>"
        "Put a bright star in the middle of the frame and defocus until it's a ring with the secondary's shadow "
        "inside (ring 40–100 px across). Goal: shadow centred in the ring.<br><br>"
        "On an SCT: turn the secondary screw that moves the star <b>toward the thin side of the ring</b> (the arrow), "
        "a small amount, then <b>re-centre the star</b> with the mount and look again. Finish with smaller defocus and "
        "then at focus in steady seeing.");
    how->setWordWrap(true);
    col->addWidget(how);
    enable_ = new QCheckBox("Analyse the live view");
    col->addWidget(enable_);
    offset_ = new QLabel("—");
    QFont big = offset_->font();
    big.setPointSizeF(big.pointSizeF() * 2.0);
    big.setBold(true);
    offset_->setFont(big);
    offset_->setAlignment(Qt::AlignCenter);
    col->addWidget(offset_);
    detail_ = new QLabel;
    detail_->setWordWrap(true);
    col->addWidget(detail_);
    col->addWidget(new QLabel("Shadow offset, % of ring radius (dashed: 2 %)"));
    history_ = new OffsetHistory;
    col->addWidget(history_);
    col->addStretch(1);
    connect(enable_, &QCheckBox::toggled, this, [this](bool on) {
        if (!on) clearOverlay();
        history_->clear();
        recent_.clear();
        preview_->setCrosshair(on);
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
        detail_->setText(QString::fromStdString(d.problem));
        clearOverlay();
        return;
    }
    // Average the last few results: seeing makes single frames jump.
    recent_.push_back(d);
    while (recent_.size() > 8) recent_.pop_front();
    analysis::DonutResult a = d;
    a.offsetX = a.offsetY = a.offsetFraction = 0;
    for (const auto& r : recent_) a.offsetX += r.offsetX / recent_.size(), a.offsetY += r.offsetY / recent_.size();
    a.offsetFraction = std::hypot(a.offsetX, a.offsetY) / a.outerR;
    const double pct = 100 * a.offsetFraction;
    history_->add(pct);
    const QColor c = pct < 2 ? QColor(110, 210, 120) : pct < 5 ? QColor(240, 200, 80) : QColor(240, 100, 90);
    offset_->setStyleSheet(QString("color: %1").arg(c.name()));
    offset_->setText(QString("%1 %").arg(pct, 0, 'f', 1));
    const double ang = std::atan2(a.offsetY, a.offsetX) * 180 / std::numbers::pi;
    detail_->setText(QString("Thin side toward %1° (0° = right, 90° = down)  ·  ring Ø %2 px, shadow Ø %3 px  ·  roundness rms %4 / %5 px<br>"
                             "Star is %6, %7 px from the frame centre%8")
                         .arg(ang, 0, 'f', 0)
                         .arg(2 * a.outerR, 0, 'f', 0)
                         .arg(2 * a.innerR, 0, 'f', 0)
                         .arg(a.outerRms, 0, 'f', 1)
                         .arg(a.innerRms, 0, 'f', 1)
                         .arg(a.fromCentreX, 0, 'f', 0)
                         .arg(a.fromCentreY, 0, 'f', 0)
                         .arg(std::hypot(a.fromCentreX, a.fromCentreY) > 0.1 * std::min(fi.roi.width, fi.roi.height) ? " — re-centre it" : ""));
    preview_->setOverlay("collimation", [a, c](QPainter& p, double k) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(90, 160, 255, 220), 1.5 / k));
        p.drawEllipse(QPointF(a.outerX, a.outerY), a.outerR, a.outerR);
        p.setPen(QPen(QColor(255, 170, 70, 220), 1.5 / k));
        p.drawEllipse(QPointF(a.innerX, a.innerY), a.innerR, a.innerR);
        // arrow from the ring centre toward the thin side, exaggerated so small offsets are visible
        const double len = std::hypot(a.offsetX, a.offsetY);
        if (len > 1e-6) {
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

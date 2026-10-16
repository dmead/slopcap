#include "preview/histogram_widget.hpp"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>

namespace slopcap::preview {

HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent) { setFixedHeight(56); }

void HistogramWidget::setData(const std::array<uint32_t, 256>& histogram, const Levels& levels) {
    hist_ = histogram;
    levels_ = levels;
    update();
}

void HistogramWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(QColor(0x23, 0x27, 0x2d), 1));
    p.setBrush(QColor(0x15, 0x18, 0x1c));
    p.drawRoundedRect(frame, 6, 6);
    // 48 bars of ~5 bins each, log height: grey outside the levels, amber inside, red at the clipping end
    constexpr int kBars = 48;
    std::array<double, kBars> bars{};
    for (int b = 0; b < 256; ++b) bars[static_cast<std::size_t>(b * kBars / 256)] += hist_[static_cast<std::size_t>(b)];
    const double peak = *std::max_element(bars.begin(), bars.end());
    if (peak <= 0) return;
    const QRectF r = frame.adjusted(4, 4, -4, -4);
    const double gap = 2, w = (r.width() - gap * (kBars - 1)) / kBars;
    for (int i = 0; i < kBars; ++i) {
        const double v = bars[static_cast<std::size_t>(i)];
        if (v <= 0) continue;
        const double h = std::max(1.0, r.height() * std::log1p(v) / std::log1p(peak));
        const double lvl = (i + 0.5) / kBars;
        const QColor c = i >= kBars - 2 ? QColor(0xc9, 0x35, 0x3a)
                         : (lvl >= levels_.black && lvl <= levels_.white) ? QColor(0xf2, 0xa5, 0x41)
                                                                          : QColor(0x5d, 0x66, 0x70);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(r.left() + i * (w + gap), r.bottom() - h, w, h), 1, 1);
    }
}

}  // namespace slopcap::preview

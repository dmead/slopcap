#include "preview/histogram_widget.hpp"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace slopcap::preview {

HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent) { setMinimumHeight(60); }

void HistogramWidget::setData(const std::array<uint32_t, 256>& histogram, const Levels& levels) {
    hist_ = histogram;
    levels_ = levels;
    update();
}

void HistogramWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect().adjusted(1, 1, -1, -1);
    p.fillRect(r, QColor(14, 15, 18));
    const uint32_t peak = *std::max_element(hist_.begin(), hist_.end());
    if (peak > 0) {
        const double top = std::log1p(static_cast<double>(peak));
        QPainterPath path(QPointF(r.left(), r.bottom()));
        for (int b = 0; b < 256; ++b) {
            const double x = r.left() + r.width() * (b + 0.5) / 256.0;
            const double y = r.bottom() - r.height() * std::log1p(static_cast<double>(hist_[static_cast<std::size_t>(b)])) / top;
            path.lineTo(x, y);
        }
        path.lineTo(r.right(), r.bottom());
        path.closeSubpath();
        p.fillPath(path, QColor(150, 160, 175, 160));
    }
    // black / white points
    p.setPen(QPen(QColor(90, 160, 255), 1.5));
    const double bx = r.left() + r.width() * levels_.black, wx = r.left() + r.width() * levels_.white;
    p.drawLine(QPointF(bx, r.top()), QPointF(bx, r.bottom()));
    p.setPen(QPen(QColor(255, 190, 80), 1.5));
    p.drawLine(QPointF(wx, r.top()), QPointF(wx, r.bottom()));
}

}  // namespace slopcap::preview

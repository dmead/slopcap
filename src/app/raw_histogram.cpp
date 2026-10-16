#include "app/raw_histogram.hpp"

#include "app/theme.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace slopcap::app {

namespace {

constexpr int kPlotH = 110, kAxisH = 16, kRowH = 18;

QString thousands(long v) { return QLocale(QLocale::English).toString(static_cast<qlonglong>(v)); }

}  // namespace

// ---------------------------------------------------------------- RawHistogramPlot

RawHistogramPlot::RawHistogramPlot(QWidget* parent) : QWidget(parent) { setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); }

QSize RawHistogramPlot::sizeHint() const { return {240, kPlotH + kAxisH + 6 + std::max(1, h_.channels) * kRowH}; }

void RawHistogramPlot::setData(const preview::RawHistogram& h) {
    const bool resize = h.channels != h_.channels;
    h_ = h;
    if (resize) updateGeometry();
    update();
}

void RawHistogramPlot::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(0, 0, width(), kPlotH).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(theme::kLine, 1));
    p.setBrush(theme::kControl);
    p.drawRoundedRect(box, 6, 6);
    const QFont mono = theme::monoFont();
    QFont small = mono;
    small.setPixelSize(10);

    if (h_.channels == 0) {
        p.setPen(theme::kDim);
        p.drawText(box, Qt::AlignCenter, "No frames");
        return;
    }

    // Log heights against the tallest bin of any channel, so the channels compare directly.
    const QRectF r = box.adjusted(6, 6, -6, -4);
    double peak = 0;
    for (int c = 0; c < h_.channels; ++c)
        for (auto v : h_.bins[static_cast<std::size_t>(c)]) peak = std::max(peak, static_cast<double>(v));
    // quarter-range gridlines
    p.setPen(QPen(theme::kLine, 1, Qt::DotLine));
    for (int q = 1; q < 4; ++q) p.drawLine(QPointF(r.left() + r.width() * q / 4, r.top()), QPointF(r.left() + r.width() * q / 4, r.bottom()));

    // Draw blue, red, then green on top (green has twice the samples and usually the most detail).
    const int order[3] = {2, 0, 1};
    for (int k = 0; k < 3; ++k) {
        const int c = h_.channels == 1 ? 0 : order[k];
        if (h_.channels == 1 && k > 0) break;
        const auto& bins = h_.bins[static_cast<std::size_t>(c)];
        if (bins.empty() || peak <= 0) continue;
        const QColor col = h_.channels == 1 ? theme::kText : theme::kChannel[c];
        const double n = static_cast<double>(bins.size());
        QPainterPath line;
        for (std::size_t i = 0; i < bins.size(); ++i) {
            const double v = bins[i] ? std::log1p(static_cast<double>(bins[i])) / std::log1p(peak) : 0.0;
            const QPointF pt(r.left() + r.width() * (static_cast<double>(i) + 0.5) / n, r.bottom() - r.height() * v);
            if (i == 0) line.moveTo(pt);
            else line.lineTo(pt);
        }
        QPainterPath area = line;
        area.lineTo(r.right() - r.width() * 0.5 / n, r.bottom());
        area.lineTo(r.left() + r.width() * 0.5 / n, r.bottom());
        area.closeSubpath();
        QColor fill = col;
        fill.setAlpha(40);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(area);
        p.setPen(QPen(col, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);
    }
    // the clipping end
    bool clipping = false;
    for (int c = 0; c < h_.channels; ++c) clipping |= h_.clipped[static_cast<std::size_t>(c)] > 0;
    if (clipping) {
        p.setPen(Qt::NoPen);
        p.setBrush(theme::kRec);
        p.drawRect(QRectF(r.right() - 2, r.top(), 3, r.height()));
    }

    // ADU axis: 0, half, full scale
    p.setFont(small);
    p.setPen(theme::kDim);
    const QRectF axis(r.left(), kPlotH + 2, r.width(), kAxisH - 2);
    p.drawText(axis, Qt::AlignLeft | Qt::AlignVCenter, "0");
    p.drawText(axis, Qt::AlignHCenter | Qt::AlignVCenter, thousands((h_.fullScale + 1) / 2));
    p.drawText(axis, Qt::AlignRight | Qt::AlignVCenter, thousands(h_.fullScale));

    // one row per channel: marker, median, max, clipped
    QFont rowFont = mono;
    rowFont.setPixelSize(11);
    p.setFont(rowFont);
    double y = kPlotH + kAxisH + 6;
    for (int c = 0; c < h_.channels; ++c, y += kRowH) {
        const auto ci = static_cast<std::size_t>(c);
        const double cy = y + kRowH / 2.0;
        if (h_.channels == 1) {
            p.setPen(Qt::NoPen);
            p.setBrush(theme::kText);
            p.drawEllipse(QPointF(5, cy), 4, 4);
        } else {
            theme::drawChannelMarker(p, c, 5, cy, 9);
        }
        const double w = width() - 16;
        const QRectF cell(16, y, w / 3, kRowH);
        p.setPen(theme::kMuted);
        p.drawText(cell, Qt::AlignLeft | Qt::AlignVCenter, "med " + thousands(h_.median[ci]));
        p.drawText(cell.translated(w / 3, 0), Qt::AlignLeft | Qt::AlignVCenter, "max " + thousands(h_.max[ci]));
        const double clip = h_.clipped[ci] * 100.0;
        p.setPen(clip > 0.1 ? theme::kRecText : theme::kMuted);
        p.drawText(cell.translated(2 * w / 3, 0), Qt::AlignRight | Qt::AlignVCenter, QString("clip %1%").arg(clip, 0, 'f', clip >= 10 ? 0 : 2));
    }
}

// ---------------------------------------------------------------- RawHistogramSection

RawHistogramSection::RawHistogramSection(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    auto* head = new QHBoxLayout;
    head->addWidget(theme::section("Raw histogram"));
    head->addStretch(1);
    units_ = theme::label("", "monoDim");
    head->addWidget(units_);
    v->addLayout(head);
    plot_ = new RawHistogramPlot;
    v->addWidget(plot_);
}

void RawHistogramSection::setData(const preview::RawHistogram& h) {
    units_->setText(h.channels ? QString("%1-bit · ADU").arg(h.adcBits) : "");
    plot_->setData(h);
}

}  // namespace slopcap::app

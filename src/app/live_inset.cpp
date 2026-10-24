#include "app/live_inset.hpp"

#include "app/theme.hpp"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace slopcap::app {

LiveInset::LiveInset(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 107);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void LiveInset::setView(View v) {
    view_ = v;
    crop_ = {};
    clock_.invalidate();   // the next frame redraws at once
}

void LiveInset::clear() {
    image_ = {};
    crop_ = {};
    window_ = {};
    update();
}

void LiveInset::showFrame(const LatestFrame::Frame& f, Bayer bayer, const preview::Levels& levels, const analysis::FocusScores& scores) {
    if (!isVisible() || (clock_.isValid() && clock_.elapsed() < 100)) return;
    clock_.start();
    const auto& fi = f.info;
    const int fw = fi.roi.width, fh = fi.roi.height;
    if (fw < 2 || fh < 2) return;

    QRectF want(0, 0, fw, fh);
    if (view_ == View::Target && scores.channels > 0 && scores.targetX1 > scores.targetX0 && scores.targetY1 > scores.targetY0) {
        // the target with room around it, in this widget's 3:2, no smaller than 96 px across
        const QRectF box(QPointF(scores.targetX0, scores.targetY0), QPointF(scores.targetX1, scores.targetY1));
        const double w = std::min<double>(fw, std::max({box.width() * 1.8, box.height() * 1.8 * 1.5, 96.0}));
        const double h = std::min<double>(fh, w / 1.5);
        QRectF r(box.center().x() - w / 2, box.center().y() - h / 2, w, h);
        r.moveLeft(std::clamp(r.left(), 0.0, fw - r.width()));
        r.moveTop(std::clamp(r.top(), 0.0, fh - r.height()));
        // seeing makes the target box jitter: ease towards it, unless it went somewhere else entirely
        if (crop_.isValid() && crop_.contains(r.center()))
            r = QRectF(crop_.left() * 0.75 + r.left() * 0.25, crop_.top() * 0.75 + r.top() * 0.25, crop_.width() * 0.75 + r.width() * 0.25,
                       crop_.height() * 0.75 + r.height() * 0.25);
        crop_ = want = r;
    } else {
        crop_ = {};
    }

    const QRect r = want.toAlignedRect() & QRect(0, 0, fw, fh);
    const double target = std::max(64.0, std::max(width(), height()) * devicePixelRatioF());
    const int step = std::max(1, static_cast<int>(std::floor(r.width() / target)));
    int ow = 0, oh = 0;
    const auto px = std::span(f.pixels).first(fi.bytes);
    const auto cut = preview::sampleMosaic(px, fw, fh, fi.bitDepth, r.x(), r.y(), r.width(), r.height(), step, ow, oh);
    if (ow == 0 || oh == 0) return;
    const auto rgb = preview::renderRgb(cut, ow, oh, fi.bitDepth, bayer, levels);
    QImage img(ow, oh, QImage::Format_RGB888);
    for (int y = 0; y < oh; ++y) std::memcpy(img.scanLine(y), rgb.data() + static_cast<std::size_t>(y) * ow * 3, static_cast<std::size_t>(ow) * 3);
    image_ = std::move(img);
    // the scored window, where the cut started (an even corner) and as thinned
    const double x0 = r.x() & ~1, y0 = r.y() & ~1;
    window_ = view_ == View::Frame && scores.windowW > 0
                  ? QRectF((scores.windowX - x0) / step, (scores.windowY - y0) / step, scores.windowW / static_cast<double>(step),
                           scores.windowH / static_cast<double>(step))
                  : QRectF();
    update();
}

void LiveInset::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF all = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(theme::kLine, 1));
    p.setBrush(Qt::black);
    p.drawRoundedRect(all, 6, 6);
    if (image_.isNull()) {
        p.setPen(theme::kDim);
        p.drawText(rect(), Qt::AlignCenter, "No camera picture");
        return;
    }
    const QSizeF s = QSizeF(image_.size()).scaled(QSizeF(size()) - QSizeF(2, 2), Qt::KeepAspectRatio);
    const QRectF dst((width() - s.width()) / 2, (height() - s.height()) / 2, s.width(), s.height());
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(dst, image_);
    if (!window_.isEmpty()) {
        const double k = dst.width() / image_.width();
        p.setPen(QPen(theme::kAccent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(dst.left() + window_.left() * k, dst.top() + window_.top() * k, window_.width() * k, window_.height() * k).intersected(dst));
    }
}

void LiveInset::mouseDoubleClickEvent(QMouseEvent*) { emit activated(); }

}  // namespace slopcap::app

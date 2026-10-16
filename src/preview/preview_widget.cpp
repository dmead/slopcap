#include "preview/preview_widget.hpp"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace slopcap::preview {

/// Transparent child covering the preview; paints the overlays with the raster engine (QPainter straight onto a
/// core-profile GL widget doesn't reliably show).
class OverlayLayer : public QWidget {
public:
    explicit OverlayLayer(PreviewWidget* owner) : QWidget(owner), owner_(owner) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        owner_->paintOverlays(p);
    }

private:
    PreviewWidget* owner_;
};

PreviewWidget::PreviewWidget(QWidget* parent) : QOpenGLWidget(parent) {
    overlayLayer_ = new OverlayLayer(this);
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &PreviewWidget::poll);
    fpsClock_.start();
}

PreviewWidget::~PreviewWidget() {
    if (glReady_) {
        makeCurrent();
        renderer_.destroy();
        doneCurrent();
    }
}

void PreviewWidget::setSource(LatestFrame* source, Bayer bayer) {
    source_ = source;
    bayer_ = bayer;
    if (source_) {
        source_->request();
        timer_.start(8);   // poll faster than the display so a frame waits at most ~8 ms
    } else {
        timer_.stop();
        fps_ = 0;
    }
}

void PreviewWidget::setAutoStretch(bool on) { auto_ = on; }

void PreviewWidget::setAutoWhiteBalance(bool on) {
    autoWb_ = on;
    if (!on) levels_.wb = {1, 1, 1};
}

void PreviewWidget::setLevels(const Levels& lv) {
    auto_ = false;
    const auto wb = levels_.wb;
    levels_ = lv;
    levels_.wb = wb;
    if (glReady_ && !timer_.isActive()) {   // paused: re-stretch the frame on screen
        makeCurrent();
        renderer_.reprocess(levels_);
        doneCurrent();
        update();
    }
}

void PreviewWidget::setOverlay(const QString& key, Overlay overlay) {
    if (overlay) overlays_[key] = std::move(overlay);
    else overlays_.erase(key);
    update();
}

void PreviewWidget::setCrosshair(bool on) {
    crosshair_ = on;
    update();
}

void PreviewWidget::fitToWindow() {
    zoom_ = 1;
    centre_ = {-1, -1};
    update();
    emit zoomChanged();
}

void PreviewWidget::zoomOneToOne() { zoomTo(1.0); }

void PreviewWidget::zoomTo(double k) {
    const double fs = fitScale();
    zoom_ = fs > 0 ? k / fs : 1;
    update();
    emit zoomChanged();
}

void PreviewWidget::setDebayer(bool on) { debayer_ = on; }

void PreviewWidget::initializeGL() {
    try {
        renderer_.initialize();
        glReady_ = true;
    } catch (const std::exception& e) {
        // e.g. Remote Desktop or an old driver without OpenGL 3.3: capture still works, the preview can't
        glError_ = QString("The preview needs OpenGL 3.3, which isn't available here (%1). Capture and recording still work.")
                       .arg(QString::fromLocal8Bit(e.what()).left(300));
    }
}

void PreviewWidget::poll() {
    // Without a GL context frames still flow (histogram, focus, collimation); only the picture is missing.
    if (!source_ || (!glReady_ && glError_.isEmpty())) return;
    // take() swaps buffers, so the frame is only valid until the next take(): use it right here.
    const LatestFrame::Frame* f = source_->take();
    if (!f) return;
    const auto& fi = f->info;
    const auto px = static_cast<std::size_t>(fi.roi.width) * static_cast<std::size_t>(fi.roi.height);
    const int stride = static_cast<int>(std::max<std::size_t>(1, px / 500'000));
    const auto hist = histogram(std::span(f->pixels).first(fi.bytes), fi.bitDepth, stride);
    if (auto_) {
        const auto wb = levels_.wb;
        levels_ = autoLevels(hist);
        levels_.wb = wb;
    }
    if (autoWb_ && bayer_ != Bayer::None) {
        // smoothed so seeing and noise don't make the colour flicker
        const auto g = autoWhiteBalance(std::span(f->pixels).first(fi.bytes), fi.roi.width, fi.roi.height, fi.bitDepth, bayer_);
        for (std::size_t c = 0; c < 3; ++c) levels_.wb[c] = levels_.wb[c] == 1.0 && c != 1 ? g[c] : 0.85 * levels_.wb[c] + 0.15 * g[c];
    }
    if (glReady_) {
        const QSize before = imageSize();
        makeCurrent();
        auto lv = levels_;
        if (!debayer_) lv.wb = {1, 1, 1};
        renderer_.process(f->pixels.data(), fi.roi.width, fi.roi.height, fi.bitDepth, debayer_ ? bayer_ : Bayer::None, lv);
        doneCurrent();
        if (imageSize() != before) centre_ = {-1, -1};   // new ROI: recentre
    }
    update();

    ++fpsFrames_;
    if (fpsClock_.elapsed() >= 1000) {
        fps_ = fpsFrames_ * 1000.0 / static_cast<double>(fpsClock_.restart());
        fpsFrames_ = 0;
    }
    emit rawFrame(*f);
    emit frameShown(hist, levels_, fi);
}

double PreviewWidget::fitScale() const {
    const QSize s = imageSize();
    if (s.isEmpty()) return 1;
    return std::min(static_cast<double>(width()) / s.width(), static_cast<double>(height()) / s.height());
}

QTransform PreviewWidget::imageToWidget() const {
    const QSize s = imageSize();
    const QPointF c = centre_.x() < 0 ? QPointF(s.width() / 2.0, s.height() / 2.0) : centre_;
    const double k = scale();
    return QTransform(k, 0, 0, k, width() / 2.0 - c.x() * k, height() / 2.0 - c.y() * k);
}

void PreviewWidget::paintGL() {
    if (!glReady_) {
        overlayLayer_->update();
        return;
    }
    const qreal dpr = devicePixelRatioF();
    const QTransform t = imageToWidget();
    const QSize s = imageSize();
    const QRectF r = t.mapRect(QRectF(0, 0, s.width(), s.height()));
    renderer_.draw(defaultFramebufferObject(), static_cast<int>(width() * dpr), static_cast<int>(height() * dpr), r.x() * dpr,
                   r.y() * dpr, r.width() * dpr, r.height() * dpr);
    overlayLayer_->update();
}

void PreviewWidget::resizeEvent(QResizeEvent* e) {
    QOpenGLWidget::resizeEvent(e);
    overlayLayer_->setGeometry(rect());
}

void PreviewWidget::paintOverlays(QPainter& p) const {
    if (!glError_.isEmpty()) {
        p.fillRect(rect(), QColor(13, 15, 18));
        p.setPen(QColor(255, 138, 141));
        p.drawText(rect().adjusted(40, 40, -40, -40), Qt::AlignCenter | Qt::TextWordWrap, glError_);
        return;
    }
    if (!renderer_.hasImage() || (overlays_.empty() && !crosshair_)) return;
    const QTransform t = imageToWidget();
    const QSize s = imageSize();
    p.setRenderHint(QPainter::Antialiasing);
    if (crosshair_) {
        p.setPen(QPen(QColor(255, 80, 80, 170), 1));
        const QPointF c = t.map(QPointF(s.width() / 2.0, s.height() / 2.0));
        p.drawLine(QPointF(c.x(), 0), QPointF(c.x(), height()));
        p.drawLine(QPointF(0, c.y()), QPointF(width(), c.y()));
        p.drawEllipse(c, 30, 30);
    }
    p.setTransform(t);
    for (const auto& [_, o] : overlays_) {
        p.save();
        o(p, scale());
        p.restore();
    }
}

void PreviewWidget::wheelEvent(QWheelEvent* e) {
    if (!renderer_.hasImage()) return;
    const QPointF at = e->position();
    const QPointF img = imageToWidget().inverted().map(at);   // keep this image point under the cursor
    const double factor = std::pow(1.0015, e->angleDelta().y());
    zoom_ = std::clamp(zoom_ * factor, 0.5, 64.0);
    const double k = scale();
    centre_ = QPointF(img.x() - (at.x() - width() / 2.0) / k, img.y() - (at.y() - height() / 2.0) / k);
    update();
    emit zoomChanged();
}

void PreviewWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    dragging_ = true;
    dragStart_ = e->position();
    const QSize s = imageSize();
    dragCentre_ = centre_.x() < 0 ? QPointF(s.width() / 2.0, s.height() / 2.0) : centre_;
    setCursor(Qt::ClosedHandCursor);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return;
    centre_ = dragCentre_ - (e->position() - dragStart_) / scale();
    update();
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent*) {
    dragging_ = false;
    unsetCursor();
}

void PreviewWidget::mouseDoubleClickEvent(QMouseEvent*) { fitToWindow(); }

void PreviewWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_F) fitToWindow();
    else if (e->key() == Qt::Key_1) zoomOneToOne();
    else if (e->key() == Qt::Key_C) setCrosshair(!crosshair_);
    else QOpenGLWidget::keyPressEvent(e);
}

void PreviewWidget::contextMenuEvent(QContextMenuEvent* e) {
    if (!renderer_.hasImage()) return;
    const QPointF img = imageToWidget().inverted().map(QPointF(e->pos()));
    QMenu m(this);
    m.addAction("Centre ROI here", this, [this, img] { emit centreRequested(img); });
    m.addSeparator();
    m.addAction("Fit (F)", this, &PreviewWidget::fitToWindow);
    m.addAction("1:1 (1)", this, &PreviewWidget::zoomOneToOne);
    auto* ch = m.addAction("Crosshair (C)", this, [this] { setCrosshair(!crosshair_); });
    ch->setCheckable(true);
    ch->setChecked(crosshair_);
    m.exec(e->globalPos());
}

}  // namespace slopcap::preview

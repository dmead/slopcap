#include "preview/preview_widget.hpp"

#include <QContextMenuEvent>
#include <QDebug>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPainter>
#include <QScreen>
#include <QSurfaceFormat>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace slopcap::preview {

namespace {

QString needsGl(const QString& why) {
    return QString("The preview needs OpenGL 3.3, which isn't available here (%1). Capture and recording still work.").arg(why.left(300));
}

/// Can this machine run the GPU view? Asked once, on a throwaway context, before any QOpenGLWidget exists. Empty when
/// it can, else what it has instead ("OpenGL 1.1, GDI Generic").
QString glMissing() {
    static const QString why = [] {
        const QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
        QOffscreenSurface surface;
        surface.setFormat(fmt);
        surface.create();
        QOpenGLContext ctx;
        ctx.setFormat(fmt);
        if (!ctx.create() || !ctx.makeCurrent(&surface)) return QString("no OpenGL context");
        const QSurfaceFormat f = ctx.format();
        QString r;
        if (ctx.isOpenGLES() || f.majorVersion() * 10 + f.minorVersion() < 33) {
            const auto* name = reinterpret_cast<const char*>(ctx.functions()->glGetString(GL_RENDERER));
            r = QString("%1 %2.%3%4").arg(ctx.isOpenGLES() ? "OpenGL ES" : "OpenGL").arg(f.majorVersion()).arg(f.minorVersion())
                    .arg(name ? QString(", ") + name : QString());
        }
        ctx.doneCurrent();
        return r;
    }();
    return why;
}

}  // namespace

QSurfaceFormat surfaceFormat() {
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
#ifdef Q_OS_WIN
    // No vsync: Intel's Windows OpenGL driver busy-waits for each vertical blank, which kept a whole core busy at any
    // frame size (2026-10-29, UHD 630: 1.03 cores at 60 paints/s with vsync, 0.11 without). Windows composites every
    // window, so it doesn't tear; the preview paces its own painting (PreviewWidget::poll).
    int swap = 0;
#else
    int swap = 1;
#endif
    if (qEnvironmentVariableIsSet("SLOPCAP_SWAP_INTERVAL")) swap = qEnvironmentVariableIntValue("SLOPCAP_SWAP_INTERVAL");
    fmt.setSwapInterval(swap);
    return fmt;
}

/// The GPU view: a QOpenGLWidget covering the preview, there only when OpenGL 3.3 is. Input goes through to the
/// PreviewWidget (transparent for mouse events); the GL hooks call back into it.
class GlView : public QOpenGLWidget {
public:
    explicit GlView(PreviewWidget* owner) : QOpenGLWidget(owner), owner_(owner) { setAttribute(Qt::WA_TransparentForMouseEvents); }

protected:
    void initializeGL() override { owner_->initializeGL(); }
    void paintGL() override { owner_->paintGL(); }

private:
    PreviewWidget* owner_;
};

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

PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent) {
    if (const QString why = glMissing(); why.isEmpty()) {
        gl_ = new GlView(this);
    } else {
        glError_ = needsGl(why);
        qWarning().noquote() << "preview:" << glError_;
    }
    overlayLayer_ = new OverlayLayer(this);   // after the GPU view, so it stacks on top
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &PreviewWidget::poll);
    fpsClock_.start();
}

PreviewWidget::~PreviewWidget() {
    disconnect(glLost_);   // the GPU view's context goes after this body, when renderer_ already has
    if (glReady_) {
        gl_->makeCurrent();
        renderer_.destroy();
        gl_->doneCurrent();
    }
}

void PreviewWidget::makeCurrent() {
    ++ownContextSwitches_;
    gl_->makeCurrent();
}

void PreviewWidget::refresh() {
    if (gl_) gl_->update();   // paintGL also repaints the overlays
    else overlayLayer_->update();
}

void PreviewWidget::setSource(LatestFrame* source, Bayer bayer) {
    source_ = source;
    bayer_ = bayer;
    shownClock_.invalidate();   // show the new source's first frame at once, balanced at once
    wbClock_.invalidate();
    peakWhite_ = 0;
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
        gl_->doneCurrent();
        refresh();
    }
}

void PreviewWidget::setOverlay(const QString& key, Overlay overlay) {
    if (!overlay && !overlays_.contains(key)) return;   // nothing to remove (the target box is cleared every frame without a target)
    if (overlay) overlays_[key] = std::move(overlay);
    else overlays_.erase(key);
    overlayDirty_ = true;
    if (!timer_.isActive()) updateOverlays();   // not previewing: no poll to do it
}

void PreviewWidget::setCrosshair(bool on) {
    crosshair_ = on;
    overlayDirty_ = true;
    if (!timer_.isActive()) updateOverlays();
}

void PreviewWidget::updateOverlays() {
    // Only the overlay layer: the picture under it hasn't changed. At most once per shown frame while previewing, as
    // the target box moves with every frame.
    if (timer_.isActive() && overlayClock_.isValid() && static_cast<double>(overlayClock_.elapsed()) < frameMs() - 2.0) return;
    overlayClock_.start();
    overlayDirty_ = false;
    overlayLayer_->update();
}

double PreviewWidget::refreshMs() const {
    const double hz = screen() && screen()->refreshRate() > 1 ? screen()->refreshRate() : 60.0;
    return 1000.0 / hz;
}

double PreviewWidget::frameMs() const { return displayLimit_ > 0 ? std::max(refreshMs(), 1000.0 / displayLimit_) : refreshMs(); }

void PreviewWidget::fitToWindow() {
    zoom_ = 1;
    centre_ = {-1, -1};
    refresh();
    emit zoomChanged();
}

void PreviewWidget::zoomOneToOne() { zoomTo(1.0); }

void PreviewWidget::zoomTo(double k) {
    const double fs = fitScale();
    zoom_ = fs > 0 ? k / fs : 1;
    refresh();
    emit zoomChanged();
}

void PreviewWidget::setDebayer(bool on) { debayer_ = on; }

void PreviewWidget::initializeGL() {
    try {
        renderer_.initialize();
        glReady_ = true;
        // Qt swaps in a new context when the view lands in a different window: started inside a grab while hidden (the
        // window opened on the Focus tab), then shown. Let go of this one's objects; the next initializeGL starts again.
        disconnect(glLost_);
        glLost_ = connect(gl_->context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
            if (!glReady_) return;
            makeCurrent();
            renderer_.destroy();
            gl_->doneCurrent();
            glReady_ = false;
            pending_ = false;
        });
    } catch (const std::exception& e) {
        // The probe said 3.3, but the renderer can't start (shaders, framebuffers): capture still works
        glError_ = needsGl(QString::fromLocal8Bit(e.what()));
        qWarning().noquote() << "preview:" << glError_;
    }
}

void PreviewWidget::poll() {
    // Without a GL context frames still flow (histogram, focus, collimation); only the picture is missing. A GPU view
    // that is on screen but not started yet gets a moment; a hidden one (the window opened on the Focus tab) may not
    // start for a long time, and focus sweeps can't wait for it.
    if (overlayDirty_) updateOverlays();
    if (!source_ || (!glReady_ && glError_.isEmpty() && isVisible())) return;
    // take() swaps buffers, so the frame is only valid until the next take(): use it right here.
    const LatestFrame::Frame* f = source_->take();
    if (!f) return;
    // Every frame goes to the analyses (focus, collimation, ...); the display work only to one per shown frame (the
    // display limit, at most the screen's rate), and not while the last one still waits for its paint (unless no paint
    // has come for 100 ms: minimised). Done for every frame, up to 125 a second, it kept SlopCap's window thread busy
    // (2026-10-29).
    const bool waiting = pending_ && shownClock_.elapsed() < 100;
    if (!waiting && (!shownClock_.isValid() || static_cast<double>(shownClock_.elapsed()) >= frameMs() - 2.0)) present(*f);
    emit rawFrame(*f);
}

void PreviewWidget::present(const LatestFrame::Frame& f) {
    shownClock_.start();
    const auto& fi = f.info;
    const auto pixels = std::span(f.pixels).first(fi.bytes);
    const auto px = static_cast<std::size_t>(fi.roi.width) * static_cast<std::size_t>(fi.roi.height);
    const auto hist = histogram(pixels, fi.bitDepth, histogramStride(px));
    if (auto_) {
        const auto wb = levels_.wb;
        levels_ = autoLevels(hist);
        levels_.wb = wb;
    }
    if (autoWb_ && bayer_ != Bayer::None) {
        // ten times a second: smoothed anyway, so seeing and noise don't make the colour flicker
        if (!wbClock_.isValid() || wbClock_.elapsed() >= 100) {
            wbClock_.start();
            const auto g = autoWhiteBalance(pixels, fi.roi.width, fi.roi.height, fi.bitDepth, bayer_);
            for (std::size_t c = 0; c < 3; ++c) levels_.wb[c] = levels_.wb[c] == 1.0 && c != 1 ? g[c] : 0.85 * levels_.wb[c] + 0.15 * g[c];
            // the gains apply before the levels: keep the white point above the balanced planet, so the gains don't paint
            // its bright zones white (the raw histogram that set it doesn't know about them)
            if (auto_) peakWhite_ = balancedPeak(pixels, fi.roi.width, fi.roi.height, fi.bitDepth, bayer_, levels_.wb) * 1.02;
        }
        if (auto_) levels_.white = std::clamp(peakWhite_, levels_.white, 1.0);
    }
    if (glReady_) {
        // Uploaded in paintGL, where Qt makes the context current anyway: making it current here as well, per frame, is
        // costly on Intel's Windows driver.
        shownPixels_.assign(pixels.begin(), pixels.end());
        shownInfo_ = fi;
        shownLevels_ = levels_;
        pending_ = true;
        gl_->update();
    } else {
        refresh();
    }
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
    if (pending_) {
        const auto& fi = shownInfo_;
        const QSize before = imageSize();
        auto lv = shownLevels_;
        if (!debayer_) lv.wb = {1, 1, 1};
        renderer_.process(shownPixels_.data(), fi.roi.width, fi.roi.height, fi.bitDepth, debayer_ ? bayer_ : Bayer::None, lv);
        pending_ = false;
        if (imageSize() != before) centre_ = {-1, -1};   // new ROI: recentre
        ++fpsFrames_;
        if (fpsClock_.elapsed() >= 1000) {
            fps_ = fpsFrames_ * 1000.0 / static_cast<double>(fpsClock_.restart());
            fpsFrames_ = 0;
        }
    }
    const qreal dpr = devicePixelRatioF();
    const QTransform t = imageToWidget();
    const QSize s = imageSize();
    const QRectF r = t.mapRect(QRectF(0, 0, s.width(), s.height()));
    renderer_.draw(gl_->defaultFramebufferObject(), static_cast<int>(width() * dpr), static_cast<int>(height() * dpr), r.x() * dpr,
                   r.y() * dpr, r.width() * dpr, r.height() * dpr);
    // The overlays sit in image coordinates: repaint them when zoom, pan, size or the image changed, not with every
    // frame (Qt then copies the whole overlay layer into a texture, every paint).
    if (t != overlayTransform_) {
        overlayTransform_ = t;
        overlayLayer_->update();
    }
}

void PreviewWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (gl_) gl_->setGeometry(rect());
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
    refresh();
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
    refresh();
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
    else QWidget::keyPressEvent(e);
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

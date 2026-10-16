#include "preview/preview_widget.hpp"

namespace slopcap::preview {

PreviewWidget::PreviewWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(320, 240);
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

void PreviewWidget::setLevels(const Levels& lv) {
    auto_ = false;
    levels_ = lv;
    if (glReady_ && !timer_.isActive()) {   // paused: re-stretch the frame on screen
        makeCurrent();
        renderer_.reprocess(levels_);
        doneCurrent();
        update();
    }
}

void PreviewWidget::initializeGL() {
    renderer_.initialize();
    glReady_ = true;
}

void PreviewWidget::poll() {
    if (!source_ || !glReady_) return;
    // take() swaps buffers, so the frame is only valid until the next take(): upload it right here.
    const LatestFrame::Frame* f = source_->take();
    if (!f) return;
    const auto& fi = f->info;
    const int stride = std::max<std::size_t>(1, static_cast<std::size_t>(fi.roi.width) * static_cast<std::size_t>(fi.roi.height) / 500'000);
    const auto hist = histogram(std::span(f->pixels).first(fi.bytes), fi.bitDepth, static_cast<int>(stride));
    if (auto_) levels_ = autoLevels(hist);
    makeCurrent();
    renderer_.process(f->pixels.data(), fi.roi.width, fi.roi.height, fi.bitDepth, bayer_, levels_);
    doneCurrent();
    update();

    ++fpsFrames_;
    if (fpsClock_.elapsed() >= 1000) {
        fps_ = fpsFrames_ * 1000.0 / static_cast<double>(fpsClock_.restart());
        fpsFrames_ = 0;
    }
    emit frameShown(hist, levels_, fi);
}

void PreviewWidget::paintGL() {
    const qreal dpr = devicePixelRatioF();
    renderer_.draw(defaultFramebufferObject(), static_cast<int>(width() * dpr), static_cast<int>(height() * dpr));
}

}  // namespace slopcap::preview

// The live view: polls a LatestFrame at display rate, uploads each new frame to the GPU (GlRenderer) and keeps the
// histogram and auto-stretch levels up to date. Zoom with the wheel (around the cursor), pan by dragging, double-click
// or F to fit, 1 for 1:1. Overlays (crosshair, focus window, collimation circles, ...) are painted on top in image
// coordinates. Owns no camera; whoever runs the CaptureSession hands it the tap.
#pragma once

#include "preview/gl_renderer.hpp"
#include "preview/latest_frame.hpp"
#include "preview/render_cpu.hpp"

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QPointF>
#include <QTimer>
#include <QTransform>

#include <array>
#include <functional>
#include <map>

class QPainter;

namespace slopcap::preview {

class PreviewWidget : public QOpenGLWidget {
    Q_OBJECT
    friend class OverlayLayer;
public:
    /// Paints in image pixel coordinates (the painter's transform maps them to the widget); `scale` is widget px per
    /// image px, for line widths and text that should stay a constant size.
    using Overlay = std::function<void(QPainter& painter, double scale)>;

    explicit PreviewWidget(QWidget* parent = nullptr);
    ~PreviewWidget() override;

    void setSource(LatestFrame* source, Bayer bayer);   ///< nullptr = stop polling (last image stays)
    void setAutoStretch(bool on);
    void setLevels(const Levels& lv);                   ///< manual levels (turns auto-stretch off; keeps white balance)
    void setAutoWhiteBalance(bool on);                  ///< display-only gray-world balance on the target
    const Levels& levels() const { return levels_; }
    bool autoStretch() const { return auto_; }
    double displayFps() const { return fps_; }

    void setOverlay(const QString& key, Overlay overlay);   ///< replaces; an empty function removes it
    void setCrosshair(bool on);
    void fitToWindow();
    void zoomOneToOne();
    QSize imageSize() const { return {renderer_.imageWidth(), renderer_.imageHeight()}; }

signals:
    /// A new frame was shown: its histogram, the levels used, and its metadata.
    void frameShown(const std::array<uint32_t, 256>& histogram, const slopcap::preview::Levels& levels,
                    const slopcap::FrameInfo& info);
    /// The raw frame just shown. Only valid during the call (connect directly; copy what you keep).
    void rawFrame(const slopcap::LatestFrame::Frame& frame);
    /// User asked to centre the ROI on this image pixel (right-click menu).
    void centreRequested(QPointF imagePixel);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeEvent(QResizeEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    void poll();
    double fitScale() const;          ///< widget px per image px at "fit"
    double scale() const { return fitScale() * zoom_; }
    QTransform imageToWidget() const;
    void paintOverlays(QPainter& p) const;
    QWidget* overlayLayer_ = nullptr;   ///< transparent child on top: overlays are raster-painted there

    GlRenderer renderer_;
    LatestFrame* source_ = nullptr;
    Bayer bayer_ = Bayer::None;
    Levels levels_;
    bool auto_ = true;
    bool autoWb_ = true;
    bool glReady_ = false;
    QTimer timer_;
    QElapsedTimer fpsClock_;
    int fpsFrames_ = 0;
    double fps_ = 0;

    double zoom_ = 1;                 ///< 1 = fit
    QPointF centre_{-1, -1};          ///< image point at the widget centre (-1 = image centre)
    bool dragging_ = false;
    QPointF dragStart_, dragCentre_;
    bool crosshair_ = false;
    std::map<QString, Overlay> overlays_;
};

}  // namespace slopcap::preview

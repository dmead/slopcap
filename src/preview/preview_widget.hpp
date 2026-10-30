// The live view: polls a LatestFrame faster than the display, hands every frame it takes to the analyses (rawFrame),
// and prepares one per shown frame (histogram, auto-stretch levels; 30 a second by default) for the GPU (GlRenderer)
// to upload when it paints. Zoom with the wheel (around the cursor), pan by dragging, double-click
// or F to fit, 1 for 1:1. Overlays (crosshair, focus window, collimation circles, ...) are painted on top in image
// coordinates. Owns no camera; whoever runs the CaptureSession hands it the tap.
#pragma once

#include "preview/gl_renderer.hpp"
#include "preview/latest_frame.hpp"
#include "preview/render_cpu.hpp"

#include <QElapsedTimer>
#include <QPointF>
#include <QSurfaceFormat>
#include <QTimer>
#include <QTransform>
#include <QWidget>

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <vector>

class QOpenGLWidget;
class QPainter;

namespace slopcap::preview {

/// The OpenGL format for the whole program: set it as the default before the QApplication exists. Vsync off on
/// Windows (Intel's driver busy-waits for it), on elsewhere; SLOPCAP_SWAP_INTERVAL=N overrides.
QSurfaceFormat surfaceFormat();

/// A plain widget holding the GPU view (a QOpenGLWidget child) only when the machine has OpenGL 3.3: on an older
/// context (Windows' own OpenGL 1.1 without a GPU driver, some Remote Desktop sessions) Qt can't even composite a
/// QOpenGLWidget into the window and crashes, so then there is none, and the view says why instead.
class PreviewWidget : public QWidget {
    Q_OBJECT
    friend class OverlayLayer;
    friend class GlView;
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
    /// Most frames a second the view shows; 0 = the screen's refresh rate. Every shown frame costs CPU here and in
    /// whatever copies the screen (a remote desktop viewer); the analyses still get every frame the view takes.
    void setDisplayLimit(int fps) { displayLimit_ = std::max(0, fps); }
    int displayLimit() const { return displayLimit_; }
    static constexpr int kDefaultDisplayLimit = 30;
    /// During rawFrame: whether this frame is also being shown. Widgets changed now are redrawn with its paint, in one
    /// composite of the window; changed between shown frames, each costs a composite of its own (and a frame for
    /// whatever copies the screen).
    bool rawFrameShown() const { return rawShown_; }

    void setOverlay(const QString& key, Overlay overlay);   ///< replaces; an empty function removes it
    void setCrosshair(bool on);
    void fitToWindow();
    void zoomOneToOne();
    void zoomTo(double widgetPxPerImagePx);   ///< 1 = 100 %, 2 = 200 %
    void setDebayer(bool on);                 ///< off: show the raw mosaic as grey (checks focus/hot pixels per site)
    bool fitted() const { return zoom_ == 1.0; }
    QSize imageSize() const { return {renderer_.imageWidth(), renderer_.imageHeight()}; }
    /// Times the preview made its GL context current itself rather than in paintGL (re-stretching a paused image,
    /// say). None per frame: on Intel's Windows driver every switch is costly. For the tests.
    int ownContextSwitches() const { return ownContextSwitches_; }

signals:
    void zoomChanged();
    /// A new frame was shown: its histogram, the levels used, and its metadata.
    void frameShown(const std::array<uint32_t, 256>& histogram, const slopcap::preview::Levels& levels,
                    const slopcap::FrameInfo& info);
    /// The raw frame just shown. Only valid during the call (connect directly; copy what you keep).
    void rawFrame(const slopcap::LatestFrame::Frame& frame);
    /// User asked to centre the ROI on this image pixel (right-click menu).
    void centreRequested(QPointF imagePixel);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    void initializeGL();   ///< from GlView
    void paintGL();        ///< from GlView
    void refresh();        ///< repaint: the GPU view if there is one, else the overlays (the message)
    void poll();
    void present(const LatestFrame::Frame& f);   ///< the display work for a frame the screen will show
    void updateOverlays();                       ///< repaint the overlay layer, at most once per shown frame
    double refreshMs() const;                    ///< the screen's frame interval
    double frameMs() const;                      ///< the shown frames' interval: the screen's, or longer with a limit
    void makeCurrent();                          ///< outside paintGL: counted (ownContextSwitches)
    double fitScale() const;          ///< widget px per image px at "fit"
    double scale() const { return fitScale() * zoom_; }
    QTransform imageToWidget() const;
    void paintOverlays(QPainter& p) const;
    QWidget* overlayLayer_ = nullptr;   ///< transparent child on top: overlays are raster-painted there
    QOpenGLWidget* gl_ = nullptr;       ///< the GPU view (GlView), or none without OpenGL 3.3

    GlRenderer renderer_;
    LatestFrame* source_ = nullptr;
    Bayer bayer_ = Bayer::None;
    Levels levels_;
    bool auto_ = true;
    bool autoWb_ = true;
    bool debayer_ = true;
    bool glReady_ = false;
    QMetaObject::Connection glLost_;   ///< to the context's aboutToBeDestroyed
    QString glError_;   ///< why the GPU preview couldn't start (shown in the view instead of crashing)
    // The frame on its way to the screen, for paintGL to upload: a copy, so the camera keeps flowing meanwhile.
    std::vector<std::byte> shownPixels_;
    FrameInfo shownInfo_;
    Levels shownLevels_;
    bool pending_ = false;        ///< shownPixels_ not uploaded yet
    QElapsedTimer shownClock_;    ///< since the last frame was prepared for the screen
    QElapsedTimer wbClock_;       ///< auto white balance and its white point: ten times a second
    double peakWhite_ = 0;        ///< white point that keeps the balanced target's bright zones below white
    int displayLimit_ = kDefaultDisplayLimit;
    bool rawShown_ = false;       ///< the frame being handed out by rawFrame is also being shown
    bool overlayDirty_ = false;
    QElapsedTimer overlayClock_;
    QTransform overlayTransform_;   ///< the image-to-widget transform the overlays were last painted with
    int ownContextSwitches_ = 0;
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

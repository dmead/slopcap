// The live view: polls a LatestFrame at display rate, uploads each new frame to the GPU (GlRenderer) and keeps the
// histogram and auto-stretch levels up to date. Owns no camera; whoever runs the CaptureSession hands it the tap.
#pragma once

#include "preview/gl_renderer.hpp"
#include "preview/latest_frame.hpp"
#include "preview/render_cpu.hpp"

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QTimer>

#include <array>

namespace slopcap::preview {

class PreviewWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);
    ~PreviewWidget() override;

    void setSource(LatestFrame* source, Bayer bayer);   ///< nullptr = stop polling (last image stays)
    void setAutoStretch(bool on);
    void setLevels(const Levels& lv);                   ///< manual levels (turns auto-stretch off)
    const Levels& levels() const { return levels_; }
    bool autoStretch() const { return auto_; }
    double displayFps() const { return fps_; }

signals:
    /// A new frame was shown: its histogram, the levels used, and its metadata.
    void frameShown(const std::array<uint32_t, 256>& histogram, const slopcap::preview::Levels& levels,
                    const slopcap::FrameInfo& info);

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    void poll();

    GlRenderer renderer_;
    LatestFrame* source_ = nullptr;
    Bayer bayer_ = Bayer::None;
    Levels levels_;
    bool auto_ = true;
    bool glReady_ = false;
    QTimer timer_;
    QElapsedTimer fpsClock_;
    int fpsFrames_ = 0;
    double fps_ = 0;
};

}  // namespace slopcap::preview

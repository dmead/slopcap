// The camera's picture, small, for where the main preview isn't on screen (the Focus tab's side column): the target
// close up, or the whole frame with the scored window outlined. Drawn on the CPU from the frame the main preview just
// showed, a few times a second, with that preview's levels and white balance, so the two look alike.
#pragma once

#include "analysis/focus.hpp"
#include "camera/camera.hpp"
#include "preview/latest_frame.hpp"
#include "preview/render_cpu.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QWidget>

namespace slopcap::app {

class LiveInset : public QWidget {
    Q_OBJECT
public:
    enum class View { Target, Frame };

    explicit LiveInset(QWidget* parent = nullptr);
    void setView(View v);
    View view() const { return view_; }
    /// The frame the main preview just showed (valid only during the call) and its focus scores, for the target's
    /// position. Drawn only while this is on screen, at most every 100 ms.
    void showFrame(const LatestFrame::Frame& f, Bayer bayer, const preview::Levels& levels, const analysis::FocusScores& scores);
    void clear();
    bool hasImage() const { return !image_.isNull(); }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w * 2 / 3; }
    QSize sizeHint() const override { return {260, 174}; }

signals:
    void activated();   ///< double-click: the full preview

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    View view_ = View::Target;
    QImage image_;
    QRectF window_;   ///< Frame view: the scored window, in image pixels
    QRectF crop_;     ///< Target view: the part of the frame shown, frame pixels, eased from frame to frame
    QElapsedTimer clock_;
};

}  // namespace slopcap::app

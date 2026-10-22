// ADC mode: lining up the colour channels with an atmospheric dispersion corrector. Measures where the planet's red,
// green and blue images sit (analysis/dispersion), shows what's left between red and blue as an arrow over the
// preview, a target plot and a history, and compares the dispersion with what the atmosphere should be doing at the
// target's altitude. Remembering the uncorrected reading (levers at zero) gives the direction to correct along, so the
// panel can tell an under-corrected, an over-corrected and a rotated ADC apart. left() holds the steps, right() the
// readouts.
#pragma once

#include "analysis/dispersion.hpp"
#include "preview/latest_frame.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QWidget>

#include <deque>
#include <optional>

class QLabel;
class QPushButton;

namespace slopcap::preview {
class PreviewWidget;
}

namespace slopcap::app {

/// The red − blue offset (px) as a point on a target: the recent trail, the uncorrected direction (dashed) and the
/// "lined up" zone in the middle.
class DispersionPlot : public QWidget {
    Q_OBJECT
public:
    explicit DispersionPlot(QWidget* parent = nullptr);
    void add(QPointF offset, const QColor& colour);
    void setReference(std::optional<QPointF> uncorrected);
    void clear();
    QSize sizeHint() const override { return {240, 240}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    std::deque<QPointF> trail_;
    QColor colour_;
    std::optional<QPointF> reference_;
};

/// The offset's size over the session, with the best so far.
class ResidualHistory : public QWidget {
    Q_OBJECT
public:
    explicit ResidualHistory(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(90); }
    void add(double px);
    void clear();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    std::deque<double> v_;
    std::optional<double> best_;
};

class AdcPanel : public QObject {
    Q_OBJECT
public:
    /// What the atmosphere is doing depends on how high the target is.
    struct Altitude {
        double degrees = 0;
        QString of;   ///< what is that high, as a sentence starts: "Jupiter", "The telescope"
    };
    static constexpr double kLinedUpPx = 0.3;   ///< a residual this small counts as corrected

    explicit AdcPanel(preview::PreviewWidget* preview, QObject* parent = nullptr);
    QWidget* left() const { return left_; }
    QWidget* right() const { return right_; }

    /// Measuring while ADC mode shows; off clears the overlay.
    void setActive(bool on);
    bool active() const { return active_; }
    void onFrame(const LatestFrame::Frame& frame, Bayer bayer);
    /// The image scale (for arcseconds) and the target's altitude (for what the atmosphere does), when known.
    void setContext(std::optional<double> arcsecPerPixel, std::optional<Altitude> altitude);

    /// Red − blue (px), median of the recent frames; none before a measurement.
    std::optional<QPointF> residual() const { return residual_; }
    std::optional<QPointF> uncorrected() const { return reference_; }
    void rememberUncorrected();
    void reset();

private:
    void refresh();
    void clearOverlay();
    QColor stateColour() const;

    preview::PreviewWidget* preview_;
    QWidget* left_ = nullptr;
    QWidget* right_ = nullptr;
    QPushButton* remember_ = nullptr;
    QLabel* offset_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel *direction_ = nullptr, *alongAcross_ = nullptr, *expected_ = nullptr, *altitudeValue_ = nullptr;
    QLabel* note_ = nullptr;
    QLabel* best_ = nullptr;
    DispersionPlot* plot_ = nullptr;
    ResidualHistory* history_ = nullptr;

    bool active_ = false;
    QElapsedTimer measureClock_, logClock_;
    std::deque<QPointF> recent_;
    std::optional<QPointF> residual_, reference_;
    std::optional<double> bestPx_;
    double noisePx_ = 0;
    QString problem_;
    bool atEdge_ = false;
    std::optional<double> scale_;
    std::optional<Altitude> altitude_;
};

}  // namespace slopcap::app

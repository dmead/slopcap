// Focus tab: INDI focuser control, live per-channel (R/G/B) sharpness with peak hold, and the focus sweep
// (PLAN.md §7a): step the focuser through a range, score frames at each position with every metric, fit each channel's
// curve and offer the position for a chosen strategy.
#pragma once

#include "analysis/focus.hpp"
#include "analysis/sweep.hpp"
#include "devices/focuser.hpp"
#include "devices/indi_client.hpp"
#include "preview/latest_frame.hpp"

#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QWidget>

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace slopcap::preview {
class PreviewWidget;
}

namespace slopcap::app {

/// Three horizontal bars (R, G, B), each the live score as a fraction of that channel's own peak, with a peak marker.
class RgbBars : public QWidget {
    Q_OBJECT
public:
    explicit RgbBars(QWidget* parent = nullptr);
    void setValues(int channels, const std::array<double, 3>& fraction, const std::array<bool, 3>& clipped);
    QSize sizeHint() const override { return {260, 84}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    int channels_ = 0;
    std::array<double, 3> v_{};
    std::array<bool, 3> clipped_{};
};

/// The sweep's curves: best-quarter scores per position, the fitted parabolas and each channel's best.
class SweepChart : public QWidget {
    Q_OBJECT
public:
    explicit SweepChart(QWidget* parent = nullptr);
    void setResult(const analysis::SweepResult& r, std::optional<double> marker);
    QSize sizeHint() const override { return {280, 170}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    analysis::SweepResult r_;
    std::optional<double> marker_;
};

class FocusPanel : public QWidget {
    Q_OBJECT
public:
    FocusPanel(preview::PreviewWidget* preview, QWidget* parent = nullptr);
    ~FocusPanel() override;

    void onFrame(const LatestFrame::Frame& frame, Bayer bayer);   ///< every displayed frame
    void setOutputDir(const QString& dir) { outDir_ = dir; }
    void connectFocuser(const QString& hostPort);
    QString focuserServer() const;
    int focuserPosition() const { return focuser_.ready() ? focuser_.position() : -1; }
    int backlash() const;
    bool approachOutward() const;
    void setBacklash(int steps, bool outward);
    QString metricName() const;
    void setMetric(const QString& name);
    void sweepWhenReady(int range, int step, int frames);   ///< 0 = keep the current setting

private:
    enum class Sweep { Idle, Moving, Settling, Collecting };
    void buildUi();
    void updateFocuserUi();
    void resetPeaks();
    void startSweep();
    void stopSweep(const QString& why);
    void nextSweepPosition();
    void showSweepResult();
    analysis::Metric liveMetric() const;

    preview::PreviewWidget* preview_;
    devices::IndiClient indi_;
    devices::Focuser focuser_;
    QString outDir_;

    // live
    std::deque<analysis::FocusScores> recent_;
    std::array<double, 3> peak_{};
    analysis::FocusScores last_;

    // sweep
    Sweep sweep_ = Sweep::Idle;
    std::vector<int> positions_;
    std::size_t posIndex_ = 0;
    int collected_ = 0, startPosition_ = 0;
    QTimer settle_;
    analysis::SweepAnalysis analysis_;
    std::unique_ptr<QFile> csv_;
    std::unique_ptr<QTextStream> csvOut_;

    // widgets
    QLineEdit* server_ = nullptr;
    QPushButton* connect_ = nullptr;
    QLabel* device_ = nullptr;
    QLabel* position_ = nullptr;
    QSpinBox* goto_ = nullptr;
    QSpinBox* backlash_ = nullptr;
    QComboBox* approach_ = nullptr;
    QComboBox* metric_ = nullptr;
    QSpinBox* smooth_ = nullptr;
    RgbBars* bars_ = nullptr;
    QLabel* liveText_ = nullptr;
    QSpinBox* range_ = nullptr;
    QSpinBox* step_ = nullptr;
    QSpinBox* frames_ = nullptr;
    QSpinBox* settleMs_ = nullptr;
    QCheckBox* fourier_ = nullptr;
    QPushButton* sweepBtn_ = nullptr;
    QLabel* sweepStatus_ = nullptr;
    SweepChart* chart_ = nullptr;
    QComboBox* resultMetric_ = nullptr;
    QTableWidget* table_ = nullptr;
    QComboBox* target_ = nullptr;
    QPushButton* goTarget_ = nullptr;
    QList<QWidget*> moveButtons_;
    bool sweepWhenReady_ = false;
};

}  // namespace slopcap::app

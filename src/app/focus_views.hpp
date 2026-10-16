// The views onto FocusController, laid out as in the design canvas:
//   FocuserSection     Capture sidebar: position, nudges, live R/G/B sharpness, link to Focus mode
//   SweepPanel         Focus mode, left: sweep range / step / frames, metric, channels, run
//   FocusChartPanel    Focus mode, centre: per-channel curves with fits, critical focus zone, summary cards
//   FocusResultsPanel  Focus mode, right: best focus per channel, verdict, strategy, go to, metric comparison
#pragma once

#include "analysis/sweep.hpp"
#include "app/focus_controller.hpp"

#include <QWidget>

#include <array>
#include <optional>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace slopcap::app {

/// R/G/B rows: marker, bar (live score as a fraction of the channel's best), peak tick, value.
class ChannelBars : public QWidget {
    Q_OBJECT
public:
    explicit ChannelBars(QWidget* parent = nullptr);
    void setValues(int channels, const std::array<double, 3>& fraction, const std::array<bool, 3>& clipped);
    QSize sizeHint() const override { return {240, 66}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    int channels_ = 0;
    std::array<double, 3> v_{};
    std::array<bool, 3> clipped_{};
};

class FocuserSection : public QWidget {
    Q_OBJECT
public:
    FocuserSection(FocusController& fc, QWidget* parent = nullptr);

signals:
    void openFocusMode();
    void openSettings();

private:
    void refresh();
    FocusController& fc_;
    QLabel* title_ = nullptr;
    QLabel* position_ = nullptr;
    QLabel* max_ = nullptr;
    QWidget* nudges_ = nullptr;
    QWidget* offline_ = nullptr;
    QLabel* offlineText_ = nullptr;
    ChannelBars* bars_ = nullptr;
    QLabel* note_ = nullptr;
};

class SweepPanel : public QWidget {
    Q_OBJECT
public:
    SweepPanel(FocusController& fc, QWidget* parent = nullptr);
    std::array<bool, 3> channelsShown() const;

signals:
    void displayChanged();   ///< metric or channel visibility changed

private:
    void preset(int halfRange, int step);
    void refresh();
    FocusController& fc_;
    QButtonGroup* presets_ = nullptr;
    QSpinBox *from_ = nullptr, *to_ = nullptr, *step_ = nullptr, *frames_ = nullptr, *settle_ = nullptr;
    QComboBox* metric_ = nullptr;
    std::array<QCheckBox*, 3> channel_{};
    QCheckBox* fourier_ = nullptr;
    QLabel* backlash_ = nullptr;
    QPushButton* run_ = nullptr;
    QLabel* status_ = nullptr;
};

/// The chart itself.
class FocusChart : public QWidget {
    Q_OBJECT
public:
    explicit FocusChart(QWidget* parent = nullptr);
    void setData(const analysis::SweepResult& r, std::array<bool, 3> shown, std::optional<double> chosen, std::optional<double> zone);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    analysis::SweepResult r_;
    std::array<bool, 3> shown_{true, true, true};
    std::optional<double> chosen_, zone_;
};

class FocusChartPanel : public QWidget {
    Q_OBJECT
public:
    FocusChartPanel(FocusController& fc, QWidget* parent = nullptr);
    void update(analysis::Metric m, std::array<bool, 3> shown, std::optional<double> chosen);

private:
    FocusController& fc_;
    QLabel* subtitle_ = nullptr;
    FocusChart* chart_ = nullptr;
    QLabel *spread_ = nullptr, *zone_ = nullptr, *zoneCaption_ = nullptr, *worst_ = nullptr;
    QLabel* footnote_ = nullptr;
};

class FocusResultsPanel : public QWidget {
    Q_OBJECT
public:
    FocusResultsPanel(FocusController& fc, QWidget* parent = nullptr);
    analysis::FocusTarget target() const { return target_; }
    std::optional<double> chosen(analysis::Metric m) const;
    void update(analysis::Metric m);

signals:
    void targetChanged();
    void metricPicked(analysis::Metric m);

private:
    FocusController& fc_;
    analysis::Metric metric_ = analysis::Metric::Gradient;
    analysis::FocusTarget target_ = analysis::FocusTarget::Green;
    std::array<QLabel*, 3> best_{}, vsG_{};
    QFrame* verdict_ = nullptr;
    QLabel* verdictText_ = nullptr;
    std::array<QPushButton*, 5> modes_{};
    QPushButton* go_ = nullptr;
    QLabel* note_ = nullptr;
    QTableWidget* metrics_ = nullptr;
};

}  // namespace slopcap::app

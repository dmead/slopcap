// The views onto FocusController, laid out as in the design canvas:
//   FocuserControls    every tab: position, STOP, nudges (or Connect); all of them drive the one focuser
//   FocuserSection     Capture sidebar: the controls, live R/G/B sharpness, link to Focus mode
//   SweepPanel         Focus mode, left: sweep range / step / frames, metric, channels, run
//   FocusChartPanel    Focus mode, centre: per-channel curves with fits, critical focus zone, summary cards
//   FocusResultsPanel  Focus mode, right: best focus per channel, verdict, strategy, go to, metric comparison
//   FocusCurveInset    the other tabs: the sweep's curves small, over a corner of the live preview
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
class QVBoxLayout;

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

/// Title, position, STOP and the step buttons, or Connect while there's no focuser. Every tab has one; they all follow
/// the FocusController, so a move, a sweep or a lost link shows the same on each.
class FocuserControls : public QWidget {
    Q_OBJECT
public:
    explicit FocuserControls(FocusController& fc, QWidget* parent = nullptr);

private:
    void refresh();
    FocusController& fc_;
    QLabel* title_ = nullptr;
    QWidget* posBox_ = nullptr;
    QLabel* position_ = nullptr;
    QLabel* max_ = nullptr;
    QPushButton* stop_ = nullptr;
    QWidget* nudges_ = nullptr;
    QWidget* offline_ = nullptr;
    QLabel* offlineText_ = nullptr;
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
    ChannelBars* bars_ = nullptr;
    QLabel* note_ = nullptr;
};

class SweepPanel : public QWidget {
    Q_OBJECT
public:
    SweepPanel(FocusController& fc, QWidget* parent = nullptr);
    std::array<bool, 3> channelsShown() const;
    void setHeader(QWidget* w);   ///< above the settings, scrolling with them (the window: the camera and the focuser)

signals:
    void displayChanged();   ///< metric or channel visibility changed

private:
    void preset(int halfRange, int step);
    void centre();          ///< the range, same width, around the focuser (while it follows it)
    void refresh();
    FocusController& fc_;
    QVBoxLayout* header_ = nullptr;
    QButtonGroup* presets_ = nullptr;
    QSpinBox *from_ = nullptr, *to_ = nullptr, *step_ = nullptr, *frames_ = nullptr, *settle_ = nullptr;
    QLabel* follows_ = nullptr;
    bool follow_ = true;    ///< the range stays centred on the focuser until From or To is set by hand
    bool setting_ = false;  ///< the panel itself is changing From / To
    QComboBox* metric_ = nullptr;
    std::array<QCheckBox*, 3> channel_{};
    QCheckBox* fourier_ = nullptr;
    QCheckBox* extend_ = nullptr;
    QCheckBox* goToBest_ = nullptr;
    QPushButton* run_ = nullptr;
    QLabel* status_ = nullptr;
};

/// The chart itself.
class FocusChart : public QWidget {
    Q_OBJECT
public:
    explicit FocusChart(QWidget* parent = nullptr);
    void setData(const analysis::SweepResult& r, std::array<bool, 3> shown, std::optional<double> chosen, std::optional<double> zone);
    /// While a sweep runs: the positions it will cover (the x axis), and where the focuser is now (a marker).
    void setProgress(std::optional<std::pair<int, int>> span, std::optional<int> at);
    void setCompact(bool on);   ///< no axis labels or empty-state text: the inset over the preview

protected:
    void paintEvent(QPaintEvent*) override;

private:
    analysis::SweepResult r_;
    std::array<bool, 3> shown_{true, true, true};
    std::optional<double> chosen_, zone_;
    std::optional<std::pair<int, int>> span_;
    std::optional<int> at_;
    bool compact_ = false;
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

/// The sweep, small, over a corner of the live preview on the tabs other than Focus: the curves filling in as the
/// sweep runs, a line of status, and a click away from the Focus tab.
class FocusCurveInset : public QWidget {
    Q_OBJECT
public:
    FocusCurveInset(FocusController& fc, QWidget* parent = nullptr);
    void update(analysis::Metric m, std::array<bool, 3> shown, std::optional<double> chosen);
    bool hasData() const { return hasData_; }

signals:
    void openFocusMode();
    void dismissed();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    FocusController& fc_;
    QLabel* status_ = nullptr;
    FocusChart* chart_ = nullptr;
    bool hasData_ = false;
};

}  // namespace slopcap::app

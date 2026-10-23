// Focus logic shared by the Capture sidebar and Focus mode: the INDI focuser, live per-channel sharpness (median of the
// last N frames, as a fraction of each channel's own best), and the focus sweep (PLAN.md §7a): step the focuser
// through a range, score every frame with every metric, fit each channel's curve.
#pragma once

#include "analysis/focus.hpp"
#include "analysis/sweep.hpp"
#include "devices/focuser.hpp"
#include "devices/indi_client.hpp"
#include "preview/latest_frame.hpp"

#include <QFile>
#include <QObject>
#include <QTextStream>
#include <QTimer>

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace slopcap::app {

struct LiveFocus {
    analysis::FocusScores last;        ///< this frame
    std::array<double, 3> median{};    ///< median of the recent frames, per channel
    std::array<double, 3> fraction{};  ///< median / peak so far, per channel
    std::array<bool, 3> clipped{};
    int frameWidth = 0, frameHeight = 0;
};

struct SweepSettings {
    int from = 0, to = 0, step = 50, frames = 60, settleMs = 400;
    bool fourier = true;
};

class FocusController : public QObject {
    Q_OBJECT
public:
    explicit FocusController(QObject* parent = nullptr);
    ~FocusController() override;

    devices::Focuser& focuser() { return focuser_; }
    devices::IndiClient& indi() { return indi_; }
    void connectFocuser(const QString& hostPort);
    void disconnectFocuser() { indi_.disconnectFromServer(); }
    QString server() const { return server_; }
    void setServer(const QString& hostPort) { server_ = hostPort; }   ///< remembered for connectFocuser(server())

    void setOutputDir(const QString& dir) { outDir_ = dir; }
    void setLiveMetric(analysis::Metric m);
    analysis::Metric liveMetric() const { return metric_; }
    void setSmoothing(int frames) { smooth_ = std::max(1, frames); }
    void resetPeaks();
    const LiveFocus& live() const { return live_; }

    /// Every displayed frame. Scores it for the live readout; collects it during a sweep.
    void onFrame(const LatestFrame::Frame& frame, Bayer bayer);

    void startSweep(const SweepSettings& s);
    void stopSweep();
    void stopFocuser();                    ///< STOP: halt the motor now, ending any sweep where it is
    bool sweeping() const { return state_ != State::Idle; }
    const analysis::SweepAnalysis& sweep() const { return analysis_; }
    int sweepStart() const { return startPosition_; }
    const SweepSettings& sweepSettings() const { return settings_; }

    // Optics, for the critical focus zone: ±2.44 λ N² (λ = 550 nm), converted to steps.
    void setOptics(double focalRatio, double micronsPerStep) { fRatio_ = focalRatio, umPerStep_ = micronsPerStep; }
    std::optional<double> criticalZoneSteps() const;
    double focalRatio() const { return fRatio_; }

signals:
    void focuserChanged();                 ///< connection, position or motion changed
    void liveUpdated();                    ///< live() has a new frame
    void sweepStateChanged(bool running);
    void sweepProgress(const QString& text);
    void sweepUpdated();                   ///< new points (or a new sweep)

private:
    enum class State { Idle, Moving, Settling, Collecting };
    void nextPosition();
    void finish(const QString& why, bool putBack);

    devices::IndiClient indi_;
    devices::Focuser focuser_;
    QString server_ = "localhost:7624", outDir_;
    analysis::Metric metric_ = analysis::Metric::Gradient;
    int smooth_ = 9;
    std::deque<analysis::FocusScores> recent_;
    std::array<double, 3> peak_{};
    LiveFocus live_;

    State state_ = State::Idle;
    SweepSettings settings_;
    std::vector<int> positions_;
    std::size_t index_ = 0;
    int collected_ = 0, startPosition_ = 0;
    QTimer settle_;
    analysis::SweepAnalysis analysis_;
    std::unique_ptr<QFile> csv_;
    std::unique_ptr<QTextStream> csvOut_;
    double fRatio_ = 0, umPerStep_ = 0;
};

}  // namespace slopcap::app

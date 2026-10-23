#include "app/focus_controller.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

namespace slopcap::app {

namespace {

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

}  // namespace

FocusController::FocusController(QObject* parent) : QObject(parent), focuser_(indi_) {
    settle_.setSingleShot(true);
    connect(&settle_, &QTimer::timeout, this, [this] {
        if (state_ == State::Settling) {
            collected_ = 0;
            state_ = State::Collecting;
        }
    });
    connect(&focuser_, &devices::Focuser::readyChanged, this, &FocusController::focuserChanged);
    connect(&focuser_, &devices::Focuser::positionChanged, this, &FocusController::focuserChanged);
    connect(&focuser_, &devices::Focuser::movingChanged, this, &FocusController::focuserChanged);
    connect(&focuser_, &devices::Focuser::arrived, this, [this] {
        if (state_ == State::Moving) {
            state_ = State::Settling;
            settle_.start(settings_.settleMs);
        }
    });
    connect(&focuser_, &devices::Focuser::failed, this, [this](const QString& why) {
        if (state_ != State::Idle) finish(why, true);
        emit focuserChanged();
    });
    connect(&indi_, &devices::IndiClient::serverDisconnected, this, &FocusController::focuserChanged);
    connect(&indi_, &devices::IndiClient::serverConnected, this, &FocusController::focuserChanged);
}

FocusController::~FocusController() {
    // Members die before QObject cuts connections, and closing the socket emits signals: detach first.
    indi_.disconnect(this);
    focuser_.disconnect(this);
    settle_.stop();
}

void FocusController::connectFocuser(const QString& hostPort) {
    server_ = hostPort;
    const auto parts = hostPort.split(':');
    indi_.connectToServer(parts.value(0, "localhost"), static_cast<quint16>(parts.value(1, "7624").toUInt()));
}

void FocusController::setLiveMetric(analysis::Metric m) {
    metric_ = m;
    resetPeaks();
}

void FocusController::resetPeaks() {
    peak_ = {};
    recent_.clear();
}

std::optional<double> FocusController::criticalZoneSteps() const {
    if (fRatio_ <= 0 || umPerStep_ <= 0) return std::nullopt;
    return 2.44 * 0.55 * fRatio_ * fRatio_ / umPerStep_;   // µm / (µm per step)
}

void FocusController::onFrame(const LatestFrame::Frame& frame, Bayer bayer) {
    const auto& fi = frame.info;
    const auto px = std::span(frame.pixels).first(fi.bytes);
    analysis::FocusOptions opt;
    opt.metric = metric_;
    const auto s = analysis::scoreFrame(px, fi.roi.width, fi.roi.height, fi.bitDepth, bayer, opt);

    live_.last = s;
    live_.frameWidth = fi.roi.width;
    live_.frameHeight = fi.roi.height;
    if (s.channels > 0) {
        recent_.push_back(s);
        while (static_cast<int>(recent_.size()) > smooth_) recent_.pop_front();
        for (int c = 0; c < s.channels; ++c) {
            const auto k = static_cast<std::size_t>(c);
            std::vector<double> v;
            for (const auto& r : recent_) v.push_back(r.ch[k].sharpness);
            live_.median[k] = median(v);
            peak_[k] = std::max(peak_[k], live_.median[k]);
            live_.fraction[k] = peak_[k] > 0 ? live_.median[k] / peak_[k] : 0;
            live_.clipped[k] = s.ch[k].clipped > 0.002;
        }
    }
    emit liveUpdated();

    if (state_ != State::Collecting || s.channels == 0) return;
    const int pos = focuser_.position();
    std::vector<analysis::Metric> ms = {analysis::Metric::Gradient, analysis::Metric::Laplacian, analysis::Metric::Variance};
    if (settings_.fourier) ms.push_back(analysis::Metric::Fourier);
    for (auto m : ms) {
        opt.metric = m;
        const auto sm = m == metric_ ? s : analysis::scoreFrame(px, fi.roi.width, fi.roi.height, fi.bitDepth, bayer, opt);
        analysis_.add(pos, sm, m);
        if (csvOut_)
            *csvOut_ << pos << ',' << fi.sequence << ',' << analysis::metricName(m) << ',' << sm.ch[0].sharpness << ',' << sm.ch[1].sharpness
                     << ',' << sm.ch[2].sharpness << ',' << sm.ch[0].clipped << ',' << sm.ch[1].clipped << ',' << sm.ch[2].clipped << '\n';
    }
    ++collected_;
    emit sweepProgress(QString("Position %1 of %2 · %3 · frame %4 / %5")
                           .arg(index_ + 1)
                           .arg(positions_.size())
                           .arg(pos)
                           .arg(collected_)
                           .arg(settings_.frames));
    if (collected_ >= settings_.frames) {
        ++index_;
        emit sweepUpdated();
        nextPosition();
    }
}

void FocusController::startSweep(const SweepSettings& s) {
    if (!focuser_.ready() || state_ != State::Idle) return;
    settings_ = s;
    startPosition_ = focuser_.position();
    positions_.clear();
    const int lo = std::min(s.from, s.to), hi = std::max(s.from, s.to), step = std::max(1, s.step);
    for (int p = lo; p <= hi; p += step) positions_.push_back(p);
    if (!focuser_.approachOutward()) std::reverse(positions_.begin(), positions_.end());   // each move in the approach direction
    if (focuser_.maxPosition() > 0) std::erase_if(positions_, [&](int p) { return p < 0 || p > focuser_.maxPosition(); });
    if (positions_.size() < 3) {
        emit sweepProgress("Range too small for a sweep (needs 3+ positions)");
        return;
    }
    analysis_.clear();
    index_ = 0;
    QDir dir(outDir_.isEmpty() ? QDir::homePath() : outDir_);
    dir.mkpath(".");
    csv_ = std::make_unique<QFile>(dir.filePath("focus-sweep-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") + ".csv"));
    if (csv_->open(QIODevice::WriteOnly | QIODevice::Text)) {
        csvOut_ = std::make_unique<QTextStream>(csv_.get());
        *csvOut_ << "position,frame,metric,R,G,B,clipR,clipG,clipB\n";
    } else {
        csv_.reset();
    }
    emit sweepUpdated();
    emit sweepStateChanged(true);
    nextPosition();
}

void FocusController::nextPosition() {
    if (index_ >= positions_.size()) {
        finish(QString("Sweep complete · %1 positions × %2 frames").arg(positions_.size()).arg(settings_.frames), false);
        return;
    }
    state_ = State::Moving;
    emit sweepProgress(QString("Moving to %1 (%2 of %3)").arg(positions_[index_]).arg(index_ + 1).arg(positions_.size()));
    focuser_.goTo(positions_[index_]);
}

void FocusController::stopSweep() {
    if (state_ != State::Idle) finish("Sweep stopped", true);
}

void FocusController::stopFocuser() {
    if (state_ != State::Idle) finish("Sweep stopped", false);   // not back to the start: that's another move
    focuser_.abort();
}

void FocusController::finish(const QString& why, bool putBack) {
    state_ = State::Idle;
    settle_.stop();
    if (csvOut_) csvOut_->flush();
    const QString file = csv_ ? QFileInfo(csv_->fileName()).fileName() : QString();
    csvOut_.reset();
    csv_.reset();
    emit sweepProgress(why + (file.isEmpty() ? QString() : " · scores in " + file));
    if (putBack && focuser_.ready()) focuser_.goTo(startPosition_);   // interrupted: put focus back
    emit sweepUpdated();
    emit sweepStateChanged(false);
}

}  // namespace slopcap::app

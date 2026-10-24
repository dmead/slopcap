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
        if (state_ != State::Idle) finish(why, startPosition_);
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
    // extensions: up to twice the range again past either end, inside the focuser's travel
    const int span = std::max(hi - lo, 2 * step);
    lowLimit_ = std::max(0, lo - 2 * span);
    highLimit_ = focuser_.maxPosition() > 0 ? std::min(hi + 2 * span, focuser_.maxPosition()) : hi + 2 * span;
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

std::optional<std::pair<int, int>> FocusController::sweepSpan() const {
    if (state_ == State::Idle || positions_.empty()) return std::nullopt;
    const auto [lo, hi] = std::minmax_element(positions_.begin(), positions_.end());
    return std::pair{*lo, *hi};
}

void FocusController::nextPosition() {
    if (index_ >= positions_.size()) {
        const auto more = extension();
        if (more.empty()) {
            complete();
            return;
        }
        const auto [lo, hi] = std::minmax_element(positions_.begin(), positions_.end());
        const bool down = *std::min_element(more.begin(), more.end()) < *lo, up = *std::max_element(more.begin(), more.end()) > *hi;
        emit sweepProgress(QString("Sharpest at %1 so far: %2 more positions")
                               .arg(down && up ? "both ends" : down ? "the low end" : "the high end")
                               .arg(more.size()));
        positions_.insert(positions_.end(), more.begin(), more.end());
        emit sweepUpdated();   // the span grew
    }
    state_ = State::Moving;
    emit sweepProgress(QString("Moving to %1 (%2 of %3)").arg(positions_[index_]).arg(index_ + 1).arg(positions_.size()));
    focuser_.goTo(positions_[index_]);
}

std::vector<int> FocusController::extension() const {
    if (!settings_.extend || positions_.empty()) return {};
    // A peak needs the curve coming down on both sides of it (fitPeak wants two points past the top each way): a side
    // where any channel's sharpest point has fewer gets more positions
    const auto r = analysis_.result(metric_);
    bool low = false, high = false;
    for (int c = 0; c < r.channels; ++c) {
        const auto& pts = r.ch[static_cast<std::size_t>(c)].points;
        if (pts.size() < 3) continue;
        std::size_t top = 0;
        for (std::size_t i = 1; i < pts.size(); ++i)
            if (pts[i].bestQuarter > pts[top].bestQuarter) top = i;
        low = low || top < 2;
        high = high || pts.size() - 1 - top < 2;
    }
    const auto [lo, hi] = std::minmax_element(positions_.begin(), positions_.end());
    const int step = std::max(1, settings_.step), dir = focuser_.approachOutward() ? 1 : -1;
    constexpr int kChunk = 3;
    auto beyond = [&](int edge, int sign) {
        std::vector<int> c;
        for (int k = 1; k <= kChunk; ++k) {
            const int p = edge + sign * k * step;
            if (p < lowLimit_ || p > highLimit_) break;
            c.push_back(p);
        }
        if (sign != dir) std::reverse(c.begin(), c.end());   // every move still ends in the approach direction
        return c;
    };
    // the side ahead (where the last move was heading) first: no reversal, so no backlash take-up
    auto more = (dir > 0 ? high : low) ? beyond(dir > 0 ? *hi : *lo, dir) : std::vector<int>{};
    if (dir > 0 ? low : high) {
        const auto behind = beyond(dir > 0 ? *lo : *hi, -dir);
        more.insert(more.end(), behind.begin(), behind.end());
    }
    return more;
}

void FocusController::complete() {
    const auto [lo, hi] = std::minmax_element(positions_.begin(), positions_.end());
    QString why = QString("Sweep complete · %1 positions × %2 frames").arg(positions_.size()).arg(settings_.frames);
    if (*lo < std::min(settings_.from, settings_.to) || *hi > std::max(settings_.from, settings_.to))
        why += QString(" · carried on to %1–%2").arg(*lo).arg(*hi);
    std::optional<int> dest;
    if (settings_.goToBest) {
        const auto r = analysis_.result(metric_);
        if (const auto best = r.position(target_); best && *best >= *lo && *best <= *hi) {
            dest = static_cast<int>(std::lround(*best));
            why += QString(" · going to %1 (%2)").arg(*dest).arg(analysis::focusTargetName(target_));
        } else if (r.channels > 0) {
            // No peak inside what was swept (still at an end at the limit, or a flat curve): the sharpest position is
            // the nearest to focus, and the next sweep, centred there, carries on from it
            const int c = r.channels == 1 ? 0 : target_ == analysis::FocusTarget::Red ? 0 : target_ == analysis::FocusTarget::Blue ? 2 : 1;
            const auto& pts = r.ch[static_cast<std::size_t>(c)].points;
            if (!pts.empty()) {
                dest = std::max_element(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.bestQuarter < b.bestQuarter; })->position;
                why += QString(" · no peak inside the range: going to the sharpest position, %1").arg(*dest);
            }
        }
    }
    finish(why, dest);
}

void FocusController::stopSweep() {
    if (state_ != State::Idle) finish("Sweep stopped", startPosition_);   // interrupted: put focus back
}

void FocusController::stopFocuser() {
    if (state_ != State::Idle) finish("Sweep stopped", std::nullopt);   // not back to the start: that's another move
    focuser_.abort();
}

void FocusController::finish(const QString& why, std::optional<int> thenGoTo) {
    state_ = State::Idle;
    settle_.stop();
    if (csvOut_) csvOut_->flush();
    const QString file = csv_ ? QFileInfo(csv_->fileName()).fileName() : QString();
    csvOut_.reset();
    csv_.reset();
    emit sweepProgress(why + (file.isEmpty() ? QString() : " · scores in " + file));
    if (thenGoTo && focuser_.ready()) focuser_.goTo(*thenGoTo);
    emit sweepUpdated();
    emit sweepStateChanged(false);
}

}  // namespace slopcap::app

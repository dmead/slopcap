#include "app/focus_panel.hpp"

#include "preview/preview_widget.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace slopcap::app {

namespace {

const QColor kChannel[3] = {QColor(235, 85, 75), QColor(95, 200, 100), QColor(90, 140, 255)};
const char* kChannelName[3] = {"R", "G", "B"};

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

}  // namespace

// ---------------------------------------------------------------- RgbBars

RgbBars::RgbBars(QWidget* parent) : QWidget(parent) { setMinimumHeight(70); }

void RgbBars::setValues(int channels, const std::array<double, 3>& fraction, const std::array<bool, 3>& clipped) {
    channels_ = channels;
    v_ = fraction;
    clipped_ = clipped;
    update();
}

void RgbBars::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int n = std::max(1, channels_);
    const double rowH = height() / static_cast<double>(n);
    const double labelW = 22, pctW = 74;
    for (int c = 0; c < n; ++c) {
        const QRectF row(0, c * rowH + 3, width(), rowH - 6);
        const QColor col = channels_ == 1 ? QColor(210, 210, 215) : kChannel[c];
        p.setPen(col);
        p.drawText(QRectF(row.left(), row.top(), labelW, row.height()), Qt::AlignVCenter | Qt::AlignLeft, channels_ == 1 ? "L" : kChannelName[c]);
        const QRectF track(row.left() + labelW, row.top(), row.width() - labelW - pctW, row.height());
        p.fillRect(track, QColor(28, 30, 34));
        const double f = std::clamp(v_[static_cast<std::size_t>(c)], 0.0, 1.0);
        p.fillRect(QRectF(track.left(), track.top(), track.width() * f, track.height()), col.darker(f > 0.97 ? 100 : 140));
        p.setPen(QPen(Qt::white, 2));   // peak marker: the channel's own best so far = the right end
        p.drawLine(QPointF(track.right() - 1, track.top()), QPointF(track.right() - 1, track.bottom()));
        p.setPen(clipped_[static_cast<std::size_t>(c)] ? QColor(255, 90, 90) : QColor(220, 222, 228));
        p.drawText(QRectF(track.right() + 6, row.top(), pctW - 6, row.height()), Qt::AlignVCenter | Qt::AlignLeft,
                   clipped_[static_cast<std::size_t>(c)] ? "CLIPPED" : QString("%1 %").arg(f * 100, 0, 'f', 1));
    }
}

// ---------------------------------------------------------------- SweepChart

SweepChart::SweepChart(QWidget* parent) : QWidget(parent) { setMinimumHeight(150); }

void SweepChart::setResult(const analysis::SweepResult& r, std::optional<double> marker) {
    r_ = r;
    marker_ = marker;
    update();
}

void SweepChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF plot = rect().adjusted(8, 8, -8, -22);
    p.fillRect(rect(), QColor(18, 19, 22));
    if (r_.channels == 0 || r_.ch[0].points.empty()) {
        p.setPen(QColor(120, 122, 128));
        p.drawText(plot, Qt::AlignCenter, "No sweep yet");
        return;
    }
    double x0 = 1e18, x1 = -1e18;
    for (int c = 0; c < r_.channels; ++c)
        for (const auto& pt : r_.ch[static_cast<std::size_t>(c)].points) x0 = std::min<double>(x0, pt.position), x1 = std::max<double>(x1, pt.position);
    if (x1 <= x0) x1 = x0 + 1;
    auto X = [&](double pos) { return plot.left() + (pos - x0) / (x1 - x0) * plot.width(); };
    // each channel normalised to its own peak, so curves of different brightness share the axis
    for (int c = 0; c < r_.channels; ++c) {
        const auto& cv = r_.ch[static_cast<std::size_t>(c)];
        double top = 0, bot = 1e18;
        for (const auto& pt : cv.points) top = std::max(top, pt.bestQuarter), bot = std::min(bot, pt.bestQuarter);
        if (top <= bot) continue;
        auto Y = [&](double v) { return plot.bottom() - (v - bot) / (top - bot) * plot.height(); };
        const QColor col = r_.channels == 1 ? QColor(210, 210, 215) : kChannel[c];
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        for (const auto& pt : cv.points) p.drawEllipse(QPointF(X(pt.position), Y(pt.bestQuarter)), 2.5, 2.5);
        if (cv.best) {
            QPainterPath path;
            bool first = true;
            for (int i = 0; i <= 80; ++i) {
                const double pos = x0 + (x1 - x0) * i / 80.0;
                const double v = cv.fitted(pos);
                if (v < bot - 0.1 * (top - bot)) {
                    first = true;
                    continue;
                }
                const QPointF q(X(pos), Y(v));
                if (first) path.moveTo(q), first = false;
                else path.lineTo(q);
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(col, 1.5));
            p.drawPath(path);
            p.setPen(QPen(col, 1, Qt::DashLine));
            p.drawLine(QPointF(X(*cv.best), plot.top()), QPointF(X(*cv.best), plot.bottom()));
        }
    }
    if (marker_) {
        p.setPen(QPen(Qt::white, 1.5));
        p.drawLine(QPointF(X(*marker_), plot.top()), QPointF(X(*marker_), plot.bottom()));
    }
    p.setPen(QColor(150, 152, 158));
    p.drawText(QRectF(plot.left(), plot.bottom() + 4, plot.width(), 16), Qt::AlignLeft, QString::number(static_cast<int>(x0)));
    p.drawText(QRectF(plot.left(), plot.bottom() + 4, plot.width(), 16), Qt::AlignRight, QString::number(static_cast<int>(x1)));
}

// ---------------------------------------------------------------- FocusPanel

FocusPanel::FocusPanel(preview::PreviewWidget* preview, QWidget* parent) : QWidget(parent), preview_(preview), focuser_(indi_) {
    buildUi();
    settle_.setSingleShot(true);
    connect(&settle_, &QTimer::timeout, this, [this] {
        if (sweep_ == Sweep::Settling) {
            collected_ = 0;
            sweep_ = Sweep::Collecting;
        }
    });
    connect(&focuser_, &devices::Focuser::readyChanged, this, [this](bool ready) {
        updateFocuserUi();
        if (ready && sweepWhenReady_) {
            sweepWhenReady_ = false;
            QTimer::singleShot(500, this, &FocusPanel::startSweep);
        }
    });
    connect(&focuser_, &devices::Focuser::positionChanged, this, &FocusPanel::updateFocuserUi);
    connect(&focuser_, &devices::Focuser::movingChanged, this, &FocusPanel::updateFocuserUi);
    connect(&focuser_, &devices::Focuser::arrived, this, [this] {
        if (sweep_ == Sweep::Moving) {
            sweep_ = Sweep::Settling;
            settle_.start(settleMs_->value());
        }
    });
    connect(&focuser_, &devices::Focuser::failed, this, [this](const QString& why) {
        device_->setText("⚠ " + why);
        if (sweep_ != Sweep::Idle) stopSweep(why);
    });
    connect(&indi_, &devices::IndiClient::serverConnected, this, [this] {
        connect_->setText("Disconnect");
        device_->setText("looking for a focuser…");
    });
    connect(&indi_, &devices::IndiClient::serverDisconnected, this, [this] {
        connect_->setText("Connect");
        device_->setText("not connected");
        updateFocuserUi();
    });
    connect(&indi_, &devices::IndiClient::errorOccurred, this, [this](const QString& e) { device_->setText("⚠ " + e); });
    updateFocuserUi();
}

FocusPanel::~FocusPanel() {
    // Members are destroyed before QObject's destructor cuts our connections, and closing the INDI socket emits
    // signals whose handlers touch members: detach first.
    indi_.disconnect(this);
    focuser_.disconnect(this);
    settle_.stop();
}

void FocusPanel::buildUi() {
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);

    // Focuser
    auto* fb = new QGroupBox("Focuser (INDI)");
    auto* fl = new QVBoxLayout(fb);
    auto* row = new QHBoxLayout;
    server_ = new QLineEdit("localhost:7624");
    connect_ = new QPushButton("Connect");
    row->addWidget(server_, 1);
    row->addWidget(connect_);
    fl->addLayout(row);
    device_ = new QLabel("not connected");
    device_->setWordWrap(true);
    fl->addWidget(device_);
    position_ = new QLabel("—");
    QFont big = position_->font();
    big.setPointSizeF(big.pointSizeF() * 1.8);
    big.setBold(true);
    position_->setFont(big);
    position_->setAlignment(Qt::AlignCenter);
    fl->addWidget(position_);
    auto* nudges = new QGridLayout;
    const int steps[] = {-1000, -200, -50, 50, 200, 1000};
    for (int i = 0; i < 6; ++i) {
        auto* b = new QPushButton(QString("%1%2").arg(steps[i] > 0 ? "+" : "").arg(steps[i]));
        connect(b, &QPushButton::clicked, this, [this, s = steps[i]] { focuser_.moveBy(s); });
        nudges->addWidget(b, i < 3 ? 0 : 1, i % 3);
        moveButtons_ << b;
    }
    fl->addLayout(nudges);
    auto* gotoRow = new QHBoxLayout;
    goto_ = new QSpinBox;
    goto_->setRange(0, 1'000'000);
    auto* go = new QPushButton("Go");
    auto* abort = new QPushButton("Stop");
    gotoRow->addWidget(goto_, 1);
    gotoRow->addWidget(go);
    gotoRow->addWidget(abort);
    fl->addLayout(gotoRow);
    moveButtons_ << go;
    auto* bl = new QFormLayout;
    backlash_ = new QSpinBox;
    backlash_->setRange(0, 5000);
    backlash_->setValue(600);
    backlash_->setSuffix(" steps");
    approach_ = new QComboBox;
    approach_->addItems({"outward (increasing)", "inward (decreasing)"});
    bl->addRow("Backlash", backlash_);
    bl->addRow("Final approach", approach_);
    fl->addLayout(bl);
    col->addWidget(fb);
    connect(connect_, &QPushButton::clicked, this, [this] {
        if (indi_.isConnected()) indi_.disconnectFromServer();
        else connectFocuser(server_->text());
    });
    connect(go, &QPushButton::clicked, this, [this] { focuser_.goTo(goto_->value()); });
    connect(abort, &QPushButton::clicked, this, [this] {
        if (sweep_ != Sweep::Idle) stopSweep("stopped");
        focuser_.abort();
    });
    connect(backlash_, &QSpinBox::valueChanged, this, [this](int v) { focuser_.setBacklash(v); });
    connect(approach_, &QComboBox::currentIndexChanged, this, [this](int i) { focuser_.setApproachOutward(i == 0); });

    // Live sharpness
    auto* lb = new QGroupBox("Live sharpness per channel");
    auto* ll = new QVBoxLayout(lb);
    auto* lrow = new QHBoxLayout;
    metric_ = new QComboBox;
    for (auto m : {analysis::Metric::Gradient, analysis::Metric::Laplacian, analysis::Metric::Variance, analysis::Metric::Fourier})
        metric_->addItem(analysis::metricName(m), static_cast<int>(m));
    smooth_ = new QSpinBox;
    smooth_->setRange(1, 99);
    smooth_->setValue(9);
    smooth_->setPrefix("median of ");
    smooth_->setSuffix(" frames");
    auto* reset = new QPushButton("Reset peaks");
    lrow->addWidget(metric_);
    lrow->addWidget(smooth_);
    lrow->addWidget(reset);
    ll->addLayout(lrow);
    bars_ = new RgbBars;
    ll->addWidget(bars_);
    liveText_ = new QLabel;
    liveText_->setWordWrap(true);
    ll->addWidget(liveText_);
    col->addWidget(lb);
    connect(reset, &QPushButton::clicked, this, &FocusPanel::resetPeaks);
    connect(metric_, &QComboBox::currentIndexChanged, this, &FocusPanel::resetPeaks);

    // Sweep
    auto* sb = new QGroupBox("Focus sweep");
    auto* sl = new QVBoxLayout(sb);
    auto* sf = new QGridLayout;
    auto spin = [](int lo, int hi, int v, const QString& suffix) {
        auto* s = new QSpinBox;
        s->setRange(lo, hi);
        s->setValue(v);
        s->setSuffix(suffix);
        return s;
    };
    range_ = spin(50, 20000, 800, " steps");
    step_ = spin(5, 5000, 100, " steps");
    frames_ = spin(5, 2000, 60, " frames");
    settleMs_ = spin(0, 10000, 400, " ms");
    sf->addWidget(new QLabel("± range"), 0, 0);
    sf->addWidget(range_, 0, 1);
    sf->addWidget(new QLabel("step"), 0, 2);
    sf->addWidget(step_, 0, 3);
    sf->addWidget(new QLabel("per position"), 1, 0);
    sf->addWidget(frames_, 1, 1);
    sf->addWidget(new QLabel("settle"), 1, 2);
    sf->addWidget(settleMs_, 1, 3);
    sl->addLayout(sf);
    fourier_ = new QCheckBox("Also score with fourier (slower)");
    fourier_->setChecked(true);
    sl->addWidget(fourier_);
    auto* srow = new QHBoxLayout;
    sweepBtn_ = new QPushButton("Start sweep around current position");
    sweepStatus_ = new QLabel;
    sweepStatus_->setWordWrap(true);
    srow->addWidget(sweepBtn_);
    sl->addLayout(srow);
    sl->addWidget(sweepStatus_);
    chart_ = new SweepChart;
    sl->addWidget(chart_);
    auto* rrow = new QHBoxLayout;
    resultMetric_ = new QComboBox;
    rrow->addWidget(new QLabel("Curves:"));
    rrow->addWidget(resultMetric_, 1);
    sl->addLayout(rrow);
    table_ = new QTableWidget(0, 5);
    table_->setHorizontalHeaderLabels({"metric", "R best ± err", "G best ± err", "B best ± err", "spread"});
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    table_->setHorizontalHeaderLabels({"metric", "R ± err", "G ± err", "B ± err", "spread"});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setMaximumHeight(130);
    sl->addWidget(table_);
    auto* trow = new QHBoxLayout;
    target_ = new QComboBox;
    for (auto t : {analysis::FocusTarget::Green, analysis::FocusTarget::Luminance, analysis::FocusTarget::Balanced, analysis::FocusTarget::Red,
                   analysis::FocusTarget::Blue})
        target_->addItem(analysis::focusTargetName(t), static_cast<int>(t));
    goTarget_ = new QPushButton("Go to");
    goTarget_->setEnabled(false);
    trow->addWidget(new QLabel("Focus for"));
    trow->addWidget(target_, 1);
    trow->addWidget(goTarget_);
    sl->addLayout(trow);
    col->addWidget(sb);
    col->addStretch(1);
    connect(sweepBtn_, &QPushButton::clicked, this, [this] {
        if (sweep_ == Sweep::Idle) startSweep();
        else stopSweep("stopped");
    });
    connect(resultMetric_, &QComboBox::currentIndexChanged, this, &FocusPanel::showSweepResult);
    connect(target_, &QComboBox::currentIndexChanged, this, &FocusPanel::showSweepResult);
    connect(goTarget_, &QPushButton::clicked, this, [this] {
        if (resultMetric_->count() == 0) return;
        const auto r = analysis_.result(static_cast<analysis::Metric>(resultMetric_->currentData().toInt()));
        if (auto pos = r.position(static_cast<analysis::FocusTarget>(target_->currentData().toInt())))
            focuser_.goTo(static_cast<int>(std::lround(*pos)));
    });
}

void FocusPanel::connectFocuser(const QString& hostPort) {
    server_->setText(hostPort);
    const auto parts = hostPort.split(':');
    indi_.connectToServer(parts.value(0, "localhost"), static_cast<quint16>(parts.value(1, "7624").toUInt()));
}

QString FocusPanel::focuserServer() const { return server_->text(); }
int FocusPanel::backlash() const { return backlash_->value(); }
bool FocusPanel::approachOutward() const { return approach_->currentIndex() == 0; }
void FocusPanel::setBacklash(int steps, bool outward) {
    backlash_->setValue(steps);
    approach_->setCurrentIndex(outward ? 0 : 1);
    focuser_.setBacklash(steps);
    focuser_.setApproachOutward(outward);
}
QString FocusPanel::metricName() const { return metric_->currentText(); }
void FocusPanel::setMetric(const QString& name) {
    if (const int i = metric_->findText(name); i >= 0) metric_->setCurrentIndex(i);
}

void FocusPanel::sweepWhenReady(int range, int step, int frames) {
    if (range > 0) range_->setValue(range);
    if (step > 0) step_->setValue(step);
    if (frames > 0) frames_->setValue(frames);
    sweepWhenReady_ = true;
    if (focuser_.ready()) {
        sweepWhenReady_ = false;
        startSweep();
    }
}

analysis::Metric FocusPanel::liveMetric() const { return static_cast<analysis::Metric>(metric_->currentData().toInt()); }

void FocusPanel::updateFocuserUi() {
    const bool ok = focuser_.ready();
    if (ok) {
        device_->setText(focuser_.device() + (focuser_.moving() ? QString("  · moving to %1").arg(focuser_.target()) : QString()));
        position_->setText(QString::number(focuser_.position()));
        if (!goto_->hasFocus()) goto_->setValue(focuser_.position());
        if (focuser_.maxPosition() > 0) goto_->setMaximum(focuser_.maxPosition());
    } else {
        position_->setText("—");
    }
    for (auto* b : moveButtons_) b->setEnabled(ok && sweep_ == Sweep::Idle);
    sweepBtn_->setEnabled(ok);
    goTarget_->setEnabled(ok && resultMetric_->count() > 0 && sweep_ == Sweep::Idle);
}

void FocusPanel::resetPeaks() {
    peak_ = {};
    recent_.clear();
}

void FocusPanel::onFrame(const LatestFrame::Frame& frame, Bayer bayer) {
    if (!isVisible() && sweep_ == Sweep::Idle) return;   // don't spend time when nobody's looking
    const auto& fi = frame.info;
    const auto px = std::span(frame.pixels).first(fi.bytes);
    analysis::FocusOptions opt;
    opt.metric = liveMetric();
    const auto s = analysis::scoreFrame(px, fi.roi.width, fi.roi.height, fi.bitDepth, bayer, opt);

    // live bars: median of the last N frames, as a fraction of each channel's best median so far
    if (s.channels > 0) {
        recent_.push_back(s);
        while (static_cast<int>(recent_.size()) > smooth_->value()) recent_.pop_front();
        std::array<double, 3> frac{};
        std::array<bool, 3> clipped{};
        for (int c = 0; c < s.channels; ++c) {
            std::vector<double> v;
            for (const auto& r : recent_) v.push_back(r.ch[static_cast<std::size_t>(c)].sharpness);
            const double m = median(v);
            peak_[static_cast<std::size_t>(c)] = std::max(peak_[static_cast<std::size_t>(c)], m);
            frac[static_cast<std::size_t>(c)] = peak_[static_cast<std::size_t>(c)] > 0 ? m / peak_[static_cast<std::size_t>(c)] : 0;
            clipped[static_cast<std::size_t>(c)] = s.ch[static_cast<std::size_t>(c)].clipped > 0.002;
        }
        bars_->setValues(s.channels, frac, clipped);
        if (s.channels == 3)
            liveText_->setText(QString("Dispersion R−B: %1, %2 px   ·   target at %3, %4")
                                   .arg(s.ch[0].cx - s.ch[2].cx, 0, 'f', 1)
                                   .arg(s.ch[0].cy - s.ch[2].cy, 0, 'f', 1)
                                   .arg(s.ch[1].cx, 0, 'f', 0)
                                   .arg(s.ch[1].cy, 0, 'f', 0));
        const QRectF win(s.windowX, s.windowY, s.windowW, s.windowH);
        preview_->setOverlay("focus-window", isVisible() ? preview::PreviewWidget::Overlay([win](QPainter& p, double k) {
            p.setPen(QPen(QColor(255, 210, 90, 150), 1.2 / k, Qt::DashLine));
            p.drawRect(win);
        })
                                                          : preview::PreviewWidget::Overlay{});
    } else {
        bars_->setValues(0, {}, {});
        liveText_->setText("No target found (too dark, or nothing in view)");
    }
    last_ = s;

    // sweep collection: every metric on the same frames
    if (sweep_ == Sweep::Collecting && s.channels > 0) {
        const int pos = focuser_.position();
        std::vector<analysis::Metric> ms = {analysis::Metric::Gradient, analysis::Metric::Laplacian, analysis::Metric::Variance};
        if (fourier_->isChecked()) ms.push_back(analysis::Metric::Fourier);
        for (auto m : ms) {
            opt.metric = m;
            const auto sm = m == liveMetric() ? s : analysis::scoreFrame(px, fi.roi.width, fi.roi.height, fi.bitDepth, bayer, opt);
            analysis_.add(pos, sm, m);
            if (csvOut_)
                *csvOut_ << pos << ',' << fi.sequence << ',' << analysis::metricName(m) << ',' << sm.ch[0].sharpness << ',' << sm.ch[1].sharpness << ','
                         << sm.ch[2].sharpness << ',' << sm.ch[0].clipped << ',' << sm.ch[1].clipped << ',' << sm.ch[2].clipped << '\n';
        }
        ++collected_;
        sweepStatus_->setText(QString("Position %1 of %2 (%3): frame %4 / %5")
                                  .arg(posIndex_ + 1)
                                  .arg(positions_.size())
                                  .arg(pos)
                                  .arg(collected_)
                                  .arg(frames_->value()));
        if (collected_ >= frames_->value()) {
            ++posIndex_;
            showSweepResult();
            nextSweepPosition();
        }
    }
}

void FocusPanel::startSweep() {
    if (!focuser_.ready()) return;
    startPosition_ = focuser_.position();
    positions_.clear();
    const int half = range_->value(), step = step_->value();
    for (int p = startPosition_ - half; p <= startPosition_ + half; p += step) positions_.push_back(p);
    if (!focuser_.approachOutward()) std::reverse(positions_.begin(), positions_.end());   // every move in the approach direction
    if (focuser_.maxPosition() > 0)
        std::erase_if(positions_, [&](int p) { return p < 0 || p > focuser_.maxPosition(); });
    if (positions_.size() < 3) {
        sweepStatus_->setText("Range too small for a sweep");
        return;
    }
    analysis_.clear();
    resultMetric_->clear();
    posIndex_ = 0;
    // per-frame scores to CSV next to the recordings
    QDir dir(outDir_.isEmpty() ? QDir::homePath() : outDir_);
    dir.mkpath(".");
    csv_ = std::make_unique<QFile>(dir.filePath("focus-sweep-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") + ".csv"));
    if (csv_->open(QIODevice::WriteOnly | QIODevice::Text)) {
        csvOut_ = std::make_unique<QTextStream>(csv_.get());
        *csvOut_ << "position,frame,metric,R,G,B,clipR,clipG,clipB\n";
    } else {
        csv_.reset();
    }
    sweepBtn_->setText("Stop sweep");
    sweep_ = Sweep::Moving;
    nextSweepPosition();
    updateFocuserUi();
}

void FocusPanel::nextSweepPosition() {
    if (posIndex_ >= positions_.size()) {
        stopSweep(QString("Done: %1 positions").arg(positions_.size()));
        return;
    }
    sweep_ = Sweep::Moving;
    focuser_.goTo(positions_[posIndex_]);
}

void FocusPanel::stopSweep(const QString& why) {
    const bool finished = posIndex_ >= positions_.size();
    sweep_ = Sweep::Idle;
    settle_.stop();
    if (csvOut_) csvOut_->flush();
    const QString file = csv_ ? csv_->fileName() : QString();
    csvOut_.reset();
    csv_.reset();
    sweepBtn_->setText("Start sweep around current position");
    sweepStatus_->setText(why + (file.isEmpty() ? QString() : "  ·  scores saved to " + QFileInfo(file).fileName()));
    showSweepResult();
    if (!finished && focuser_.ready()) focuser_.goTo(startPosition_);   // interrupted: put focus back
    updateFocuserUi();
}

void FocusPanel::showSweepResult() {
    // metric list (keep the selection)
    const QString keep = resultMetric_->currentText();
    {
        const QSignalBlocker b(resultMetric_);
        resultMetric_->clear();
        for (auto m : analysis_.metrics()) resultMetric_->addItem(analysis::metricName(m), static_cast<int>(m));
        const int i = resultMetric_->findText(keep.isEmpty() ? metric_->currentText() : keep);
        resultMetric_->setCurrentIndex(std::max(0, i));
    }
    const auto metrics = analysis_.metrics();
    table_->setRowCount(static_cast<int>(metrics.size()));
    for (int row = 0; row < static_cast<int>(metrics.size()); ++row) {
        const auto r = analysis_.result(metrics[static_cast<std::size_t>(row)]);
        table_->setItem(row, 0, new QTableWidgetItem(analysis::metricName(metrics[static_cast<std::size_t>(row)])));
        for (int c = 0; c < 3; ++c) {
            const auto& cv = r.ch[static_cast<std::size_t>(c)];
            QString t = c < r.channels && cv.best ? QString("%1 ± %2%3").arg(*cv.best, 0, 'f', 0).arg(cv.bestError, 0, 'f', 0).arg(cv.atEdge ? " (edge)" : "")
                                                  : QString("—");
            table_->setItem(row, c + 1, new QTableWidgetItem(t));
        }
        table_->setItem(row, 4, new QTableWidgetItem(r.spread ? QString::number(*r.spread, 'f', 0) : "—"));
    }
    if (resultMetric_->count() == 0) {
        chart_->setResult({}, std::nullopt);
        return;
    }
    const auto r = analysis_.result(static_cast<analysis::Metric>(resultMetric_->currentData().toInt()));
    const auto pos = r.position(static_cast<analysis::FocusTarget>(target_->currentData().toInt()));
    chart_->setResult(r, pos);
    goTarget_->setText(pos ? QString("Go to %1").arg(*pos, 0, 'f', 0) : "Go to");
    goTarget_->setEnabled(pos && focuser_.ready() && sweep_ == Sweep::Idle);
}

}  // namespace slopcap::app

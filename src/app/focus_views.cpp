#include "app/focus_views.hpp"

#include "app/theme.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace slopcap::app {

namespace {

using analysis::FocusTarget;
using analysis::Metric;

const char* kChannelName[3] = {"Red", "Green", "Blue"};
const Qt::PenStyle kChannelLine[3] = {Qt::DashLine, Qt::SolidLine, Qt::DotLine};

QString thousands(double v) { return QLocale(QLocale::English).toString(static_cast<qlonglong>(std::lround(v))); }
QString signedSteps(double v) {
    const long r = std::lround(v);
    return r > 0 ? QString("+%1").arg(r) : r < 0 ? QString("−%1").arg(-r) : QString("0");
}

const char* metricLabel(Metric m) {
    switch (m) {
        case Metric::Gradient: return "Edge contrast (gradient)";
        case Metric::Laplacian: return "Fine detail (Laplacian)";
        case Metric::Variance: return "Contrast (normalised variance)";
        case Metric::Fourier: return "Fourier mid-band";
    }
    return "";
}

QSpinBox* monoSpin(int lo, int hi, int v, int step = 1) {
    auto* s = new QSpinBox;
    s->setRange(lo, hi);
    s->setValue(v);
    s->setSingleStep(step);
    s->setGroupSeparatorShown(true);
    s->setProperty("role", "mono");
    return s;
}

QWidget* labelled(const QString& caption, QWidget* w) {
    auto* box = new QWidget;
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    v->addWidget(theme::label(caption, "muted"));
    v->addWidget(w);
    return box;
}

}  // namespace

// ---------------------------------------------------------------- ChannelBars

ChannelBars::ChannelBars(QWidget* parent) : QWidget(parent) { setMinimumHeight(60); }

void ChannelBars::setValues(int channels, const std::array<double, 3>& fraction, const std::array<bool, 3>& clipped) {
    channels_ = channels;
    v_ = fraction;
    clipped_ = clipped;
    update();
}

void ChannelBars::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int n = channels_ == 1 ? 1 : 3;
    const double rowH = height() / 3.0;
    const QFont mono = theme::monoFont();
    p.setFont(mono);
    for (int c = 0; c < n; ++c) {
        const double cy = rowH * c + rowH / 2;
        if (channels_ == 1) {
            p.setPen(Qt::NoPen);
            p.setBrush(theme::kText);
            p.drawEllipse(QPointF(6, cy), 5, 5);
        } else {
            theme::drawChannelMarker(p, c, 6, cy, 10);
        }
        const QRectF track(22, cy - 4, width() - 22 - 52, 8);
        p.setPen(Qt::NoPen);
        p.setBrush(theme::kLine);
        p.drawRoundedRect(track, 4, 4);
        const double f = std::clamp(channels_ ? v_[static_cast<std::size_t>(c)] : 0.0, 0.0, 1.0);
        // bar = live median; the white tick = the best so far (fractions are relative to it)
        p.setBrush(channels_ == 1 ? theme::kText : theme::kChannel[c]);
        p.drawRoundedRect(QRectF(track.left(), track.top(), track.width() * 0.96 * f, track.height()), 4, 4);
        p.setBrush(theme::kText);
        p.drawRect(QRectF(track.left() + track.width() * 0.96 - 1, track.top() - 3, 2, track.height() + 6));
        const bool clip = clipped_[static_cast<std::size_t>(c)];
        p.setPen(clip ? theme::kRecText : theme::kText);
        const QString letter = channels_ == 1 ? "L" : QString("RGB").mid(c, 1);
        p.drawText(QRectF(width() - 48, cy - rowH / 2, 48, rowH), Qt::AlignVCenter | Qt::AlignRight,
                   !channels_ ? QString("%1 —").arg(letter) : clip ? QString("%1 clip").arg(letter) : QString("%1 .%2").arg(letter).arg(std::min(99, static_cast<int>(std::lround(f * 100))), 2, 10, QChar('0')));
    }
}

// ---------------------------------------------------------------- FocuserControls

FocuserControls::FocuserControls(FocusController& fc, QWidget* parent) : QWidget(parent), fc_(fc) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    title_ = theme::section("Focuser");
    v->addWidget(title_);

    offline_ = new QWidget;
    auto* ol = new QHBoxLayout(offline_);
    ol->setContentsMargins(0, 0, 0, 0);
    offlineText_ = theme::label("Not connected", "muted");
    offlineText_->setWordWrap(true);
    auto* conn = theme::button("Connect");
    ol->addWidget(offlineText_, 1);
    ol->addWidget(conn);
    v->addWidget(offline_);
    connect(conn, &QPushButton::clicked, this, [this] { fc_.connectFocuser(fc_.server()); });

    posBox_ = new QWidget;
    auto* posRow = new QHBoxLayout(posBox_);
    posRow->setContentsMargins(0, 0, 0, 0);
    position_ = theme::label("—", "monoLarge");
    max_ = theme::label("", "dim");
    posRow->addWidget(position_);
    posRow->addStretch(1);
    posRow->addWidget(max_, 0, Qt::AlignBottom);
    v->addWidget(posBox_);

    // Outside nudges_, so it stays live during a sweep (the nudges are disabled then)
    stop_ = theme::button("STOP", "stop");
    connect(stop_, &QPushButton::clicked, this, [this] { fc_.stopFocuser(); });
    v->addWidget(stop_);

    nudges_ = new QWidget;
    auto* g = new QGridLayout(nudges_);
    g->setContentsMargins(0, 0, 0, 0);
    g->setSpacing(6);
    const int steps[] = {-100, -10, 10, 100};
    for (int i = 0; i < 4; ++i) {
        auto* b = theme::button(steps[i] > 0 ? QString("+%1").arg(steps[i]) : QString("−%1").arg(-steps[i]), "mono");
        b->setMinimumHeight(30);
        connect(b, &QPushButton::clicked, this, [this, s = steps[i]] { fc_.focuser().moveBy(s); });
        g->addWidget(b, 0, i);
    }
    v->addWidget(nudges_);

    connect(&fc_, &FocusController::focuserChanged, this, &FocuserControls::refresh);
    // A sweep that ends where it is (complete) moves nothing afterwards: the step buttons must come back anyway.
    connect(&fc_, &FocusController::sweepStateChanged, this, &FocuserControls::refresh);
    refresh();
}

void FocuserControls::refresh() {
    auto& f = fc_.focuser();
    const bool ok = f.ready();
    title_->setText(QString("FOCUSER%1").arg(ok ? " · " + f.device().toUpper() : ""));
    offline_->setVisible(!ok);
    offlineText_->setText(fc_.indi().isConnected() ? "INDI connected · no focuser yet" : "Not connected · " + fc_.server());
    posBox_->setVisible(ok);
    stop_->setVisible(ok);
    nudges_->setVisible(ok);
    nudges_->setEnabled(!fc_.sweeping());
    position_->setText(ok ? thousands(f.position()) : "—");
    max_->setText(ok && f.maxPosition() > 0 ? "of " + thousands(f.maxPosition()) + (f.moving() ? " · moving" : "") : "");
}

// ---------------------------------------------------------------- FocuserSection

FocuserSection::FocuserSection(FocusController& fc, QWidget* parent) : QWidget(parent), fc_(fc) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    v->addWidget(new FocuserControls(fc_));

    auto* liveCard = theme::card();
    auto* lv = new QVBoxLayout(liveCard);
    lv->setContentsMargins(10, 10, 10, 10);
    lv->setSpacing(6);
    auto* hdr = new QHBoxLayout;
    hdr->addWidget(theme::label("Live sharpness", "dim"));
    hdr->addStretch(1);
    hdr->addWidget(theme::label("▏ = best so far", "dim"));
    lv->addLayout(hdr);
    bars_ = new ChannelBars;
    lv->addWidget(bars_);
    v->addWidget(liveCard);

    note_ = theme::label("", "muted");
    note_->setWordWrap(true);
    v->addWidget(note_);
    auto* links = new QHBoxLayout;
    links->setSpacing(6);
    auto* sweep = theme::button("Run RGB focus sweep →");
    auto* settings = theme::button("⚙");
    settings->setToolTip("Focuser settings: server, backlash, approach direction, optics");
    settings->setFixedWidth(36);
    links->addWidget(sweep, 1);
    links->addWidget(settings);
    v->addLayout(links);
    connect(sweep, &QPushButton::clicked, this, &FocuserSection::openFocusMode);
    connect(settings, &QPushButton::clicked, this, &FocuserSection::openSettings);

    connect(&fc_, &FocusController::focuserChanged, this, &FocuserSection::refresh);
    connect(&fc_, &FocusController::liveUpdated, this, [this] {
        const auto& l = fc_.live();
        bars_->setValues(l.last.channels, l.fraction, l.clipped);
    });
    refresh();
}

void FocuserSection::refresh() {
    auto& f = fc_.focuser();
    note_->setText(f.ready() ? QString("Backlash %1 steps — moves finish %2.").arg(f.backlash()).arg(f.approachOutward() ? "outward" : "inward") : "");
}

// ---------------------------------------------------------------- SweepPanel

SweepPanel::SweepPanel(FocusController& fc, QWidget* parent) : QWidget(parent), fc_(fc) {
    // The settings scroll; the status and Run stay at the bottom, in view however tall the column gets
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body);
    v->setContentsMargins(16, 16, 16, 8);
    v->setSpacing(14);
    header_ = new QVBoxLayout;
    header_->setContentsMargins(0, 0, 0, 0);
    header_->setSpacing(10);
    v->addLayout(header_);
    v->addWidget(theme::section("Sweep"));
    auto* seg = new QWidget;
    auto* sh = new QHBoxLayout(seg);
    sh->setContentsMargins(0, 0, 0, 0);
    sh->setSpacing(6);
    presets_ = new QButtonGroup(this);
    auto* coarse = theme::button("Coarse ±1500");
    auto* fine = theme::button("Fine ±350");
    for (auto* b : {coarse, fine}) {
        b->setCheckable(true);
        sh->addWidget(b);
    }
    presets_->addButton(coarse, 0);
    presets_->addButton(fine, 1);
    fine->setChecked(true);
    v->addWidget(seg);
    connect(coarse, &QPushButton::clicked, this, [this] { preset(1500, 150); });
    connect(fine, &QPushButton::clicked, this, [this] { preset(350, 50); });

    auto* grid = new QGridLayout;
    grid->setSpacing(8);
    from_ = monoSpin(0, 1'000'000, 0, 50);
    to_ = monoSpin(0, 1'000'000, 0, 50);
    step_ = monoSpin(1, 10000, 50, 10);
    frames_ = monoSpin(5, 2000, 60, 10);
    settle_ = monoSpin(0, 10000, 400, 100);
    settle_->setSuffix(" ms");
    grid->addWidget(labelled("From", from_), 0, 0);
    grid->addWidget(labelled("To", to_), 0, 1);
    grid->addWidget(labelled("Step", step_), 1, 0);
    grid->addWidget(labelled("Frames / step", frames_), 1, 1);
    grid->addWidget(labelled("Settle", settle_), 2, 0);
    frames_->setToolTip("Frames scored at each position");
    settle_->setToolTip("Wait after each move before scoring (the image stops shaking)");
    v->addLayout(grid);
    follows_ = theme::label("", "dim");
    follows_->setWordWrap(true);
    v->addWidget(follows_);
    // By hand, From / To stay put; the presets centre them on the focuser again and follow it from then on
    for (auto* s : {from_, to_})
        connect(s, &QSpinBox::valueChanged, this, [this] {
            if (setting_) return;
            follow_ = false;
            refresh();
        });

    metric_ = new QComboBox;
    for (auto m : {Metric::Gradient, Metric::Laplacian, Metric::Variance, Metric::Fourier}) metric_->addItem(metricLabel(m), static_cast<int>(m));
    v->addWidget(labelled("Score by", metric_));
    connect(metric_, &QComboBox::currentIndexChanged, this, [this] {
        fc_.setLiveMetric(static_cast<Metric>(metric_->currentData().toInt()));
        emit displayChanged();
    });

    auto* chBox = new QWidget;
    auto* cv = new theme::FlowLayout(chBox, 12);
    auto* chLabel = theme::label("Channels", "muted");
    chLabel->setToolTip("Raw Bayer planes, not interpolated: green is G1 and G2 together");
    cv->addWidget(chLabel);
    const char* names[3] = {"Red", "Green", "Blue"};
    for (int c = 0; c < 3; ++c) {
        channel_[static_cast<std::size_t>(c)] = new QCheckBox(names[c]);
        channel_[static_cast<std::size_t>(c)]->setChecked(true);
        channel_[static_cast<std::size_t>(c)]->setStyleSheet(QString("QCheckBox { color: %1; } QCheckBox::indicator:checked { background: %2; border-color: %2; }")
                                                                 .arg(theme::kText.name(), theme::kChannel[c].name()));
        cv->addWidget(channel_[static_cast<std::size_t>(c)]);
        connect(channel_[static_cast<std::size_t>(c)], &QCheckBox::toggled, this, &SweepPanel::displayChanged);
    }
    v->addWidget(chBox);
    fourier_ = new QCheckBox("Also score Fourier (slower)");
    fourier_->setChecked(true);
    v->addWidget(fourier_);
    // (check box texts don't wrap: short enough for the narrowest column)
    extend_ = new QCheckBox("Carry on past the ends");
    extend_->setChecked(true);
    extend_->setToolTip("While the sharpest position is at an end of the range, add 3 positions past it at a time (up to "
                        "twice the range again), until the curve comes down on both sides of its peak");
    v->addWidget(extend_);
    goToBest_ = new QCheckBox("Go to the best focus after");
    goToBest_->setChecked(true);
    goToBest_->setToolTip("The position for the channel chosen under Focus on (right); without a peak, the sharpest position "
                          "swept. Off: the focuser stays at the last position.");
    v->addWidget(goToBest_);

    v->addStretch(1);
    status_ = theme::label("", "muted");
    status_->setWordWrap(true);
    run_ = theme::button("Run sweep");
    run_->setMinimumHeight(40);
    auto* scroll = new QScrollArea;
    scroll->setWidget(body);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll, 1);
    auto* bar = new QVBoxLayout;
    bar->setContentsMargins(16, 8, 16, 16);
    bar->setSpacing(8);
    bar->addWidget(status_);
    bar->addWidget(run_);
    outer->addLayout(bar);
    connect(run_, &QPushButton::clicked, this, [this] {
        if (fc_.sweeping()) {
            fc_.stopSweep();
            return;
        }
        SweepSettings s;
        s.from = from_->value(), s.to = to_->value(), s.step = step_->value(), s.frames = frames_->value(), s.settleMs = settle_->value();
        s.fourier = fourier_->isChecked();
        s.extend = extend_->isChecked();
        s.goToBest = goToBest_->isChecked();
        fc_.startSweep(s);
    });
    connect(&fc_, &FocusController::sweepProgress, status_, &QLabel::setText);
    connect(&fc_, &FocusController::sweepStateChanged, this, &SweepPanel::refresh);
    connect(&fc_, &FocusController::focuserChanged, this, &SweepPanel::refresh);
    refresh();
}

void SweepPanel::setHeader(QWidget* w) { header_->addWidget(w); }

std::array<bool, 3> SweepPanel::channelsShown() const {
    return {channel_[0]->isChecked(), channel_[1]->isChecked(), channel_[2]->isChecked()};
}

void SweepPanel::preset(int halfRange, int step) {
    const int at = fc_.focuser().ready() ? fc_.focuser().position() : (from_->value() + to_->value()) / 2;
    setting_ = true;
    from_->setValue(std::max(0, at - halfRange));
    to_->setValue(at + halfRange);
    step_->setValue(step);
    setting_ = false;
    follow_ = true;
    refresh();
}

void SweepPanel::centre() {
    auto& f = fc_.focuser();
    // Not while sweeping (the range on screen is the one running), nor halfway through a move: where it ends up counts
    if (!follow_ || !f.ready() || f.moving() || fc_.sweeping()) return;
    const int half = (to_->value() - from_->value()) / 2;
    if ((from_->value() + to_->value()) / 2 == f.position()) return;
    setting_ = true;
    from_->setValue(std::max(0, f.position() - half));
    to_->setValue(f.position() + half);
    setting_ = false;
}

void SweepPanel::refresh() {
    auto& f = fc_.focuser();
    const bool ok = f.ready();
    if (ok && from_->value() == 0 && to_->value() == 0) preset(350, 50);   // first time: centre on the focuser
    centre();
    const QString backlash = ok ? QString(" · backlash take-up %1 steps").arg(f.backlash()) : QString();
    follows_->setText((follow_ ? "Centred on the focuser, wherever it goes" : "Fixed range (set by hand): a preset centres it again") + backlash);
    run_->setEnabled(ok);
    run_->setText(fc_.sweeping() ? "Stop sweep" : ok ? "Run sweep" : "Connect the focuser first");
    run_->setProperty("variant", fc_.sweeping() ? "stop" : "accent");
    theme::restyle(run_);
    for (auto* w : std::initializer_list<QWidget*>{from_, to_, step_, frames_, settle_, fourier_, extend_, goToBest_}) w->setEnabled(!fc_.sweeping());
}

// ---------------------------------------------------------------- FocusChart

FocusChart::FocusChart(QWidget* parent) : QWidget(parent) { setMinimumHeight(320); }

void FocusChart::setData(const analysis::SweepResult& r, std::array<bool, 3> shown, std::optional<double> chosen, std::optional<double> zone) {
    r_ = r;
    shown_ = shown;
    chosen_ = chosen;
    zone_ = zone;
    update();
}

void FocusChart::setProgress(std::optional<std::pair<int, int>> span, std::optional<int> at) {
    span_ = span;
    at_ = at;
    update();
}

void FocusChart::setCompact(bool on) {
    compact_ = on;
    setMinimumHeight(on ? 60 : 320);
    update();
}

void FocusChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QFont mono = theme::monoFont();
    QFont small = mono;
    small.setPixelSize(11);
    const QRectF frame = compact_ ? QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5) : QRectF(rect()).adjusted(44, 0, -1, -26);
    p.setPen(QPen(theme::kLine, 1));
    p.setBrush(compact_ ? Qt::NoBrush : QBrush(theme::kPanel));
    p.drawRoundedRect(frame, compact_ ? 4 : 8, compact_ ? 4 : 8);
    const bool empty = r_.channels == 0 || r_.ch[0].points.empty();
    if (empty && !span_) {
        if (compact_) return;
        p.setPen(theme::kDim);
        p.drawText(frame, Qt::AlignCenter, "No sweep yet — connect the focuser, set a range and Run sweep");
        return;
    }
    const QRectF plot = frame.adjusted(compact_ ? 6 : 12, compact_ ? 6 : 12, compact_ ? -6 : -12, compact_ ? -6 : -12);
    // x: the whole planned range while a sweep runs (the points fill it in), else what was swept
    double x0 = 1e18, x1 = -1e18;
    if (span_) x0 = span_->first, x1 = span_->second;
    for (int c = 0; c < r_.channels; ++c)
        for (const auto& pt : r_.ch[static_cast<std::size_t>(c)].points)
            x0 = std::min<double>(x0, pt.position), x1 = std::max<double>(x1, pt.position);
    if (x1 <= x0) x1 = x0 + 1;
    // Each channel normalised to its own best (the fitted peak, when it lies in the range). The floor goes as low as
    // the curves do, so a wide sweep shows its whole V and not only the top 40 %.
    auto normOf = [&](const analysis::ChannelCurve& cv) {
        double top = 0;
        for (const auto& pt : cv.points) top = std::max(top, pt.bestQuarter);
        if (cv.best && *cv.best >= x0 && *cv.best <= x1) top = std::max(top, cv.fitted(*cv.best));
        return top;
    };
    double lowest = 0.6;
    for (int c = 0; c < r_.channels; ++c) {
        const auto& cv = r_.ch[static_cast<std::size_t>(c)];
        const double norm = normOf(cv);
        if (!shown_[static_cast<std::size_t>(c)] || norm <= 0) continue;
        for (const auto& pt : cv.points) lowest = std::min(lowest, pt.bestQuarter / norm);
    }
    const double y0 = std::clamp(std::floor((lowest - 0.02) * 10) / 10, 0.0, 0.6), y1 = 1.02;
    auto X = [&](double pos) { return plot.left() + (pos - x0) / (x1 - x0) * plot.width(); };
    auto Y = [&](double v) { return plot.bottom() - (v - y0) / (y1 - y0) * plot.height(); };

    // grid + axes labels
    p.setFont(small);
    for (int i = 0; i <= 4; ++i) {
        const double v = 1.0 - (1.0 - y0) * i / 4;
        p.setPen(QPen(QColor(0x1f, 0x23, 0x28), 1));
        if (i > 0 && i < 4) p.drawLine(QPointF(plot.left(), Y(v)), QPointF(plot.right(), Y(v)));
        if (compact_) continue;
        p.setPen(theme::kDim);
        p.drawText(QRectF(0, Y(v) - 8, 38, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(v, 'f', 2));
    }
    for (int i = 0; i <= 4 && !compact_; ++i) {
        const double pos = x0 + (x1 - x0) * i / 4;
        p.setPen(theme::kDim);
        const double x = X(pos);
        const QRectF r = i == 0 ? QRectF(x, frame.bottom() + 6, 80, 16) : i == 4 ? QRectF(x - 80, frame.bottom() + 6, 80, 16) : QRectF(x - 40, frame.bottom() + 6, 80, 16);
        p.drawText(r, i == 0 ? Qt::AlignLeft : i == 4 ? Qt::AlignRight : Qt::AlignHCenter, thousands(pos));
    }
    // where the focuser is while the sweep runs
    if (at_ && *at_ >= x0 && *at_ <= x1) {
        p.setPen(QPen(theme::kMuted, 1, Qt::DashLine));
        p.drawLine(QPointF(X(*at_), frame.top() + 2), QPointF(X(*at_), frame.bottom() - 2));
    }
    // critical focus zone around the chosen position
    if (chosen_ && zone_) {
        const QRectF z(X(*chosen_ - *zone_), frame.top() + 1, X(*chosen_ + *zone_) - X(*chosen_ - *zone_), frame.height() - 2);
        p.fillRect(z.intersected(frame), QColor(242, 165, 65, 26));
        p.setPen(QPen(theme::kAccentLine, 1));
        p.drawLine(z.topLeft(), z.bottomLeft());
        p.drawLine(z.topRight(), z.bottomRight());
    }
    p.save();
    p.setClipRect(frame.adjusted(1, 1, -1, -1));
    for (int c = 0; c < r_.channels; ++c) {
        if (!shown_[static_cast<std::size_t>(c)]) continue;
        const auto& cv = r_.ch[static_cast<std::size_t>(c)];
        const double norm = normOf(cv);
        if (norm <= 0) continue;
        const QColor col = r_.channels == 1 ? theme::kText : theme::kChannel[c];
        if (cv.best) {
            // the parabola describes the top of the curve only: draw it where it's above the floor
            QPainterPath path;
            bool pen = false;
            for (int i = 0; i <= 120; ++i) {
                const double pos = x0 + (x1 - x0) * i / 120.0, y = cv.fitted(pos) / norm;
                if (y < y0) {
                    pen = false;
                    continue;
                }
                const QPointF q(X(pos), Y(y));
                if (!pen) path.moveTo(q);
                else path.lineTo(q);
                pen = true;
            }
            QPen line(col, compact_ ? 1.5 : 2.5, r_.channels == 1 ? Qt::SolidLine : kChannelLine[c]);
            line.setCapStyle(Qt::RoundCap);
            p.setPen(line);
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
            p.setPen(QPen(col, 1, r_.channels == 1 ? Qt::SolidLine : kChannelLine[c]));
            p.drawLine(QPointF(X(*cv.best), frame.top()), QPointF(X(*cv.best), frame.bottom()));
        }
        const double marker = compact_ ? 6 : 10;
        for (const auto& pt : cv.points) {
            const double x = X(pt.position), y = Y(pt.bestQuarter / norm);
            p.setPen(QPen(theme::kGround, compact_ ? 1 : 1.5));
            if (r_.channels == 1) {
                p.setBrush(col);
                p.drawEllipse(QPointF(x, y), marker * 0.45, marker * 0.45);
            } else {
                theme::drawChannelMarker(p, c, x, y, marker);
            }
        }
    }
    p.restore();
    if (chosen_ && !compact_) {
        const QString t = "focus " + thousands(*chosen_);
        p.setFont(mono);
        const QFontMetrics fm(mono);
        const double w = fm.horizontalAdvance(t) + 16;
        const QRectF tag(std::clamp(X(*chosen_) - w / 2, frame.left() + 4, frame.right() - w - 4), frame.top() + 8, w, 22);
        p.setPen(QPen(theme::kAccentLine, 1));
        p.setBrush(theme::kAccentBg);
        p.drawRoundedRect(tag, 4, 4);
        p.setPen(theme::kAccent);
        p.drawText(tag, Qt::AlignCenter, t);
    }
}

// ---------------------------------------------------------------- FocusChartPanel

FocusChartPanel::FocusChartPanel(FocusController& fc, QWidget* parent) : QWidget(parent), fc_(fc) {
    setObjectName("ground");
    setAttribute(Qt::WA_StyledBackground);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 20, 24, 20);
    v->setSpacing(14);
    auto* title = theme::label("Sharpness per channel vs focuser position", "title");
    title->setWordWrap(true);
    v->addWidget(title);
    subtitle_ = theme::label("each channel normalised to its own best", "muted");
    subtitle_->setWordWrap(true);
    v->addWidget(subtitle_);

    // legend: line style + marker per channel, the zone swatch
    auto* legend = new QHBoxLayout;
    legend->setSpacing(16);
    const char* style[3] = {"dashed", "solid", "dotted"};
    for (int c = 0; c < 3; ++c) {
        auto* sw = new QLabel;
        QPixmap pm(44 * devicePixelRatioF(), 12 * devicePixelRatioF());
        pm.setDevicePixelRatio(devicePixelRatioF());
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(theme::kChannel[c], 2.5, kChannelLine[c]));
            p.drawLine(QPointF(0, 6), QPointF(28, 6));
            theme::drawChannelMarker(p, c, 37, 6, 9);
        }
        sw->setPixmap(pm);
        auto* item = new QHBoxLayout;
        item->setSpacing(6);
        item->addWidget(sw);
        item->addWidget(theme::label(QString("%1 · %2").arg(kChannelName[c], style[c]), "muted"));
        legend->addLayout(item);
    }
    auto* zoneSw = new QLabel;
    zoneSw->setFixedSize(18, 10);
    zoneSw->setStyleSheet(QString("background: rgba(242,165,65,40); border: 1px solid %1;").arg(theme::kAccentLine.name()));
    legend->addWidget(zoneSw);
    auto* zoneText = theme::label("Critical focus zone at chosen position", "muted");
    zoneText->setWordWrap(true);
    legend->addWidget(zoneText, 1);
    legend->addStretch(1);
    v->addLayout(legend);

    chart_ = new FocusChart;
    v->addWidget(chart_, 1);

    auto* cards = new QHBoxLayout;
    cards->setSpacing(10);
    auto mk = [&](const QString& cap, QLabel*& val, QLabel** capOut = nullptr) {
        auto* c = new QFrame;
        c->setStyleSheet(QString("QFrame { background: %1; border: 1px solid %2; border-radius: 8px; }").arg(theme::kPanel.name(), theme::kLine.name()));
        auto* cl = new QVBoxLayout(c);
        cl->setContentsMargins(12, 10, 12, 10);
        auto* capL = theme::label(cap, "dim");
        capL->setStyleSheet("border: none;");
        capL->setWordWrap(true);
        cl->addWidget(capL);
        val = theme::label("—", "mono");
        val->setStyleSheet("border: none; font-size: 16px;");
        cl->addWidget(val);
        if (capOut) *capOut = capL;
        cards->addWidget(c, 1);
    };
    mk("Channel spread (R − B)", spread_);
    mk("Critical focus zone", zone_, &zoneCaption_);
    mk("Worst channel at chosen focus", worst_);
    v->addLayout(cards);
    footnote_ = theme::label("", "dim");
    footnote_->setWordWrap(true);
    v->addWidget(footnote_);
}

void FocusChartPanel::update(Metric m, std::array<bool, 3> shown, std::optional<double> chosen) {
    const auto r = fc_.sweep().result(m);
    const auto zone = fc_.criticalZoneSteps();
    chart_->setData(r, shown, chosen, zone);
    chart_->setProgress(fc_.sweepSpan(), fc_.sweeping() && fc_.focuser().ready() ? std::optional<int>(fc_.focuser().position()) : std::nullopt);
    subtitle_->setText(QString("%1 · each channel normalised to its own best · best quarter of %2 frames")
                           .arg(metricLabel(m))
                           .arg(fc_.sweepSettings().frames));
    spread_->setText(r.spread ? QString("%1 steps").arg(thousands(*r.spread)) : "—");
    zoneCaption_->setText(fc_.focalRatio() > 0 ? QString("Critical focus zone (f/%1)").arg(fc_.focalRatio(), 0, 'f', 0) : "Critical focus zone");
    zone_->setText(zone ? QString("±%1 steps").arg(std::lround(*zone)) : "set optics in Settings");
    footnote_->setText(zone ? QString("±2.44 λN² at 550 nm, converted with the focuser step size from Settings.")
                            : QString("The zone needs the focal ratio and the focuser's µm per step (Settings)."));
    if (chosen && r.channels == 3) {
        int worstC = 0;
        double worstOff = -1;
        for (int c = 0; c < 3; ++c)
            if (const auto& b = r.ch[static_cast<std::size_t>(c)].best; b && std::abs(*chosen - *b) > worstOff) worstOff = std::abs(*chosen - *b), worstC = c;
        worst_->setText(worstOff >= 0 ? QString("%1 %2 steps off").arg(kChannelName[worstC]).arg(std::lround(worstOff)) : "—");
        const bool inside = zone && worstOff <= *zone;
        worst_->setStyleSheet(QString("border: none; font-size: 16px; color: %1;").arg((zone ? (inside ? theme::kGood : theme::kAccent) : theme::kText).name()));
    } else {
        worst_->setText("—");
    }
}

// ---------------------------------------------------------------- FocusResultsPanel

FocusResultsPanel::FocusResultsPanel(FocusController& fc, QWidget* parent) : QWidget(parent), fc_(fc) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(16, 16, 16, 16);
    v->setSpacing(14);
    v->addWidget(theme::section("Best focus per channel"));
    auto* table = new QGridLayout;
    table->setHorizontalSpacing(8);
    table->setVerticalSpacing(6);
    table->addWidget(theme::label("Channel", "dim"), 0, 0);
    auto* h1 = theme::label("Best", "dim");
    auto* h2 = theme::label("vs G", "dim");
    h1->setAlignment(Qt::AlignRight);
    h2->setAlignment(Qt::AlignRight);
    table->addWidget(h1, 0, 1);
    table->addWidget(h2, 0, 2);
    for (int c = 0; c < 3; ++c) {
        auto* name = new QLabel;
        QPixmap pm(12 * devicePixelRatioF(), 12 * devicePixelRatioF());
        pm.setDevicePixelRatio(devicePixelRatioF());
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            theme::drawChannelMarker(p, c, 6, 6, 10);
        }
        auto* nameRow = new QWidget;
        auto* nh = new QHBoxLayout(nameRow);
        nh->setContentsMargins(0, 0, 0, 0);
        nh->setSpacing(8);
        name->setPixmap(pm);
        nh->addWidget(name);
        nh->addWidget(new QLabel(kChannelName[c]));
        nh->addStretch(1);
        table->addWidget(nameRow, c + 1, 0);
        best_[static_cast<std::size_t>(c)] = theme::label("—", "mono");
        vsG_[static_cast<std::size_t>(c)] = theme::label("—", "mono");
        best_[static_cast<std::size_t>(c)]->setAlignment(Qt::AlignRight);
        vsG_[static_cast<std::size_t>(c)]->setAlignment(Qt::AlignRight);
        table->addWidget(best_[static_cast<std::size_t>(c)], c + 1, 1);
        table->addWidget(vsG_[static_cast<std::size_t>(c)], c + 1, 2);
    }
    table->setColumnStretch(0, 1);
    v->addLayout(table);

    verdict_ = new QFrame;
    verdict_->setProperty("role", "note");
    auto* vl = new QVBoxLayout(verdict_);
    vl->setContentsMargins(12, 10, 12, 10);
    verdictText_ = new QLabel;
    verdictText_->setWordWrap(true);
    verdictText_->setTextFormat(Qt::RichText);
    vl->addWidget(verdictText_);
    v->addWidget(verdict_);

    v->addWidget(theme::section("Focus on"));
    const struct { FocusTarget t; const char* label; const char* hint; } defs[5] = {
        {FocusTarget::Green, "Green", "most of the luminance detail"},
        {FocusTarget::Luminance, "Luminance-weighted", "0.30 R + 0.59 G + 0.11 B"},
        {FocusTarget::Balanced, "Balanced", "smallest worst-channel defocus"},
        {FocusTarget::Red, "Red", "IR / R-pass processing"},
        {FocusTarget::Blue, "Blue", "blue-channel detail"},
    };
    auto* group = new QButtonGroup(this);
    for (int i = 0; i < 5; ++i) {
        auto* b = theme::button(QString("%1\n%2").arg(defs[i].label, defs[i].hint), "option");
        b->setCheckable(true);
        b->setMinimumHeight(40);
        group->addButton(b, i);
        modes_[static_cast<std::size_t>(i)] = b;
        v->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, t = defs[i].t] {
            target_ = t;
            fc_.setTarget(t);   // where a finished sweep goes
            emit targetChanged();
        });
    }
    modes_[0]->setChecked(true);
    go_ = theme::button("Go to —", "accent");
    go_->setMinimumHeight(40);
    v->addWidget(go_);
    connect(go_, &QPushButton::clicked, this, [this] {
        if (auto c = chosen(metric_)) fc_.focuser().goTo(static_cast<int>(std::lround(*c)));
    });
    note_ = theme::label("", "dim");
    note_->setWordWrap(true);
    v->addWidget(note_);

    v->addWidget(theme::section("Which metric is tightest"));
    metrics_ = new QTableWidget(0, 3);
    metrics_->setHorizontalHeaderLabels({"Metric", "G ± err", "Spread"});
    metrics_->verticalHeader()->hide();
    metrics_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    metrics_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    metrics_->setSelectionBehavior(QAbstractItemView::SelectRows);
    metrics_->setShowGrid(false);
    metrics_->setFocusPolicy(Qt::NoFocus);
    metrics_->setMaximumHeight(130);
    metrics_->setToolTip("Every metric is computed on the same frames; click one to show its curves");
    v->addWidget(metrics_);
    connect(metrics_, &QTableWidget::cellClicked, this, [this](int row, int) {
        const auto ms = fc_.sweep().metrics();
        if (row >= 0 && row < static_cast<int>(ms.size())) emit metricPicked(ms[static_cast<std::size_t>(row)]);
    });
    v->addStretch(1);
}

std::optional<double> FocusResultsPanel::chosen(Metric m) const { return fc_.sweep().result(m).position(target_); }

void FocusResultsPanel::update(Metric m) {
    metric_ = m;
    const auto r = fc_.sweep().result(m);
    const auto& G = r.ch[1].best;
    for (int c = 0; c < 3; ++c) {
        const auto& cv = r.ch[static_cast<std::size_t>(c)];
        const bool have = c < r.channels && cv.best;
        best_[static_cast<std::size_t>(c)]->setText(
            have ? QString("%1 <span style='color:%2'>±%3%4</span>")
                       .arg(thousands(*cv.best), theme::kDim.name())
                       .arg(std::lround(cv.bestError))
                       .arg(cv.atEdge ? " edge" : "")
                 : "—");
        best_[static_cast<std::size_t>(c)]->setTextFormat(Qt::RichText);
        vsG_[static_cast<std::size_t>(c)]->setText(have && G && r.channels == 3 ? (c == 1 ? "0" : signedSteps(*cv.best - *G)) : "—");
    }
    // verdict: can all three be sharp at the chosen position?
    const auto pos = r.position(target_);
    const auto zone = fc_.criticalZoneSteps();
    if (!r.spread || !pos) {
        verdict_->setProperty("role", "note");
        verdictText_->setText(r.channels ? "Not enough of a peak yet: widen the range or add frames." : "Run a sweep to see each channel's best focus.");
    } else if (!zone) {
        verdict_->setProperty("role", "note");
        verdictText_->setText(QString("Channels are spread over <b>%1 steps</b>. Set the focal ratio and µm/step in Settings to see whether that's "
                                      "inside the critical focus zone.")
                                  .arg(std::lround(*r.spread)));
    } else {
        double worst = 0;
        for (int c = 0; c < 3; ++c)
            if (const auto& b = r.ch[static_cast<std::size_t>(c)].best) worst = std::max(worst, std::abs(*pos - *b));
        const bool all = worst <= *zone;
        verdict_->setProperty("role", all ? "good" : "warn");
        verdictText_->setText(all ? QString("<b style='color:%1'>All three can be sharp.</b> At the chosen position every channel is inside the "
                                            "critical focus zone.")
                                        .arg(theme::kGood.name())
                                  : QString("<b style='color:%1'>The channels can't all be sharp.</b> Spread is larger than the focus zone: pick "
                                            "the channel that matters for this target.")
                                        .arg(theme::kAccent.name()));
    }
    theme::restyle(verdict_);

    const char* labels[5] = {"Green", "Luminance-weighted", "Balanced", "Red", "Blue"};
    const char* hints[5] = {"most of the luminance detail", "0.30 R + 0.59 G + 0.11 B", "smallest worst-channel defocus", "IR / R-pass processing",
                            "blue-channel detail"};
    const FocusTarget ts[5] = {FocusTarget::Green, FocusTarget::Luminance, FocusTarget::Balanced, FocusTarget::Red, FocusTarget::Blue};
    for (int i = 0; i < 5; ++i) {
        const auto p = r.position(ts[i]);
        modes_[static_cast<std::size_t>(i)]->setText(QString("%1   %2\n%3").arg(labels[i], p ? thousands(*p) : "—", hints[i]));
    }
    go_->setText(pos ? "Go to " + thousands(*pos) : "Go to —");
    go_->setEnabled(pos && fc_.focuser().ready() && !fc_.sweeping());
    auto& f = fc_.focuser();
    note_->setText(f.ready() ? QString("Approaches from %1, like the sweep (backlash %2 steps).")
                                   .arg(f.approachOutward() ? "below" : "above")
                                   .arg(f.backlash())
                             : "");

    const auto ms = fc_.sweep().metrics();
    metrics_->setRowCount(static_cast<int>(ms.size()));
    for (int row = 0; row < static_cast<int>(ms.size()); ++row) {
        const auto rr = fc_.sweep().result(ms[static_cast<std::size_t>(row)]);
        auto* name = new QTableWidgetItem(analysis::metricName(ms[static_cast<std::size_t>(row)]));
        if (ms[static_cast<std::size_t>(row)] == m) name->setForeground(theme::kAccent);
        metrics_->setItem(row, 0, name);
        const auto& g = rr.ch[rr.channels == 3 ? 1 : 0];
        metrics_->setItem(row, 1, new QTableWidgetItem(g.best ? QString("±%1").arg(std::lround(g.bestError)) : "—"));
        metrics_->setItem(row, 2, new QTableWidgetItem(rr.spread ? QString::number(std::lround(*rr.spread)) : "—"));
    }
}

// ---------------------------------------------------------------- FocusCurveInset

FocusCurveInset::FocusCurveInset(FocusController& fc, QWidget* parent) : QWidget(parent), fc_(fc) {
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setCursor(Qt::PointingHandCursor);
    setToolTip("The focus sweep: click for the Focus tab");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(10, 6, 6, 10);
    v->setSpacing(4);
    auto* top = new QHBoxLayout;
    top->setSpacing(4);
    status_ = theme::label("", "monoMuted");
    status_->setAttribute(Qt::WA_TransparentForMouseEvents);
    top->addWidget(status_, 1);
    auto* close = theme::button("×", "ghost");
    // the size in the style sheet: the theme's min-height and padding win over setFixedSize
    close->setStyleSheet("padding: 0; font-size: 15px; min-width: 20px; max-width: 20px; min-height: 20px; max-height: 20px;");
    close->setToolTip("Hide until the next sweep");
    connect(close, &QPushButton::clicked, this, &FocusCurveInset::dismissed);
    top->addWidget(close);
    v->addLayout(top);
    chart_ = new FocusChart;
    chart_->setCompact(true);
    chart_->setAttribute(Qt::WA_TransparentForMouseEvents);
    v->addWidget(chart_, 1);
}

void FocusCurveInset::update(Metric m, std::array<bool, 3> shown, std::optional<double> chosen) {
    const auto r = fc_.sweep().result(m);
    hasData_ = r.channels > 0 && !r.ch[0].points.empty();
    const bool running = fc_.sweeping();
    chart_->setData(r, shown, chosen, std::nullopt);
    chart_->setProgress(fc_.sweepSpan(), running && fc_.focuser().ready() ? std::optional<int>(fc_.focuser().position()) : std::nullopt);
    const auto [done, total] = fc_.sweepCount();
    status_->setText(running    ? QString("Sweep %1 / %2 · at %3").arg(std::min(done + 1, total)).arg(total).arg(thousands(fc_.focuser().position()))
                     : chosen ? QString("Best focus %1").arg(thousands(*chosen))
                              : QString("Sweep: no peak found"));
}

void FocusCurveInset::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor bg = theme::kGround;
    bg.setAlpha(225);
    p.setPen(QPen(theme::kLine, 1));
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

void FocusCurveInset::mousePressEvent(QMouseEvent*) { emit openFocusMode(); }

}  // namespace slopcap::app

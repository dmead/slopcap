#include "app/adc_panel.hpp"

#include "app/theme.hpp"
#include "preview/preview_widget.hpp"

#include <QDebug>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace slopcap::app {

namespace {

constexpr std::size_t kSmooth = 15;   // readings in the median (~1.2 s at the measuring rate)
constexpr int kMeasureMs = 80;        // ~12 readings a second is plenty, and keeps big frames off the GUI thread
constexpr std::size_t kTrail = 80;

double length(QPointF v) { return std::hypot(v.x(), v.y()); }

double median(std::vector<double> v) {
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

QString px(double v) { return QString::number(v, 'f', v < 10 ? 2 : 1); }

enum class State { Waiting, LinedUp, NoReference, Under, Over, Turned };

struct Reading {
    State state = State::Waiting;
    double along = 0, across = 0;   ///< px, relative to the uncorrected direction
    double leftPct = 0;             ///< |along| as a share of the uncorrected dispersion
};

Reading read(std::optional<QPointF> residual, std::optional<QPointF> reference, std::optional<double> scale) {
    Reading r;
    if (!residual) return r;
    const double m = length(*residual);
    if (m <= AdcPanel::kLinedUpPx || (scale && m * *scale <= 0.1)) r.state = State::LinedUp;
    if (!reference || length(*reference) < 2 * AdcPanel::kLinedUpPx) {
        if (r.state != State::LinedUp) r.state = State::NoReference;
        return r;
    }
    const QPointF u = *reference / length(*reference);
    r.along = residual->x() * u.x() + residual->y() * u.y();
    r.across = residual->y() * u.x() - residual->x() * u.y();
    r.leftPct = 100 * std::abs(r.along) / length(*reference);
    if (r.state == State::LinedUp) return r;
    r.state = std::abs(r.across) > std::max(AdcPanel::kLinedUpPx, 0.5 * std::abs(r.along)) ? State::Turned
            : r.along > 0                                                                  ? State::Under
                                                                                           : State::Over;
    return r;
}

}  // namespace

// ---------------------------------------------------------------- DispersionPlot

DispersionPlot::DispersionPlot(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 160);
    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
}

void DispersionPlot::add(QPointF offset, const QColor& colour) {
    trail_.push_back(offset);
    while (trail_.size() > kTrail) trail_.pop_front();
    colour_ = colour;
    update();
}

void DispersionPlot::setReference(std::optional<QPointF> uncorrected) {
    reference_ = uncorrected;
    update();
}

void DispersionPlot::clear() {
    trail_.clear();
    reference_.reset();
    update();
}

void DispersionPlot::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double side = std::min(width(), height());
    const QRectF box((width() - side) / 2, (height() - side) / 2, side, side);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::kControl);
    p.drawRoundedRect(box, 6, 6);
    const QPointF c = box.center();
    // the scale: a round number of pixels just beyond the largest reading
    double top = 1;
    for (const auto& t : trail_) top = std::max(top, length(t));
    if (reference_) top = std::max(top, length(*reference_));
    double range = 100;
    for (double n : {1.0, 2.0, 5.0, 10.0, 20.0, 50.0})
        if (top * 1.1 <= n) {
            range = n;
            break;
        }
    const double r = side / 2 - 14;
    auto at = [&](QPointF v) { return c + v * (r / range); };

    // the lined-up zone, rings at half and the full range, axes
    p.setBrush(QColor(theme::kGood.red(), theme::kGood.green(), theme::kGood.blue(), 45));
    const double zone = std::max(2.0, AdcPanel::kLinedUpPx * r / range);
    p.drawEllipse(c, zone, zone);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(theme::kLine, 1));
    for (double f : {0.5, 1.0}) p.drawEllipse(c, r * f, r * f);
    p.drawLine(QPointF(c.x() - r, c.y()), QPointF(c.x() + r, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - r), QPointF(c.x(), c.y() + r));
    QFont small = theme::monoFont();
    small.setPixelSize(10);
    p.setFont(small);
    p.setPen(theme::kDim);
    // the rings' sizes, up the diagonal to the top right (the least likely place for the arrow: dispersion is vertical)
    for (double f : {0.5, 1.0})
        p.drawText(c + QPointF(r * f * 0.707 + 3, -r * f * 0.707 - 3), f < 1 ? QString::number(range / 2) : QString("%1 px").arg(range));

    // the direction to correct along (the uncorrected reading), dashed through the centre
    if (reference_) {
        const QPointF e = at(*reference_);
        p.setPen(QPen(theme::kMuted, 1, Qt::DashLine));
        p.drawLine(c - (e - c), e);
        p.setPen(Qt::NoPen);
        p.setBrush(theme::kMuted);
        p.drawEllipse(e, 3, 3);
    }
    // the recent readings, fading out, and the latest as an arrow from the centre
    p.setPen(Qt::NoPen);
    for (std::size_t i = 0; i < trail_.size(); ++i) {
        QColor t = colour_;
        t.setAlpha(static_cast<int>(25 + 140 * (i + 1) / trail_.size()));
        p.setBrush(t);
        p.drawEllipse(at(trail_[i]), 2, 2);
    }
    if (!trail_.empty()) {
        p.setPen(QPen(colour_, 2));
        p.drawLine(c, at(trail_.back()));
        p.setBrush(colour_);
        p.drawEllipse(at(trail_.back()), 4.5, 4.5);
    }
}

// ---------------------------------------------------------------- ResidualHistory

void ResidualHistory::add(double px) {
    v_.push_back(px);
    while (v_.size() > 300) v_.pop_front();
    if (!best_ || px < *best_) best_ = px;
    update();
}

void ResidualHistory::clear() {
    v_.clear();
    best_.reset();
    update();
}

void ResidualHistory::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::kControl);
    p.drawRoundedRect(rect(), 6, 6);
    const QRectF r = rect().adjusted(8, 8, -8, -8);
    const double top = std::max(1.0, v_.empty() ? 0.0 : *std::max_element(v_.begin(), v_.end()) * 1.1);
    auto Y = [&](double v) { return r.bottom() - v / top * r.height(); };
    p.setPen(QPen(theme::kGood, 1, Qt::DashLine));   // lined up below this
    p.drawLine(QPointF(r.left(), Y(AdcPanel::kLinedUpPx)), QPointF(r.right(), Y(AdcPanel::kLinedUpPx)));
    if (best_) {
        p.setPen(QPen(theme::kMuted, 1, Qt::DotLine));
        p.drawLine(QPointF(r.left(), Y(*best_)), QPointF(r.right(), Y(*best_)));
    }
    if (v_.size() < 2) return;
    QPainterPath path;
    for (std::size_t i = 0; i < v_.size(); ++i) {
        const QPointF q(r.left() + r.width() * static_cast<double>(i) / 299.0, Y(v_[i]));
        if (i == 0) path.moveTo(q);
        else path.lineTo(q);
    }
    p.setPen(QPen(theme::kAccent, 1.8));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

// ---------------------------------------------------------------- AdcPanel

AdcPanel::AdcPanel(preview::PreviewWidget* preview, QObject* parent) : QObject(parent), preview_(preview) {
    left_ = new QWidget;
    auto* l = new QVBoxLayout(left_);
    l->setContentsMargins(16, 16, 16, 16);
    l->setSpacing(14);
    l->addWidget(theme::section("Atmospheric dispersion"));
    auto* how = new QLabel(
        "Low in the sky the air lifts a planet's blue image above its red one: colour fringes top and bottom. An ADC's "
        "two prisms push them back together. The arrow is red minus blue; the goal is no arrow.");
    how->setWordWrap(true);
    l->addWidget(how);
    l->addWidget(theme::section("Steps"));
    const char* steps[] = {
        "Centre the planet and set both ADC levers to <b>zero</b>.",
        "Press <b>Remember uncorrected</b>: the dashed line on the plot is the direction to correct along.",
        ("Rotate the ADC in its holder so the levers are <b>level with the horizon</b> (or spread them a little and turn "
         "the ADC until the arrow only shrinks)."),
        "Spread the levers evenly until the arrow is gone. Too far reverses it. Check again as the planet climbs or sinks."};
    for (int i = 0; i < 4; ++i) {
        auto* row = new QHBoxLayout;
        auto* num = new QLabel(QString::number(i + 1));
        num->setFixedSize(22, 22);
        num->setAlignment(Qt::AlignCenter);
        num->setStyleSheet(QString("background: %1; color: %2; border-radius: 11px; font-size: 12px; font-weight: 600;")
                               .arg(theme::kAccentBg.name(), theme::kAccent.name()));
        auto* t = new QLabel(steps[i]);
        t->setWordWrap(true);
        row->addWidget(num, 0, Qt::AlignTop);
        row->addWidget(t, 1);
        l->addLayout(row);
    }
    l->addStretch(1);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    remember_ = theme::button("Remember uncorrected", "accent");
    remember_->setToolTip("With both levers at zero: the reading to correct from");
    auto* reset = theme::button("Reset");
    reset->setToolTip("Forget the uncorrected reading, the history and the best so far");
    buttons->addWidget(remember_, 1);
    buttons->addWidget(reset);
    l->addLayout(buttons);
    connect(remember_, &QPushButton::clicked, this, &AdcPanel::rememberUncorrected);
    connect(reset, &QPushButton::clicked, this, &AdcPanel::reset);

    right_ = new QWidget;
    auto* r = new QVBoxLayout(right_);
    r->setContentsMargins(16, 16, 16, 16);
    r->setSpacing(14);
    r->addWidget(theme::section("Red − blue"));
    offset_ = theme::label("—", "monoHuge");
    r->addWidget(offset_);
    status_ = theme::label("", "muted");
    status_->setWordWrap(true);
    r->addWidget(status_);
    auto* grid = new QGridLayout;
    grid->setSpacing(8);
    grid->addWidget(theme::stat("Direction (R − B)", direction_), 0, 0);
    grid->addWidget(theme::stat("Along / across", alongAcross_), 0, 1);
    grid->addWidget(theme::stat("The air, now", expected_), 1, 0);
    grid->addWidget(theme::stat("Altitude", altitudeValue_), 1, 1);
    r->addLayout(grid);
    note_ = theme::label("", "dim");
    note_->setWordWrap(true);
    r->addWidget(note_);
    plot_ = new DispersionPlot;
    r->addWidget(plot_);
    r->addWidget(theme::section("This session (px)"));
    history_ = new ResidualHistory;
    r->addWidget(history_);
    best_ = theme::label("", "dim");
    r->addWidget(best_);
    r->addStretch(1);
    refresh();
}

void AdcPanel::setActive(bool on) {
    active_ = on;
    measureClock_.invalidate();
    if (!on) clearOverlay();
    refresh();
}

void AdcPanel::setContext(std::optional<double> arcsecPerPixel, std::optional<Altitude> altitude) {
    scale_ = arcsecPerPixel;
    altitude_ = altitude;
    refresh();
}

void AdcPanel::clearOverlay() { preview_->setOverlay("adc", {}); }

QColor AdcPanel::stateColour() const {
    switch (read(residual_, reference_, scale_).state) {
        case State::LinedUp: return theme::kGood;
        case State::Over:
        case State::Turned: return theme::kRecText;
        case State::Waiting:
        case State::NoReference:
        case State::Under: break;
    }
    return theme::kAccent;
}

void AdcPanel::onFrame(const LatestFrame::Frame& frame, Bayer bayer) {
    if (!active_ || (measureClock_.isValid() && measureClock_.elapsed() < kMeasureMs)) return;
    measureClock_.start();
    const auto& fi = frame.info;
    const auto d = analysis::measureDispersion(std::span(frame.pixels).first(fi.bytes), fi.roi.width, fi.roi.height, fi.bitDepth, bayer);
    if (!d.found) {
        problem_ = QString::fromStdString(d.problem);
        clearOverlay();
        refresh();
        return;
    }
    problem_.clear();
    atEdge_ = d.atEdge;
    // Seeing moves single frames around: the median of the recent ones per axis, and how far they scatter from it.
    recent_.push_back({d.dx(), d.dy()});
    while (recent_.size() > kSmooth) recent_.pop_front();
    std::vector<double> xs, ys, dev;
    for (const auto& q : recent_) xs.push_back(q.x()), ys.push_back(q.y());
    residual_ = QPointF(median(xs), median(ys));
    for (const auto& q : recent_) dev.push_back(length(q - *residual_));
    noisePx_ = median(dev);
    const double m = length(*residual_);
    if (!bestPx_ || m < *bestPx_) bestPx_ = m;
    const QColor colour = stateColour();
    plot_->add(*residual_, colour);
    history_->add(m);

    // Over the preview: each channel's centroid, and red − blue from the green one, ten times longer so a pixel shows.
    preview_->setOverlay("adc", [d, res = *residual_, colour](QPainter& p, double k) {
        const QPointF centres[3] = {{d.rx, d.ry}, {d.gx, d.gy}, {d.bx, d.by}};
        p.setBrush(Qt::NoBrush);
        for (int c = 0; c < 3; ++c) {
            p.setPen(QPen(theme::kChannel[c], 1.5 / k));
            p.drawEllipse(centres[c], 4 / k, 4 / k);
        }
        const double len = length(res);
        if (len < 1e-6) return;
        const QPointF u = res / len, s = centres[1], e = s + u * std::max(14 / k, 10 * len);
        p.setPen(QPen(colour, 2.5 / k));
        p.drawLine(s, e);
        const double h = 10 / k;
        p.drawLine(e, e - QPointF(u.x() * h - u.y() * h * 0.5, u.y() * h + u.x() * h * 0.5));
        p.drawLine(e, e - QPointF(u.x() * h + u.y() * h * 0.5, u.y() * h - u.x() * h * 0.5));
    });

    if (!logClock_.isValid() || logClock_.elapsed() >= 10000) {   // the night's timeline, every 10 s
        logClock_.start();
        const auto rd = read(residual_, reference_, scale_);
        qInfo().noquote() << QString("adc: R−B %1, %2 px (%3 px ± %4%5)").arg(residual_->x(), 0, 'f', 2).arg(residual_->y(), 0, 'f', 2)
                                 .arg(px(m), px(noisePx_), scale_ ? QString(", %1″").arg(m * *scale_, 0, 'f', 2) : QString())
                          << (reference_ ? QString("· along %1 across %2 px").arg(rd.along, 0, 'f', 2).arg(rd.across, 0, 'f', 2) : QString())
                          << (!altitude_                ? QString()
                              : altitude_->degrees <= 0 ? QString("· %1 below the horizon").arg(altitude_->of)
                                                        : QString("· %1 at %2°, the air %3″").arg(altitude_->of).arg(altitude_->degrees, 0, 'f', 1)
                                                              .arg(analysis::dispersionArcsec(altitude_->degrees), 0, 'f', 2));
    }
    refresh();
}

void AdcPanel::rememberUncorrected() {
    if (!residual_) return;
    reference_ = residual_;
    plot_->setReference(reference_);
    qInfo().noquote() << QString("adc: uncorrected R−B %1, %2 px").arg(reference_->x(), 0, 'f', 2).arg(reference_->y(), 0, 'f', 2);
    refresh();
}

void AdcPanel::reset() {
    recent_.clear();
    residual_.reset();
    reference_.reset();
    bestPx_.reset();
    problem_.clear();
    plot_->clear();
    history_->clear();
    clearOverlay();
    refresh();
}

void AdcPanel::refresh() {
    const auto rd = read(residual_, reference_, scale_);
    remember_->setEnabled(residual_.has_value());
    const double m = residual_ ? length(*residual_) : 0;
    offset_->setText(residual_ ? px(m) + " px" + (scale_ ? QString(" · %1″").arg(m * *scale_, 0, 'f', 2) : QString()) : "—");
    offset_->setStyleSheet(QString("color: %1").arg(stateColour().name()));

    QString status;
    switch (rd.state) {
        case State::Waiting: status = !active_ ? "Open ADC mode with a colour camera on a planet." : "Looking for the planet…"; break;
        case State::LinedUp:
            status = QString("Lined up: red and blue are within %1 px. Check again as the planet climbs or sinks.").arg(px(std::max(m, noisePx_)));
            break;
        case State::NoReference:
            status = "Set both levers to zero and press Remember uncorrected; then level the ADC and spread the levers until "
                     "the arrow shrinks to nothing.";
            break;
        case State::Under:
            status = QString("Under-corrected: %1 % of the uncorrected dispersion is left. Spread the levers further.").arg(std::lround(rd.leftPct));
            break;
        case State::Over:
            status = QString("Over-corrected: red and blue have crossed (%1 % the other way). Close the levers a little.").arg(std::lround(rd.leftPct));
            break;
        case State::Turned:
            status = "The ADC is turned: what's left points across the dashed line. Rotate the ADC itself, not the levers, "
                     "until the arrow lies along the line.";
            break;
    }
    if (!problem_.isEmpty()) status = problem_;
    else if (residual_ && atEdge_) status += " The planet touches the frame's edge: centre it.";
    status_->setText(status);

    direction_->setText(residual_ && m > 1e-9 ? QString("%1°").arg(std::lround(std::atan2(residual_->y(), residual_->x()) * 180 / std::numbers::pi))
                                              : "—");
    alongAcross_->setText(residual_ && reference_ ? QString("%1 / %2").arg(rd.along, 0, 'f', 2).arg(rd.across, 0, 'f', 2) : "—");

    // what the air does at the target's altitude, and in pixels when the image scale is known
    QStringList note;
    expected_->setText("—");
    altitudeValue_->setText(altitude_ ? QString("%1°").arg(altitude_->degrees, 0, 'f', 1) : "—");
    if (altitude_ && altitude_->degrees > 0) {
        const double e = analysis::dispersionArcsec(altitude_->degrees);
        expected_->setText(QString("%1″").arg(e, 0, 'f', 2) + (scale_ ? QString(" · %1 px").arg(px(e / *scale_)) : QString()));
        if (altitude_->degrees < 15) note << QString("%1 is only %2° up: the estimate is rough.").arg(altitude_->of).arg(altitude_->degrees, 0, 'f', 0);
        else note << QString("%1 is %2° up: the air lifts blue %3″ above red.").arg(altitude_->of).arg(altitude_->degrees, 0, 'f', 0).arg(e, 0, 'f', 2);
        if (reference_ && scale_) note << QString("Uncorrected you measured %1″.").arg(length(*reference_) * *scale_, 0, 'f', 2);
    } else if (altitude_) {
        note << QString("%1 is below the horizon.").arg(altitude_->of);
    } else {
        note << "Settings → Site, and a target (or the mount), give the dispersion the air causes now.";
    }
    if (!scale_) note << "Settings → Focal length, with a camera that reports its pixel size, gives arcseconds.";
    note_->setText(note.join(" "));
    best_->setText(bestPx_ ? QString("Best so far %1 px · dotted line").arg(px(*bestPx_)) : "");
}

}  // namespace slopcap::app

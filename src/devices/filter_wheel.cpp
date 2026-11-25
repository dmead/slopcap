#include "devices/filter_wheel.hpp"

#include <QSettings>

namespace slopcap::devices {

namespace {
constexpr int kIdlePollMs = 1000, kMovePollMs = 150;
constexpr int kMoveTimeoutMs = 30000;   // a full turn of an 8-slot EFW is ~6 s
}

FilterWheel::FilterWheel(QObject* parent) : QObject(parent) {
    connect(&poll_, &QTimer::timeout, this, &FilterWheel::poll);
}

FilterWheel::~FilterWheel() = default;

bool FilterWheel::open(const QString& spec) {
    close();
    try {
        wheel_ = openWheel(spec.toStdString());
    } catch (const WheelError& e) {
        emit failed(QString::fromStdString(e.what()));
        return false;
    }
    spec_ = spec;
    model_ = QString::fromStdString(wheel_->model());
    serial_ = QString::fromStdString(wheel_->serial());
    const QStringList saved = QSettings().value("wheel/" + serial_ + "/names").toStringList();
    names_.clear();
    for (int i = 0; i < wheel_->slotCount(); ++i) names_ << (i < saved.size() && !saved[i].trimmed().isEmpty() ? saved[i].trimmed() : QString::number(i + 1));
    position_ = target_ = -1;
    emit namesChanged();
    emit readyChanged(true);
    poll();
    poll_.start(kIdlePollMs);
    return wheel_ != nullptr;
}

void FilterWheel::close() {
    if (!wheel_) return;
    poll_.stop();
    wheel_.reset();
    position_ = target_ = -1;
    emit readyChanged(false);
}

void FilterWheel::setNames(const QStringList& names) {
    QStringList n;
    for (int i = 0; i < names_.size(); ++i) n << (i < names.size() && !names[i].trimmed().isEmpty() ? names[i].trimmed() : QString::number(i + 1));
    if (n == names_) return;
    names_ = n;
    if (!serial_.isEmpty()) QSettings().setValue("wheel/" + serial_ + "/names", names_);
    emit namesChanged();
}

QString FilterWheel::name(int slot) const { return slot >= 0 && slot < names_.size() ? names_[slot] : QString(); }

int FilterWheel::slotNamed(const QString& name) const {
    const QString n = name.trimmed();
    if (n.isEmpty()) return -1;
    for (int i = 0; i < names_.size(); ++i)
        if (names_[i].compare(n, Qt::CaseInsensitive) == 0) return i;
    return -1;
}

void FilterWheel::goTo(int slot) {
    if (!wheel_ || slot < 0 || slot >= names_.size()) return;
    if (slot == target_ && (moving() || position_ == slot)) return;
    try {
        wheel_->setPosition(slot);
    } catch (const WheelError& e) {
        emit failed(QString::fromStdString(e.what()));
        return;
    }
    target_ = slot;
    moveClock_.start();
    if (position_ != -1) {
        position_ = -1;
        emit positionChanged(-1);
    }
    poll_.start(kMovePollMs);
}

void FilterWheel::poll() {
    if (!wheel_) return;
    int p = -1;
    try {
        p = wheel_->position();
    } catch (const WheelError& e) {
        lost(QString::fromStdString(e.what()));
        return;
    }
    if (p != position_) {
        position_ = p;
        if (!moveClock_.isValid() && p >= 0) target_ = p;   // opened, or turned by another program
        emit positionChanged(p);
    }
    if (moveClock_.isValid()) {
        if (p == target_) {
            moveClock_.invalidate();
            poll_.start(kIdlePollMs);
            emit arrived(p);
        } else if (p >= 0 && p != target_) {
            // stopped somewhere else (the SDK reports the slot it ended on)
            moveClock_.invalidate();
            poll_.start(kIdlePollMs);
            emit failed(QString("the wheel stopped at slot %1, not %2").arg(p + 1).arg(target_ + 1));
            target_ = p;
        } else if (moveClock_.elapsed() > kMoveTimeoutMs) {
            moveClock_.invalidate();
            poll_.start(kIdlePollMs);
            emit failed(QString("the wheel didn't reach slot %1 in %2 s").arg(target_ + 1).arg(kMoveTimeoutMs / 1000));
        }
    }
}

void FilterWheel::lost(const QString& why) {
    emit failed(why);
    close();
}

}  // namespace slopcap::devices

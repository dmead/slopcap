#include "mounts/mount_worker.hpp"

#include "analysis/ephemeris.hpp"

#include <QMetaObject>

namespace slopcap::mounts {

namespace {
int axisOf(Dir d) { return isNorthSouth(d) ? 0 : 1; }
}  // namespace

MountWorker::MountWorker(Make make, QObject* parent, int retryMs, int deadmanMs)
    : QObject(parent), make_(std::move(make)), retryMs_(retryMs), deadmanMs_(deadmanMs) {
    thread_ = std::thread([this] { loop(); });
}

MountWorker::~MountWorker() {
    {
        std::lock_guard lock(m_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

bool MountWorker::connected() const {
    std::lock_guard lock(m_);
    return connected_;
}

MountInfo MountWorker::info() const {
    std::lock_guard lock(m_);
    return info_;
}

MountStatus MountWorker::status() const {
    std::lock_guard lock(m_);
    return status_;
}

double MountWorker::jdNow() const { return analysis::julianDate(std::chrono::system_clock::now()); }

void MountWorker::post(std::function<void()> run, bool motion) {
    {
        std::lock_guard lock(m_);
        queue_.push_back({std::move(run), motion});
    }
    wake_.notify_all();
}

void MountWorker::goTo(double ra, double dec) {
    post([this, ra, dec] {
        double r = ra, d = dec;
        if (info_.j2000) analysis::dateToJ2000(r, d, jdNow());
        driver_->slewTo(r, d);
        nextPoll_ = Clock::now();
    }, true);
}

void MountWorker::abort() {
    {
        // first in the queue, and nothing that moves stays queued behind it
        std::lock_guard lock(m_);
        std::erase_if(queue_, [](const Command& c) { return c.motion; });
        queue_.push_front({[this] {
                               for (auto& a : axes_) a.moving.reset(), a.emulated.reset(), a.pulseEnd = {};
                               driver_->abort();
                               nextPoll_ = Clock::now();
                           },
                           false});
    }
    wake_.notify_all();
}

void MountWorker::move(Dir d, int rate) {
    post([this, d, rate] {
        auto& a = axes_[axisOf(d)];
        driver_->move(d, rate);
        a.moving = d, a.since = Clock::now(), a.emulated.reset();
        nextPoll_ = Clock::now();
    }, true);
}

void MountWorker::stopMoves() {
    post([this] {
        for (int i = 0; i < 2; ++i)
            if (axes_[i].moving) stopAxis(i);
        nextPoll_ = Clock::now();
    });
}

void MountWorker::pulse(Dir d, int ms) {
    post([this, d, ms] {
        auto& a = axes_[axisOf(d)];
        if (a.moving) return;   // a move has the axis
        if (info_.caps.pulse) {
            driver_->pulse(d, ms);
            a.emulated.reset();
        } else {
            driver_->move(d, 0);   // the slowest rate, for that long
            a.emulated = d;
        }
        a.pulseEnd = Clock::now() + std::chrono::milliseconds(ms);
        nextPoll_ = Clock::now();
    }, true);
}

void MountWorker::setTracking(bool on) {
    post([this, on] {
        driver_->setTracking(on);
        nextPoll_ = Clock::now();
    });
}

void MountWorker::setTrackRate(TrackRate r) {
    post([this, r] { driver_->setTrackRate(r); });
}

void MountWorker::park() {
    post([this] {
        driver_->park();
        nextPoll_ = Clock::now();
    }, true);
}

void MountWorker::unpark() {
    post([this] {
        driver_->unpark();
        nextPoll_ = Clock::now();
    });
}

void MountWorker::sync(double ra, double dec) {
    post([this, ra, dec] {
        double r = ra, d = dec;
        if (info_.j2000) analysis::dateToJ2000(r, d, jdNow());
        driver_->sync(r, d);
        nextPoll_ = Clock::now();
    });
}

void MountWorker::stopAxis(int axis) {
    auto& a = axes_[axis];
    const auto d = a.moving ? a.moving : a.emulated;
    a.moving.reset(), a.emulated.reset();
    if (d) driver_->stop(*d);
}

void MountWorker::report(const std::string& why) {
    QMetaObject::invokeMethod(this, [this, w = QString::fromStdString(why)] { emit failed(w); }, Qt::QueuedConnection);
}

bool MountWorker::tryConnect() {
    try {
        if (!driver_) driver_ = make_();
        auto info = driver_->connect();
        {
            std::lock_guard lock(m_);
            info_ = std::move(info);
            connected_ = true;
        }
        pollFailures_ = 0;
        lastError_.clear();
        QMetaObject::invokeMethod(this, [this] { emit connectedChanged(true); }, Qt::QueuedConnection);
        return true;
    } catch (const std::exception& e) {
        // the same failure every 10 s is said once
        if (e.what() != lastError_) report(e.what());
        lastError_ = e.what();
        retryAt_ = Clock::now() + std::chrono::milliseconds(retryMs_);
        return false;
    }
}

void MountWorker::lost(const std::string& why) {
    {
        std::lock_guard lock(m_);
        connected_ = false;
        status_ = {};
    }
    try {
        driver_->disconnect();
    } catch (const std::exception&) {
    }
    report("lost the mount: " + why);
    lastError_ = why;
    retryAt_ = Clock::now() + std::chrono::milliseconds(retryMs_);
    QMetaObject::invokeMethod(this, [this] { emit connectedChanged(false); }, Qt::QueuedConnection);
}

void MountWorker::pollNow() {
    try {
        auto st = driver_->poll();
        if (info_.j2000 && st.haveCoords) analysis::j2000ToDate(st.ra, st.dec, jdNow());
        const auto now = Clock::now();
        st.pulsing = axes_[0].pulseEnd > now || axes_[1].pulseEnd > now;
        {
            std::lock_guard lock(m_);
            status_ = st;
        }
        pollFailures_ = 0;
        QMetaObject::invokeMethod(this, [this] { emit statusChanged(); }, Qt::QueuedConnection);
        const bool busy = st.slewing || st.pulsing || axes_[0].moving || axes_[1].moving;
        nextPoll_ = Clock::now() + std::chrono::milliseconds(busy ? 250 : 1000);
    } catch (const std::exception& e) {
        nextPoll_ = Clock::now() + std::chrono::milliseconds(250);
        if (++pollFailures_ >= 3) lost(e.what());   // one slow reply isn't a lost mount
    }
}

void MountWorker::loop() {
    bool wasMoving = false;   // when the link dropped: stop first on reconnecting (§5.3)
    while (!quit_) {
        auto now = Clock::now();
        if (!connected_ && now >= retryAt_ && tryConnect()) {
            if (wasMoving) try {
                    driver_->abort();
                } catch (const std::exception&) {
                }
            wasMoving = false;
            nextPoll_ = now;
        }
        std::deque<Command> cmds;
        {
            std::lock_guard lock(m_);
            cmds.swap(queue_);
        }
        for (auto& c : cmds) {
            if (quit_) break;
            if (!connected_) {
                report("the mount isn't connected");
                break;
            }
            try {
                c.run();
            } catch (const std::exception& e) {
                report(e.what());
            }
        }
        if (connected_) {
            now = Clock::now();
            for (int i = 0; i < 2; ++i) {
                auto& a = axes_[i];
                try {
                    if (a.emulated && now >= a.pulseEnd) stopAxis(i);
                    if (a.moving && now - a.since >= std::chrono::milliseconds(deadmanMs_)) {
                        stopAxis(i);
                        report("a move ran " + std::to_string(deadmanMs_ / 1000) + " s without being renewed, so it was stopped");
                    }
                } catch (const std::exception& e) {
                    report(e.what());
                }
            }
            if (now >= nextPoll_) pollNow();
            if (!connected_) {
                wasMoving = axes_[0].moving || axes_[1].moving || axes_[0].emulated || axes_[1].emulated;
                for (auto& a : axes_) a = {};
            }
        }
        // sleep until the next poll, pulse end or deadman, or a command
        auto until = connected_ ? nextPoll_ : retryAt_;
        for (const auto& a : axes_) {
            if (a.emulated) until = std::min(until, a.pulseEnd);
            if (a.moving) until = std::min(until, a.since + std::chrono::milliseconds(deadmanMs_));
        }
        std::unique_lock lock(m_);
        wake_.wait_until(lock, until, [this] { return quit_ || !queue_.empty(); });
    }
    if (!driver_) return;
    try {
        if (connected_ && (axes_[0].moving || axes_[1].moving || axes_[0].emulated || axes_[1].emulated || status_.slewing)) driver_->abort();
    } catch (const std::exception&) {
    }
    try {
        driver_->disconnect();
    } catch (const std::exception&) {
    }
    driver_.reset();
}

}  // namespace slopcap::mounts

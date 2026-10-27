// A built-in mount's own thread (docs/design/mounts.md §3.2, §5). It makes and connects the driver (again every 10 s
// while it can't, and after the link drops), polls it (every second; four times a second while anything moves), runs
// commands in order, except Stop, which goes first and drops the moves and gotos queued behind it. For drivers
// without guide pulses it times a move at the slowest rate instead; a move nobody renews stops after 10 s. It speaks
// JNow whatever the mount speaks. Called from the GUI thread; what it learns comes back there as signals, with the
// details in info() and status().
#pragma once

#include "mounts/mount_driver.hpp"

#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace slopcap::mounts {

class MountWorker : public QObject {
    Q_OBJECT
public:
    using Make = std::function<std::unique_ptr<IMountDriver>()>;
    static constexpr int kDeadmanMs = 10000;
    static constexpr int kRetryMs = 10000;

    /// (Tests shorten the retry and the deadman.)
    explicit MountWorker(Make make, QObject* parent = nullptr, int retryMs = kRetryMs, int deadmanMs = kDeadmanMs);
    ~MountWorker() override;   ///< stops what moves, disconnects, joins

    bool connected() const;
    MountInfo info() const;
    MountStatus status() const;   ///< JNow

    void goTo(double ra, double dec);   ///< JNow
    void abort();                       ///< every goto and move, now
    void move(Dir d, int rate);         ///< (again within kDeadmanMs to keep it going)
    void stopMoves();
    void pulse(Dir d, int ms);
    void setTracking(bool on);
    void setTrackRate(TrackRate r);
    void park();
    void unpark();
    void sync(double ra, double dec);   ///< JNow

signals:
    void connectedChanged(bool connected);
    void statusChanged();
    void failed(const QString& why);

private:
    using Clock = std::chrono::steady_clock;
    struct Command {
        std::function<void()> run;
        bool motion = false;   ///< dropped by abort()
    };
    void post(std::function<void()> run, bool motion = false);
    void loop();
    bool tryConnect();
    void pollNow();
    void lost(const std::string& why);
    void report(const std::string& why);
    void stopAxis(int axis);   ///< (worker thread) end a move or emulated pulse on that axis
    double jdNow() const;

    Make make_;
    const int retryMs_, deadmanMs_;
    std::unique_ptr<IMountDriver> driver_;   // worker thread only
    mutable std::mutex m_;                   // guards queue_, info_, status_, connected_
    std::condition_variable wake_;
    std::deque<Command> queue_;
    MountInfo info_;
    MountStatus status_;
    bool connected_ = false;
    std::atomic<bool> quit_{false};
    std::string lastError_;
    // worker thread only
    struct Axis {
        std::optional<Dir> moving;   ///< a move that way, started at `since`
        Clock::time_point since;
        Clock::time_point pulseEnd{};
        std::optional<Dir> emulated;   ///< the pulse is a move to stop at pulseEnd
    } axes_[2];
    Clock::time_point nextPoll_{}, retryAt_{};
    int pollFailures_ = 0;
    std::thread thread_;
};

}  // namespace slopcap::mounts

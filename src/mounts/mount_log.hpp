// What the built-in mount drivers said and heard, kept in memory for Settings and log... in the mount panel, whatever
// the log level: every command and its reply (CommandChannel), and every error, fault and note (MountWorker). The
// last kMax entries; thread-safe.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace slopcap::mounts {

class MountLog {
public:
    enum class Kind { Exchange, Error, Info };
    struct Entry {
        std::uint64_t seq = 0;
        std::chrono::system_clock::time_point at;
        Kind kind = Kind::Info;
        std::string text;
    };
    static constexpr std::size_t kMax = 5000;

    static MountLog& instance();
    void add(Kind kind, std::string text);
    std::vector<Entry> since(std::uint64_t seq) const;   ///< the entries after `seq` (0: all kept)
    std::uint64_t errors() const;                        ///< how many errors so far
    void clear();

private:
    mutable std::mutex m_;
    std::deque<Entry> entries_;
    std::uint64_t next_ = 1, errors_ = 0;
};

}  // namespace slopcap::mounts

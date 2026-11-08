#include "mounts/mount_log.hpp"

namespace slopcap::mounts {

thread_local bool MountLog::polling_ = false;

MountLog& MountLog::instance() {
    static MountLog log;
    return log;
}

void MountLog::add(Kind kind, std::string text) {
    std::lock_guard lock(m_);
    entries_.push_back({next_++, std::chrono::system_clock::now(), kind, std::move(text)});
    if (kind == Kind::Error) ++errors_;
    while (entries_.size() > kMax) entries_.pop_front();
}

std::vector<MountLog::Entry> MountLog::since(std::uint64_t seq) const {
    std::lock_guard lock(m_);
    std::vector<Entry> out;
    for (const auto& e : entries_)
        if (e.seq > seq) out.push_back(e);
    return out;
}

std::uint64_t MountLog::errors() const {
    std::lock_guard lock(m_);
    return errors_;
}

void MountLog::clear() {
    std::lock_guard lock(m_);
    entries_.clear();
}

}  // namespace slopcap::mounts

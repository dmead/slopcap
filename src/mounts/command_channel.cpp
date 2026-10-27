#include "mounts/command_channel.hpp"

#include <QDebug>
#include <QElapsedTimer>
#include <QLoggingCategory>

namespace slopcap::mounts {

Q_LOGGING_CATEGORY(lcMount, "slopcap.mount", QtInfoMsg)

std::string printable(std::string_view bytes) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 32 && c < 127 && c != '\\') out += ch;
        else out += std::string("\\x") + hex[c >> 4] + hex[c & 15];
    }
    return out;
}

CommandChannel::CommandChannel(std::unique_ptr<ITransport> t, std::string tag) : t_(std::move(t)), tag_(std::move(tag)) {}

void CommandChannel::send(std::string_view cmd) {
    pending_.clear();
    t_->drain();
    t_->write(cmd);
    log(cmd, std::nullopt);
}

std::optional<std::string> CommandChannel::exchange(std::string_view cmd, char term, std::size_t n, int ms) {
    pending_.clear();
    t_->drain();
    t_->write(cmd);
    const auto r = n ? readCount(n, ms) : readUntil(term, ms);
    log(cmd, r);
    return r;
}

std::optional<std::string> CommandChannel::readCount(std::size_t n, int ms) {
    QElapsedTimer clock;
    clock.start();
    std::string got = std::move(pending_);
    pending_.clear();
    while (got.size() < n) {
        const int left = ms - static_cast<int>(clock.elapsed());
        if (left <= 0) return std::nullopt;
        got += t_->read(left);
        if (t_->datagrams() && !got.empty()) break;   // a datagram is the whole reply
    }
    if (got.size() > n) pending_ = got.substr(n), got.resize(n);
    if (got.size() < n) return std::nullopt;
    return got;
}

std::optional<std::string> CommandChannel::readUntil(char term, int ms) {
    QElapsedTimer clock;
    clock.start();
    std::string got = std::move(pending_);
    pending_.clear();
    for (;;) {
        if (const auto at = got.find(term); at != std::string::npos) {
            pending_ = got.substr(at + 1);
            got.resize(at);
            return got;
        }
        const int left = ms - static_cast<int>(clock.elapsed());
        if (left <= 0) return std::nullopt;
        const auto more = t_->read(left);
        if (t_->datagrams() && !more.empty() && more.find(term) == std::string::npos) return got + more;   // unterminated datagram
        got += more;
    }
}

std::optional<std::string> CommandChannel::tryUntil(std::string_view cmd, char term, int ms, bool retry) {
    auto r = exchange(cmd, term, 0, ms);
    if (!r && retry) r = exchange(cmd, term, 0, ms);
    return r;
}

std::string CommandChannel::until(std::string_view cmd, char term, int ms, bool retry) {
    if (auto r = tryUntil(cmd, term, ms, retry)) return *r;
    throw MountError("the mount didn't answer " + printable(cmd) + " (" + t_->name() + ")");
}

std::optional<std::string> CommandChannel::tryCount(std::string_view cmd, std::size_t n, int ms, bool retry) {
    auto r = exchange(cmd, 0, n, ms);
    if (!r && retry) r = exchange(cmd, 0, n, ms);
    return r;
}

std::string CommandChannel::count(std::string_view cmd, std::size_t n, int ms, bool retry) {
    if (auto r = tryCount(cmd, n, ms, retry)) return *r;
    throw MountError("the mount didn't answer " + printable(cmd) + " (" + t_->name() + ")");
}

void CommandChannel::log(std::string_view cmd, const std::optional<std::string>& reply) const {
    if (!lcMount().isDebugEnabled()) return;
    qCDebug(lcMount).noquote() << QString::fromStdString("mount[" + tag_ + "]> " + printable(cmd) +
                                                         (reply ? "  < " + printable(*reply) : std::string()));
}

}  // namespace slopcap::mounts

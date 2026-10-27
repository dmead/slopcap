// One command at a time over a transport, the way every mount protocol here works: drop any stray input, send, read
// the reply (nothing, a fixed number of bytes, or up to a terminator), with a timeout. Only reads are retried: a
// resent goto is a second goto. Every exchange goes to the "slopcap.mount" debug log
// (QT_LOGGING_RULES="slopcap.mount.debug=true").
#pragma once

#include "mounts/transport.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace slopcap::mounts {

class CommandChannel {
public:
    CommandChannel(std::unique_ptr<ITransport> t, std::string tag);

    void send(std::string_view cmd);   ///< no reply
    /// The reply up to `term` (not included), or nullopt after `ms` without it. `retry`: send once more first.
    std::optional<std::string> tryUntil(std::string_view cmd, char term, int ms, bool retry = false);
    std::string until(std::string_view cmd, char term, int ms, bool retry = false);   ///< throws MountError on a timeout
    /// Exactly n bytes, or nullopt after `ms`.
    std::optional<std::string> tryCount(std::string_view cmd, std::size_t n, int ms, bool retry = false);
    std::string count(std::string_view cmd, std::size_t n, int ms, bool retry = false);
    /// More of the same reply (after a first part read with tryCount), up to `term`.
    std::optional<std::string> readUntil(char term, int ms);

    ITransport& transport() { return *t_; }

private:
    std::optional<std::string> exchange(std::string_view cmd, char term, std::size_t n, int ms);
    std::optional<std::string> readCount(std::size_t n, int ms);
    void log(std::string_view cmd, const std::optional<std::string>& reply) const;
    std::unique_ptr<ITransport> t_;
    std::string tag_, pending_;
};

/// Printable form of protocol bytes for logs and messages: control and binary bytes as \xNN.
std::string printable(std::string_view bytes);

}  // namespace slopcap::mounts

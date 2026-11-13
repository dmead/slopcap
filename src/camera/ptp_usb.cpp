#include "camera/ptp_usb.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace slopcap::ptp {

namespace {

constexpr uint16_t kCommandBlock = 1, kDataBlock = 2, kResponseBlock = 3, kEventBlock = 4;
constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kMaxRead = 1 << 20;   // one bulk-in request at most (a 24 MP JPEG comes in about ten)

}  // namespace

UsbTransport::UsbTransport(std::unique_ptr<UsbPipe> pipe, std::chrono::milliseconds timeout) : pipe_(std::move(pipe)), timeout_(timeout) {
    // OpenSession is transaction 0; a session another program left open is closed first
    const uint32_t session = 1;
    auto r = doTransact(kOpenSession, std::span(&session, 1), Data::None, {}, nullptr);
    if (r.code == kSessionAlreadyOpen) {
        doTransact(kCloseSession, {}, Data::None, {}, nullptr);
        tid_ = 0;
        r = doTransact(kOpenSession, std::span(&session, 1), Data::None, {}, nullptr);
    }
    if (!r.ok()) throw CameraError(std::format("open a PTP session: camera answered {}", responseText(r.code)));
    session_ = true;
}

UsbTransport::~UsbTransport() {
    if (!session_) return;
    try {
        doTransact(kCloseSession, {}, Data::None, {}, nullptr);
    } catch (const std::exception&) {   // (unplugged: nothing to close)
    }
}

Response UsbTransport::transact(uint16_t op, std::span<const uint32_t> params, Data phase, std::span<const uint8_t> out,
                                std::vector<uint8_t>* in) {
    return doTransact(op, params, phase, out, in);
}

Response UsbTransport::doTransact(uint16_t op, std::span<const uint32_t> params, Data phase, std::span<const uint8_t> out,
                                  std::vector<uint8_t>* in) {
    if (params.size() > 5) throw Error(std::format("PTP operation 0x{:04X}: {} parameters (at most 5)", op, params.size()));
    const uint32_t tid = tid_++;
    Writer cmd;
    for (auto p : params) cmd.u32(p);
    writeContainer(kCommandBlock, op, tid, cmd.data());
    if (phase == Data::Out) writeContainer(kDataBlock, op, tid, out);
    if (in) in->clear();
    for (;;) {
        const Header h = nextHeader();
        if (h.length < kHeaderBytes || h.length > kMaxData) throw LinkError(std::format("PTP container of {} bytes", h.length));
        const std::size_t n = h.length - kHeaderBytes;
        if (h.type == kDataBlock) {
            std::vector<uint8_t> scratch;
            readPayload(n, in && phase == Data::In ? *in : scratch);
            continue;   // the response follows
        }
        if (h.type != kResponseBlock) throw LinkError(std::format("PTP container type {} where a response was due", h.type));
        std::vector<uint8_t> payload;
        readPayload(n, payload);
        if (h.tid != tid) continue;   // a late answer to an earlier, abandoned transaction
        Response r{h.code, {}};
        Reader rd(payload);
        while (rd.left() >= 4) r.params.push_back(rd.u32());
        return r;
    }
}

void UsbTransport::writeContainer(uint16_t type, uint16_t code, uint32_t tid, std::span<const uint8_t> payload) {
    Writer w;
    w.u32(static_cast<uint32_t>(kHeaderBytes + payload.size())).u16(type).u16(code).u32(tid);
    w.bytes(payload);
    pipe_->write(w.data());
    if (w.data().size() % pipe_->packetSize() == 0) pipe_->write({});   // a whole number of packets: end it with an empty one
}

void UsbTransport::fill(std::size_t n) {
    if (pendingPos_ == pending_.size()) pending_.clear(), pendingPos_ = 0;
    while (pending_.size() - pendingPos_ < n) {
        // ask for what is still missing, in whole packets: a short packet (or an empty one) ends the transfer
        const std::size_t want = n - (pending_.size() - pendingPos_);
        const std::size_t packet = pipe_->packetSize();
        const std::size_t ask = std::min(kMaxRead, (want + packet - 1) / packet * packet);
        const std::size_t at = pending_.size();
        pending_.resize(at + ask);
        std::size_t got = 0;
        try {
            got = pipe_->read(std::span(pending_).subspan(at, ask), timeout_);
        } catch (...) {
            pending_.resize(at);
            throw;
        }
        pending_.resize(at + got);
    }
}

UsbTransport::Header UsbTransport::nextHeader() {
    for (;;) {
        fill(kHeaderBytes);
        Reader r{std::span<const uint8_t>(pending_).subspan(pendingPos_, kHeaderBytes)};
        Header h{r.u32(), r.u16(), r.u16(), r.u32()};
        pendingPos_ += kHeaderBytes;
        if (h.type != kEventBlock) return h;
        std::vector<uint8_t> skip;   // events belong on the interrupt pipe; some devices also send them here
        readPayload(h.length > kHeaderBytes ? h.length - kHeaderBytes : 0, skip);
    }
}

void UsbTransport::readPayload(std::size_t n, std::vector<uint8_t>& dst) {
    dst.reserve(dst.size() + n);
    while (n > 0) {
        fill(std::min(n, kMaxRead));
        const std::size_t take = std::min(n, pending_.size() - pendingPos_);
        dst.insert(dst.end(), pending_.begin() + static_cast<std::ptrdiff_t>(pendingPos_),
                   pending_.begin() + static_cast<std::ptrdiff_t>(pendingPos_ + take));
        pendingPos_ += take;
        n -= take;
    }
}

}  // namespace slopcap::ptp

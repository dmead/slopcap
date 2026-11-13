#include "camera/ptp_wpd.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace slopcap::ptp::wpd {

namespace {

constexpr uint32_t kDefaultChunk = 256 * 1024;   // read / write size when the driver doesn't suggest one

}  // namespace

std::string hresultText(int32_t hr) {
    const char* what = nullptr;
    switch (static_cast<uint32_t>(hr)) {
        case 0x80070005: what = "access denied"; break;
        case 0x80070015: what = "device not ready"; break;
        case 0x8007001F: what = "device not functioning"; break;
        case 0x80070032: what = "not supported"; break;
        case 0x80070057: what = "invalid parameter"; break;
        case 0x80070079: what = "timed out"; break;
        case 0x800700AA: what = "busy"; break;
        case 0x8007048F: what = "device not connected"; break;
        case 0x800704C7: what = "cancelled"; break;
        case 0x80004005: what = "unspecified failure"; break;
        case 0x8000FFFF: what = "unexpected failure"; break;
        default: break;
    }
    return what ? std::format("0x{:08X} {}", static_cast<uint32_t>(hr), what) : std::format("0x{:08X}", static_cast<uint32_t>(hr));
}

WpdTransport::WpdTransport(std::unique_ptr<Device> device) : dev_(std::move(device)) {}

Values WpdTransport::send(Command c, const Values& in, std::string_view what) {
    Values r = dev_->send(c, in);
    if (r.hresult < 0) throw Error(std::format("{}: Windows answered {}", what, hresultText(r.hresult)));
    return r;
}

std::vector<uint32_t> WpdTransport::vendorOpcodes() { return send(Command::VendorOpcodes, {}, "list the vendor operations").params; }

Response WpdTransport::finish(const std::string& context, uint16_t op) {
    Values in;
    in.context = context;
    const Values r = send(Command::EndTransfer, in, std::format("end of operation 0x{:04X}", op));
    return {static_cast<uint16_t>(r.responseCode.value_or(0)), r.params};
}

Response WpdTransport::transact(uint16_t op, std::span<const uint32_t> params, Data phase, std::span<const uint8_t> out,
                                std::vector<uint8_t>* in) {
    if (params.size() > 5) throw Error(std::format("PTP operation 0x{:04X}: {} parameters (at most 5)", op, params.size()));
    Values cmd;
    cmd.opCode = op;
    cmd.params.assign(params.begin(), params.end());
    const auto what = std::format("operation 0x{:04X}", op);
    if (phase == Data::None) {
        const Values r = send(Command::ExecuteNoData, cmd, what);
        return {static_cast<uint16_t>(r.responseCode.value_or(0)), r.params};
    }
    if (phase == Data::Out) {
        cmd.totalSize = out.size();
        const Values started = send(Command::ExecuteWrite, cmd, what);
        try {
            const uint32_t chunk = started.optimalBuffer.value_or(0) ? *started.optimalBuffer : kDefaultChunk;
            for (std::size_t at = 0; at < out.size();) {
                Values w;
                w.context = started.context;
                const auto n = static_cast<uint32_t>(std::min<std::size_t>(chunk, out.size() - at));
                w.bytes = n;
                w.data.assign(out.begin() + static_cast<std::ptrdiff_t>(at), out.begin() + static_cast<std::ptrdiff_t>(at + n));
                const Values r = send(Command::WriteData, w, what);
                const uint32_t done = r.bytes.value_or(n);
                if (done == 0) throw Error(std::format("{}: the camera took no data", what));
                at += done;
            }
        } catch (...) {
            try {
                finish(started.context, op);   // releases the transfer
            } catch (const std::exception&) {
            }
            throw;
        }
        return finish(started.context, op);
    }
    const Values started = send(Command::ExecuteRead, cmd, what);
    std::vector<uint8_t> scratch;
    auto& dst = in ? *in : scratch;
    dst.clear();
    try {
        const uint64_t total = started.totalSize.value_or(0);
        if (total > kMaxData) throw Error(std::format("{}: {} bytes announced", what, total));
        const uint32_t chunk = started.optimalBuffer.value_or(0) ? *started.optimalBuffer : kDefaultChunk;
        dst.reserve(static_cast<std::size_t>(total));
        while (dst.size() < total) {
            Values rd;
            rd.context = started.context;
            rd.bytes = static_cast<uint32_t>(std::min<uint64_t>(chunk, total - dst.size()));
            const Values r = send(Command::ReadData, rd, what);
            const std::size_t got = std::min<std::size_t>(r.bytes.value_or(static_cast<uint32_t>(r.data.size())), r.data.size());
            if (got == 0) break;   // the camera sent less than it announced
            dst.insert(dst.end(), r.data.begin(), r.data.begin() + static_cast<std::ptrdiff_t>(got));
        }
    } catch (...) {
        try {
            finish(started.context, op);
        } catch (const std::exception&) {
        }
        throw;
    }
    return finish(started.context, op);
}

std::string usbIdsOf(const std::string& id) {
    std::string lower = id;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto v = lower.find("vid_");
    if (v == std::string::npos) return {};
    const auto p = lower.find("pid_", v);
    if (p == std::string::npos || p + 8 > lower.size()) return lower.substr(v, 8);
    return lower.substr(v, 8) + "&" + lower.substr(p, 8);
}

#ifndef _WIN32
std::vector<DeviceEntry> listDevices() { return {}; }
std::unique_ptr<Device> openDevice(const std::string&) { throw CameraError("Windows Portable Devices: only on Windows"); }
#endif

}  // namespace slopcap::ptp::wpd

#include "mounts/open_mount.hpp"

#include "mounts/lx200.hpp"
#include "mounts/nexstar.hpp"
#include "mounts/onstep.hpp"
#include "mounts/sim_mount.hpp"
#include "mounts/skywatcher.hpp"
#include "mounts/synscan_app.hpp"
#include "mounts/transport.hpp"

namespace slopcap::mounts {

namespace {

struct Spec {
    std::string kind, where;
};

Spec split(const std::string& spec) {
    const auto colon = spec.find(':');
    if (colon == std::string::npos) return {spec, {}};
    return {spec.substr(0, colon), spec.substr(colon + 1)};
}

const char* const kKinds[] = {"sim", "lx200", "lx200-classic", "lx200-meade", "lx200-onstep", "onstep", "nexstar", "synscan", "skywatcher", "synscan-app"};

}  // namespace

bool isBuiltInMount(const std::string& spec) {
    const auto kind = split(spec).kind;
    for (const char* k : kKinds)
        if (kind == k) return true;
    return false;
}

std::function<std::unique_ptr<IMountDriver>()> mountFactory(const std::string& spec, const std::optional<Site>& site) {
    const auto [kind, where] = split(spec);
    auto link = [w = where](int baud, int port) -> TransportFactory { return [w, baud, port] { return openTransport(w, baud, port); }; };
    if (kind == "sim") return [] { return std::make_unique<SimDriver>(); };
    if (!isBuiltInMount(spec)) throw MountError("not a mount SlopCap drives itself: " + spec);
    if (kind == "synscan-app") {
        const std::string w = where.empty() ? "udp://127.0.0.1" : where;
        return [w] { return std::make_unique<SynScanAppDriver>([w] { return openTransport(w, 0, 11881); }); };
    }
    if (where.empty()) throw MountError(kind + " needs a port: " + kind + ":COM3, " + kind + ":/dev/ttyUSB0 or " + kind + ":tcp://HOST:PORT");
    const bool speedGiven = where.find("://") != std::string::npos || where.find('@') != std::string::npos;
    if (kind == "onstep" || kind == "lx200-onstep") {
        // OnStep's own driver; a serial port without @BAUD tries OnStepX's default, then the speeds ESP32 builds use
        std::vector<TransportFactory> opens;
        for (const int baud : speedGiven ? std::vector<int>{9600} : std::vector<int>{9600, 230400, 115200, 57600, 38400, 19200})
            opens.push_back(link(baud, 9999));
        return [opens, site] { return std::make_unique<OnStepDriver>(opens, site); };
    }
    if (kind.rfind("lx200", 0) == 0) {
        const auto dialect = kind == "lx200-classic" ? Lx200Dialect::MeadeClassic
                             : kind == "lx200-meade" ? Lx200Dialect::Meade
                                                     : Lx200Dialect::Auto;
        return [open = link(9600, 9999), dialect] { return std::make_unique<Lx200Driver>(open, dialect); };
    }
    if (kind == "nexstar" || kind == "synscan")
        return [open = link(9600, 4030), hc = kind == "synscan" ? HandController::SynScan : HandController::Celestron] {
            return std::make_unique<NexStarDriver>(open, hc);
        };
    // skywatcher: the mount's own USB port runs at 115200 baud, EQDIR cables at 9600; try both unless told
    std::vector<TransportFactory> opens;
    if (speedGiven) opens = {link(9600, 11880)};
    else opens = {link(115200, 11880), link(9600, 11880)};
    return [opens, site] { return std::make_unique<SkyWatcherDriver>(opens, site); };
}

std::string mountSpecHelp() {
    std::string s =
        "Mounts SlopCap drives itself (--mount SPEC, or Settings > Mount):\n"
        "  sim                        a simulated mount\n"
        "  lx200:PORT                 Meade LX200, Autostar, LX200GPS or OnStep (asks which)\n"
        "  onstep:PORT                OnStep / OnStepX: finds the speed itself (or COM6@230400);\n"
        "                             its Wi-Fi: onstep:tcp://HOST (port 9999)\n"
        "  nexstar:PORT               Celestron, through the hand controller\n"
        "  synscan:PORT               Sky-Watcher, through the SynScan hand controller\n"
        "  skywatcher:PORT            Sky-Watcher motor controller: USB on the mount, EQDIR cable,\n"
        "                             or its Wi-Fi: skywatcher:udp://192.168.4.1\n"
        "  synscan-app[:udp://HOST]   the SynScan app (this computer by default)\n"
        "PORT: a serial port (add @BAUD to set the speed), tcp://HOST:PORT or udp://HOST[:PORT].\n"
        "Anything else is an INDI server's mount (the focuser's server).\n\nSerial ports here:\n";
    const auto ports = listSerialPorts();
    for (const auto& p : ports) s += "  " + p + "\n";
    if (ports.empty()) s += "  (none)\n";
    return s;
}

}  // namespace slopcap::mounts

// slopcap-mountprobe: one of SlopCap's own mount drivers from the command line (docs/design/mounts.md §6), for trying a
// driver on a real mount without the app.
//   slopcap-mountprobe SPEC [--site LAT,LON] [--settings] [--set KEY=VALUE ...] [--move] [--log]
// Without --set or --move it only reads: what the mount is, what the driver found on connecting (limits, guide rate, a
// clock or site that's off) and its state; --settings adds the mount's own settings (OnStep). --set writes one of them
// the way Mount > Settings and log... does (KEY as the driver names it: horizon=-10, buzzer=1, goHome) and reads them
// back. --move then, with tracking on, sends 1 s guide pulses each way, short moves at 8x
// and a visible one at half the maximum speed east and back west, a goto 10 degrees south of where it starts, and
// (OnStep) back home, printing where the mount says it is after each, and how far that is from before. Stop it with
// Ctrl+C: the driver is told to stop first. --log prints every command and reply.
#include "mounts/onstep.hpp"
#include "mounts/open_mount.hpp"
#include "mounts/transport.hpp"

#include <QCoreApplication>
#include <QLoggingCategory>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace slopcap::mounts;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void sleepMs(int ms) {
    for (int t = 0; t < ms && !g_stop; t += 20) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

const char* pierText(PierSide p) { return p == PierSide::East ? "east" : p == PierSide::West ? "west" : "-"; }

void show(const char* what, const MountStatus& st) {
    std::printf("%-26s RA %10.6f h  Dec %+10.5f   %s%s%s%s pier %s%s%s\n", what, st.ra, st.dec,
                st.tracking ? (*st.tracking ? "tracking" : "not tracking") : "tracking ?", st.slewing ? ", slewing" : "",
                st.parked && *st.parked ? ", parked" : "", st.atHome ? ", at home" : "", pierText(st.pier),
                st.fault.empty() ? "" : ", FAULT: ", st.fault.c_str());
    std::fflush(stdout);
}

/// How far b is from a: RA (as an angle on the sky, arcsec), Dec (arcsec)
void delta(const char* what, const MountStatus& a, const MountStatus& b) {
    const double dRa = std::remainder(b.ra - a.ra, 24.0) * 15 * 3600 * std::cos(a.dec * 3.14159265358979 / 180);
    const double dDec = (b.dec - a.dec) * 3600;
    std::printf("  %-24s moved RA %+9.1f\"  Dec %+9.1f\"\n", what, dRa, dDec);
    std::fflush(stdout);
}

MountStatus settle(IMountDriver& d, int ms = 120000) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        auto st = d.poll();
        if ((!st.slewing && !st.pulsing) || g_stop || std::chrono::steady_clock::now() - start > std::chrono::milliseconds(ms)) return st;
        sleepMs(250);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    std::string spec;
    std::optional<Site> site;
    bool doMove = false, doSettings = false;
    std::vector<std::string> sends;
    std::vector<std::pair<std::string, std::string>> sets;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--move")) doMove = true;
        else if (!std::strcmp(argv[i], "--settings")) doSettings = true;
        else if (!std::strcmp(argv[i], "--set") && i + 1 < argc) {
            const std::string kv = argv[++i];
            const auto eq = kv.find('=');
            sets.emplace_back(kv.substr(0, eq), eq == std::string::npos ? std::string() : kv.substr(eq + 1));
            doSettings = true;
        }
        else if (!std::strcmp(argv[i], "--log")) QLoggingCategory::setFilterRules("slopcap.mount.debug=true");
        else if (!std::strcmp(argv[i], "--site") && i + 1 < argc) {
            Site s;
            if (std::sscanf(argv[++i], "%lf,%lf", &s.latitude, &s.longitude) == 2) site = s;
        } else if (!std::strcmp(argv[i], "--send") && i + 1 < argc) sends.emplace_back(argv[++i]);
        else spec = argv[i];
    }
    if (spec.empty()) {
        std::fputs("usage: slopcap-mountprobe SPEC [--site LAT,LON] [--settings] [--set KEY=VALUE ...] [--move] [--log]\n"
                   "       slopcap-mountprobe PORT --send CMD [--send CMD ...]   raw commands; replies up to '#' or 500 ms\n"
                   "                                                          (\"wait:MS\" pauses)\n\n",
                   stderr);
        std::fputs(mountSpecHelp().c_str(), stderr);
        return 2;
    }
    std::signal(SIGINT, [](int) { g_stop = 1; });

    if (!sends.empty()) {   // raw: the spec is just where (COM6@230400, /dev/ttyUSB0@230400, tcp://HOST:9999)
        try {
            const auto colon = spec.find(':');
            const auto where = colon != std::string::npos && spec.find("://") != colon ? spec.substr(colon + 1) : spec;
            const auto t = openTransport(where, 9600, 9999);
            for (const auto& c : sends) {
                if (c.rfind("wait:", 0) == 0) {
                    sleepMs(std::atoi(c.c_str() + 5));
                    continue;
                }
                t->drain();
                t->write(c);
                std::string got;   // everything up to a '#', or what came in 500 ms
                const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                while (got.find('#') == std::string::npos && std::chrono::steady_clock::now() < end) got += t->read(50);
                std::printf("%-18s < %s\n", c.c_str(), got.empty() ? "(nothing)" : printable(got).c_str());
                std::fflush(stdout);
            }
            return 0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
    }

    std::unique_ptr<IMountDriver> d;
    try {
        d = mountFactory(spec, site)();
        const auto info = d->connect();
        std::printf("mount      %s (%s)\n", info.model.c_str(), info.backend.c_str());
        if (!info.details.empty()) std::printf("details    %s\n", info.details.c_str());
        std::string rates;
        for (const auto& r : info.rates) rates += (rates.empty() ? "" : ", ") + r;
        std::printf("rates      %s\n", rates.c_str());
        std::printf("can        %s%s%s%s%s%s%s%s\n", info.caps.pulse ? "pulse " : "", info.caps.park ? "park " : "",
                    info.caps.unpark ? "unpark " : "", info.caps.tracking ? "tracking " : "", info.caps.trackRates ? "rates " : "",
                    info.caps.sync ? "sync " : "", info.caps.rateOffsets ? "rate-offsets " : "", info.caps.settings ? "settings" : "");
        for (const auto& n : info.notes) std::printf("NOTE       %s\n", n.c_str());
        const auto s0 = d->poll();
        show("now", s0);
        for (const auto& [key, value] : sets) {   // written as Settings and log... writes them, then read back below
            try {
                d->changeSetting(key, value);
                std::printf("set        %s%s%s\n", key.c_str(), value.empty() ? "" : " = ", value.c_str());
            } catch (const MountError& e) {
                std::printf("REFUSED    %s%s%s: %s\n", key.c_str(), value.empty() ? "" : " = ", value.c_str(), e.what());
            }
            std::fflush(stdout);
        }
        if (doSettings) {   // what Mount > Settings and log would show
            std::string group;
            for (const auto& s : d->settings()) {
                if (s.group != group) std::printf("\n[%s]\n", (group = s.group).c_str());
                if (s.kind == MountSetting::Kind::Action) std::printf("  %-14s (action) %s\n", s.key.c_str(), s.label.c_str());
                else std::printf("  %-14s %-34s %s%s%s\n", s.key.c_str(), s.label.c_str(), s.value.c_str(), s.unit.empty() ? "" : " ", s.unit.c_str());
            }
        }
        if (!doMove) return 0;

        auto* onStep = dynamic_cast<OnStepDriver*>(d.get());
        auto run = [&](const char* what, auto act) {
            if (g_stop) throw MountError("stopped (Ctrl+C)");
            const auto before = settle(*d);
            act();
            const auto after = settle(*d);
            show(what, after);
            delta(what, before, after);
            return after;
        };
        d->setTracking(true);
        show("tracking on", d->poll());
        for (const Dir dir : {Dir::East, Dir::West, Dir::South, Dir::North}) {   // south first: it may start at the pole
            const char* name[] = {"pulse N 1000 ms", "pulse S 1000 ms", "pulse E 1000 ms", "pulse W 1000 ms"};
            run(name[static_cast<int>(dir)], [&] {
                d->pulse(dir, 1000);
                sleepMs(1500);
            });
        }
        auto timedMove = [&](Dir dir, int rate, int ms) {
            d->move(dir, rate);
            sleepMs(ms);
            d->stop(dir);
            sleepMs(1500);   // the mount decelerates
        };
        run("move S 8x 2 s", [&] { timedMove(Dir::South, 5, 2000); });
        run("move N 8x 2 s", [&] { timedMove(Dir::North, 5, 2000); });
        run("move E 0.5x 2 s (slow)", [&] { timedMove(Dir::East, 1, 2000); });
        run("move E half max 3 s", [&] { timedMove(Dir::East, 8, 3000); });
        run("move W half max 3 s", [&] { timedMove(Dir::West, 8, 3000); });
        const auto here = d->poll();
        run("goto 10 deg south", [&] { d->slewTo(here.ra, std::min(here.dec, 89.0) - 10); });
        if (onStep) {
            run("home", [&] {
                onStep->home();
                sleepMs(1000);
            });
        }
        d->setTracking(false);
        show("tracking off", d->poll());
        const auto again = d->connect();   // what the mount says now (the pulse-guide rate)
        std::printf("details    %s\n", again.details.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        if (d) try {
                d->abort();
            } catch (...) {
            }
        return 1;
    }
}

// SlopCap's own mount drivers (src/mounts/) against fakes of each protocol (tests/fakes/fake_mounts.*): one
// conformance body for every driver, then what's particular to each, the worker thread they run on, the Mount they sit
// behind, and the links themselves (a pseudo-terminal for serial, TCP and UDP on this machine).
#include "devices/indi_client.hpp"
#include "devices/mount.hpp"
#include "fakes/fake_mounts.hpp"
#include "mounts/command_channel.hpp"
#include "mounts/mount_worker.hpp"
#include "mounts/open_mount.hpp"
#include "mounts/skywatcher.hpp"
#include "mounts/synscan_app.hpp"
#include "analysis/ephemeris.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <future>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace slopcap;
using namespace slopcap::mounts;
using namespace slopcap::test;
using Catch::Approx;

namespace {

template <typename F>
bool waitFor(F done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done();
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::shared_ptr<SimMount> quickSim() {
    auto sim = std::make_shared<SimMount>();
    sim->slewRate = 30;     // degrees per second: gotos in a moment
    sim->guideRate = 1.0;   // pulses big enough to measure
    return sim;
}

double raDiffDeg(double a, double b) { return std::abs(std::remainder(a - b, 24.0)) * 15.0; }

MountStatus settle(IMountDriver& d, int ms = 10000) {
    QElapsedTimer t;
    t.start();
    for (;;) {
        const auto st = d.poll();
        if (!st.slewing || t.elapsed() > ms) return st;
        sleepMs(30);
    }
}

/// What every driver must do, against a fake mount whose truth is `sim`. `tol`: the protocol's resolution, degrees.
void conformance(IMountDriver& d, SimMount& sim, const std::string& backend, double tol) {
    const auto info = d.connect();
    CHECK(info.backend == backend);
    CHECK_FALSE(info.model.empty());
    REQUIRE(info.rates.size() >= 2);

    sim.place(10.0, 30.0);
    auto st = d.poll();
    REQUIRE(st.haveCoords);
    CHECK(raDiffDeg(st.ra, 10.0) < tol + 0.01);
    CHECK(st.dec == Approx(30.0).margin(tol));

    // a goto, and the mount tracking there
    d.slewTo(11.5, -12.25);
    st = settle(d);
    CHECK_FALSE(st.slewing);
    CHECK(raDiffDeg(st.ra, 11.5) < tol + 0.01);
    CHECK(st.dec == Approx(-12.25).margin(tol));
    if (st.tracking) CHECK(*st.tracking);

    // stopped half-way
    d.slewTo(20.0, 80.0);
    d.abort();
    CHECK_FALSE(d.poll().slewing);

    if (info.caps.tracking) {
        d.setTracking(false);
        CHECK(d.poll().tracking == false);
        d.setTracking(true);
        CHECK(d.poll().tracking == true);
    }

    // moves: north raises Dec, east raises RA (both against the sky)
    sim.place(5.0, 10.0);
    d.move(Dir::North, 1);
    sleepMs(300);
    d.stop(Dir::North);
    CHECK(d.poll().dec > 10.0 + tol);
    const double ra0 = d.poll().ra;
    d.move(Dir::East, 1);
    sleepMs(300);
    d.stop(Dir::East);
    CHECK(std::remainder(d.poll().ra - ra0, 24.0) * 15.0 > tol);
    const double dec1 = d.poll().dec;
    d.move(Dir::South, 1);
    sleepMs(300);
    d.stop(Dir::South);
    CHECK(d.poll().dec < dec1 - tol);

    if (info.caps.pulse) {
        const double dec0 = d.poll().dec;
        d.pulse(Dir::North, 300);
        sleepMs(450);
        CHECK(d.poll().dec > dec0 + tol);
    }
    if (info.caps.trackRates) {
        d.setTrackRate(TrackRate::Lunar);
        CHECK(sim.state().rate == TrackRate::Lunar);
        d.setTrackRate(TrackRate::Sidereal);
    }
    if (info.caps.sync) {
        d.sync(12.0, 40.0);
        st = d.poll();
        CHECK(raDiffDeg(st.ra, 12.0) < tol + 0.01);
        CHECK(st.dec == Approx(40.0).margin(tol));
    }
    if (info.caps.park) {
        d.park();
        settle(d);
        CHECK(sim.state().parked);
        if (const auto p = d.poll().parked) CHECK(*p);
        if (info.caps.unpark) {
            d.unpark();
            CHECK_FALSE(sim.state().parked);
        } else {
            sim.unpark();
        }
    }
    d.disconnect();
}

}  // namespace

TEST_CASE("mounts: LX200 dialects, NexStar, SynScan, the SynScan app and the simulator pass the same checks", "[mounts]") {
    const double arcsec = 1.5 / 3600;
    SECTION("LX200: Meade Classic (no pulses, no tracking control)") {
        auto sim = quickSim();
        auto link = std::make_shared<LinkState>();
        Lx200Driver d(fakeLink(fakeLx200(sim, Lx200Dialect::MeadeClassic), link), Lx200Dialect::Auto);
        conformance(d, *sim, "lx200", arcsec);
        CHECK(d.dialect() == Lx200Dialect::MeadeClassic);
        CHECK(link->wrote(":U#"));   // it started in low precision
    }
    SECTION("LX200: Meade Autostar") {
        auto sim = quickSim();
        Lx200Driver d(fakeLink(fakeLx200(sim, Lx200Dialect::Meade), std::make_shared<LinkState>()), Lx200Dialect::Auto);
        conformance(d, *sim, "lx200", arcsec);
        CHECK(d.dialect() == Lx200Dialect::Meade);
    }
    SECTION("LX200: OnStep") {
        auto sim = quickSim();
        Lx200Driver d(fakeLink(fakeLx200(sim, Lx200Dialect::OnStep), std::make_shared<LinkState>()), Lx200Dialect::Auto);
        conformance(d, *sim, "lx200", arcsec);
        CHECK(d.dialect() == Lx200Dialect::OnStep);
    }
    SECTION("NexStar (Celestron)") {
        auto sim = quickSim();
        NexStarDriver d(fakeLink(fakeNexStar(sim, HandController::Celestron), std::make_shared<LinkState>()), HandController::Auto);
        conformance(d, *sim, "nexstar", 0.001);
        CHECK(d.kind() == HandController::Celestron);
    }
    SECTION("SynScan hand controller") {
        auto sim = quickSim();
        NexStarDriver d(fakeLink(fakeNexStar(sim, HandController::SynScan), std::make_shared<LinkState>()), HandController::Auto);
        conformance(d, *sim, "synscan", 0.001);
        CHECK(d.kind() == HandController::SynScan);
    }
    SECTION("the SynScan app") {
        auto sim = quickSim();
        SynScanAppDriver d(fakeLink(fakeSynScanApp(sim), std::make_shared<LinkState>(), true));
        conformance(d, *sim, "synscan-app", 1e-6);
    }
    SECTION("the simulator") {
        auto sim = quickSim();
        SimDriver d(sim);
        conformance(d, *sim, "sim", 1e-6);
    }
}

TEST_CASE("mounts: what each protocol says when it can't", "[mounts]") {
    auto sim = quickSim();
    // nothing on the line
    Lx200Driver silent(fakeLink([](std::string_view) { return std::string(); }, std::make_shared<LinkState>()), Lx200Dialect::Auto);
    CHECK_THROWS_AS(silent.connect(), MountError);
    CHECK_THROWS_AS(silent.poll(), MountError);   // not connected
    NexStarDriver deaf(fakeLink([](std::string_view) { return std::string(); }, std::make_shared<LinkState>()), HandController::Auto);
    CHECK_THROWS_AS(deaf.connect(), MountError);

    // a parked mount refuses gotos: OnStep with a code, Meade with its message
    for (const auto dialect : {Lx200Dialect::OnStep, Lx200Dialect::Meade}) {
        sim->unpark();
        Lx200Driver d(fakeLink(fakeLx200(sim, dialect), std::make_shared<LinkState>()), dialect);
        d.connect();
        sim->park();
        settle(d);
        try {
            d.slewTo(3, 3);
            FAIL("the goto went through");
        } catch (const MountError& e) {
            const std::string why = e.what();
            CHECK(why.find(dialect == Lx200Dialect::OnStep ? "parked" : "below horizon") != std::string::npos);
        }
        if (dialect == Lx200Dialect::Meade) CHECK_THROWS_AS(d.setTrackRate(TrackRate::Solar), MountError);
    }
    sim->unpark();

    // an unaligned hand controller has no coordinates and won't go anywhere
    NexStarDriver unaligned(fakeLink(fakeNexStar(sim, HandController::SynScan, false), std::make_shared<LinkState>()), HandController::Auto);
    CHECK(unaligned.connect().model.find("not aligned") != std::string::npos);
    CHECK_FALSE(unaligned.poll().haveCoords);
    CHECK_THROWS_AS(unaligned.slewTo(1, 1), MountError);

    // the app's errors, and the app over a stream (it can't: no way to tell where a reply ends)
    SynScanAppDriver app(fakeLink(fakeSynScanApp(sim), std::make_shared<LinkState>(), true));
    app.connect();
    sim->park();
    settle(app);
    CHECK_THROWS_AS(app.slewTo(1, 1), MountError);
    sim->unpark();
    SynScanAppDriver overTcp(fakeLink(fakeSynScanApp(sim), std::make_shared<LinkState>(), false));
    CHECK_THROWS_AS(overTcp.connect(), MountError);

    // coordinates as the protocols write them
    CHECK(*parseLx200Ra("12:34:56") == Approx(12 + 34 / 60.0 + 56 / 3600.0));
    CHECK(*parseLx200Ra("12:34.5") == Approx(12 + 34.5 / 60.0));
    CHECK(*parseLx200Dec("-05*30'00") == Approx(-5.5));
    CHECK(*parseLx200Dec("+45\xdf" "30:15") == Approx(45 + 30 / 60.0 + 15 / 3600.0));
    CHECK(*parseLx200Dec("-00*30") == Approx(-0.5));
    CHECK_FALSE(parseLx200Ra("25:00:00"));
    CHECK_FALSE(parseLx200Dec("garbage"));
    const auto c = parseNexStarRaDec(nexStarRaDec(23.999, -45.0));
    REQUIRE(c);
    CHECK(c->first == Approx(23.999).margin(1e-5));
    CHECK(c->second == Approx(-45.0).margin(1e-5));
    CHECK(parseNexStarRaDec("34AB,12CE")->second == Approx(4814 / 65536.0 * 360));   // the document's example
    CHECK_FALSE(parseNexStarRaDec("34AB0500"));
    CHECK(skyWatcherHex24(0x123456) == "563412");   // the document's example
    CHECK(skyWatcherValue("563412") == 0x123456);
    CHECK(skyWatcherValue("12") == 0x12);
    CHECK(printable(std::string("\x06:GR#\r", 6)) == "\\x06:GR#\\x0d");
}

TEST_CASE("mounts: Sky-Watcher's motor controller, straight", "[mounts]") {
    FakeSkyWatcherMc mc;
    const Site site{51.5, -0.1};
    auto link = std::make_shared<LinkState>();
    // the first link doesn't answer (the wrong baud rate): the driver tries the next
    SkyWatcherDriver d({fakeLink([](std::string_view) { return std::string(); }, std::make_shared<LinkState>()), fakeLink(mc.device(), link)}, site);
    const auto info = d.connect();
    CHECK(info.backend == "skywatcher");
    CHECK(info.rates.size() == 7);
    CHECK(mc.axis(0).init);   // just switched on: it initialised the axes at home
    // at home it points at the pole
    auto st = d.poll();
    REQUIRE(st.haveCoords);
    CHECK(st.dec == Approx(90).margin(0.01));

    // goto, twice over (the sky turns meanwhile), then tracking
    const double lst = analysis::localSiderealDeg(analysis::julianDate(std::chrono::system_clock::now()), site.longitude) / 15.0;
    const double ra = std::fmod(lst + 1.5 + 24, 24.0);   // an hour and a half east of the meridian
    d.slewTo(ra, 20.0);
    CHECK(d.poll().slewing);
    st = settle(d, 20000);
    CHECK_FALSE(st.slewing);
    CHECK(raDiffDeg(st.ra, ra) < 0.02);
    CHECK(st.dec == Approx(20).margin(0.02));
    CHECK(st.tracking == true);
    CHECK(mc.axis(0).running);   // the RA axis tracks
    sleepMs(500);
    CHECK(raDiffDeg(d.poll().ra, ra) < 0.02);   // and holds the sky still

    // moves against the sky: north, east, and back to tracking
    const double dec0 = d.poll().dec;
    d.move(Dir::North, 6);
    sleepMs(300);
    d.stop(Dir::North);
    CHECK(d.poll().dec > dec0 + 0.05);
    const double ra0 = d.poll().ra;
    d.move(Dir::East, 5);
    sleepMs(300);
    d.stop(Dir::East);
    CHECK(std::remainder(d.poll().ra - ra0, 24.0) * 15.0 > 0.05);
    CHECK(mc.axis(0).running);
    CHECK_FALSE(mc.axis(0).ccw);   // tracking again
    d.move(Dir::West, 2);
    d.stop(Dir::West);
    d.move(Dir::South, 1);
    d.stop(Dir::South);

    // sync shifts the model
    d.sync(std::fmod(ra + 0.1, 24.0), 21.0);
    st = d.poll();
    CHECK(raDiffDeg(st.ra, ra + 0.1) < 0.02);
    CHECK(st.dec == Approx(21).margin(0.02));

    d.setTrackRate(TrackRate::Lunar);
    d.setTracking(false);
    CHECK_FALSE(mc.axis(0).running);
    CHECK(d.poll().tracking == false);
    d.setTracking(true);
    d.slewTo(std::fmod(lst - 2 + 24, 24.0), 60.0);   // west of the meridian: the other side of the pier
    d.abort();
    CHECK_FALSE(d.poll().slewing);

    // park at home, refuse to move there, unpark
    d.park();
    st = settle(d, 20000);
    CHECK(st.parked == true);
    CHECK(st.dec == Approx(90).margin(0.05));
    CHECK_THROWS_AS(d.slewTo(ra, 20), MountError);
    CHECK_THROWS_AS(d.move(Dir::North, 0), MountError);
    d.unpark();
    CHECK(d.poll().tracking == true);
    d.disconnect();

    // without a site: no coordinates, no gotos, moves still work
    SkyWatcherDriver noSite({fakeLink(mc.device(), std::make_shared<LinkState>())}, std::nullopt);
    CHECK(noSite.connect().model.find("no site") != std::string::npos);
    CHECK_FALSE(noSite.poll().haveCoords);
    CHECK_THROWS_AS(noSite.slewTo(1, 1), MountError);
    CHECK_THROWS_AS(noSite.sync(1, 1), MountError);
    noSite.move(Dir::North, 1);
    noSite.stop(Dir::North);
    // the controller's own errors
    SkyWatcherDriver broken({fakeLink([](std::string_view) { return std::string("!0\r"); }, std::make_shared<LinkState>())}, site);
    try {
        broken.connect();
        FAIL("connected to a controller that knows no commands");
    } catch (const MountError& e) {
        CHECK(std::string(e.what()).find("unknown command") != std::string::npos);
    }
}

TEST_CASE("mounts: the worker times pulses, speaks JNow, stops moves and survives a lost link", "[mounts]") {
    auto sim = quickSim();
    auto link = std::make_shared<LinkState>();
    // Meade Classic has no pulses: the worker moves at the guide rate and stops it
    {
        MountWorker w([sim, link] { return std::make_unique<Lx200Driver>(fakeLink(fakeLx200(sim, Lx200Dialect::MeadeClassic), link), Lx200Dialect::Auto); },
                      nullptr, 300, 400);
        REQUIRE(waitFor([&] { return w.connected(); }, 5000));
        CHECK(w.info().model == "LX200 Classic");
        REQUIRE(waitFor([&] { return w.status().haveCoords; }, 5000));
        const double dec0 = w.status().dec;
        int pulsingSeen = 0;
        QObject::connect(&w, &MountWorker::statusChanged, [&] { pulsingSeen += w.status().pulsing; });
        w.pulse(Dir::North, 250);
        CHECK(waitFor([&] { return link->wrote(":Qn#"); }, 3000));
        CHECK(link->wrote(":RG#"));
        CHECK(link->wrote(":Mn#"));
        CHECK(waitFor([&] { return w.status().dec > dec0; }, 3000));
        CHECK(waitFor([&] { return !w.status().pulsing; }, 3000));
        // a move nobody renews stops by itself (400 ms here, 10 s for real)
        QStringList failures;
        QObject::connect(&w, &MountWorker::failed, [&](const QString& why) { failures << why; });
        w.move(Dir::East, 2);
        CHECK(waitFor([&] { return link->wrote(":Qe#"); }, 3000));
        CHECK(waitFor([&] { return failures.join(" ").contains("renewed"); }, 3000));
        // abort drops the queued motion; Stop goes first
        w.goTo(14.0, 30.0);
        w.abort();
        w.stopMoves();
        w.setTracking(true);
        w.sync(14.0, 30.0);
        w.setTrackRate(TrackRate::Sidereal);   // (Classic: no tracking rates: an error, said)
        CHECK(waitFor([&] { return failures.join(" ").contains("sidereal and lunar") || link->wrote(":CM#"); }, 3000));
        // the link drops: lost, then found again
        bool lostIt = false;
        QObject::connect(&w, &MountWorker::connectedChanged, [&](bool ok) { lostIt = lostIt || !ok; });
        link->dead = true;
        CHECK(waitFor([&] { return lostIt; }, 10000));
        CHECK(failures.join(" ").contains("lost the mount"));
        w.goTo(1, 1);   // not connected: said, not queued
        CHECK(waitFor([&] { return failures.join(" ").contains("isn't connected"); }, 3000));
        link->dead = false;
        CHECK(waitFor([&] { return w.connected(); }, 10000));
        w.park();
        w.unpark();
    }
    // a J2000 mount: the worker reports JNow and sends J2000
    {
        sim->place(6.0, 22.0);
        MountWorker w([sim] { return std::make_unique<SynScanAppDriver>(fakeLink(fakeSynScanApp(sim, true), std::make_shared<LinkState>(), true)); });
        REQUIRE(waitFor([&] { return w.connected() && w.status().haveCoords; }, 5000));
        double ra = 6.0, dec = 22.0;
        analysis::j2000ToDate(ra, dec, analysis::julianDate(std::chrono::system_clock::now()));
        CHECK(raDiffDeg(w.status().ra, ra) < 0.001);
        CHECK(w.status().dec == Approx(dec).margin(0.001));
        CHECK(raDiffDeg(w.status().ra, 6.0) > 0.05);   // 26 years of precession
        w.goTo(ra, dec);
        CHECK(waitFor([&] { return std::abs(sim->state().dec - 22.0) < 0.001 && !sim->state().slewing; }, 5000));
        w.pulse(Dir::South, 100);   // native pulses
        w.move(Dir::North, 0);
        w.pulse(Dir::North, 100);   // (the axis is moving: ignored)
        w.stopMoves();
        sleepMs(200);
    }
    // a mount that never connects says so once, and keeps trying
    {
        MountWorker w([] { return std::make_unique<Lx200Driver>(fakeLink([](std::string_view) { return std::string(); }, std::make_shared<LinkState>()), Lx200Dialect::Auto); },
                      nullptr, 200);
        QStringList failures;
        QObject::connect(&w, &MountWorker::failed, [&](const QString& why) { failures << why; });
        CHECK(waitFor([&] { return !failures.isEmpty(); }, 15000));   // (ACK and :GR#, twice each, 1.5 s a time)
        waitFor([] { return false; }, 1500);
        CHECK(failures.size() == 1);
        CHECK_FALSE(w.connected());
    }
}

TEST_CASE("mounts: specs, and the Mount the app uses, with a built-in driver", "[mounts]") {
    CHECK(isBuiltInMount("lx200:COM3"));
    CHECK(isBuiltInMount("sim"));
    CHECK(isBuiltInMount("synscan-app"));
    CHECK_FALSE(isBuiltInMount(""));
    CHECK_FALSE(isBuiltInMount("indi://localhost:7624"));
    CHECK_THROWS_AS(mountFactory("lx200", std::nullopt), MountError);   // no port
    CHECK_THROWS_AS(mountFactory("indi:x", std::nullopt), MountError);
    for (const char* spec : {"lx200:COM3", "lx200-classic:/dev/ttyUSB0", "lx200-meade:COM1@19200", "onstep:tcp://192.168.1.20", "nexstar:COM4",
                             "synscan:/dev/ttyUSB1", "skywatcher:COM5", "skywatcher:udp://192.168.4.1", "synscan-app", "synscan-app:udp://192.168.1.20"})
        CHECK(mountFactory(spec, Site{51.5, -0.1}));
    CHECK(mountSpecHelp().find("skywatcher:udp://192.168.4.1") != std::string::npos);
    // a port that isn't there: the driver says so when it connects
    CHECK_THROWS_AS(mountFactory("lx200:/dev/no-such-port", std::nullopt)()->connect(), MountError);
    CHECK_THROWS_AS(openTransport("tcp://", 0, 0), MountError);
    CHECK_THROWS_AS(openTransport("", 9600, 0), MountError);

    devices::IndiClient indi;   // (not connected: the built-in driver stands alone)
    devices::Mount mount(indi);
    QStringList failures;
    QObject::connect(&mount, &devices::Mount::failed, [&](const QString& why) { failures << why; });
    mount.setSpec("not-a-mount:1");
    CHECK_FALSE(mount.builtInSpec());
    mount.openBuiltIn();   // nothing to open
    CHECK_FALSE(mount.builtIn());
    mount.setSpec("sim", Site{51.5, -0.1});
    CHECK(mount.builtInSpec());
    mount.openBuiltIn();
    REQUIRE(waitFor([&] { return mount.ready() && mount.hasCoords(); }, 5000));
    CHECK(mount.device() == "Simulator");
    CHECK(mount.canPulse());
    CHECK(mount.slewRates() == QStringList({"Guide", "Centre", "Find", "Max"}));
    CHECK(mount.slewRate() == "Centre");
    mount.setSlewRate("Find");
    CHECK(mount.slewRate() == "Find");
    bool pulseDone = false;
    QObject::connect(&mount, &devices::Mount::pulseDone, [&] { pulseDone = true; });
    mount.pulse(devices::Mount::Direction::West, 200);
    CHECK(mount.pulsing());
    CHECK(waitFor([&] { return pulseDone; }, 5000));
    mount.goTo(8.0, 45.0);
    CHECK(waitFor([&] { return std::abs(mount.dec() - 45.0) < 0.01 && !mount.slewing(); }, 15000));
    mount.startMove(devices::Mount::Direction::North);
    waitFor([] { return false; }, 300);
    mount.stopMove();
    mount.setTracking(false);
    CHECK(waitFor([&] { return mount.tracking() == false; }, 3000));
    mount.setTracking(true);
    mount.park();
    CHECK(waitFor([&] { return mount.parked() == true; }, 15000));
    mount.unpark();
    CHECK(waitFor([&] { return mount.parked() == false; }, 3000));
    mount.abort();
    mount.setSpec("sim", Site{51.5, -0.1});   // the same: stays open
    CHECK(mount.builtIn());
    mount.setSpec("sim");                      // the site changed: closed
    CHECK_FALSE(mount.builtIn());
    CHECK_FALSE(mount.ready());
    mount.pulse(devices::Mount::Direction::North, 100);   // no mount: said
    CHECK(failures.join(" ").contains("no mount"));
    mount.setSpec("lx200");
    mount.openBuiltIn();   // a spec without a port
    CHECK(failures.join(" ").contains("needs a port"));
}

#ifndef _WIN32
TEST_CASE("mounts: a serial port (a pseudo-terminal), TCP and UDP", "[mounts]") {
    // serial: the driver's end is the pseudo-terminal's, the test's the master
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    REQUIRE(master >= 0);
    REQUIRE(grantpt(master) == 0);
    REQUIRE(unlockpt(master) == 0);
    const std::string slave = ptsname(master);
    CHECK_THROWS_AS(openTransport(slave + "@12345", 9600, 0), MountError);   // no such speed
    {
        auto port = openTransport(slave + "@115200", 9600, 0);
        CHECK(port->name().find("115200") != std::string::npos);
        CommandChannel ch(std::move(port), "test");
        std::thread device([master] {
            char buf[64];
            std::string got;
            while (got.find('#') == std::string::npos) {
                const auto n = ::read(master, buf, sizeof buf);
                if (n <= 0) return;
                got.append(buf, static_cast<std::size_t>(n));
            }
            const std::string reply = "12:34:56#";
            CHECK(::write(master, reply.data(), reply.size()) == static_cast<ssize_t>(reply.size()));
        });
        CHECK(ch.until(":GR#", '#', 3000) == "12:34:56");
        device.join();
        CHECK_FALSE(ch.tryUntil(":GD#", '#', 200));   // nothing: a timeout
        ch.send(":Q#");
    }
    ::close(master);   // unplugged: the far end goes away
    CHECK_THROWS_AS(openTransport("/dev/no-such-port", 9600, 0), MountError);
    CHECK_THROWS_AS(openTransport("/dev/null", 9600, 0), MountError);   // not a terminal

    // TCP: an LX200 (OnStep) on its Wi-Fi bridge
    auto sim = quickSim();
    const auto lx = fakeLx200(sim, Lx200Dialect::OnStep);
    std::promise<quint16> tcpPort;
    std::thread tcpServer([&] {
        QTcpServer server;
        REQUIRE(server.listen(QHostAddress::LocalHost, 0));
        tcpPort.set_value(server.serverPort());
        if (!server.waitForNewConnection(5000)) return;
        std::unique_ptr<QTcpSocket> s(server.nextPendingConnection());
        std::string pending;
        while (s->state() == QAbstractSocket::ConnectedState && s->waitForReadyRead(3000)) {
            pending += s->readAll().toStdString();
            // one command at a time: up to '#', or the ACK byte
            for (std::size_t end; !pending.empty() && (pending[0] == '\x06' || (end = pending.find('#')) != std::string::npos);) {
                const std::size_t len = pending[0] == '\x06' ? 1 : pending.find('#') + 1;
                const auto reply = lx(pending.substr(0, len));
                pending.erase(0, len);
                s->write(reply.data(), static_cast<qint64>(reply.size()));
                s->waitForBytesWritten(1000);
            }
        }
    });
    const auto port = tcpPort.get_future().get();
    {
        Lx200Driver d([port] { return openTransport("tcp://127.0.0.1:" + std::to_string(port), 0, 9999); }, Lx200Dialect::Auto);
        CHECK(d.connect().model.find("On-Step") != std::string::npos);
        sim->place(3.0, 4.0);
        CHECK(d.poll().dec == Approx(4.0).margin(0.001));
    }   // the driver hangs up
    tcpServer.join();

    // UDP: the SynScan app
    const auto app = fakeSynScanApp(sim);
    std::promise<quint16> udpPort;
    std::atomic<bool> stop{false};
    std::thread udpServer([&] {
        QUdpSocket s;
        REQUIRE(s.bind(QHostAddress::LocalHost, 0));
        udpPort.set_value(s.localPort());
        while (!stop) {
            if (!s.waitForReadyRead(100)) continue;
            while (s.hasPendingDatagrams()) {
                QByteArray d(static_cast<qsizetype>(s.pendingDatagramSize()), Qt::Uninitialized);
                QHostAddress from;
                quint16 fromPort = 0;
                s.readDatagram(d.data(), d.size(), &from, &fromPort);
                const auto reply = app(d.toStdString());
                s.writeDatagram(reply.data(), static_cast<qint64>(reply.size()), from, fromPort);
            }
        }
    });
    const auto uport = udpPort.get_future().get();
    {
        SynScanAppDriver d([uport] { return openTransport("udp://127.0.0.1:" + std::to_string(uport), 0, 11881); });
        CHECK(d.connect().model == "SynScan app 2.5.30");
        sim->place(7.0, -8.0);
        CHECK(d.poll().dec == Approx(-8.0).margin(1e-5));
    }
    stop = true;
    udpServer.join();
    CHECK_THROWS_AS(openTransport("tcp://127.0.0.1:1", 0, 0), MountError);   // nothing listens there
    CHECK(listSerialPorts().size() < 1000);
}
#endif

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}

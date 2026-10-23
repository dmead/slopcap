// INDI client, focuser and mount against INDI's simulators on a private server (skipped if INDI isn't installed).
#include "devices/focuser.hpp"
#include "devices/indi_client.hpp"
#include "devices/mount.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <vector>

using namespace slopcap::devices;

namespace {

/// Spin the event loop until `done` or the timeout.
template <typename F>
bool waitFor(F done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

struct SimServer {
    QProcess proc;
    quint16 port = 0;
    QString fifo;
    bool ok = false;
    /// `later`: drivers that must be installed but are started afterwards, with start().
    explicit SimServer(const QStringList& drivers, const QStringList& later = {}) {
        if (QStandardPaths::findExecutable("indiserver").isEmpty()) return;
        for (const auto& d : drivers + later)
            if (QStandardPaths::findExecutable(d).isEmpty()) return;
        std::random_device rd;
        port = static_cast<quint16>(20000 + rd() % 20000);
        const QString sock = QString("/tmp/slopcap-indi-test-%1").arg(port);
        QStringList args{"-p", QString::number(port)};
#ifdef __linux__
        // Linux indiserver also listens on a local socket; a private one keeps clear of a running server's. Elsewhere
        // (macOS) indiserver has no -u and quits with its usage text.
        args = QStringList{"-u", sock} + args;
#endif
        if (!later.isEmpty()) {   // a control FIFO, so drivers can be started in a known order
            fifo = sock + ".fifo";
            if (QProcess::execute("mkfifo", {fifo}) != 0) return;
            args << "-f" << fifo;
        }
        proc.start("indiserver", args + drivers);
        ok = proc.waitForStarted(3000);
        waitFor([] { return false; }, 700);   // let it bind
    }
    void start(const QString& driver) {
        QFile f(fifo);
        if (f.open(QIODevice::WriteOnly)) f.write(QString("start %1\n").arg(driver).toUtf8());
    }
    ~SimServer() {
        proc.kill();
        proc.waitForFinished(2000);
        if (!fifo.isEmpty()) QFile::remove(fifo);
    }
};

}  // namespace

TEST_CASE("INDI focuser: connects, and moves with a fixed final approach direction") {
    SimServer server({"indi_simulator_focus"});
    if (!server.ok) SKIP("indiserver / indi_simulator_focus not installed");
    IndiClient client;
    Focuser f(client);
    f.setBacklash(300);
    f.setApproachOutward(true);
    std::vector<int> seen;
    QObject::connect(&f, &Focuser::positionChanged, [&](int p) { seen.push_back(p); });
    client.connectToServer("127.0.0.1", server.port);
    REQUIRE(waitFor([&] { return f.ready(); }, 8000));
    CHECK(f.device() == "Focuser Simulator");

    const int start = f.position();
    bool arrived = false;
    QObject::connect(&f, &Focuser::arrived, [&] { arrived = true; });

    // Inward move: must overshoot below the target, then come back out to it.
    const int in = start - 1000;
    seen.clear();
    f.goTo(in);
    REQUIRE(waitFor([&] { return arrived; }, 30000));
    CHECK(f.position() == in);
    CHECK(*std::min_element(seen.begin(), seen.end()) <= in - 300);

    // Outward move: no overshoot.
    arrived = false;
    seen.clear();
    f.goTo(in + 500);
    REQUIRE(waitFor([&] { return arrived; }, 30000));
    CHECK(f.position() == in + 500);
    CHECK(*std::min_element(seen.begin(), seen.end()) >= in);

    // Relative moves add to the target.
    arrived = false;
    f.moveBy(-50);
    REQUIRE(waitFor([&] { return arrived; }, 30000));
    CHECK(f.position() == in + 450);
}

TEST_CASE("INDI focuser: the backlash is taken up unless the gears are known to be loaded the approach way") {
    SimServer server({"indi_simulator_focus"});
    if (!server.ok) SKIP("indiserver / indi_simulator_focus not installed");
    IndiClient client;
    Focuser f(client);
    f.setBacklash(300);
    f.setApproachOutward(true);
    std::vector<int> seen;
    bool arrived = false;
    QObject::connect(&f, &Focuser::positionChanged, [&](int p) { seen.push_back(p); });
    QObject::connect(&f, &Focuser::arrived, [&] { arrived = true; });
    client.connectToServer("127.0.0.1", server.port);
    REQUIRE(waitFor([&] { return f.ready(); }, 8000));
    const int start = f.position();
    auto go = [&](int to) {
        arrived = false;
        seen.clear();
        f.goTo(to);
        REQUIRE(waitFor([&] { return arrived; }, 30000));
        CHECK(f.position() == to);
    };

    // Just connected, nobody knows which way it last ran: even an outward move comes from below.
    go(start + 100);
    CHECK(*std::min_element(seen.begin(), seen.end()) <= start + 100 - 300);
    // Loaded outward now: a further outward step goes straight there.
    go(start + 200);
    CHECK(*std::min_element(seen.begin(), seen.end()) >= start + 100);

    // Sent inward, then at once a little outward of where it still says it is (tonight's clicks on the real EAF): it's
    // heading inward by then, so the outward move still has to come back out from 300 below its target.
    arrived = false;
    seen.clear();
    f.goTo(start + 150);
    f.goTo(f.position() + 50);
    REQUIRE(waitFor([&] { return arrived && f.position() == start + 250; }, 30000));
    CHECK(std::find(seen.begin(), seen.end(), start + 250 - 300) != seen.end());
}

TEST_CASE("INDI focuser: a serial port that went away under the driver is reconnected") {
    SimServer server({"indi_simulator_focus"});
    if (!server.ok) SKIP("indiserver / indi_simulator_focus not installed");
    IndiClient client;
    Focuser f(client);
    std::vector<QString> failures;
    int offs = 0;
    QObject::connect(&f, &Focuser::failed, [&](const QString& why) { failures.push_back(why); });
    QObject::connect(&client, &IndiClient::propertyUpdated, [&](const QString& dev, const QString& name) {
        const auto* p = client.property(dev, name);
        if (name == "CONNECTION" && p->elements.count("CONNECT") && !p->elements.at("CONNECT").on()) ++offs;
    });
    client.connectToServer("127.0.0.1", server.port);
    REQUIRE(waitFor([&] { return f.ready(); }, 8000));

    // What MyFocuserPro2 said every second after its USB adapter dropped off the bus (2026-10-22), while its CONNECTION
    // still read On. The simulator's port can't vanish, so the message is played in.
    const QString dev = f.device();
    emit client.message(dev, "[ERROR] Connection write error: Write Error: Input/output error.");
    REQUIRE(waitFor([&] { return offs > 0; }, 5000));
    // ready() can still be true from before the driver has deleted its properties, so wait for CONNECT itself
    REQUIRE(waitFor([&] { return client.property(dev, "CONNECTION")->elements.at("CONNECT").on() && f.ready(); }, 8000));
    REQUIRE(failures.size() == 1);
    CHECK(failures[0].contains("serial port"));

    // The driver repeats the error on every poll: that's one reconnect, not one per message.
    emit client.message(dev, "[ERROR] Connection write error: Write Error: Input/output error.");
    waitFor([] { return false; }, 1000);
    CHECK(offs == 1);
    CHECK(failures.size() == 1);
}

TEST_CASE("INDI focuser and mount on one server: an LX200's focuser port is left to the mount") {
    // Every LX200 driver says TELESCOPE | GUIDER | FOCUSER; with it seen first, the focuser must still be the EAF.
    SimServer server({"indi_lx200classic"}, {"indi_simulator_focus"});
    if (!server.ok) SKIP("indiserver / indi_lx200classic / indi_simulator_focus not installed");
    IndiClient client;
    Focuser f(client);
    Mount m(client);
    client.connectToServer("127.0.0.1", server.port);
    REQUIRE(waitFor([&] { return client.property("LX200 Classic", "DRIVER_INFO") != nullptr; }, 5000));
    server.start("indi_simulator_focus");
    REQUIRE(waitFor([&] { return f.ready(); }, 8000));
    CHECK(f.device() == "Focuser Simulator");
    REQUIRE(waitFor([&] { return !m.device().isEmpty(); }, 3000));
    CHECK(m.device() == "LX200 Classic");   // claimed, though it can't connect without the hardware
}

TEST_CASE("INDI mount: tracking, guide pulses, axis moves, park") {
    SimServer server({"indi_simulator_telescope"});
    if (!server.ok) SKIP("indiserver / indi_simulator_telescope not installed");
    IndiClient client;
    Mount m(client);
    std::vector<QString> failures;
    QObject::connect(&m, &Mount::failed, [&](const QString& why) { failures.push_back(why); });
    client.connectToServer("127.0.0.1", server.port);
    REQUIRE(waitFor([&] { return m.ready(); }, 8000));
    CHECK(m.device() == "Telescope Simulator");
    // Guide and park properties come after the coordinates (ready); on a slow machine, in a later read.
    CHECK(waitFor([&] { return m.canPulse(); }, 5000));
    REQUIRE(waitFor([&] { return m.parked().has_value(); }, 5000));
    if (*m.parked()) {
        m.unpark();
        REQUIRE(waitFor([&] { return m.parked() == false; }, 10000));
    }

    m.setTracking(true);
    REQUIRE(waitFor([&] { return m.tracking() == true; }, 5000));

    // Away from the pole (the simulator starts at Dec +90, and slews at ~6°/s), so Dec moves are plain.
    bool sawSlewing = false;
    auto watch = QObject::connect(&m, &Mount::stateChanged, [&] { sawSlewing |= m.slewing(); });
    m.goTo(m.ra(), 20.0);
    REQUIRE(waitFor([&] { return std::abs(m.dec() - 20.0) < 0.01 && !m.slewing(); }, 40000));
    QObject::disconnect(watch);
    CHECK(sawSlewing);
    CHECK(m.tracking() == true);

    // A 2 s pulse at the simulator's 0.5x guide rate is 2 × 0.5 × 15.04″ ≈ 15″ ≈ 0.0042° in Dec.
    auto pulseDec = [&](Mount::Direction d) {
        const double before = m.dec();
        bool done = false;
        auto c = QObject::connect(&m, &Mount::pulseDone, [&] { done = true; });
        m.pulse(d, 2000);
        CHECK(m.pulsing());
        REQUIRE(waitFor([&] { return done; }, 8000));
        QObject::disconnect(c);
        waitFor([] { return false; }, 1500);   // the simulator reports coordinates once a second
        return m.dec() - before;
    };
    const double north = pulseDec(Mount::Direction::North);
    const double south = pulseDec(Mount::Direction::South);
    CHECK(std::abs(north) > 0.002);
    CHECK(std::abs(north) < 0.01);
    CHECK(std::abs(south) > 0.002);
    CHECK(north * south < 0);   // opposite ways
    CHECK(!m.pulsing());

    // Slew rates come in the driver's order; an axis move runs until stopped.
    CHECK(m.slewRates() == QStringList{"1x", "2x", "3x", "4x"});
    m.setSlewRate("4x");
    REQUIRE(waitFor([&] { return m.slewRate() == "4x"; }, 3000));
    const double dec0 = m.dec();
    m.startMove(Mount::Direction::North);
    waitFor([] { return false; }, 2000);
    m.stopMove();
    waitFor([] { return false; }, 1500);
    const double moved = m.dec() - dec0;
    CHECK(std::abs(moved) > 0.05);
    const double dec1 = m.dec();
    waitFor([] { return false; }, 2000);
    CHECK(std::abs(m.dec() - dec1) < 0.01);   // stopped

    // Park and unpark.
    m.park();
    REQUIRE(waitFor([&] { return m.parked() == true; }, 60000));
    m.unpark();
    REQUIRE(waitFor([&] { return m.parked() == false; }, 10000));
    for (const auto& f : failures) UNSCOPED_INFO(f.toStdString());
    CHECK(failures.empty());
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}

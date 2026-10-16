// INDI client + focuser against INDI's focuser simulator on a private server (skipped if INDI isn't installed).
#include "devices/focuser.hpp"
#include "devices/indi_client.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

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
    bool ok = false;
    SimServer() {
        if (QStandardPaths::findExecutable("indiserver").isEmpty() || QStandardPaths::findExecutable("indi_simulator_focus").isEmpty()) return;
        std::random_device rd;
        port = static_cast<quint16>(20000 + rd() % 20000);
        const QString sock = QString("/tmp/slopcap-indi-test-%1").arg(port);
        proc.start("indiserver", {"-u", sock, "-p", QString::number(port), "indi_simulator_focus"});
        ok = proc.waitForStarted(3000);
        waitFor([] { return false; }, 700);   // let it bind
    }
    ~SimServer() {
        proc.kill();
        proc.waitForFinished(2000);
    }
};

}  // namespace

TEST_CASE("INDI focuser: connects, and moves with a fixed final approach direction") {
    SimServer server;
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

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}

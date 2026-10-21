// Remote viewing between two windows in one process: one shares its camera on loopback, the other watches and steers
// it (controls, format, recording), through the link dropping, the camera going away, discovery and the menus.
#include "app_harness.hpp"
#include "net/remote_client.hpp"
#include "net/remote_server.hpp"
#include "preview/preview_widget.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace slopcap;
using namespace slopcap::app;
using namespace slopcap::test;

TEST_CASE("remote: one window shares its camera, another watches and steers it", "[app][remote]") {
    auto& m = Modals::get();
    m.clear();
    clearSettings();
    TempDir dir;
    const bool zwo = useFakeAsi();
    const QString spec = zwo ? QString("asi") : "sim-loop:" + makeSer(dir.file("jupiter.ser"));

    MainWindow server;
    MainWindowProbe s(server);
    server.setOutputDir(dir.file("server"));
    server.show();
    REQUIRE(QTest::qWaitForWindowExposed(&server));
    server.connectCamera(spec);
    REQUIRE(s.waitForFrames(2));
    server.startSharing("127.0.0.1");
    REQUIRE(s.server);
    const quint16 port = s.server->port();
    CHECK(s.remotePill->text() == "⇄ Sharing");
    CHECK(s.fRemote->text().contains(QString("sharing on port %1").arg(port)));
    CHECK(s.remoteBadge->text() == "⇄ SHARING · NO VIEWERS");
    server.startSharing();   // already sharing
    CHECK(s.server->port() == port);

    MainWindow viewer;
    MainWindowProbe v(viewer);
    viewer.move(100, 100);
    viewer.show();
    REQUIRE(QTest::qWaitForWindowExposed(&viewer));
    viewer.connectRemote("127.0.0.1", port);
    REQUIRE(waitFor([&] { return v.cam != nullptr; }, 10000));
    REQUIRE(v.waitForFrames(3));
    CHECK(waitFor([&] { return v.remoteBadge->text().startsWith("⇄ LIVE FROM"); }, 5000));
    CHECK(v.remotePill->text().startsWith("⇄ "));
    CHECK_FALSE(v.folder->isEnabled());
    CHECK(waitFor([&] { return v.fState->text().startsWith("⇄ Watching"); }, 3000));
    CHECK(waitFor([&] { return v.nextFile->text().startsWith("Recordings stay on"); }, 3000));
    CHECK(waitFor([&] { return s.server->viewers().size() == 1; }, 5000));
    s.updateStats();
    CHECK(s.remotePill->text() == "⇄ Sharing · 1 watching");
    CHECK(s.remoteBadge->text().startsWith("⇄ SHARED WITH"));
    CHECK(s.fMessage->text().startsWith("Watching:"));

    // controls both ways
    v.gain->setValue(77);
    CHECK(waitFor([&] { return s.cam->control("Gain") == 77; }, 5000));
    CHECK(waitFor([&] { return s.gain->value() == 77; }, 5000));
    s.gain->setValue(66);
    CHECK(waitFor([&] { return v.gain->value() == 66; }, 5000));
    viewer.setNamedControl("Gain", 55);
    CHECK(waitFor([&] { return s.cam->control("Gain") == 55; }, 5000));

    // the format: the viewer asks, the server applies, the viewer gets the new camera
    {
        auto* pv = v.preview;
        const QPoint at(pv->width() / 2, pv->height() / 2);
        QContextMenuEvent ctx(QContextMenuEvent::Mouse, at, pv->mapToGlobal(at));
        m.pickMenu("Centre ROI here");
        QApplication::sendEvent(pv, &ctx);
        REQUIRE(m.done());
    }
    CHECK(waitFor([&] { return s.cam && s.cam->roi().width < s.cam->info().sensorWidth; }, 5000));
    CHECK(waitFor([&] { return v.cam && v.cam->roi() == s.cam->roi(); }, 5000));
    v.roiMax->click();
    CHECK(waitFor([&] { return s.cam && s.cam->roi().width == s.cam->info().sensorWidth; }, 5000));
    REQUIRE(waitFor([&] { return v.cam && v.bits->button(16)->isVisibleTo(&viewer); }, 5000));
    v.bits->button(16)->click();
    CHECK(waitFor([&] { return s.cam && s.cam->bitDepth() == 16; }, 5000));
    CHECK(waitFor([&] { return v.cam && v.cam->bitDepth() == 16; }, 5000));
    if (zwo) {
        REQUIRE(waitFor([&] { return v.highSpeed->isVisibleTo(&viewer); }, 3000));
        v.highSpeed->setChecked(true);
        CHECK(waitFor([&] { return s.cam && s.cam->readoutMode() == 1; }, 5000));
    }
    REQUIRE(v.waitForFrames(2));

    // recording on the server, from the viewer
    v.record->click();
    CHECK(waitFor([&] { return s.recording; }, 5000));
    CHECK(waitFor([&] { return v.fState->text().startsWith("● Recording on"); }, 5000));
    CHECK(waitFor([&] { return v.recBadge->isVisible() && v.recBadge->text().startsWith("● REC on"); }, 5000));
    v.roiMax->click();   // a format change while it records: refused, with a notice
    CHECK(waitFor([&] { return v.fMessage->text().contains("it's recording"); }, 5000));
    viewer.setNamedControl("Record", 0);
    CHECK(waitFor([&] { return !s.recording; }, 5000));
    CHECK(waitFor([&] { return !v.recBadge->isVisible(); }, 5000));
    CHECK(QDir(dir.file("server")).entryList({"*.ser"}).size() == 1);

    // the menus: the server's lists its viewer, the viewer's camera pill offers to disconnect
    m.pickMenu("");
    s.remotePill->click();
    REQUIRE(m.done());
    CHECK(m.lastSeen().contains("Stop sharing this camera"));
    CHECK(m.lastSeen().contains("Mbps"));
    m.pickMenu("");
    v.cameraPill->click();
    REQUIRE(m.done());
    CHECK(m.lastSeen().contains("Disconnect from"));

    // the link drops and comes back
    server.stopSharing();
    CHECK(waitFor([&] { return v.remoteBadge->text().contains("RECONNECTING"); }, 10000));
    CHECK(v.fMessage->text().contains("reconnecting"));
    v.updateStats();
    server.startSharing("127.0.0.1");
    REQUIRE(s.server);
    CHECK(s.server->port() == port);
    CHECK(waitFor([&] { return v.remoteBadge->text().startsWith("⇄ LIVE FROM") && v.cam; }, 20000));

    // the server's camera goes away, and comes back
    m.pickMenu("Disconnect");
    s.cameraPill->click();
    REQUIRE(m.done());
    CHECK(waitFor([&] { return !v.cam; }, 5000));
    CHECK(v.noCamera->text().contains("has no camera connected"));
    CHECK(v.cameraPill->text().contains("no camera"));
    server.connectCamera(spec);
    CHECK(waitFor([&] { return v.cam != nullptr; }, 5000));

    // the server would rather watch someone else: asked first, as someone is watching it
    m.answer(QMessageBox::No);
    server.connectRemote("127.0.0.1", 1);
    REQUIRE(m.done());
    CHECK(m.lastSeen().contains("Others are watching"));
    CHECK(s.server);

    // the viewer: disconnect through its remote menu, then watch by address, by discovery, by the menu's list
    m.pickMenu("Disconnect from");
    v.remotePill->click();
    REQUIRE(m.done());
    CHECK_FALSE(v.remote);
    CHECK_FALSE(v.cam);
    m.pickMenu("Watch an address");
    m.enterText(QString("127.0.0.1:%1").arg(port));
    v.remotePill->click();
    REQUIRE(m.done());
    REQUIRE(waitFor([&] { return v.cam != nullptr; }, 10000));
    m.pickMenu("Watch an address");
    m.enterText("");
    v.remotePill->click();
    REQUIRE(m.done());
    CHECK(v.remote);
    viewer.disconnectRemote();
    viewer.connectFirstRemote();
    CHECK(waitFor([&] { return v.remote && v.cam; }, 15000));
    m.pickMenu("Watch an address");
    m.enterText(QString("[::1]:%1").arg(port));
    v.remotePill->click();
    REQUIRE(m.done());
    REQUIRE(v.remote);
    CHECK(v.remote->host() == "::1");
    m.pickMenu(QString("(127.0.0.1"));
    v.remotePill->click();
    REQUIRE(m.done());
    CHECK(m.lastSeen().contains("Watch a SlopCap on this network"));
    CHECK(waitFor([&] { return v.remote && v.remote->host() == "127.0.0.1" && v.cam; }, 10000));
    // the viewer shares instead (stops watching)
    viewer.startSharing("127.0.0.1");
    CHECK_FALSE(v.remote);
    CHECK(v.server);
    viewer.stopSharing();

    // sharing on an address this machine doesn't have fails, and says why
    s.updateStats();
    server.stopSharing();
    m.answer(QMessageBox::Ok);
    server.startSharing("192.0.2.1");
    REQUIRE(m.done());
    CHECK(m.lastSeen().contains("Can't share"));
    CHECK_FALSE(s.server);
    // through the menu: share, then stop
    m.pickMenu("Share this camera");
    s.remotePill->click();
    REQUIRE(m.done());
    CHECK(s.server);
    m.pickMenu("Stop sharing");
    s.remotePill->click();
    REQUIRE(m.done());
    CHECK_FALSE(s.server);
    // watching someone else while sharing to viewers: Yes stops sharing and the camera
    server.startSharing("127.0.0.1");
    viewer.connectRemote("127.0.0.1", s.server->port());
    REQUIRE(waitFor([&] { return s.server->viewers().size() == 1; }, 10000));
    m.answer(QMessageBox::Yes);
    server.connectRemote("127.0.0.1", 1);   // nobody there: it keeps trying
    REQUIRE(m.done());
    CHECK_FALSE(s.server);
    CHECK_FALSE(s.cam);
    CHECK(s.remote);
    s.updateStats();
    server.printSummary();
    viewer.close();
    server.close();
    CHECK(m.takeUnexpected().join("; ").toStdString() == "");
}

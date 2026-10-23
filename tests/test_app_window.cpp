// The main window driven as a user would: dialogs and menus, a replayed recording and the fake ZWO / QHY cameras as
// cameras, the preview, recording, saved settings, full view.
#include "app/focus_views.hpp"
#include "app/full_view_bar.hpp"
#include "app/mount_section.hpp"
#include "app_harness.hpp"
#include "preview/histogram_widget.hpp"
#include "preview/preview_widget.hpp"
#if defined(SLOPCAP_FAKE_ASI) || defined(SLOPCAP_FAKE_QHY)
#include "fakes/fake_sdk.hpp"
#endif

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTest>
#include <QWheelEvent>

#include <catch2/catch_test_macros.hpp>

#include <functional>

using namespace slopcap;
using namespace slopcap::app;
using namespace slopcap::test;

namespace {

QPushButton* byTip(QWidget* root, const QString& tip) {
    for (auto* b : root->findChildren<QPushButton*>())
        if (b->toolTip().startsWith(tip)) return b;
    return nullptr;
}

QString readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

QStringList files(const QString& dir, const QString& pattern) { return QDir(dir).entryList({pattern}, QDir::Files, QDir::Time); }

std::string unexpectedModals() { return Modals::get().takeUnexpected().join("; ").toStdString(); }

/// A key for the window's shortcuts, which need it to be the active window. A test process on a Mac isn't always let
/// to the front; then the shortcut's action is called directly instead.
void press(QWidget& w, Qt::Key key, Qt::KeyboardModifiers mods, const std::function<void()>& instead) {
    w.raise();
    w.activateWindow();
    if (QTest::qWaitForWindowActive(&w, 3000)) {
        QTest::keyClick(&w, key, mods);
    } else {
        WARN("the window isn't active: calling the shortcut's action instead");
        instead();
    }
}

}  // namespace

TEST_CASE("window: settings, modes and dialogs without a camera", "[app]") {
    clearSettings();
    auto& m = Modals::get();
    m.clear();
    TempDir dir;
    {
        QSettings st;
        st.setValue("record/target", "Jupiter");
        st.setValue("record/filter", "R");
        st.setValue("record/limit", 60);
        st.setValue("mount/pulseMs", 1000);
        st.setValue("focuser/server", "localhost:1");   // nothing listens there: connecting fails
        st.setValue("focuser/autoConnect", true);
        st.setValue("optics/focalRatio", 10.0);
        st.setValue("optics/micronsPerStep", 2.0);
        st.setValue("optics/focalLengthMm", 2500.0);
        st.setValue("site/latitude", 51.5);
        st.setValue("site/longitude", -0.1);
    }
    MainWindow w;
    MainWindowProbe p(w);
    w.show();
    REQUIRE(QTest::qWaitForWindowExposed(&w));
    CHECK(p.target->currentText() == "Jupiter");
    CHECK(p.filter->currentText() == "R");
    CHECK(p.limit->currentData().toInt() == 60);
    CHECK(p.mountSection->pulseMs() == 1000);
    CHECK(p.settings.latitude == 51.5);
    CHECK(p.settings.focalLengthMm == 2500);
    CHECK(p.noCamera->isVisible());
    CHECK_FALSE(p.record->isEnabled());
    CHECK_FALSE(p.tAuto->isVisible());
    CHECK(waitFor([&] { return p.fState->text() == "○ No camera"; }, 2000));

    w.showTab("focus");
    CHECK(p.mode == MainWindow::Focus);
    CHECK(p.headerInfo->text().contains("Focus sweep"));
    w.showTab("collimate");
    CHECK(p.mode == MainWindow::Collimate);
    p.updateStats();
    CHECK(p.headerInfo->text() == "Defocused-star collimation");
    p.modes->button(MainWindow::Capture)->click();
    CHECK(p.mode == MainWindow::Capture);
    w.setMode(MainWindow::Align);   // designed, not built: stays
    CHECK(p.mode == MainWindow::Capture);
    w.showTab("capture");

    // nothing to record or set without a camera
    w.toggleRecording();
    CHECK_FALSE(p.recording);
    w.setNamedControl("Gain", 5);
    w.setNamedControl("Record", 1);
    CHECK_FALSE(p.recording);

    // the camera menu lists what there is
    m.pickMenu("");
    p.cameraPill->click();
    REQUIRE(m.done());
    CHECK(m.lastSeen().contains("Replay a recording"));
#ifdef SLOPCAP_FAKE_ASI
    CHECK(m.lastSeen().contains("asi:0"));
#endif
    // replay a recording: cancelled, then one
    const QString ser = makeSer(dir.file("replay.ser"));
    m.pickMenu("Replay");
    m.chooseFile("");
    p.cameraPill->click();
    REQUIRE(m.done());
    CHECK_FALSE(p.cam);
    m.pickMenu("Replay");
    m.chooseFile(ser);
    p.cameraPill->click();
    REQUIRE(m.done());
    REQUIRE(p.cam);
    CHECK(p.cam->info().vendor == "Simulated");
    CHECK(p.cameraPill->text().startsWith("SER replay"));
    CHECK(p.record->isEnabled());
    m.pickMenu("Disconnect");
    p.cameraPill->click();
    REQUIRE(m.done());
    CHECK_FALSE(p.cam);
    CHECK(p.noCamera->isVisible());

    // cameras that can't be opened
    m.answer(QMessageBox::Ok);
    w.connectCamera("sim:" + dir.file("missing.ser"));
    REQUIRE(m.done());
    CHECK(m.lastSeen().contains("Could not open"));
    m.answer(QMessageBox::Ok);
    w.connectCamera("bogus:1");
    REQUIRE(m.done());
    CHECK(m.lastSeen().contains("unknown camera spec"));
    CHECK_FALSE(p.cam);

    // the recording folder
    m.chooseFile(dir.q());
    p.folder->click();
    REQUIRE(m.done());
    CHECK(QDir(p.outDir) == QDir(dir.q()));
    CHECK(p.folder->text() == QFileInfo(dir.q()).fileName());
    m.chooseFile("");
    p.folder->click();
    REQUIRE(m.done());
    CHECK(QDir(p.outDir) == QDir(dir.q()));

    // settings from the header: everything changed
    m.settings([](SettingsDialog& d) {
        d.findChild<QLineEdit*>()->setText(" localhost:2 ");
        d.findChild<QCheckBox*>()->setChecked(false);
        d.findChild<QSpinBox*>()->setValue(100);
        d.findChild<QComboBox*>()->setCurrentIndex(1);
        const auto boxes = d.findChildren<QDoubleSpinBox*>();
        REQUIRE(boxes.size() == 5);
        boxes[0]->setValue(8);      // f/8
        boxes[1]->setValue(1.5);    // µm per step
        boxes[2]->setValue(4000);   // focal length
        boxes[3]->setValue(-91);    // latitude unknown: no site
    });
    byTip(&w, "Settings")->click();
    REQUIRE(m.done());
    CHECK(p.settings.indiServer == "localhost:2");
    CHECK_FALSE(p.settings.focuserAutoConnect);
    CHECK(p.settings.backlash == 100);
    CHECK_FALSE(p.settings.approachOutward);
    CHECK(p.settings.focalRatio == 8);
    CHECK_FALSE(p.settings.latitude);
    CHECK(QSettings().value("focuser/backlash").toInt() == 100);
    CHECK_FALSE(QSettings().contains("site/latitude"));
    // the focuser section's settings: cancelled, nothing changes
    m.settings(nullptr, false);
    byTip(&w, "Focuser settings")->click();
    REQUIRE(m.done());
    CHECK(p.settings.backlash == 100);
    // and with a site again
    m.settings([](SettingsDialog& d) {
        const auto boxes = d.findChildren<QDoubleSpinBox*>();
        boxes[3]->setValue(51.5);
        boxes[4]->setValue(-0.1);
    });
    byTip(&w, "Focuser settings")->click();
    REQUIRE(m.done());
    CHECK(p.settings.longitude == -0.1);

    // the focuser's and the mount's Connect try the (absent) server
    for (auto* b : w.findChildren<QPushButton*>())
        if (b->text() == "Connect" && b->isVisible()) b->click();
    pump(300);

    // the next file's name follows the target and filter
    p.target->setCurrentText("Saturn");
    p.filter->setCurrentText("IR");
    p.updateStats();
    CHECK(p.nextFile->text().contains("IR-Saturn.ser"));
    p.filter->setCurrentText("");
    p.limit->setCurrentIndex(2);

    w.close();
    CHECK(QSettings().value("record/target").toString() == "Saturn");
    CHECK(QSettings().value("record/limit").toInt() == 60);
    CHECK(unexpectedModals() == "");
}

TEST_CASE("window: a replayed recording as the camera: preview, controls, recording", "[app]") {
    clearSettings();
    auto& m = Modals::get();
    m.clear();
    TempDir dir;
    const QString ser = makeSer(dir.file("saturn.ser"), 320, 240, 40);
    const QString out = dir.file("out");
    MainWindow w;
    MainWindowProbe p(w);
    w.setOutputDir(out);
    w.resize(1300, 850);
    w.show();
    REQUIRE(QTest::qWaitForWindowExposed(&w));
    w.connectCamera("sim-loop:" + ser);
    REQUIRE(p.cam);
    REQUIRE(p.session);
    REQUIRE(p.waitForFrames(5));
    CHECK(p.histInfo->text().contains("peak"));
    CHECK(p.sFrame->text() == "320×240 · 8-bit");
    CHECK(p.roiSize->text() == "320 × 240");
    CHECK(waitFor([&] { return p.tCentre->text() != "—"; }, 5000));   // the live focus analysis found the disc
    // Target: Auto looks at the image once a second
    CHECK(p.target->currentText() == "Auto");
    CHECK(waitFor([&] { return p.tAuto->text() != "Auto: no target yet"; }, 5000));
    CHECK(p.tAuto->text().startsWith("Auto:"));

    // the toolbar
    for (int id : {1, 2, 0}) p.zoom->button(id)->click();
    CHECK(p.preview->fitted());
    p.stretch->button(0)->click();
    CHECK_FALSE(p.preview->autoStretch());
    p.stretch->button(1)->click();
    CHECK(p.preview->autoStretch());
    for (const char* name : {"Debayer", "Auto WB", "Target box", "Reticle"}) {
        auto* c = checkBox(&w, name);
        REQUIRE(c);
        c->toggle();
        REQUIRE(p.waitForFrames(2));
        c->toggle();
    }

    // the preview: wheel, drag, keys, double-click, the right-click menu
    auto* pv = p.preview;
    const QPointF at(pv->width() / 2.0, pv->height() / 2.0);
    QWheelEvent wheel(at, pv->mapToGlobal(at), QPoint(), QPoint(0, 240), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(pv, &wheel);
    CHECK_FALSE(pv->fitted());
    QTest::mousePress(pv, Qt::LeftButton, {}, at.toPoint());
    QTest::mouseMove(pv, at.toPoint() + QPoint(30, 20));
    QTest::mouseRelease(pv, Qt::LeftButton, {}, at.toPoint() + QPoint(30, 20));
    QTest::mousePress(pv, Qt::MiddleButton, {}, at.toPoint());   // only the left button pans
    QTest::mouseRelease(pv, Qt::MiddleButton, {}, at.toPoint());
    QTest::keyClick(pv, Qt::Key_1);
    QTest::keyClick(pv, Qt::Key_C);
    QTest::keyClick(pv, Qt::Key_C);
    QTest::keyClick(pv, Qt::Key_X);
    QTest::keyClick(pv, Qt::Key_F);
    CHECK(pv->fitted());
    QTest::mouseDClick(pv, Qt::LeftButton, {}, at.toPoint());
    for (const char* item : {"1:1", "Crosshair", "Crosshair", "Fit"}) {
        QContextMenuEvent ctx(QContextMenuEvent::Mouse, at.toPoint(), pv->mapToGlobal(at.toPoint()));
        m.pickMenu(item);
        QApplication::sendEvent(pv, &ctx);
        REQUIRE(m.done());
    }
    CHECK(m.lastSeen().contains("Centre ROI here"));
    // centre the ROI on a point: a full frame becomes a crop around it
    {
        QContextMenuEvent ctx(QContextMenuEvent::Mouse, at.toPoint(), pv->mapToGlobal(at.toPoint()));
        m.pickMenu("Centre ROI here");
        QApplication::sendEvent(pv, &ctx);
        REQUIRE(m.done());
    }
    CHECK(p.cam->roi().width == 160);
    CHECK(p.cam->roi().height == 120);
    REQUIRE(p.waitForFrames(3));
    // and again on a crop: it moves, same size
    {
        QContextMenuEvent ctx(QContextMenuEvent::Mouse, QPoint(10, 10), pv->mapToGlobal(QPoint(10, 10)));
        m.pickMenu("Centre ROI here");
        QApplication::sendEvent(pv, &ctx);
        REQUIRE(m.done());
    }
    CHECK(p.cam->roi().width == 160);
    p.roiMax->click();
    CHECK(p.cam->roi().width == 320);
    p.roiCentre->click();
    emit p.roiPreset->activated(0);
    REQUIRE(p.bits->button(16)->isVisibleTo(&w));
    p.bits->button(16)->click();
    CHECK(p.cam->bitDepth() == 16);
    REQUIRE(p.waitForFrames(3));
    p.bits->button(8)->click();

    // gain and exposure: slider, label, the All controls box and the camera agree
    p.gain->setValue(300);
    CHECK(p.gainValue->text() == "300");
    CHECK(waitFor([&] { return p.cam->control("Gain") == 300; }, 2000));
    p.exposure->setValue(500);
    CHECK(waitFor([&] { return p.cam->control("Exposure") != 10000; }, 2000));
    p.allControlsBtn->click();
    p.controlBoxes.at("Gain")->setValue(123);
    CHECK(p.gain->value() == 123);
    CHECK(waitFor([&] { return p.cam->control("Gain") == 123; }, 2000));
    p.controlBoxes.at("Exposure")->setValue(20000);
    CHECK(p.expValue->text() == "20.0 ms");

    // a recording: start, the badge, stop (Ctrl+R) → the file, renamed to mid-capture, and its sidecar
    p.target->setCurrentText("Saturn");
    p.filter->setCurrentText("R");
    p.record->click();
    REQUIRE(p.recording);
    pump(1300);
    p.updateStats();
    CHECK(p.recBadge->isVisible());
    CHECK(p.recBadge->text().startsWith("● REC 00:0"));
    CHECK(p.fState->text() == "● Recording");
    CHECK(p.record->text().startsWith("■  Stop"));
    p.roiMax->click();   // no format changes while recording
    press(w, Qt::Key_R, Qt::ControlModifier, [&] { w.toggleRecording(); });
    CHECK_FALSE(p.recording);
    CHECK(p.fMessage->text().startsWith("Saved"));
    auto sers = files(out, "*.ser");
    REQUIRE(sers.size() == 1);
    CHECK(sers[0].endsWith("-R-Saturn.ser"));
    const QString side = readAll(out + "/" + QFileInfo(sers[0]).completeBaseName() + ".txt");
    CHECK(side.contains("Target=Saturn"));
    CHECK(side.contains("Filter=R"));
    CHECK(side.contains("Camera=Simulated SER replay"));
    CHECK(side.contains("Gain=123"));

    // Auto as the target: the sidecar says why
    p.target->setCurrentText("Auto");
    p.filter->setCurrentText("");
    w.setNamedControl("Record", 1);
    REQUIRE(p.recording);
    pump(700);
    w.setNamedControl("Record", 1);   // already recording: nothing changes
    w.setNamedControl("Record", 0);
    CHECK_FALSE(p.recording);
    sers = files(out, "*.txt");
    REQUIRE(sers.size() == 2);
    CHECK(readAll(out + "/" + sers[0]).contains("TargetAuto="));

    // a replay that runs out ends the recording by itself, back to the live view; started within seconds of the last
    // one, with the same target, its name gets a -2
    w.connectCamera("sim:" + ser);
    p.record->click();
    REQUIRE(p.recording);
    CHECK(waitFor([&] { return !p.recording; }, 8000));
    CHECK(files(out, "*.ser").size() == 3);
    CHECK(files(out, "*.txt").size() == 3);

    // a folder that can't be made
    {
        QFile blocker(dir.file("file"));
        REQUIRE(blocker.open(QIODevice::WriteOnly));
    }
    w.connectCamera("sim-loop:" + ser);
    w.setOutputDir(dir.file("file") + "/sub");
    m.answer(QMessageBox::Ok);
    p.record->click();
    REQUIRE(m.done());
    CHECK(m.lastSeen().contains("Can't create"));
    CHECK_FALSE(p.recording);
    w.setOutputDir(out);

    // disconnecting or closing while recording asks first
    p.record->click();
    REQUIRE(p.recording);
    m.pickMenu("Disconnect");
    m.answer(QMessageBox::No);
    p.cameraPill->click();
    REQUIRE(m.done());
    CHECK(p.cam);
    CHECK(p.recording);
    m.answer(QMessageBox::No);
    w.close();
    REQUIRE(m.done());
    CHECK(w.isVisible());
    m.pickMenu("Disconnect");
    m.answer(QMessageBox::Yes);
    p.cameraPill->click();
    REQUIRE(m.done());
    CHECK_FALSE(p.cam);
    w.connectCamera("sim-loop:" + ser);
    p.record->click();
    REQUIRE(p.recording);
    w.printSummary();
    m.answer(QMessageBox::Yes);
    w.close();
    REQUIRE(m.done());
    CHECK_FALSE(w.isVisible());
    CHECK(unexpectedModals() == "");
}

TEST_CASE("window: numbers that change every frame don't move the layout", "[app]") {
    // The target, sharpness and histogram readouts change on every frame. When their text widths drove the side
    // columns' widths (280-320 px, from their content) and the stats beside the histogram, the preview, the histogram
    // and both columns jumped about with every frame (2026-10-22, the user on the rig).
    clearSettings();
    MainWindow w;
    MainWindowProbe p(w);
    w.resize(1600, 900);
    w.show();
    REQUIRE(QTest::qWaitForWindowExposed(&w));
    p.statsTimer.stop();   // it would clear the footer readouts set below (no camera)
    auto layout = [&] {
        waitFor([] { return false; }, 100);   // let the layouts settle
        std::vector<std::pair<const char*, QRect>> out;
        for (auto [name, wid] : std::initializer_list<std::pair<const char*, QWidget*>>{
                 {"left column", p.left}, {"right column", p.right}, {"preview", p.preview}, {"histogram", p.histogram},
                 {"centre", p.tCentre}, {"margin", p.tMargin}, {"size", p.tDisc}, {"dispersion", p.tDisp},
                 {"sharpness", p.sSharp}, {"strip dispersion", p.sDisp}, {"frame", p.sFrame}, {"peak and clipped", p.histInfo},
                 {"footer fps", p.fFps}, {"footer message", p.fMessage}})
            out.emplace_back(name, QRect(wid->mapTo(&w, QPoint()), wid->size()));
        return out;
    };
    auto show = [&](const QString& centre, const QString& margin, const QString& size, const QString& disp, const QString& sharp,
                    const QString& hist, const QString& fps) {
        p.tCentre->setText(centre);
        p.tMargin->setText(margin);
        p.tDisc->setText(size);
        p.tDisp->setText(disp);
        p.sDisp->setText(disp);
        p.sSharp->setText(sharp);
        p.histInfo->setText(hist);
        p.fState->setText("○ Previewing");
        p.fFps->setText(fps);
        return layout();
    };
    const auto small = show("8, 6", "0 px", "9 × 9 px", "0.0, 0.0 px", "0.50", "peak 5% · clipped 0.00%", "0.0 fps");
    const auto large = show("3839, 2159", "1079 px", "3840 × 2160 px", "-12.3, -45.6 px", "1.00", "peak 100% · clipped 100.00%", "154.1 fps");
    for (std::size_t i = 0; i < small.size(); ++i) {
        const auto r = [](QRect q) { return QString("%1,%2 %3x%4").arg(q.x()).arg(q.y()).arg(q.width()).arg(q.height()).toStdString(); };
        INFO(small[i].first << ": " << r(small[i].second) << " then " << r(large[i].second));
        CHECK(small[i].second == large[i].second);
    }
}

TEST_CASE("window: full view", "[app]") {
    clearSettings();
    auto& m = Modals::get();
    m.clear();
    TempDir dir;
    const QString ser = makeSer(dir.file("moon.ser"));
    MainWindow w;
    MainWindowProbe p(w);
    w.setOutputDir(dir.file("out"));
    w.show();
    REQUIRE(QTest::qWaitForWindowExposed(&w));
    w.connectCamera("sim-loop:" + ser);
    REQUIRE(p.waitForFrames(2));

    w.setMode(MainWindow::Focus);
    button(&w, "Full screen")->click();   // one screen: straight to full view (back to Capture: it's the live view)
    REQUIRE(p.fullView);
    CHECK(p.mode == MainWindow::Capture);
    CHECK_FALSE(p.cameraPill->isVisible());
    CHECK(p.fullBar->isVisible());
    // the bar's controls work like the sidebar's
    p.fullBar->gain->setValue(250);
    CHECK(p.gain->value() == 250);
    CHECK(waitFor([&] { return p.cam->control("Gain") == 250; }, 2000));
    p.fullBar->exposure->setValue(400);
    emit p.fullBar->stretch->idClicked(0);
    CHECK_FALSE(p.preview->autoStretch());
    emit p.fullBar->stretch->idClicked(1);
    p.fullBar->record->click();
    CHECK(p.recording);
    pump(600);
    p.fullBar->record->click();
    CHECK_FALSE(p.recording);
    // the bar hides while the mouse rests, and comes back when it moves
    p.fullIdle.start(1);
    CHECK(waitFor([&] { return !p.fullBar->isVisible(); }, 2000));
    QTest::mouseMove(p.preview, QPoint(50, 50));
    QMouseEvent move(QEvent::MouseMove, QPointF(60, 60), p.preview->mapToGlobal(QPointF(60, 60)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(p.preview, &move);
    CHECK(waitFor([&] { return p.fullBar->isVisible(); }, 2000));
    w.resize(w.width() - 10, w.height());
    pump(100);
    p.fullBar->exit->click();
    CHECK_FALSE(p.fullView);
    CHECK(p.cameraPill->isVisible());
    // the keys: F11 toggles, Esc leaves; the toolbar button leaves too
    press(w, Qt::Key_F11, Qt::NoModifier, [&] { w.setFullView(true); });
    CHECK(p.fullView);
    press(w, Qt::Key_Escape, Qt::NoModifier, [&] { w.setFullView(false); });
    CHECK_FALSE(p.fullView);
    w.setFullView(true);
    w.setFullView(true);   // already
    CHECK(p.fullView);
    QMetaObject::invokeMethod(&w, [&] { button(&w, "Full screen")->click(); });   // hidden in full view: the menu path
    pump(100);
    w.setFullView(false);
    CHECK_FALSE(p.fullView);
    w.showMaximized();
    w.setFullView(true);
    w.setFullView(false);
#ifndef Q_OS_MACOS   // (macOS animates in and out of full screen: the window state settles later)
    CHECK(waitFor([&] { return w.isMaximized(); }, 2000));
#endif
    // stopped, the frame on screen is re-stretched in place
    w.stopCapture();
    p.stretch->button(0)->click();
    p.stretch->button(1)->click();
    CHECK(unexpectedModals() == "");
}

#ifdef SLOPCAP_FAKE_ASI
TEST_CASE("window: a ZWO camera (fake SDK): gain presets, controls, formats, saved settings", "[app]") {
    REQUIRE(useFakeAsi());
    auto& f = fake::asi();
    clearSettings();
    auto& m = Modals::get();
    m.clear();
    MainWindow w;
    MainWindowProbe p(w);
    w.show();
    REQUIRE(QTest::qWaitForWindowExposed(&w));
    w.connectCamera("asi");
    REQUIRE(p.cam);
    REQUIRE(p.waitForFrames(3));
    CHECK(p.cameraPill->text().contains("ZWO ASI585MC · 21.5 °C"));

    // the SDK's gain landmarks as buttons (repeats left out)
    const auto presets = p.gainPresets->findChildren<QPushButton*>();
    CHECK(presets.size() == 5);
    button(p.gainPresets, "Low noise 252")->click();
    CHECK(p.gain->value() == 252);
    CHECK(p.controlBoxes.at("Offset")->value() == 50);
    CHECK(waitFor([&] { return f.control("Gain")->value == 252 && f.control("Offset")->value == 50; }, 3000));
    button(p.gainPresets, "Preset 400")->click();
    CHECK(p.gain->value() == 400);

    p.exposure->setValue(700);
    CHECK(waitFor([&] { return f.control("Exposure")->value != 10000; }, 3000));
    CHECK(p.controlBoxes.size() == 5);   // the writable ones
    p.controlBoxes.at("WB_R")->setValue(60);
    CHECK(waitFor([&] { return f.control("WB_R")->value == 60; }, 3000));

    // two readout modes: the high-speed switch
    REQUIRE(p.highSpeed->isVisibleTo(&w));
    CHECK_FALSE(p.readout->isVisibleTo(&w));
    p.highSpeed->setChecked(true);
    CHECK(f.control("HighSpeedMode")->value == 1);
    REQUIRE(p.waitForFrames(2));
    p.bits->button(16)->click();
    CHECK(p.cam->bitDepth() == 16);
    REQUIRE(p.waitForFrames(2));
    const int i = p.roiPreset->findData(QSize(320, 240));
    REQUIRE(i >= 0);
    p.roiPreset->setCurrentIndex(i);
    emit p.roiPreset->activated(i);
    CHECK(f.width == 320);
    CHECK(f.startX == 160);
    CHECK(f.startY == 120);
    REQUIRE(p.waitForFrames(2));

    // disconnecting keeps the camera's format and controls; connecting again restores them
    m.pickMenu("Disconnect");
    p.cameraPill->click();
    REQUIRE(m.done());
    const QString key = "cameras/ZWO-ZWO_ASI585MC-a0a1a2a3a4a5a6a7/";
    CHECK(QSettings().value(key + "bits").toInt() == 16);
    CHECK(QSettings().value(key + "readout").toInt() == 1);
    CHECK(QSettings().value(key + "controls/WB_R").toInt() == 60);
    f.control("WB_R")->value = 52;
    m.pickMenu("asi:0");
    p.cameraPill->click();
    REQUIRE(m.done());
    REQUIRE(p.cam);
    CHECK(p.cam->bitDepth() == 16);
    CHECK(p.cam->roi().width == 320);
    CHECK(p.highSpeed->isChecked());
    CHECK(f.control("WB_R")->value == 60);
    p.roiMax->click();
    CHECK(f.width == 640);

    // a saved format the camera refuses, and a control it doesn't have
    m.pickMenu("Disconnect");
    p.cameraPill->click();
    REQUIRE(m.done());
    QSettings().setValue(key + "roi", QRect(0, 0, 100, 100));
    QSettings().setValue(key + "controls/Bogus", 1);
    w.connectCamera("asi");
    CHECK(p.fMessage->text().contains("Saved format not accepted"));

    // the camera won't start: no session, so controls go to it directly (and its errors show)
    m.pickMenu("Disconnect");
    p.cameraPill->click();
    REQUIRE(m.done());
    f.fail["ASIStartVideoCapture"] = 1;
    m.answer(QMessageBox::Ok);
    w.connectCamera("asi");
    REQUIRE(m.done());
    CHECK(m.lastSeen().contains("Could not start"));
    CHECK_FALSE(p.session);
    p.gain->setValue(5);
    CHECK(f.control("Gain")->value == 5);
    w.setNamedControl("Gain", 100000);
    CHECK(p.fMessage->text().contains("out of range"));
    f.fail.clear();
    m.pickMenu("Disconnect");
    p.cameraPill->click();
    REQUIRE(m.done());

    // the camera won't open
    f.fail["ASIOpenCamera"] = 2;
    m.answer(QMessageBox::Ok);
    w.connectCamera("asi");
    REQUIRE(m.done());
    CHECK(m.lastSeen().contains("Could not open asi"));
    f.fail.clear();
    // a camera with neither gain nor exposure: those sliders are off
    std::erase_if(f.cam().controls, [](const fake::AsiControl& c) { return c.name == "Gain" || c.name == "Exposure"; });
    w.connectCamera("asi");
    CHECK_FALSE(p.gain->isEnabled());
    CHECK_FALSE(p.exposure->isEnabled());
    w.printSummary();
    w.stopCapture();
    m.pickMenu("Disconnect");
    p.cameraPill->click();
    REQUIRE(m.done());

#ifdef SLOPCAP_FAKE_QHY
    // a QHY camera with three read modes: a list instead of the switch
    auto& q = fake::qhy();
    q.reset();
    q.frameMs = 10;
    q.cam().width = 640, q.cam().height = 480;
    q.cam().modes[2].width = 640, q.cam().modes[2].height = 400;
    w.connectCamera("qhy");
    REQUIRE(p.cam);
    CHECK(p.readout->isVisibleTo(&w));
    CHECK_FALSE(p.highSpeed->isVisibleTo(&w));
    REQUIRE(p.readout->count() == 3);
    REQUIRE(p.waitForFrames(2));
    p.readout->setCurrentIndex(2);
    emit p.readout->activated(2);
    CHECK(p.cam->readoutMode() == 2);
    CHECK(p.roiPreset->itemText(0) == "Full 640 × 400");
    REQUIRE(p.waitForFrames(2));
    CHECK(button(p.gainPresets, "HCG"));
    w.close();
#endif
    CHECK(unexpectedModals() == "");
}
#endif

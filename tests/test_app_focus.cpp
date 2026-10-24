// Focus, mount and collimation in the window, against INDI's focuser and telescope simulators and the fake ZWO camera
// drawing frames whose sharpness follows the focuser (each colour sharpest at its own position).
#include "app/adc_panel.hpp"
#include "app/collimation_panel.hpp"
#include "app/focus_controller.hpp"
#include "app/focus_views.hpp"
#include "app/live_inset.hpp"
#include "app/mount_section.hpp"
#include "app_harness.hpp"
#include "devices/focuser.hpp"
#include "devices/mount.hpp"
#include "preview/preview_widget.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace slopcap;
using namespace slopcap::app;
using namespace slopcap::test;

namespace {

QPushButton* byName(QWidget* root, const QString& accessibleName) {
    for (auto* b : root->findChildren<QPushButton*>())
        if (b->accessibleName() == accessibleName) return b;
    return nullptr;
}

bool arrive(devices::Focuser& f, int at, int ms = 30000) {
    f.goTo(at);
    return waitFor([&] { return f.ready() && !f.moving() && f.position() == at; }, ms);
}

}  // namespace

TEST_CASE("focus: INDI focuser and mount simulators, sweeps, results, collimation", "[app][indi]") {
    SimServer server({"indi_simulator_focus", "indi_simulator_telescope"});
    if (!server.ok) SKIP("indiserver / INDI simulators not installed");
    if (!useFakeAsi()) SKIP("no fake ZWO SDK in this build");
    auto& m = Modals::get();
    m.clear();
    clearSettings();
    {
        QSettings st;
        st.setValue("focuser/server", server.address());
        st.setValue("focuser/autoConnect", true);
        st.setValue("focuser/backlash", 20);
        st.setValue("site/latitude", 51.5);   // (for ADC mode's altitude, from the mount)
        st.setValue("site/longitude", -0.1);
    }
    TempDir dir;
    std::atomic<int> position{0};
    MainWindow w;
    MainWindowProbe p(w);
    w.setOutputDir(dir.q());
    w.show();
    REQUIRE(QTest::qWaitForWindowExposed(&w));
    auto& focuser = p.focus->focuser();
    QObject::connect(&focuser, &devices::Focuser::positionChanged, &w, [&](int pos) { position = pos; });
    REQUIRE(waitFor([&] { return focuser.ready(); }, 15000));
    REQUIRE(arrive(focuser, 5000));
    position = 5000;
    drawFocusFrames(&position, {4940, 5000, 5060});   // red focuses inside, blue outside
    w.connectCamera("asi");
    REQUIRE(p.waitForFrames(3));

    // the Capture sidebar's focuser: position and nudges
    auto* section = w.findChild<FocuserSection*>();
    REQUIRE(section);
    CHECK(waitFor([&] { return labelWith(section, "5,000") != nullptr; }, 3000));
    CHECK(labelWith(section, "Backlash 20 steps"));
    button(section, "+10")->click();
    CHECK(waitFor([&] { return focuser.position() == 5010 && !focuser.moving(); }, 20000));
    button(section, "−10")->click();
    CHECK(waitFor([&] { return focuser.position() == 5000 && !focuser.moving(); }, 20000));
    CHECK(waitFor([&] { return p.tCentre->text() != "—"; }, 5000));

    // the mount, on the same server
    REQUIRE(waitFor([&] { return p.mount->ready(); }, 20000));
    auto* ms = p.mountSection;
    CHECK(waitFor([&] { return p.mount->parked().has_value(); }, 5000));
    if (auto* unpark = button(ms, "Unpark"); unpark && unpark->isVisible()) {
        unpark->click();
        CHECK(waitFor([&] { return p.mount->parked() == false; }, 15000));
    }
    if (auto* track = button(ms, "Start tracking"); track && track->isVisible()) {
        track->click();
        CHECK(waitFor([&] { return p.mount->tracking() == true; }, 5000));
    }
    auto* pulse = ms->findChild<QComboBox*>();
    pulse->setCurrentIndex(0);
    CHECK(ms->pulseMs() == 100);
    CHECK(waitFor([&] { return p.mount->canPulse(); }, 5000));
    for (const char* name : {"Nudge north", "Nudge east", "Nudge west", "Nudge south", "Stop mount"}) {
        auto* b = byName(ms, name);
        REQUIRE(b);
        b->click();
        pump(250);
    }
    CHECK(labelWith(ms, "RA "));

    // Focus mode: a sweep around the focuser
    p.modes->button(MainWindow::Focus)->click();
    CHECK(p.mode == MainWindow::Focus);
    auto* sp = p.sweepPanel;
    button(sp, "Coarse")->click();
    button(sp, "Fine")->click();
    const auto spins = sp->findChildren<QSpinBox*>();
    REQUIRE(spins.size() == 5);   // from, to, step, frames, settle
    CHECK(spins[0]->value() == 5000 - 350);
    spins[0]->setValue(4900);
    spins[1]->setValue(5100);
    spins[2]->setValue(50);
    spins[3]->setValue(6);
    spins[4]->setValue(100);
    auto* metric = sp->findChild<QComboBox*>();
    metric->setCurrentIndex(2);
    metric->setCurrentIndex(0);
    checkBox(sp, "Blue")->toggle();
    checkBox(sp, "Blue")->toggle();
    button(sp, "Run sweep")->click();
    CHECK(p.focus->sweeping());
    CHECK(waitFor([&] { return p.headerInfo->text().startsWith("Sweep running"); }, 2000));
    REQUIRE(waitFor([&] { return !p.focus->sweeping(); }, 120000));
    CHECK(labelWith(sp, "Sweep complete"));
    CHECK_FALSE(QDir(dir.q()).entryList({"focus-sweep-*.csv"}).isEmpty());
    const auto r = p.focus->sweep().result(analysis::Metric::Gradient);
    REQUIRE(r.channels == 3);
    REQUIRE(r.ch[0].best);
    REQUIRE(r.ch[2].best);
    CHECK(*r.ch[0].best < *r.ch[2].best);   // red inside blue
    CHECK(r.spread);

    // the results: every strategy, the metric table, going to the chosen position
    auto* rp = p.resultsPanel;
    for (const char* t : {"Luminance-weighted", "Balanced", "Red", "Blue", "Green"}) button(rp, t)->click();
    auto* table = rp->findChild<QTableWidget*>();
    REQUIRE(table->rowCount() == 4);
    emit table->cellClicked(2, 0);
    CHECK(p.focus->liveMetric() == analysis::Metric::Variance);
    emit table->cellClicked(0, 0);
    p.chartPanel->grab();
    auto* go = button(rp, "Go to ");
    REQUIRE(go->isEnabled());
    go->click();
    CHECK(waitFor([&] { return !focuser.moving() && std::abs(focuser.position() - 5000) < 40; }, 30000));
    // with no optics set, the verdict asks for them; with them, it judges
    CHECK(labelWith(rp, "Set the focal ratio"));
    m.settings([](SettingsDialog& d) {
        const auto boxes = d.findChildren<QDoubleSpinBox*>();
        boxes[0]->setValue(10);   // f/10
        boxes[1]->setValue(4);    // 4 µm per step: the zone is ±34 steps
    });
    button(section, "⚙")->click();
    REQUIRE(m.done());
    CHECK(p.focus->criticalZoneSteps());
    CHECK((labelWith(rp, "can't all be sharp") || labelWith(rp, "All three can be sharp")));
    m.settings([](SettingsDialog& d) { d.findChildren<QDoubleSpinBox*>()[1]->setValue(0.5); });   // a wide zone
    button(section, "⚙")->click();
    REQUIRE(m.done());
    CHECK(labelWith(rp, "All three can be sharp"));
    p.chartPanel->grab();

    // stopping part-way puts the focuser back where the sweep started
    button(sp, "Run sweep")->click();
    REQUIRE(p.focus->sweeping());
    const int start = p.focus->sweepStart();
    pump(1500);
    button(sp, "Stop sweep")->click();
    CHECK_FALSE(p.focus->sweeping());
    CHECK(labelWith(sp, "Sweep stopped"));
    CHECK(waitFor([&] { return !focuser.moving() && focuser.position() == start; }, 30000));
    // too small a range
    spins[0]->setValue(5000);
    spins[1]->setValue(5000);
    button(sp, "Run sweep")->click();
    CHECK(labelWith(sp, "Range too small"));

    // inward approach, a server named differently (reconnects), and a sweep asked for at start (--sweep)
    m.settings([&](SettingsDialog& d) {
        d.findChild<QLineEdit*>()->setText(QString("127.0.0.1:%1").arg(server.port));
        d.findChild<QComboBox*>()->setCurrentIndex(1);
    });
    button(section, "⚙")->click();
    REQUIRE(m.done());
    REQUIRE(waitFor([&] { return focuser.ready(); }, 15000));
    checkBox(sp, "Fourier")->setChecked(false);
    w.startFocuser(QString("127.0.0.1:%1").arg(server.port), true, 60, 30, 5);
    REQUIRE(waitFor([&] { return p.focus->sweeping(); }, 20000));
    REQUIRE(waitFor([&] { return !p.focus->sweeping(); }, 120000));
    CHECK(labelWith(sp, "Sweep complete"));

    // Collimate mode: a defocused star, off centre and centred
    drawDonut(6);
    w.showTab("collimate");
    auto* right = p.collim->right();
    QLabel* offset = nullptr;
    for (auto* l : right->findChildren<QLabel*>())
        if (l->property("role").toString() == "monoHuge") offset = l;
    REQUIRE(offset);
    CHECK(waitFor([&] { return offset->text().endsWith(" %"); }, 10000));
    drawDonut(0);
    CHECK(waitFor([&] { return labelWith(right, "Well collimated") != nullptr; }, 10000));
    p.collim->right()->grab();
    drawFocusFrames(&position, {5000, 5000, 5000});   // a planet: no ring to find
    CHECK(waitFor([&] { return offset->text() == "—"; }, 10000));
    CHECK_FALSE(labelWith(right, "Analysis off"));
    checkBox(p.collim->left(), "Analyse")->setChecked(false);
    CHECK(labelWith(right, "Analysis off"));

    // ADC mode with a target that isn't a planet: the altitude is where the mount points
    p.target->setCurrentText("Star");
    w.showTab("adc");
    p.updateStats();
    CHECK(labelWith(p.adc->right(), "The telescope"));

    // back in Capture: no target box over the preview when it's turned off
    w.showTab("capture");
    p.targetBox->setChecked(false);
    REQUIRE(p.waitForFrames(3));
    p.targetBox->setChecked(true);
    w.close();
    CHECK(Modals::get().takeUnexpected().join("; ").toStdString() == "");
}

// The default sweep started well outside focus (what happened at the telescope: every sweep peaked at its first
// position and stopped there). It has to carry on past its range until the curve comes down on both sides, then go to
// the peak and hand the focuser back to every tab. Meanwhile the camera stays in view on the Focus tab, and the curves
// show over the preview on the others. The window opens on the Focus tab, so its preview has never been on screen.
TEST_CASE("focus: a sweep from outside focus finds it; camera and focuser on every tab", "[app][indi]") {
    SimServer server({"indi_simulator_focus"});
    if (!server.ok) SKIP("indiserver / indi_simulator_focus not installed");
    if (!useFakeAsi()) SKIP("no fake ZWO SDK in this build");
    auto& m = Modals::get();
    m.clear();
    clearSettings();
    {
        QSettings st;
        st.setValue("focuser/server", server.address());
        st.setValue("focuser/autoConnect", true);
        st.setValue("focuser/backlash", 20);
    }
    TempDir dir;
    std::atomic<int> position{0};
    MainWindow w;
    MainWindowProbe p(w);
    w.setOutputDir(dir.q());
    w.showTab("focus");
    w.show();
    REQUIRE(QTest::qWaitForWindowExposed(&w));
    auto& focuser = p.focus->focuser();
    QObject::connect(&focuser, &devices::Focuser::positionChanged, &w, [&](int pos) { position = pos; });
    REQUIRE(waitFor([&] { return focuser.ready(); }, 15000));
    REQUIRE(arrive(focuser, 4400));
    position = 4400;
    drawFocusFrames(&position, {5000, 5000, 5000});
    w.connectCamera("asi");
    REQUIRE(p.waitForFrames(3));   // frames flow, though the GPU preview hasn't started

    // the camera, small, beside the chart: close up and whole
    REQUIRE(waitFor([&] { return p.liveInset->hasImage(); }, 5000));
    p.liveInset->setView(LiveInset::View::Frame);
    pump(300);
    p.liveInset->grab();
    p.liveInset->setView(LiveInset::View::Target);
    pump(300);
    QTest::mouseDClick(p.liveInset, Qt::LeftButton);
    CHECK(p.mode == MainWindow::Capture);
    p.modes->button(MainWindow::Focus)->click();

    // a set of focuser controls on every tab; the Focus tab's nudges move the range along with the focuser
    CHECK(w.findChildren<FocuserControls*>().size() == 4);
    auto* focusControls = p.left->widget(MainWindow::Focus)->findChild<FocuserControls*>();
    REQUIRE(focusControls);
    auto* sp = p.sweepPanel;
    const auto spins = sp->findChildren<QSpinBox*>();
    REQUIRE(spins.size() == 5);   // from, to, step, frames, settle
    CHECK(waitFor([&] { return spins[0]->value() == 4400 - 350; }, 3000));
    button(focusControls, "+100")->click();
    CHECK(waitFor([&] { return !focuser.moving() && focuser.position() == 4500 && spins[0]->value() == 4500 - 350; }, 20000));
    CHECK(labelWith(sp, "Centred on the focuser"));
    button(focusControls, "−100")->click();
    CHECK(waitFor([&] { return !focuser.moving() && focuser.position() == 4400 && spins[1]->value() == 4400 + 350; }, 20000));
    spins[3]->setValue(6);     // frames and settle don't fix the range
    spins[4]->setValue(100);
    CHECK(labelWith(sp, "Centred on the focuser"));

    // focus is 250 steps past the top of the range
    button(sp, "Run sweep")->click();
    REQUIRE(p.focus->sweeping());
    CHECK_FALSE(button(focusControls, "+10")->isEnabled());
    w.showTab("capture");   // the curves come in over the preview
    CHECK(waitFor([&] { return p.curveInset->isVisible(); }, 3000));
    CHECK(waitFor([&] { return labelWith(p.curveInset, "Sweep ") != nullptr; }, 3000));
    p.curveInset->grab();
    REQUIRE(waitFor([&] { return !p.focus->sweeping(); }, 240000));
    CHECK(labelWith(sp, "Sweep complete"));
    CHECK(labelWith(sp, "carried on to"));
    const auto r = p.focus->sweep().result(analysis::Metric::Gradient);
    REQUIRE(r.ch[1].best);
    CHECK(std::abs(*r.ch[1].best - 5000) < 40);
    CHECK_FALSE(r.ch[1].atEdge);
    // ... went there, and every tab's step buttons are back (a completed sweep used to leave them greyed out)
    CHECK(waitFor([&] { return !focuser.moving() && std::abs(focuser.position() - 5000) < 40; }, 30000));
    for (auto* c : w.findChildren<FocuserControls*>()) CHECK(button(c, "+10")->isEnabled());
    CHECK(waitFor([&] { return spins[0]->value() == focuser.position() - 350; }, 3000));

    // the curves stay over the preview: a click opens the Focus tab, × hides them until the next sweep
    CHECK(p.curveInset->isVisible());
    CHECK(labelWith(p.curveInset, "Best focus"));
    QTest::mouseClick(p.curveInset, Qt::LeftButton, {}, QPoint(20, p.curveInset->height() - 10));
    CHECK(p.mode == MainWindow::Focus);
    CHECK_FALSE(p.curveInset->isVisible());
    w.showTab("adc");
    CHECK(p.curveInset->isVisible());
    button(p.curveInset, "×")->click();
    CHECK_FALSE(p.curveInset->isVisible());
    w.showTab("capture");
    CHECK_FALSE(p.curveInset->isVisible());
    w.setFullView(true);
    pump(200);
    w.setFullView(false);

    // a sweep set by hand, with neither extension nor the move at the end: it stays where it finished
    p.modes->button(MainWindow::Focus)->click();
    spins[0]->setValue(4900);
    spins[1]->setValue(5000);
    CHECK(labelWith(sp, "Fixed range"));
    checkBox(sp, "Carry on")->setChecked(false);
    checkBox(sp, "Go to the best")->setChecked(false);
    button(sp, "Run sweep")->click();
    REQUIRE(p.focus->sweeping());
    CHECK_FALSE(p.curveDismissed);
    REQUIRE(waitFor([&] { return !p.focus->sweeping(); }, 120000));
    CHECK_FALSE(labelWith(sp, "carried on"));
    pump(500);
    CHECK(focuser.position() == (focuser.approachOutward() ? 5000 : 4900));
    CHECK(spins[0]->value() == 4900);   // a fixed range stays fixed
    w.close();
    CHECK(Modals::get().takeUnexpected().join("; ").toStdString() == "");
}

// Not part of the normal run ([.] hides it): the Focus tab after a sweep of a camera whose red, green and blue come to
// focus at different places, saved for the README.   SLOPCAP_DOCS_DIR=DIR slopcap-app-tests "[docs]"
TEST_CASE("docs: the Focus tab after a per-colour sweep (screenshot)", "[.][docs]") {
    const QString out = qEnvironmentVariable("SLOPCAP_DOCS_DIR");
    if (out.isEmpty()) SKIP("set SLOPCAP_DOCS_DIR to where the screenshot goes");
    SimServer server({"indi_simulator_focus"});
    if (!server.ok) SKIP("indiserver / indi_simulator_focus not installed");
    if (!useFakeAsi()) SKIP("no fake ZWO SDK in this build");
    clearSettings();
    {
        QSettings st;
        st.setValue("focuser/server", server.address());
        st.setValue("focuser/autoConnect", true);
        st.setValue("focuser/backlash", 20);
        st.setValue("optics/focalRatio", 10.0);      // a 2.8 m SCT at f/10
        st.setValue("optics/micronsPerStep", 2.0);   // the critical focus zone is then ±67 steps
        st.setValue("record/target", "Jupiter");
    }
    TempDir dir;
    std::atomic<int> position{0};
    MainWindow w;
    MainWindowProbe p(w);
    w.setOutputDir(dir.q());
    w.resize(1500, 950);
    w.show();
    REQUIRE(QTest::qWaitForWindowExposed(&w));
    auto& focuser = p.focus->focuser();
    QObject::connect(&focuser, &devices::Focuser::positionChanged, &w, [&](int pos) { position = pos; });
    REQUIRE(waitFor([&] { return focuser.ready(); }, 15000));
    REQUIRE(arrive(focuser, 5000));
    position = 5000;
    drawFocusFrames(&position, {4890, 5000, 5130}, 70);   // red sharpest 110 steps inside green, blue 130 outside
    w.connectCamera("asi");
    REQUIRE(p.waitForFrames(3));
    p.modes->button(MainWindow::Focus)->click();
    const auto spins = p.sweepPanel->findChildren<QSpinBox*>();
    spins[0]->setValue(4600);
    spins[1]->setValue(5400);
    spins[2]->setValue(50);
    spins[3]->setValue(10);
    spins[4]->setValue(150);
    button(p.sweepPanel, "Run sweep")->click();
    // halfway, the Capture tab: the curves coming in over the live preview
    REQUIRE(waitFor([&] { return p.focus->sweepCount().first >= 9; }, 300000));
    w.showTab("capture");
    pump(800);
    CHECK(w.grab().save(out + "/capture-sweep.png"));
    p.modes->button(MainWindow::Focus)->click();
    REQUIRE(waitFor([&] { return !p.focus->sweeping(); }, 300000));
    button(p.resultsPanel, "Balanced")->click();
    pump(1500);   // (the camera, small, at the best focus it went to)
    CHECK(w.grab().save(out + "/focus.png"));
}

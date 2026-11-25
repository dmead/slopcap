// The filter wheel: the simulated wheel through the window (the Capture tab's slot buttons, the header's Filter box,
// names kept per wheel), bad specs, and ZWO's EFW backend against a fake of its SDK (tests/fakes/fake_efw.cpp).
#include "app/filter_wheel_section.hpp"
#include "app_harness.hpp"
#include "devices/filter_wheel.hpp"
#include "devices/wheel.hpp"
#ifdef SLOPCAP_FAKE_EFW
#include "fakes/fake_efw.hpp"
#endif

#include <QComboBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

using namespace slopcap;
using namespace slopcap::app;
using namespace slopcap::test;

namespace {

QStringList items(QComboBox* c) {
    QStringList out;
    for (int i = 0; i < c->count(); ++i) out << c->itemText(i);
    return out;
}

}  // namespace

TEST_CASE("wheel: the simulated wheel from the slot buttons and the Filter box", "[app][wheel]") {
    QSettings().remove("wheel");
    MainWindow w;
    MainWindowProbe p(w);
    w.show();
    CHECK_FALSE(p.wheel->ready());
    CHECK(items(p.filter).contains("UV"));   // no wheel: the usual filter list

    w.openWheel("sim:5");
    REQUIRE(p.wheel->ready());
    CHECK(QSettings().value("wheel/spec").toString() == "sim:5");
    CHECK(p.wheel->model() == "Simulated wheel (5 slots)");
    REQUIRE(waitFor([&] { return p.wheel->position() == 0; }, 2000));
    CHECK(items(p.filter) == QStringList{"", "1", "2", "3", "4", "5"});
    CHECK(p.filter->currentText() == "1");
    REQUIRE(p.wheelSection->slotButton(0));
    CHECK(p.wheelSection->slotButton(0)->isChecked());
    CHECK_FALSE(p.wheelSection->slotButton(5));

    // names, from the dialog; blanks stay numbers
    auto& m = Modals::get();
    m.enterText("L, R,  , B, Ha");
    p.wheelSection->editNames();
    REQUIRE(m.done());
    CHECK(p.wheel->names() == QStringList{"L", "R", "3", "B", "Ha"});
    CHECK(items(p.filter) == QStringList{"", "L", "R", "3", "B", "Ha"});
    CHECK(p.filter->currentText() == "L");
    CHECK(p.wheelSection->slotButton(4)->text() == "Ha");
    CHECK(p.wheel->slotNamed("ha") == 4);
    CHECK(p.wheel->slotNamed("") == -1);
    CHECK(p.wheel->slotNamed("UV") == -1);
    m.enterText("");   // cancelled: unchanged
    p.wheelSection->editNames();
    REQUIRE(m.done());
    CHECK(p.wheel->names().size() == 5);

    // the Filter box turns the wheel
    QSignalSpy arrived(p.wheel, &devices::FilterWheel::arrived);
    p.filter->setCurrentIndex(p.filter->findText("B"));
    emit p.filter->activated(p.filter->currentIndex());
    CHECK(p.wheel->moving());
    CHECK(p.wheel->target() == 3);
    CHECK_FALSE(p.wheelSection->slotButton(1)->isEnabled());   // no new moves while it turns
    REQUIRE(waitFor([&] { return arrived.count() == 1; }, 5000));
    CHECK(arrived.at(0).at(0).toInt() == 3);
    CHECK(p.filter->currentText() == "B");
    CHECK(p.wheelSection->slotButton(3)->isChecked());
    CHECK(p.wheelSection->slotButton(1)->isEnabled());
    // ... and so does a slot button, the long way round
    p.wheelSection->slotButton(0)->click();
    REQUIRE(waitFor([&] { return arrived.count() == 2; }, 5000));
    CHECK(p.wheel->position() == 0);
    CHECK(p.filter->currentText() == "L");
    // already there: nothing to do
    p.wheel->goTo(0);
    CHECK_FALSE(p.wheel->moving());

    // Disconnect: the usual list again, and no connecting at the next start
    QSettings().setValue("wheel/autoConnect", true);
    QPushButton* off = nullptr;
    for (auto* b : p.wheelSection->findChildren<QPushButton*>())
        if (b->text() == "Disconnect") off = b;
    REQUIRE(off);
    off->click();
    CHECK_FALSE(p.wheel->ready());
    CHECK(items(p.filter).contains("UV"));
    CHECK_FALSE(QSettings().value("wheel/autoConnect").toBool());
    // Connect: the remembered spec, and the names kept for that wheel
    QPushButton* on = nullptr;
    for (auto* b : p.wheelSection->findChildren<QPushButton*>())
        if (b->text() == "Connect") on = b;
    REQUIRE(on);
    on->click();
    REQUIRE(p.wheel->ready());
    CHECK(p.wheel->names() == QStringList{"L", "R", "3", "B", "Ha"});
    CHECK(QSettings().value("wheel/autoConnect").toBool());

    // the next start connects it by itself
    MainWindow w2;
    MainWindowProbe p2(w2);
    REQUIRE(waitFor([&] { return p2.wheel->ready(); }, 2000));
    CHECK(waitFor([&] { return p2.filter->currentText() == "L"; }, 2000));
    QSettings().remove("wheel");
}

TEST_CASE("wheel: specs that don't open", "[app][wheel]") {
    devices::FilterWheel wheel;
    QSignalSpy failed(&wheel, &devices::FilterWheel::failed);
    for (const char* spec : {"nope", "sim:1", "sim:x", "simx", "efw:x", "efwx"}) {
        CHECK_FALSE(wheel.open(spec));
        CHECK_FALSE(wheel.ready());
    }
    CHECK(failed.count() == 6);
    CHECK(failed.at(0).at(0).toString().contains("unknown filter wheel"));
    CHECK_THROWS_AS(openWheel("sim:21"), WheelError);
    REQUIRE(openWheel("sim"));
    CHECK(openWheel("sim")->slotCount() == 5);
    // the simulated wheel refuses what the real one does
    auto sim = openWheel("sim:3");
    CHECK_THROWS_AS(sim->setPosition(3), WheelError);
    sim->setPosition(2);
    CHECK(sim->position() == -1);
    CHECK_THROWS_AS(sim->setPosition(1), WheelError);   // still turning
    CHECK_FALSE(listWheels().empty());
    wheel.goTo(1);   // nothing open: nothing happens
    CHECK(wheel.position() == -1);
    CHECK(wheel.name(0).isEmpty());
}

#ifdef SLOPCAP_FAKE_EFW
TEST_CASE("wheel: ZWO EFW through its SDK", "[app][wheel][efw]") {
    auto& e = fake::efw();
    e.reset();
    QSettings().remove("wheel");

    CHECK(listWheels() == std::vector<std::string>{"efw      ZWO EFW"});
    e.count = 2;
    CHECK(listWheels().size() == 2);
    CHECK(listWheels()[1].starts_with("efw:1"));
    e.count = 0;
    CHECK(listWheels() == std::vector<std::string>{"(no ZWO filter wheel found)"});
    CHECK_THROWS_WITH(openWheel("efw"), "no ZWO filter wheel at index 0 (0 found)");
    e.reset();

    devices::FilterWheel wheel;
    QSignalSpy failed(&wheel, &devices::FilterWheel::failed);
    QSignalSpy arrived(&wheel, &devices::FilterWheel::arrived);
    REQUIRE(wheel.open("efw"));
    CHECK(e.open);
    CHECK(e.busyProperty == 0);   // waited for it to count its slots
    CHECK(wheel.model() == "ZWO EFW (7 slots)");
    CHECK(wheel.slotCount() == 7);
    REQUIRE(waitFor([&] { return wheel.position() == 0; }, 2000));
    wheel.setNames({"lum", "red", "green", "blue", "sulfur", "halpha", "oxygen3"});
    CHECK(QSettings().value("wheel/a0a1a2a3a4a5a6a7/names").toStringList().size() == 7);   // kept per serial

    wheel.goTo(5);
    CHECK(e.moves == 1);
    REQUIRE(waitFor([&] { return arrived.count() == 1; }, 5000));
    CHECK(wheel.position() == 5);
    CHECK(wheel.name(wheel.position()) == "halpha");
    // turned by another program: followed
    e.target = 2;
    e.at = 2;
    REQUIRE(waitFor([&] { return wheel.position() == 2; }, 3000));
    CHECK(wheel.target() == 2);
    CHECK_FALSE(wheel.moving());

    // asked while turning (another program's move): said, not crashed
    e.target = 4;
    wheel.goTo(6);
    REQUIRE(failed.count() == 1);
    CHECK(failed.at(0).at(0).toString().contains("still turning"));
    e.at = e.target;

    // an error state closes it
    e.errorState = true;
    REQUIRE(waitFor([&] { return !wheel.ready(); }, 3000));
    CHECK(failed.last().at(0).toString().contains("error state"));
    CHECK(e.closes == 1);

    // unplugged before opening
    e.reset();
    e.removed = true;
    CHECK_FALSE(wheel.open("efw"));
    CHECK(failed.last().at(0).toString().contains("removed"));
    e.reset();
    QSettings().remove("wheel");
}
#endif

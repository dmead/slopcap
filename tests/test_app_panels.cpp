// The application's panels on their own: charts and histograms painted with and without data, the focus and mount
// panels with nothing connected, the settings dialog's round trip, the theme's building blocks, the session log.
#include "app/collimation_panel.hpp"
#include "app/focus_controller.hpp"
#include "app/focus_views.hpp"
#include "app/mount_section.hpp"
#include "app/mount_settings_dialog.hpp"
#include "app/raw_histogram.hpp"
#include "app/session_log.hpp"
#include "app/settings_dialog.hpp"
#include "app/theme.hpp"
#include "app_harness.hpp"
#include "fakes/fake_mounts.hpp"
#include "mounts/mount_log.hpp"
#include "mounts/onstep.hpp"
#include "preview/preview_widget.hpp"

#include <QButtonGroup>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace slopcap;
using namespace slopcap::app;
using namespace slopcap::test;

namespace {

analysis::FocusScores scores(int channels, std::array<double, 3> s) {
    analysis::FocusScores f;
    f.channels = channels;
    for (std::size_t c = 0; c < 3; ++c) f.ch[c].sharpness = s[c];
    return f;
}

/// A sweep whose channels peak at 4950 / 5000 / 5050 (or one mono channel at 5000).
analysis::SweepResult sweep(int channels) {
    analysis::SweepAnalysis a;
    for (int pos = 4800; pos <= 5200; pos += 50)
        for (int i = 0; i < 4; ++i) {
            auto peak = [&](double at) { return 100 - std::pow((pos - at) / 40.0, 2) + i * 0.1; };
            a.add(pos, scores(channels, {peak(4950), peak(5000), peak(5050)}), analysis::Metric::Gradient);
        }
    return a.result(analysis::Metric::Gradient);
}

}  // namespace

TEST_CASE("panels: charts and histograms paint with and without data", "[app][panels]") {
    ChannelBars bars;
    bars.resize(240, 66);
    bars.grab();
    bars.setValues(1, {0.5, 0, 0}, {false, false, false});
    bars.grab();
    bars.setValues(3, {0.9, 1.2, 0.3}, {false, true, false});
    bars.grab();

    FocusChart chart;
    chart.resize(600, 360);
    chart.grab();   // no sweep yet
    const auto colour = sweep(3);
    REQUIRE(colour.channels == 3);
    chart.setData(colour, {true, true, true}, 5000.0, 30.0);
    chart.grab();
    chart.setData(colour, {true, false, true}, std::nullopt, std::nullopt);
    chart.grab();
    chart.setData(sweep(1), {true, true, true}, 5000.0, std::nullopt);
    chart.grab();

    RawHistogramSection raw;
    raw.resize(280, 220);
    raw.grab();
    preview::RawHistogram h;
    h.channels = 3;
    h.fullScale = 4095;
    h.adcBits = 12;
    for (std::size_t c = 0; c < 3; ++c) {
        h.bins[c].assign(128, 0);
        for (std::size_t i = 0; i < 128; ++i) h.bins[c][i] = static_cast<uint32_t>(1000 * std::exp(-std::pow((double(i) - 30.0 - 10.0 * double(c)) / 8, 2)));
        h.bins[c][127] = 50;
        h.median[c] = 400, h.max[c] = 4095, h.clipped[c] = c == 1 ? 0.02 : 0, h.samples[c] = 100000;
    }
    raw.setData(h);
    raw.grab();
    h.channels = 1;
    h.clipped = {};
    h.bins[0].assign(128, 0);   // nothing at all
    raw.setData(h);
    raw.grab();
    raw.clear();
    raw.grab();

    OffsetHistory hist;
    hist.resize(260, 110);
    hist.grab();
    for (int i = 0; i < 320; ++i) hist.add(5 + 3 * std::sin(i / 10.0));
    hist.grab();
    hist.clear();
}

TEST_CASE("panels: focus, mount and collimation with nothing connected", "[app][panels]") {
    FocusController fc;
    fc.setServer("localhost:1");
    FocuserSection section(fc);
    SweepPanel sweepPanel(fc);
    FocusChartPanel chartPanel(fc);
    FocusResultsPanel results(fc);
    CHECK(labelWith(&section, "Not connected · localhost:1"));
    CHECK(button(&sweepPanel, "Connect the focuser first"));
    button(&sweepPanel, "Coarse")->click();   // no focuser: around the range's middle
    chartPanel.update(analysis::Metric::Laplacian, {true, true, true}, std::nullopt);
    results.update(analysis::Metric::Gradient);
    CHECK(labelWith(&results, "Run a sweep"));
    CHECK_FALSE(results.chosen(analysis::Metric::Gradient));
    CHECK(results.target() == analysis::FocusTarget::Green);
    fc.setOptics(8, 2);
    REQUIRE(fc.criticalZoneSteps());
    chartPanel.update(analysis::Metric::Gradient, {true, true, true}, 5000.0);
    CHECK(labelWith(&chartPanel, "Critical focus zone (f/8)"));
    fc.setSmoothing(0);
    fc.resetPeaks();
    fc.startSweep({});   // nothing happens without a focuser
    CHECK_FALSE(fc.sweeping());
    fc.stopSweep();

    devices::Mount mount(fc.indi());
    MountSection ms(mount, fc);
    CHECK(labelWith(&ms, "Not connected · localhost:1"));
    ms.setPulseMs(1000);
    CHECK(ms.pulseMs() == 1000);
    ms.setPulseMs(7);   // not offered: unchanged
    CHECK(ms.pulseMs() == 1000);
    button(&ms, "Connect")->click();   // nothing listens there
    CHECK(waitFor([&] { return !fc.indi().isConnected(); }, 2000));
    CHECK(ms.speed().isEmpty());   // guide pulses until a mount says what speeds it has

    // a built-in mount: its speeds join "Guide pulse", and with one picked a held button moves the mount
    devices::Mount sim(fc.indi());
    sim.setSpec("sim");
    MountSection simSection(sim, fc);
    sim.openBuiltIn();
    REQUIRE(waitFor([&] { return sim.ready() && sim.hasCoords(); }, 5000));
    const auto rates = sim.slewRates();
    REQUIRE(rates.size() >= 2);
    // the panel lists them when the worker's "connected" reaches it, which can come a moment after ready()
    CHECK(waitFor([&] {
        simSection.setSpeed(rates.last());
        return simSection.speed() == rates.last();
    }, 5000));
    CHECK(sim.slewRate() == rates.last());
    const double dec0 = sim.dec();
    auto* north = button(&simSection, "N");
    REQUIRE(north);
    emit north->pressed();
    CHECK(waitFor([&] { return sim.dec() > dec0 + 0.05; }, 5000));
    emit north->released();
    waitFor([] { return false; }, 600);   // (the move stops)
    const double dec1 = sim.dec();
    waitFor([] { return false; }, 1200);
    CHECK(std::abs(sim.dec() - dec1) < 0.01);   // stopped on release
    simSection.setSpeed("");
    CHECK(simSection.speed().isEmpty());
    sim.close();

    preview::PreviewWidget pv;
    CollimationPanel collim(&pv);
    collim.setAnalysing(true);
    CHECK(labelWith(collim.right(), "Looking for a star"));
    collim.setAnalysing(false);
    CHECK(labelWith(collim.right(), "Analysis off"));
}

TEST_CASE("panels: an OnStep's own settings and the mount's log", "[app][panels]") {
    FocusController fc;
    fc.setServer("localhost:1");
    auto sim = std::make_shared<mounts::SimMount>();
    FakeOnStep fake(sim);
    devices::Mount mount(fc.indi());
    mount.setSpec("onstep:/dev/fake");
    MountSection ms(mount, fc);
    mount.openWith([&] {
        return std::make_unique<mounts::OnStepDriver>(std::vector<mounts::TransportFactory>{fakeLink(fake.device(), std::make_shared<LinkState>())});
    });
    REQUIRE(waitFor([&] { return mount.ready() && mount.hasSettings(); }, 5000));
    REQUIRE(button(&ms, "Settings and log…"));
    auto* d = ms.openSettings();
    REQUIRE(waitFor([&] { return !d->valueOf("horizon").isEmpty(); }, 5000));
    CHECK(d->valueOf("horizon") == "-10");
    CHECK(d->valueOf("pierSide") == "E");
    CHECK(d->valueOf("buzzer") == "1");
    CHECK(d->valueOf("longitude") == "-0.1000");

    // only what was changed is written, and it's read back
    d->edit("horizon", "5");
    d->edit("buzzer", "0");
    d->apply();
    auto fakeSays = [&](auto f) {
        std::lock_guard lock(fake.m);
        return f();
    };
    CHECK(waitFor([&] { return fakeSays([&] { return fake.horizon == 5 && !fake.buzzer; }); }, 5000));
    CHECK(waitFor([&] { return d->valueOf("horizon") == "5" && d->valueOf("buzzer") == "0"; }, 5000));
    CHECK(fakeSays([&] { return fake.overhead == 80 && fake.autoFlip && fake.pierSide == 'E' && fake.backlashDec == 70; }));

    // the log has the commands; an error goes to the log and shows in red in the panel until the log is opened
    d->showLog();
    CHECK(waitFor([&] { return d->logText().contains(":Sh+05#  < 1"); }, 3000));
    CHECK_FALSE(d->logText().contains(":GU#"));   // status polls hidden
    d->close();
    mount.changeSetting("utcOffset", "+5:20");
    CHECK(waitFor([&] { return labelWith(&ms, "like +05:00") != nullptr; }, 3000));
    CHECK(waitFor([&] { return d->logText().contains("!! a UTC offset is like +05:00"); }, 3000));
    ms.openSettings(true);
    CHECK_FALSE(labelWith(&ms, "like +05:00")->isVisible());
    mount.close();
}

TEST_CASE("panels: settings dialog round trip", "[app][panels]") {
    Settings s;
    s.indiServer = "mount.lan:7624";
    s.focuserAutoConnect = false;
    s.backlash = 250;
    s.approachOutward = false;
    s.focalRatio = 10;
    s.micronsPerStep = 1.25;
    s.focalLengthMm = 2800;
    s.latitude = -33.9;
    s.longitude = 151.2;
    SettingsDialog d(s);
    const Settings back = d.settings();
    CHECK(back.indiServer == s.indiServer);
    CHECK(back.focuserAutoConnect == s.focuserAutoConnect);
    CHECK(back.backlash == 250);
    CHECK_FALSE(back.approachOutward);
    CHECK(back.focalRatio == 10);
    CHECK(back.micronsPerStep == 1.25);
    CHECK(back.focalLengthMm == 2800);
    CHECK(back.latitude == -33.9);
    CHECK(back.longitude == 151.2);
    SettingsDialog unset(Settings{});
    CHECK_FALSE(unset.settings().latitude);
}

TEST_CASE("panels: the theme's building blocks and the session log", "[app][panels]") {
    QWidget w;
    auto* flow = new theme::FlowLayout(&w);
    for (int i = 0; i < 12; ++i) flow->addWidget(theme::button(QString("chip %1").arg(i)));
    CHECK(flow->count() == 12);
    CHECK(flow->heightForWidth(120) > flow->heightForWidth(2000));
    CHECK(flow->minimumSize().width() > 0);
    w.resize(300, 200);
    w.show();
    pump(50);
    delete flow->takeAt(0);
    CHECK_FALSE(flow->takeAt(99));
    QLabel* text = nullptr;
    QLabel* dot = nullptr;
    auto* pill = theme::pill(text, dot);
    theme::setDot(dot, theme::kGood);
    text->setText("Connected");
    QButtonGroup* group = nullptr;
    auto* seg = theme::segmented({"One", "Two", "Three"}, group);
    CHECK(group->buttons().size() == 3);
    QLabel* value = nullptr;
    auto* stat = theme::stat("Caption", value);
    for (QWidget* x : {pill, seg, stat, static_cast<QWidget*>(theme::card()), static_cast<QWidget*>(theme::section("Heading"))}) {
        x->grab();
        delete x;
    }
    theme::monoFont(13);
    w.hide();

    // the session log: this run's file, and no more than 100 kept
    const QString path = sessionlog::path();
    REQUIRE_FALSE(path.isEmpty());
    qInfo() << "panels: a line for the log";
    qWarning() << "panels: a warning for the log";
    qCritical() << "panels: an error for the log";
    qDebug() << "panels: a debug line";
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString log = QString::fromUtf8(f.readAll());
    CHECK(log.contains(" I panels: a line for the log"));
    CHECK(log.contains(" W panels: a warning"));
    CHECK(log.contains(" E panels: an error"));
    CHECK(QDir(QFileInfo(path).path()).entryList({"slopcap-*.log"}).size() <= 100);
}

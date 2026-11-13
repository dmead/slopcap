// A Sony camera in the window (tests/fakes/fake_sony.*): its test shots as the preview, the Frames / step box at one
// picture a position, no recording, and a focus sweep against INDI's focuser simulator in which a picture exposed
// while the focuser moved doesn't count. Also the JPEG decoder against Qt's (libjpeg's) pictures.
#include "app/focus_controller.hpp"
#include "app/focus_views.hpp"
#include "app_harness.hpp"
#include "camera/jpeg_luma.hpp"
#include "camera/open_camera.hpp"
#include "camera/sony_camera.hpp"
#include "devices/focuser.hpp"
#include "fakes/fake_sony.hpp"

#include <QBuffer>
#include <QDoubleSpinBox>
#include <QImage>
#include <QImageWriter>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace slopcap;
using namespace slopcap::app;
using namespace slopcap::test;

namespace {

struct Sonys {
    fake::SonyCam cam;
    fake::SonySource source;
    Sonys() {
        source.cams = {&cam};
        setSonyDeviceSource(&source);
    }
    ~Sonys() { setSonyDeviceSource(nullptr); }
    Sonys(const Sonys&) = delete;
    Sonys& operator=(const Sonys&) = delete;
};

}  // namespace

TEST_CASE("sony: a stills camera in the window", "[app][sony]") {
    Modals::get().clear();
    clearSettings();
    Sonys s;
    s.source.problem = "no permission";   // listed all the same, with why it may not open
    const auto lines = listCameras();
    CHECK(std::ranges::find(lines, std::string("sony:0  ILCE-6000  (no permission)")) != lines.end());
    s.source.problem.clear();
    MainWindow w;
    MainWindowProbe p(w);
    w.show();
    REQUIRE(QTest::qWaitForWindowExposed(&w));
    auto spins = p.sweepPanel->findChildren<QSpinBox*>();
    REQUIRE(spins.size() == 5);   // from, to, step, frames, settle
    spins[3]->setValue(80);
    w.connectCamera("sony");
    REQUIRE(p.cam);
    CHECK(p.cam->info().stills);
    REQUIRE(p.waitForFrames(2));
    CHECK(spins[3]->value() == 1);
    CHECK(spins[3]->minimum() == 1);
    CHECK_FALSE(p.record->isEnabled());
    QTest::keyClick(&w, Qt::Key_R, Qt::ControlModifier);   // nor by its shortcut
    pump(200);
    CHECK_FALSE(p.recording);
    CHECK(p.controlBoxes.contains("Exposure"));
    CHECK(p.controlBoxes.contains("ISO"));
    CHECK(p.controlBoxes.contains("Interval"));
    CHECK(p.exposure->isEnabled());
    CHECK_FALSE(p.gain->isEnabled());
    p.controlBoxes["Exposure"]->setValue(1e6 / 60);   // the camera takes it between two shots
    CHECK(waitFor([&] { return p.cam->control("Exposure").value_or(0) > 16000; }, 10000));
    p.disconnectCamera();
    CHECK(spins[3]->value() == 80);   // the video setting back
    CHECK(spins[3]->minimum() == 5);
}

TEST_CASE("sony: a focus sweep takes one picture a position, none from while the focuser moved", "[app][indi][sony]") {
    SimServer server({"indi_simulator_focus"});
    if (!server.ok) SKIP("indiserver / indi_simulator_focus not installed");
    Modals::get().clear();
    clearSettings();
    {
        QSettings st;
        st.setValue("focuser/server", server.address());
        st.setValue("focuser/autoConnect", true);
    }
    Sonys s;
    s.cam.shotMs = 250;   // a slow camera: a picture pressed during a move comes in after the focuser has settled
    std::atomic<int> position{0};
    s.cam.setDraw([&position](uint8_t* px, int w, int h, int) { fake::drawSoftDisc(px, w, h, std::abs(position.load() - 5000) / 15.0); });
    TempDir dir;
    MainWindow w;
    MainWindowProbe p(w);
    w.setOutputDir(dir.q());
    w.showTab("focus");
    w.show();
    REQUIRE(QTest::qWaitForWindowExposed(&w));
    auto& focuser = p.focus->focuser();
    QObject::connect(&focuser, &devices::Focuser::positionChanged, &w, [&](int pos) { position = pos; });
    REQUIRE(waitFor([&] { return focuser.ready(); }, 15000));
    focuser.goTo(4800);
    REQUIRE(waitFor([&] { return !focuser.moving() && focuser.position() == 4800; }, 30000));
    w.connectCamera("sony");
    REQUIRE(p.waitForFrames(2));

    auto* sp = p.sweepPanel;
    const auto spins = sp->findChildren<QSpinBox*>();
    REQUIRE(spins.size() == 5);
    CHECK(spins[3]->value() == 1);
    spins[0]->setValue(4700);
    spins[1]->setValue(5300);
    spins[2]->setValue(100);
    spins[4]->setValue(50);
    const int before = s.cam.shots.load();
    button(sp, "Run sweep")->click();
    REQUIRE(p.focus->sweeping());
    REQUIRE(waitFor([&] { return !p.focus->sweeping(); }, 180000));
    const auto r = p.focus->sweep().result(analysis::Metric::Gradient);
    REQUIRE(r.channels == 1);
    REQUIRE(r.ch[0].best);
    CHECK(std::abs(*r.ch[0].best - 5000) < 40);
    CHECK(p.focus->sweepCount().second >= 7);
    CHECK(s.cam.shots.load() - before >= p.focus->sweepCount().second);
    p.disconnectCamera();
}

TEST_CASE("jpeg: the luminance of Qt's (libjpeg's) pictures", "[app][sony][jpeg]") {
    if (!QImageWriter::supportedImageFormats().contains("jpeg")) SKIP("no JPEG plugin in this Qt");
    QImage img(97, 61, QImage::Format_RGB32);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) img.setPixel(x, y, qRgb(60 + x, 90 + y, 70 + (x + y) / 2));
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    REQUIRE(img.save(&buf, "JPG", 90));
    const std::vector<uint8_t> data(bytes.begin(), bytes.end());
    std::vector<uint8_t> y;
    const auto sz = jpeg::decodeLuma(data, y);
    REQUIRE(sz.width == 97);
    REQUIRE(sz.height == 61);
    QImage back;
    REQUIRE(back.loadFromData(bytes, "JPG"));
    back = back.convertToFormat(QImage::Format_RGB32);
    double sum = 0, worst = 0;
    for (int r = 0; r < sz.height; ++r)
        for (int c = 0; c < sz.width; ++c) {
            const QRgb px = back.pixel(c, r);
            const double ref = 0.299 * qRed(px) + 0.587 * qGreen(px) + 0.114 * qBlue(px);
            const double d = std::abs(ref - y[static_cast<std::size_t>(r * sz.width + c)]);
            sum += d;
            worst = std::max(worst, d);
        }
    CHECK(sum / (sz.width * sz.height) < 1.5);
    CHECK(worst < 8);
}

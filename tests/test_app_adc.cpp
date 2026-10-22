// ADC mode: the panel on its own (frames handed to it, what it says for each altitude and image scale), and in the
// window with the fake ZWO camera drawing a planet whose red and blue images the test moves apart and together, as
// an ADC's levers would.
#include "app/adc_panel.hpp"
#include "app_harness.hpp"
#include "preview/preview_widget.hpp"

#include <QButtonGroup>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace slopcap;
using namespace slopcap::app;
using namespace slopcap::test;

namespace {

/// A raw RGGB frame of a planet whose red image sits (dx, dy) px from its blue one.
LatestFrame::Frame frame(double dx, double dy, int w = 320, int h = 240, double cx = 160, double cy = 120) {
    LatestFrame::Frame f;
    f.info.roi = {0, 0, w, h, 1};
    f.info.bitDepth = 8;
    f.info.bytes = static_cast<std::size_t>(w) * h;
    f.pixels.resize(f.info.bytes);
    const double sx[3] = {dx / 2, 0, -dx / 2}, sy[3] = {dy / 2, 0, -dy / 2};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int site = (y & 1) * 2 + (x & 1);
            const int c = site == 0 ? 0 : site == 3 ? 2 : 1;
            const double v = 0.04 + 0.7 * std::clamp(40 - std::hypot(x - cx - sx[c], y - cy - sy[c]) + 0.5, 0.0, 1.0);
            f.pixels[static_cast<std::size_t>(y) * w + x] = static_cast<std::byte>(std::lround(v * 250));
        }
    return f;
}

/// Hand the panel the same frame until its median has settled (it measures at most every 80 ms).
void feed(AdcPanel& panel, const LatestFrame::Frame& f, Bayer bayer = Bayer::RGGB) {
    for (int i = 0; i < 18; ++i) {
        panel.onFrame(f, bayer);
        pump(85);
    }
}

QString statusOf(AdcPanel& panel) {
    for (auto* l : panel.right()->findChildren<QLabel*>())
        if (l->property("role").toString() == "muted" && l->wordWrap()) return l->text();
    return {};
}

}  // namespace

TEST_CASE("ADC panel: readings, guidance and what the air does", "[app][adc]") {
    preview::PreviewWidget pv;
    AdcPanel panel(&pv);
    CHECK(statusOf(panel).startsWith("Open ADC mode"));
    panel.onFrame(frame(0, 4), Bayer::RGGB);   // not active: ignored
    CHECK_FALSE(panel.residual());
    panel.setActive(true);
    CHECK(statusOf(panel) == "Looking for the planet…");
    CHECK_FALSE(button(panel.left(), "Remember uncorrected")->isEnabled());

    // uncorrected: red 4 px below blue
    feed(panel, frame(0, 4));
    REQUIRE(panel.residual());
    CHECK(std::abs(panel.residual()->y() - 4) < 0.2);
    CHECK(std::abs(panel.residual()->x()) < 0.2);
    CHECK(statusOf(panel).contains("Remember uncorrected"));
    button(panel.left(), "Remember uncorrected")->click();
    REQUIRE(panel.uncorrected());

    feed(panel, frame(0, 1.5));
    CHECK(statusOf(panel).startsWith("Under-corrected"));
    CHECK(statusOf(panel).contains("% of the uncorrected dispersion is left"));
    feed(panel, frame(0, -1.2));
    CHECK(statusOf(panel).startsWith("Over-corrected"));
    feed(panel, frame(2.5, 0.3));
    CHECK(statusOf(panel).startsWith("The ADC is turned"));
    feed(panel, frame(0.1, 0.05));
    CHECK(statusOf(panel).startsWith("Lined up"));
    panel.right()->grab();

    // the image scale and the altitude
    panel.setContext(std::nullopt, std::nullopt);
    CHECK(labelWith(panel.right(), "Settings → Site"));
    CHECK(labelWith(panel.right(), "gives arcseconds"));
    panel.setContext(0.25, AdcPanel::Altitude{30, "Jupiter"});
    CHECK(labelWith(panel.right(), "Jupiter is 30° up: the air lifts blue 1.30″ above red."));
    CHECK(labelWith(panel.right(), "Uncorrected you measured"));
    panel.setContext(0.25, AdcPanel::Altitude{9, "Saturn"});
    CHECK(labelWith(panel.right(), "Saturn is only 9° up"));
    panel.setContext(0.25, AdcPanel::Altitude{-4, "The telescope"});
    CHECK(labelWith(panel.right(), "The telescope is below the horizon."));

    // things it can't measure: a mono camera, no planet, a planet at the edge
    feed(panel, frame(0, 2), Bayer::None);
    CHECK(statusOf(panel).contains("needs a colour camera"));
    LatestFrame::Frame dark = frame(0, 0);
    std::fill(dark.pixels.begin(), dark.pixels.end(), std::byte{10});
    feed(panel, dark);
    CHECK(statusOf(panel) == "No planet in the frame.");
    feed(panel, frame(0, 3, 320, 240, 20, 120));
    CHECK(statusOf(panel).contains("touches the frame's edge"));

    button(panel.left(), "Reset")->click();
    CHECK_FALSE(panel.residual());
    CHECK_FALSE(panel.uncorrected());
    panel.setActive(false);
}

TEST_CASE("ADC mode in the window: the fake camera's red and blue moved by the test", "[app][adc]") {
    if (!useFakeAsi()) SKIP("no fake ZWO SDK in this build");
    auto& m = Modals::get();
    m.clear();
    clearSettings();
    {
        QSettings st;
        st.setValue("record/target", "Jupiter");
        st.setValue("optics/focalLengthMm", 2000.0);   // 2.9 µm pixels: 0.30″/px
        st.setValue("site/latitude", 51.5);
        st.setValue("site/longitude", -0.1);
    }
    Dispersed disp;
    disp.dy = 4;
    drawDispersed(&disp);
    MainWindow w;
    MainWindowProbe p(w);
    w.show();
    REQUIRE(QTest::qWaitForWindowExposed(&w));
    w.connectCamera("asi");
    REQUIRE(p.waitForFrames(3));

    p.modes->button(MainWindow::Adc)->click();
    CHECK(p.mode == MainWindow::Adc);
    CHECK(p.adc->active());
    CHECK(waitFor([&] { return p.headerInfo->text().startsWith("Atmospheric dispersion"); }, 2000));
    REQUIRE(waitFor([&] { return p.adc->residual() && std::abs(p.adc->residual()->y() - 4) < 0.3; }, 8000));
    CHECK(labelWith(p.adc->right(), "Jupiter"));        // the altitude comes from the ephemeris
    CHECK(labelWith(p.adc->right(), "px · ") != nullptr);   // and arcseconds from the image scale
    button(p.adc->left(), "Remember uncorrected")->click();
    disp.dy = 1.5;
    CHECK(waitFor([&] { return statusOf(*p.adc).startsWith("Under-corrected"); }, 8000));
    disp.dy = 0.05;
    CHECK(waitFor([&] { return statusOf(*p.adc).startsWith("Lined up"); }, 8000));

    // leaving ADC mode stops measuring; the command line's --tab adc comes back to it
    w.showTab("capture");
    CHECK_FALSE(p.adc->active());
    w.showTab("adc");
    CHECK(p.adc->active());
    // a target that isn't a planet, no mount: no altitude
    p.target->setCurrentText("Star");
    p.updateStats();
    CHECK(labelWith(p.adc->right(), "Settings → Site"));
    // disconnecting forgets the readings
    m.pickMenu("Disconnect");
    p.cameraPill->click();
    REQUIRE(m.done());
    CHECK_FALSE(p.adc->residual());
    w.close();
    CHECK(m.takeUnexpected().join("; ").toStdString() == "");
}

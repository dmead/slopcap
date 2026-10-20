// Remote viewing (src/net): the frame codec, the message framing, the camera state, and a server and client talking
// over loopback with a replayed recording as the camera.
#include "camera/sim_camera.hpp"
#include "io/ser.hpp"
#include "net/discovery.hpp"
#include "net/frame_codec.hpp"
#include "net/protocol.hpp"
#include "net/rate_control.hpp"
#include "net/remote_client.hpp"
#include "net/remote_server.hpp"
#include "pipeline/capture_session.hpp"
#include "test_util.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QSysInfo>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace slopcap;
using namespace std::chrono_literals;

namespace {

template <typename F>
bool waitFor(F done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

/// A smooth RGGB scene: each CFA site has its own gradient, so mixing sites up shows as a large error.
std::vector<std::byte> bayerScene(int w, int h) {
    std::vector<std::byte> px(static_cast<std::size_t>(w * h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int site = (y & 1) * 2 + (x & 1);   // R, G1, G2, B
            const int v = site == 0 ? 40 + x * 150 / w : site == 3 ? 200 - y * 150 / h : 100 + (x + y) * 50 / (w + h);
            px[static_cast<std::size_t>(y * w + x)] = static_cast<std::byte>(v);
        }
    return px;
}

int sample8(const std::vector<std::byte>& px, int w, int x, int y) { return std::to_integer<int>(px[static_cast<std::size_t>(y * w + x)]); }

FrameInfo frameInfo(int w, int h, int bits) {
    FrameInfo fi;
    fi.sequence = 42;
    fi.timestamp = std::chrono::system_clock::now();
    fi.roi = {16, 8, w, h, 1};
    fi.bitDepth = bits;
    fi.bytes = static_cast<std::size_t>(w * h * (bits > 8 ? 2 : 1));
    return fi;
}

std::filesystem::path makeSer(const test::TempDir& dir, int w, int h, int frames) {
    const auto path = dir / "remote.ser";
    ser::Header hd;
    hd.colorId = ser::ColorId::BayerRGGB;
    hd.width = w;
    hd.height = h;
    ser::Writer wr(path, hd);
    const auto scene = bayerScene(w, h);
    for (int i = 0; i < frames; ++i) wr.writeFrame(scene, 639270383770360000 + i * 200000);
    wr.finish();
    return path;
}

}  // namespace

TEST_CASE("frame codec: a Bayer frame comes back raw, each CFA site in place") {
    REQUIRE(net::jpegAvailable());
    const int w = 320, h = 240;
    const auto px = bayerScene(w, h);
    const FrameInfo fi = frameInfo(w, h, 8);
    for (int scale : {1, 2, 4}) {
        const auto e = net::encodeFrame(px, fi, Bayer::RGGB, {90, scale});
        CHECK(e.header["layout"].toString() == "planes");
        CHECK(e.data.size() < static_cast<qsizetype>(px.size() / 4));   // smooth planes compress well
        const FrameInfo back = net::frameInfoFrom(e.header);
        CHECK(back.roi == fi.roi);
        CHECK(back.bitDepth == 8);
        CHECK(back.sequence == 42);
        CHECK(std::chrono::abs(back.timestamp - fi.timestamp) < 2us);
        std::vector<std::byte> out(back.bytes);
        REQUIRE(net::decodeFrame(e.header, e.data, out));
        int worst = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) worst = std::max(worst, std::abs(sample8(out, w, x, y) - sample8(px, w, x, y)));
        INFO("scale " << scale);
        CHECK(worst <= 3 + 4 * scale);   // the sites differ by 60+ where they meet: a mix-up would show
    }
}

TEST_CASE("frame codec: 16-bit frames keep their range") {
    const int w = 256, h = 128;
    std::vector<std::byte> px(static_cast<std::size_t>(w * h * 2));
    auto at = [&](int x, int y) { return static_cast<uint16_t>(1000 + 39000.0 * std::pow((x + y * 0.5) / (w + h * 0.5), 2)); };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint16_t v = at(x, y);
            px[static_cast<std::size_t>(2 * (y * w + x))] = static_cast<std::byte>(v & 0xff);
            px[static_cast<std::size_t>(2 * (y * w + x) + 1)] = static_cast<std::byte>(v >> 8);
        }
    const auto e = net::encodeFrame(px, frameInfo(w, h, 16), Bayer::None, {90, 1});
    CHECK(e.header["layout"].toString() == "mono");
    CHECK(e.header["black"].toInt() == 1000);
    std::vector<std::byte> out(px.size());
    REQUIRE(net::decodeFrame(e.header, e.data, out));
    double worst = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(2 * (y * w + x));
            const int v = std::to_integer<int>(out[i]) | (std::to_integer<int>(out[i + 1]) << 8);
            worst = std::max(worst, std::abs(v - at(x, y)) / 39000.0);
        }
    CHECK(worst < 0.03);
}

TEST_CASE("messages survive being split anywhere") {
    QByteArray stream = net::packJson(net::Msg::Hello, {{"proto", 1}}) + net::packFrame({{"seq", 7}}, QByteArray(1000, 'x')) +
                        net::packJson(net::Msg::Ack, {{"seq", 7}});
    net::MessageReader r;
    std::vector<net::Message> got;
    for (char c : stream) {
        r.append(QByteArray(1, c));
        while (auto m = r.next()) got.push_back(*m);
    }
    REQUIRE(got.size() == 3);
    CHECK(got[0].type == net::Msg::Hello);
    CHECK(got[0].json()["proto"].toInt() == 1);
    QJsonObject h;
    QByteArray pixels;
    REQUIRE(net::splitFrame(got[1].payload, h, pixels));
    CHECK(h["seq"].toInt() == 7);
    CHECK(pixels == QByteArray(1000, 'x'));
    CHECK(got[2].type == net::Msg::Ack);

    net::MessageReader bad;
    bad.append(QByteArray("\xff\xff\xff\xff\x01", 5));
    CHECK(!bad.next());
    CHECK(bad.error());
}

TEST_CASE("camera state round-trips through JSON") {
    test::TempDir dir;
    SimCamera cam(makeSer(dir, 64, 48, 2));
    const auto s = net::snapshot(cam);
    const auto back = net::cameraStateFromJson(net::cameraStateToJson(s));
    CHECK(back.info.model == s.info.model);
    CHECK(back.info.bayer == Bayer::RGGB);
    CHECK(back.info.sensorWidth == 64);
    CHECK(back.roi == s.roi);
    CHECK(back.bits == s.bits);
    REQUIRE(back.controls.size() == s.controls.size());
    for (std::size_t i = 0; i < s.controls.size(); ++i) {
        CHECK(back.controls[i].name == s.controls[i].name);
        CHECK(back.controls[i].max == s.controls[i].max);
    }
    CHECK(back.values == s.values);
}

namespace {

/// A viewer's conditions: how fast the link and the viewer are, and what frames cost to make and send.
struct Conditions {
    double linkMbps = 1000, viewerFps = 1000, cameraFps = 30;
    double fullFrameKB = 1000;   ///< at quality 75, full size
    double encodeMsFull = 5;     ///< full size
};

/// What the server would see over one second with these settings (a rough JPEG size model).
net::StreamSecond simulate(const Conditions& c, const net::RateControl& rc) {
    const double kb = c.fullFrameKB * (0.3 + 0.9 * rc.quality / 100.0) / (0.3 + 0.9 * 0.75) / (rc.scale * rc.scale);
    const double encMs = c.encodeMsFull / (rc.scale * rc.scale);
    const double target = std::min(30.0, c.cameraFps);
    const double byLink = c.linkMbps * 1e6 / 8 / (kb * 1000);
    const double fps = std::min({target, byLink, c.viewerFps, 1000 / encMs});
    net::StreamSecond s;
    s.sent = static_cast<int>(fps);
    // frames go by unsent when the window is full (link or viewer); a slow encoder just takes fewer
    if (std::min(byLink, c.viewerFps) < target) s.skipped = static_cast<int>(target - s.sent);
    s.encodeMs = encMs * s.sent;
    s.bytes = kb * 1000 * s.sent;
    return s;
}

struct Run {
    int changes = 0, lateChanges = 0;   ///< setting changes, and those in the last minute
    double minFpsLate = 1e9;            ///< worst second in the last minute
};

Run run(const Conditions& c, net::RateControl& rc, int seconds) {
    Run r;
    for (int t = 0; t < seconds; ++t) {
        const auto s = simulate(c, rc);
        if (t >= seconds - 60) r.minFpsLate = std::min(r.minFpsLate, static_cast<double>(s.sent));
        if (rc.update(s, std::min(30.0, c.cameraFps))) ++r.changes, r.lateChanges += t >= seconds - 60;
    }
    return r;
}

}  // namespace

TEST_CASE("rate control: a fast link and viewer get the best picture") {
    net::RateControl rc;
    const auto r = run({}, rc, 60);
    CHECK(rc.quality == 85);
    CHECK(rc.scale == 1);
    CHECK(r.minFpsLate >= 30);
}

TEST_CASE("rate control: a 100 Mbps link keeps full detail and drops frames instead") {
    Conditions c;
    c.linkMbps = 100;   // the NUC's Ethernet: a full-size 1 MB frame gets through about 11 times a second
    net::RateControl rc;
    const auto r = run(c, rc, 600);
    INFO("quality " << rc.quality << ", 1/" << rc.scale << ", " << r.changes << " changes, worst " << r.minFpsLate << " fps");
    CHECK(rc.quality == 85);
    CHECK(rc.scale == 1);
    CHECK(r.changes == 0);
    CHECK(r.minFpsLate >= 10);
}

TEST_CASE("rate control: a link too slow for the frame-rate floor gives up detail, and stays put") {
    Conditions c;
    c.linkMbps = 10;   // full size: about one frame a second
    net::RateControl rc;
    const auto r = run(c, rc, 600);
    INFO("quality " << rc.quality << ", 1/" << rc.scale << ", " << r.lateChanges << " changes in the last minute, worst " << r.minFpsLate << " fps");
    CHECK(rc.scale == 2);              // the best rung that still makes 0.8 x kMinStreamFps
    CHECK(r.minFpsLate >= 0.8 * net::kMinStreamFps);
    CHECK(r.lateChanges == 0);         // no trying a rung the link's capacity says won't fit
}

TEST_CASE("rate control: a slow viewer keeps full detail") {
    Conditions c;
    c.viewerFps = 20;   // a laptop busy drawing a 4K projector
    c.fullFrameKB = 60;
    net::RateControl rc;
    run(c, rc, 300);
    CHECK(rc.quality == 85);
    CHECK(rc.scale == 1);
}

TEST_CASE("rate control: encoding speed only matters below the floor") {
    Conditions c;
    c.encodeMsFull = 120;   // a full QHY533M frame on a small CPU: 8 fps is fine
    c.fullFrameKB = 300;
    net::RateControl rc;
    auto r = run(c, rc, 300);
    CHECK(rc.scale == 1);
    CHECK(rc.quality == 85);
    CHECK(r.minFpsLate >= 8);

    c.encodeMsFull = 400;   // 2.5 fps: shrink (quality wouldn't help)
    net::RateControl slow;
    r = run(c, slow, 300);
    CHECK(slow.scale == 2);
    CHECK(slow.quality == 75);
    CHECK(r.minFpsLate >= 9);
    CHECK(r.lateChanges == 0);
}

TEST_CASE("rate control: the link getting better is noticed") {
    Conditions c;
    c.linkMbps = 10;
    net::RateControl rc;
    run(c, rc, 300);
    const int scaleSlow = rc.scale;
    c.linkMbps = 1000;
    run(c, rc, 120);
    CHECK(scaleSlow > 1);
    CHECK(rc.scale == 1);
    CHECK(rc.quality == 85);
}

TEST_CASE("a client watches and steers a server's camera over loopback") {
    test::TempDir dir;
    SimOptions so;
    so.loop = true;
    SimCamera cam(makeSer(dir, 320, 240, 20), so);
    LatestFrame tap;
    net::RemoteServer server(tap);
    REQUIRE(server.start(QHostAddress::LocalHost));
    server.setCameraName("Sim");
    server.setBayer(Bayer::RGGB);
    server.setState({{"camera", net::cameraStateToJson(net::snapshot(cam))}});
    CaptureOptions opt;
    opt.remote = &tap;
    CaptureSession serverSession(cam, opt);
    serverSession.start();

    {   // discovery finds it
        net::Discovery d;
        d.start(300);
        REQUIRE(waitFor([&] { return !d.servers().empty(); }, 3000));
        CHECK(d.servers()[0].port == server.port());
        CHECK(d.servers()[0].camera == "Sim");
    }

    net::RemoteClient client("127.0.0.1", server.port());
    int controls = 0, formats = 0, records = 0;
    QString lastName;
    double lastValue = 0;
    QObject::connect(&server, &net::RemoteServer::controlRequested, [&](int, const QString& n, double v) { ++controls, lastName = n, lastValue = v; });
    QObject::connect(&server, &net::RemoteServer::formatRequested, [&](int, const QJsonObject& o) { formats += o["width"].toInt() == 640; });
    QObject::connect(&server, &net::RemoteServer::recordRequested, [&](int, bool on) { records += on; });
    client.start();
    REQUIRE(waitFor([&] { return client.link() == net::RemoteClient::Link::Connected && client.hasCamera(); }, 5000));
    CHECK(client.state().info.bayer == Bayer::RGGB);
    CHECK(client.state().roi.width == 320);

    // frames: decoded through RemoteCamera by a live-view session, as the GUI does
    net::RemoteCamera rcam(client);
    LatestFrame view;
    CaptureOptions copt;
    copt.preview = &view;
    CaptureSession clientSession(rcam, copt);
    clientSession.start();
    REQUIRE(waitFor([&] { return clientSession.stats().delivered >= 10; }, 8000));
    const auto* f = [&]() -> const LatestFrame::Frame* {
        const LatestFrame::Frame* got = nullptr;
        waitFor([&] { return got || (got = view.take()) != nullptr; }, 2000);   // (waitFor asks once more at the end)
        return got;
    }();
    REQUIRE(f);
    CHECK(f->info.roi.width == 320);
    CHECK(f->info.bytes == 320u * 240u);
    const auto scene = bayerScene(320, 240);
    CHECK(std::abs(sample8(f->pixels, 320, 101, 51) - sample8(scene, 320, 101, 51)) < 12);   // a B site stays B
    CHECK(server.viewers().size() == 1);
    CHECK(server.viewers()[0].host == QSysInfo::machineHostName());

    // the client asks, the server decides
    rcam.setControl("Gain", 123);   // as the GUI does: from the capture thread, through the camera
    REQUIRE(waitFor([&] { return controls == 1; }, 3000));
    CHECK(lastName == "Gain");
    CHECK(lastValue == 123);
    client.sendFormat({{"width", 640}, {"height", 480}});
    client.sendRecord(true);
    REQUIRE(waitFor([&] { return formats == 1 && records == 1; }, 3000));

    // the server's changes reach the client
    QString changed;
    QObject::connect(&client, &net::RemoteClient::controlChanged, [&](const QString& n, double) { changed = n; });
    server.broadcastControl("Exposure", 20000);
    REQUIRE(waitFor([&] { return changed == "Exposure"; }, 3000));
    CHECK(rcam.control("Exposure") == 20000.0);
    server.sendStatus({{"recording", true}});
    REQUIRE(waitFor([&] { return client.status()["recording"].toBool(); }, 3000));
    CHECK(client.status()["viewers"].toInt() == 1);

    // a dropped link heals itself
    const quint16 port = server.port();
    server.stop();
    REQUIRE(waitFor([&] { return client.link() == net::RemoteClient::Link::Reconnecting; }, 3000));
    REQUIRE(server.start(QHostAddress::LocalHost, port));
    REQUIRE(waitFor([&] { return client.link() == net::RemoteClient::Link::Connected; }, 8000));
    const auto before = clientSession.stats().delivered;
    REQUIRE(waitFor([&] { return clientSession.stats().delivered > before + 5; }, 8000));

    clientSession.stop();
    serverSession.stop();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}

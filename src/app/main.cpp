// slopcap: the capture application.
//   slopcap [--camera SPEC] [--out DIR] [--record] [--screenshot FILE.png] [--quit-after SECONDS]
//   slopcap --camera SPEC --serve        share the camera with other SlopCaps on the network
//   slopcap --connect HOST[:PORT]|auto   watch (and steer) another SlopCap's camera
//   slopcap --mount lx200:COM3           drive the mount without INDI (--list-mounts for the others)
#include "app/main_window.hpp"
#include "app/session_log.hpp"
#include "app/theme.hpp"
#include "camera/open_camera.hpp"
#include "mounts/open_mount.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QSettings>
#include <QSurfaceFormat>
#include <QSysInfo>
#include <QTimer>

#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
/// Make the camera SDKs loadable before anything calls into them (they are delay-loaded). A DLL next to the exe wins
/// (lets you try a newer SDK); otherwise the copy embedded in the exe is written once to
/// %LOCALAPPDATA%\SlopCap\SlopCap\sdk\<content hash>\ and loaded from there. Without either, that camera brand is reported
/// as missing in the camera menu.
void prepareCameraSdks() {
    const QString exeDir = QCoreApplication::applicationDirPath();
    for (const char* name : {"ASICamera2.dll", "qhyccd.dll"}) {
        const QString beside = exeDir + "/" + name;
        if (QFile::exists(beside)) {
            LoadLibraryW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(beside).utf16()));
            continue;
        }
        QFile res(QString(":/sdk/") + name);
        if (!res.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = res.readAll();
        const QString hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(12);
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/sdk/" + hash;
        const QString path = dir + "/" + name;
        if (!QFile::exists(path)) {
            QDir().mkpath(dir);
            QFile out(path + ".part");
            if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()) continue;
            out.close();
            QFile::rename(path + ".part", path);
        }
        LoadLibraryW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()));
    }
}
#endif

}  // namespace

int main(int argc, char** argv) {
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    QApplication::setOrganizationName("SlopCap");
    QApplication::setApplicationName("SlopCap");
    QApplication::setApplicationVersion(SLOPCAP_GIT);
    const QString logFile = slopcap::app::sessionlog::start();
    qInfo().noquote() << "SlopCap" << SLOPCAP_GIT << "· Qt" << qVersion() << "·" << QSysInfo::prettyProductName() << "·"
                      << QSysInfo::machineHostName() << "· args:" << QCoreApplication::arguments().mid(1).join(' ');
    qInfo().noquote() << "log:" << (logFile.isEmpty() ? "(stderr only)" : logFile);
#ifdef _WIN32
    prepareCameraSdks();
#endif
    // Joined however main ends, so the SDK is never mid-call at exit. (Not std::jthread: Xcode 16's libc++ lacks it.)
    struct JoinOnExit {
        std::thread t;
        ~JoinOnExit() { t.join(); }
    } cameraPrep{std::thread([] { slopcap::prepareCameras(); })};
    slopcap::app::theme::apply(app);

    QCommandLineParser args;
    args.setApplicationDescription("Planetary capture");
    args.addHelpOption();
    args.addOption({"camera", "Connect to SPEC at start (asi, qhy:1, sim:FILE.ser, ...).", "SPEC"});
    args.addOption({"out", "Recording folder.", "DIR"});
    args.addOption({"record", "Start recording as soon as the camera is connected."});
    args.addOption({"window", "Window size WIDTHxHEIGHT.", "WxH"});
    args.addOption({"tab", "Open this sidebar tab: capture, focus, collimate or adc.", "NAME"});
    args.addOption({"focuser", "Connect to the INDI server at HOST:PORT for the focuser.", "HOST:PORT"});
    args.addOption({"sweep", "Start a focus sweep once the focuser is ready (with --focuser)."});
    args.addOption({"sweep-range", "Sweep +- STEPS around the current position.", "STEPS"});
    args.addOption({"sweep-step", "Sweep step size.", "STEPS"});
    args.addOption({"sweep-frames", "Frames per sweep position.", "N"});
    args.addOption({"mount", "Drive the mount with SlopCap's own driver: lx200:COM3, onstep:tcp://HOST, nexstar:PORT, synscan:PORT, "
                    "skywatcher:PORT or skywatcher:udp://HOST, synscan-app, sim (see --list-mounts).", "SPEC"});
    args.addOption({"list-mounts", "List the mount specs and this computer's serial ports, then quit."});
    args.addOption({"serve", "Share the camera with other SlopCaps on the network (remote viewing)."});
    args.addOption({"listen", "Share on this address only (implies --serve), e.g. 127.0.0.1.", "ADDRESS"});
    args.addOption({"connect", "Watch another SlopCap's camera: HOST[:PORT], or auto for the first one found on the network.", "HOST"});
    args.addOption({"set", "Set a camera control SECONDS after start (default 3): NAME=VALUE[@SECONDS]; Record=1 / Record=0 "
                    "starts / stops recording. Repeatable.", "SPEC"});
    args.addOption({"fullscreen", "Start in full view: the preview over the whole screen."});
    args.addOption({"screenshot", "Save a picture of the window to FILE just before quitting (with --quit-after).", "FILE"});
    args.addOption({"snapshot", "At SECONDS, save a picture of the window to FILE and print the status line (keeps running). Repeatable.",
                    "SECONDS:FILE"});
    args.addOption({"quit-after", "Quit after SECONDS (for smoke tests and screenshots).", "SECONDS"});
    args.addOption({"settings", "Keep settings in an INI file under DIR instead of the user's (tests; a second instance on one machine).", "DIR"});
    args.process(app);
    if (args.isSet("list-mounts")) {
        std::fputs(slopcap::mounts::mountSpecHelp().c_str(), stdout);
        return 0;
    }
    if (args.isSet("settings")) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, args.value("settings"));
    }

    slopcap::app::MainWindow w;
    if (args.isSet("out")) w.setOutputDir(args.value("out"));
    if (args.isSet("window")) {
        const auto wh = args.value("window").split('x');
        w.resize(wh.value(0).toInt(), wh.value(1).toInt());
    }
    w.show();
    if (args.isSet("camera")) w.connectCamera(args.value("camera"));
    if (args.isSet("record")) w.toggleRecording();
    if (args.isSet("tab")) w.showTab(args.value("tab"));
    if (args.isSet("serve") || args.isSet("listen")) w.startSharing(args.value("listen"));
    if (args.isSet("connect")) {
        const QString c = args.value("connect");
        if (c == "auto") w.connectFirstRemote();
        else if (c.count(':') == 1) w.connectRemote(c.section(':', 0, 0), static_cast<quint16>(c.section(':', 1).toUInt()));
        else w.connectRemote(c);
    }
    for (const QString& spec : args.values("set")) {
        const QString name = spec.section('=', 0, 0).trimmed();
        const QString rest = spec.section('=', 1);
        const double value = rest.section('@', 0, 0).toDouble();
        const double after = rest.contains('@') ? rest.section('@', 1).toDouble() : 3;
        QTimer::singleShot(static_cast<int>(after * 1000), &w, [&w, name, value] { w.setNamedControl(name, value); });
    }
    if (args.isSet("fullscreen")) w.setFullView(true);
    for (const QString& spec : args.values("snapshot")) {
        const double at = spec.section(':', 0, 0).toDouble();
        const QString file = spec.section(':', 1, -1);   // the rest: a Windows path has its own colon
        QTimer::singleShot(static_cast<int>(at * 1000), &w, [&w, file] {
            w.grab().save(file);
            w.printSummary();
        });
    }
    if (args.isSet("focuser"))
        w.startFocuser(args.value("focuser"), args.isSet("sweep"), args.value("sweep-range").toInt(), args.value("sweep-step").toInt(),
                       args.value("sweep-frames").toInt());
    if (args.isSet("mount")) w.openMount(args.value("mount"));
    if (args.isSet("quit-after")) {
        QTimer::singleShot(static_cast<int>(args.value("quit-after").toDouble() * 1000), &app, [&] {
            if (args.isSet("screenshot")) w.grab().save(args.value("screenshot"));
            w.printSummary();   // live numbers
            w.stopCapture();
            w.printSummary();   // what was saved, if recording
            w.close();
        });
    }
    return app.exec();
}

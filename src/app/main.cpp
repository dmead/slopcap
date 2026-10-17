// slopcap: the capture application.
//   slopcap [--camera SPEC] [--out DIR] [--record] [--screenshot FILE.png] [--quit-after SECONDS]
#include "app/main_window.hpp"
#include "app/session_log.hpp"
#include "app/theme.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QSurfaceFormat>
#include <QSysInfo>
#include <QTimer>

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
    const QString logFile = slopcap::app::sessionlog::start();
    qInfo().noquote() << "SlopCap" << SLOPCAP_GIT << "· Qt" << qVersion() << "·" << QSysInfo::prettyProductName() << "·"
                      << QSysInfo::machineHostName() << "· args:" << QCoreApplication::arguments().mid(1).join(' ');
    qInfo().noquote() << "log:" << (logFile.isEmpty() ? "(stderr only)" : logFile);
#ifdef _WIN32
    prepareCameraSdks();
#endif
    slopcap::app::theme::apply(app);

    QCommandLineParser args;
    args.setApplicationDescription("Planetary capture");
    args.addHelpOption();
    args.addOption({"camera", "Connect to SPEC at start (asi, qhy:1, sim:FILE.ser, ...).", "SPEC"});
    args.addOption({"out", "Recording folder.", "DIR"});
    args.addOption({"record", "Start recording as soon as the camera is connected."});
    args.addOption({"window", "Window size WIDTHxHEIGHT.", "WxH"});
    args.addOption({"tab", "Open this sidebar tab: capture, focus or collimate.", "NAME"});
    args.addOption({"focuser", "Connect to the INDI server at HOST:PORT for the focuser.", "HOST:PORT"});
    args.addOption({"sweep", "Start a focus sweep once the focuser is ready (with --focuser)."});
    args.addOption({"sweep-range", "Sweep +- STEPS around the current position.", "STEPS"});
    args.addOption({"sweep-step", "Sweep step size.", "STEPS"});
    args.addOption({"sweep-frames", "Frames per sweep position.", "N"});
    args.addOption({"screenshot", "Save a picture of the window to FILE just before quitting (with --quit-after).", "FILE"});
    args.addOption({"quit-after", "Quit after SECONDS (for smoke tests and screenshots).", "SECONDS"});
    args.process(app);

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
    if (args.isSet("focuser"))
        w.startFocuser(args.value("focuser"), args.isSet("sweep"), args.value("sweep-range").toInt(), args.value("sweep-step").toInt(),
                       args.value("sweep-frames").toInt());
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

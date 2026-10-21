// The application tests' main: a real QApplication with SlopCap's theme and session log, settings and files kept in a
// temporary home, and non-native dialogs (tests/app_harness.cpp answers them). Exits 4 (skipped) without a screen.
#include "app/session_log.hpp"
#include "app/theme.hpp"
#include "app_harness.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSurfaceFormat>

#include <catch2/catch_session.hpp>

#include <cstdio>

int main(int argc, char** argv) {
    // Everything the application keeps (settings, logs, the default recording folder, sweep files) goes under a
    // temporary home; programs the tests start inherit it.
    slopcap::test::TempDir home;
#ifndef _WIN32
    qputenv("HOME", home.q().toUtf8());
    for (const char* v : {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"}) qputenv(v, (home.q() + "/" + v).toUtf8());
#endif
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    if (!QGuiApplication::primaryScreen()) {
        std::fprintf(stderr, "no screen: the application tests need a desktop session (or Xvfb)\n");
        return 4;   // ctest's SKIP_RETURN_CODE
    }
    QApplication::setOrganizationName("SlopCap");
    QApplication::setApplicationName("SlopCap-tests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, home.q() + "/settings");

    // the session log, into a folder already holding more than the 100 logs it keeps
    const QString logs = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
    QDir().mkpath(logs);
    for (int i = 0; i < 103; ++i) {
        QFile f(logs + QString("/slopcap-2000-01-01-%1.log").arg(i, 6, 10, QChar('0')));
        if (!f.open(QIODevice::WriteOnly)) break;
    }
    slopcap::app::sessionlog::start();
    slopcap::app::theme::apply(app);
    slopcap::test::Modals::get();
    return Catch::Session().run(argc, argv);
}

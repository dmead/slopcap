// The slopcap program itself (src/app/main.cpp), run with its command-line options: a recording session with
// snapshots, one copy sharing while others watch, a focus sweep at start, --help.
#include "app_harness.hpp"

#include <QDir>
#include <QFile>
#include <QRegularExpression>

#include <catch2/catch_test_macros.hpp>

using namespace slopcap::test;

namespace {

struct Run {
    int code = -1;
    QString out, err;
};

/// Run the program to its end (or kill it after `ms`).
Run runProgram(const QStringList& args, int ms = 60000) {
    QProcess p;
    p.start(SLOPCAP_PROGRAM, args);
    Run r;
    if (!p.waitForStarted(10000)) return r;
    if (!p.waitForFinished(ms)) {
        p.kill();
        p.waitForFinished(3000);
    }
    r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.out = QString::fromUtf8(p.readAllStandardOutput());
    r.err = QString::fromUtf8(p.readAllStandardError());
    return r;
}

}  // namespace

TEST_CASE("program: a camera, recording, a tab, settings changes and snapshots from the command line", "[app][program]") {
    TempDir dir;
    const QString ser = makeSer(dir.file("mars.ser"));
    const auto r = runProgram({"--settings", dir.file("settings"), "--camera", "sim-loop:" + ser, "--out", dir.file("out"), "--record",
                            "--window", "900x700", "--tab", "focus", "--set", "Gain=150@0.5", "--set", "Record=0@1.5", "--snapshot",
                            "1:" + dir.file("snap.png"), "--screenshot", dir.file("shot.png"), "--quit-after", "2.5"});
    CHECK(r.code == 0);
    CHECK(QFile::exists(dir.file("snap.png")));
    CHECK(QFile::exists(dir.file("shot.png")));
    CHECK(r.out.contains("Gain=150"));
    CHECK(r.err.contains("record: start"));
    CHECK(QDir(dir.file("out")).entryList({"*.ser"}).size() == 1);
    CHECK(QDir(dir.file("settings")).exists());
}

TEST_CASE("program: one copy shares, others watch (--listen, --connect HOST:PORT, HOST and auto)", "[app][program]") {
    TempDir dir;
    const QString ser = makeSer(dir.file("saturn.ser"));
    QProcess server;
    server.start(SLOPCAP_PROGRAM, {"--settings", dir.file("a"), "--camera", "sim-loop:" + ser, "--out", dir.file("rec"), "--listen",
                                    "127.0.0.1", "--quit-after", "14"});
    REQUIRE(server.waitForStarted(10000));
    QString log;
    quint16 port = 0;
    const QRegularExpression sharing("Sharing on port (\\d+)");
    REQUIRE(waitFor(
        [&] {
            log += QString::fromUtf8(server.readAllStandardError());
            const auto mt = sharing.match(log);
            if (mt.hasMatch()) port = static_cast<quint16>(mt.captured(1).toUInt());
            return port != 0;
        },
        15000));

    const auto watch = runProgram({"--settings", dir.file("b"), "--connect", QString("127.0.0.1:%1").arg(port), "--tab", "collimate", "--fullscreen",
                                "--set", "Gain=99@1.5", "--set", "Record=1@2", "--set", "Record=0@3.5", "--quit-after", "4.5"});
    CHECK(watch.code == 0);
    CHECK(watch.out.contains("Watching"));
    const auto first = runProgram({"--settings", dir.file("c"), "--connect", "auto", "--quit-after", "3"});
    CHECK(first.code == 0);
    const auto byHost = runProgram({"--settings", dir.file("d"), "--connect", "127.0.0.1", "--quit-after", "2"});
    CHECK(byHost.code == 0);

    REQUIRE(server.waitForFinished(30000));
    CHECK(server.exitCode() == 0);
    log += QString::fromUtf8(server.readAllStandardError());
    CHECK(log.contains("asked by a viewer"));
    CHECK(QDir(dir.file("rec")).entryList({"*.ser"}).size() == 1);
}

TEST_CASE("program: a focus sweep at start, and --help", "[app][program]") {
    TempDir dir;
    const auto help = runProgram({"--help"});
    CHECK(help.code == 0);
    CHECK(help.out.contains("Planetary capture"));
    SimServer indi({"indi_simulator_focus"});
    if (!indi.ok) SKIP("indiserver / indi_simulator_focus not installed");
    const QString ser = makeSer(dir.file("venus.ser"));
    const auto r = runProgram({"--settings", dir.file("s"), "--camera", "sim-loop:" + ser, "--out", dir.file("out"), "--focuser", indi.address(),
                            "--sweep", "--sweep-range", "40", "--sweep-step", "20", "--sweep-frames", "5", "--quit-after", "8"});
    CHECK(r.code == 0);
    CHECK(r.err.contains("sweep: started"));
}

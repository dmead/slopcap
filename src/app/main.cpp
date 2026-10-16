// slopcap: the capture application.
//   slopcap [--camera SPEC] [--out DIR] [--record] [--screenshot FILE.png] [--quit-after SECONDS]
#include "app/main_window.hpp"
#include "app/theme.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QSurfaceFormat>



int main(int argc, char** argv) {
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    QApplication::setOrganizationName("SlopCap");
    QApplication::setApplicationName("SlopCap");
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

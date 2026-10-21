// Shared by the application tests (tests/test_app*.cpp): waiting on the event loop, answering the dialogs and menus
// the code opens (they're modal: exec() doesn't return until something closes them), INDI's simulators on a private
// server, test recordings, and MainWindowProbe, which reaches the window's widgets the way a user would.
#pragma once

#include "app/main_window.hpp"
#include "app/settings_dialog.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>

class QAbstractButton;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QShortcut;
class QSlider;
class QStackedWidget;

namespace slopcap::test {

/// Spin the event loop until `done` or the timeout.
template <typename F>
bool waitFor(F done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}
inline void pump(int ms) { waitFor([] { return false; }, ms); }

/// Handles the next modal dialogs and popup menus, in order. Each expectation is used by the next dialog or menu
/// that opens; one that opens with nothing expected is closed (and listed in unexpected()) so a test never hangs.
class Modals : public QObject {
public:
    using Step = std::function<void(QWidget*)>;
    static Modals& get();

    void expect(const QString& what, Step step);
    /// Menu: trigger the first item containing `text` ("" or no such item: close it).
    void pickMenu(const QString& text);
    void answer(QMessageBox::StandardButton b);
    /// File dialog: choose `path` ("" = cancel).
    void chooseFile(const QString& path);
    /// Input dialog: enter `text` ("" = cancel).
    void enterText(const QString& text);
    void settings(std::function<void(app::SettingsDialog&)> edit, bool accept = true);

    bool done(int ms = 10000);   ///< wait until every expectation was used
    int pending() const { return static_cast<int>(steps_.size()); }
    const QStringList& seen() const { return seen_; }   ///< every dialog / menu handled: "menu: a | b", "message: text"
    QString lastSeen() const { return seen_.isEmpty() ? QString() : seen_.back(); }
    QStringList takeUnexpected();
    void clear();

private:
    Modals();
    void poll();
    struct Pending {
        QString what;
        Step step;
    };
    std::deque<Pending> steps_;
    QPointer<QWidget> handling_;
    QElapsedTimer unexpectedSince_;
    QStringList seen_, unexpected_;
    QTimer timer_;
};

/// indiserver with INDI's simulator drivers on a random port; ok is false when INDI isn't installed.
struct SimServer {
    QProcess proc;
    quint16 port = 0;
    bool ok = false;
    explicit SimServer(const QStringList& drivers);
    ~SimServer();
    QString address() const { return QString("localhost:%1").arg(port); }
};

/// A temporary directory, removed afterwards.
struct TempDir {
    std::filesystem::path path;
    TempDir();
    ~TempDir();
    QString q() const { return QString::fromStdString(path.string()); }
    QString file(const QString& name) const { return q() + "/" + name; }
};

/// A SER recording of a disc on a dark sky (RGGB, 8-bit, 25 fps), for sim: cameras.
QString makeSer(const QString& path, int width = 320, int height = 240, int frames = 50);

/// Fresh settings (an INI file in a temporary folder, set up by app_main.cpp), with nothing in them.
void clearSettings();

/// The ZWO fake (tests/fakes) as a small camera for the window tests; returns false when this build has no fake.
bool useFakeAsi(int width = 640, int height = 480);
/// Frames whose per-channel sharpness depends on *position (a focuser): R, G and B are sharpest at best[0..2].
void drawFocusFrames(const std::atomic<int>* position, std::array<int, 3> best);
/// A defocused star: a ring with the secondary's shadow `offset` px off centre (collimation).
void drawDonut(double offset);

/// The child of `root` with this text (else the first whose text contains it).
QPushButton* button(QWidget* root, const QString& text);
QCheckBox* checkBox(QWidget* root, const QString& text);
QLabel* labelWith(QWidget* root, const QString& text);

}  // namespace slopcap::test

namespace slopcap::app {

/// The window's insides, by reference (so they follow the window's changes).
struct MainWindowProbe {
    MainWindow& w;
    explicit MainWindowProbe(MainWindow& win) : w(win) {}

    std::unique_ptr<ICamera>& cam{w.cam_};
    std::unique_ptr<CaptureSession>& session{w.session_};
    bool& recording{w.recording_};
    QString& recordingPath{w.recordingPath_};
    QString& outDir{w.outDir_};
    Settings& settings{w.settings_};
    FocusController*& focus{w.focus_};
    devices::Mount*& mount{w.mount_};
    QString& autoName{w.autoName_};
    MainWindow::Mode& mode{w.mode_};
    net::RemoteServer*& server{w.server_};
    net::RemoteClient*& remote{w.remote_};
    net::Discovery*& discovery{w.discovery_};
    bool& fullView{w.fullView_};
    FullViewBar*& fullBar{w.fullBar_};
    QTimer& fullIdle{w.fullIdle_};
    QTimer& statsTimer{w.statsTimer_};

    QButtonGroup*& modes{w.modes_};
    QComboBox*& target{w.target_};
    QComboBox*& filter{w.filter_};
    QPushButton*& cameraPill{w.cameraPill_};
    QPushButton*& remotePill{w.remotePill_};
    QLabel*& headerInfo{w.headerInfo_};
    preview::PreviewWidget*& preview{w.preview_};
    SweepPanel*& sweepPanel{w.sweepPanel_};
    FocusChartPanel*& chartPanel{w.chartPanel_};
    FocusResultsPanel*& resultsPanel{w.resultsPanel_};
    CollimationPanel*& collim{w.collim_};
    MountSection*& mountSection{w.mountSection_};
    QStackedWidget*& left{w.left_};

    QLabel*& noCamera{w.noCamera_};
    QSlider*& gain{w.gain_};
    QLabel*& gainValue{w.gainValue_};
    QWidget*& gainPresets{w.gainPresets_};
    QSlider*& exposure{w.exposure_};
    QLabel*& expValue{w.expValue_};
    QLabel*& roiSize{w.roiSize_};
    QComboBox*& roiPreset{w.roiPreset_};
    QPushButton*& roiCentre{w.roiCentreBtn_};
    QPushButton*& roiMax{w.roiMax_};
    QButtonGroup*& bits{w.bits_};
    QCheckBox*& highSpeed{w.highSpeed_};
    QComboBox*& readout{w.readout_};
    QPushButton*& allControlsBtn{w.allControlsBtn_};
    std::map<std::string, QDoubleSpinBox*>& controlBoxes{w.controlBoxes_};
    QComboBox*& limit{w.limit_};
    QPushButton*& folder{w.folder_};
    QLabel*& nextFile{w.nextFile_};
    QPushButton*& record{w.record_};
    QLabel*& tCentre{w.tCentre_};
    QLabel*& tAuto{w.tAuto_};
    QButtonGroup*& zoom{w.zoom_};
    QButtonGroup*& stretch{w.stretch_};
    QCheckBox*& targetBox{w.targetBox_};
    QLabel*& recBadge{w.recBadge_};
    QLabel*& remoteBadge{w.remoteBadge_};
    QLabel*& histInfo{w.histInfo_};
    QLabel*& sFrame{w.sFrame_};
    QLabel*& fState{w.fState_};
    QLabel*& fFps{w.fFps_};
    QLabel*& fRemote{w.fRemote_};
    QLabel*& fMessage{w.fMessage_};

    void updateStats() { w.updateStats(); }
    QString targetName() const { return w.targetName(); }
    /// Wait until the preview has shown a frame from the current session.
    bool waitForFrames(int frames = 3, int ms = 15000);
};

}  // namespace slopcap::app

// SlopCap's main window, laid out as the "SlopCap UI" design canvas: a header (modes, target, filter, camera,
// settings), three columns whose side panels change with the mode, and a status footer.
//   Capture    camera + capture settings | live preview with toolbar and histogram | target, focuser, mount, raw histogram
//   Focus      sweep settings | per-channel focus curves | best focus per channel, strategy, go to
//   Collimate  method | live preview with ring/shadow overlay | offset readouts
//   Align, Sequence: in the design, not built yet (shown disabled)
#pragma once

#include "app/settings_dialog.hpp"
#include "camera/camera.hpp"
#include "pipeline/capture_session.hpp"
#include "preview/latest_frame.hpp"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMainWindow>
#include <QPoint>
#include <QTimer>

#include <map>
#include <memory>
#include <optional>
#include <string>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QFrame;
class QLabel;
class QPushButton;
class QScreen;
class QShortcut;
class QSlider;
class QStackedWidget;
class QVBoxLayout;

namespace slopcap::preview {
class HistogramWidget;
class PreviewWidget;
}
namespace slopcap::devices {
class Mount;
}
namespace slopcap::net {
class Discovery;
class RemoteClient;
class RemoteServer;
}

namespace slopcap::app {

class CollimationPanel;
class FocusChartPanel;
class FocusController;
class FocusResultsPanel;
class FullViewBar;
class MountSection;
class RawHistogramSection;
class SweepPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    enum Mode { Capture = 0, Focus = 1, Collimate = 2, Align = 3, Sequence = 4 };

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void connectCamera(const QString& spec);
    void setOutputDir(const QString& dir);
    void toggleRecording();
    void stopCapture() { stopSession(); }
    void printSummary() const;                   ///< status to stdout (smoke tests)
    void showTab(const QString& name);           ///< capture / focus / collimate
    void setMode(Mode m);
    /// Connect the focuser; optionally start a sweep around its position once it's ready (0 = default).
    void startFocuser(const QString& hostPort, bool sweep, int range, int step, int frames);
    void setNamedControl(const QString& name, double value);   ///< as if set in the UI (here, or on the server)

    // remote viewing (src/net): share this camera, or watch and steer another SlopCap's
    void startSharing(const QString& address = {});   ///< empty: every interface
    void stopSharing();
    void connectRemote(const QString& host, quint16 port = 0);
    void connectFirstRemote();                          ///< the first server discovery finds
    void disconnectRemote();
    /// The preview over a whole screen with a reduced control bar (nullptr: the window's current screen).
    void setFullView(bool on, QScreen* screen = nullptr);

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    QWidget* buildHeader();
    QWidget* buildFooter();
    QWidget* buildCaptureLeft();
    QWidget* buildCaptureRight();
    QWidget* buildPreviewPage();
    void showCameraMenu();
    void populateCameraUi();
    void populateControls();
    void disconnectCamera();
    void restoreCameraSettings();
    void saveSettings() const;
    void startSession(bool record);
    void stopSession();
    void applyFormat(std::optional<QSize> size = std::nullopt, std::optional<int> bits = std::nullopt, std::optional<int> readout = std::nullopt);
    void setControl(const std::string& name, double value);
    void syncControlWidgets(const std::string& name, double value);
    void centreRoiOn(QPointF imagePixel);
    void updateStats();
    void updateRoiInfo();
    void updateTargetInfo();
    void logDevices();                           ///< INDI, focuser, mount and sweep events into the session log
    bool isAutoTarget() const;
    QString targetName() const;                  ///< what files and sidecars are named after (Auto: what was identified)
    void runAutoTarget(const LatestFrame::Frame& f);
    void updateAutoTarget();
    void updateFocusMode();
    void writeSidecar(const QString& serPath, const CaptureStats& s, const QDateTime& mid) const;
    QString fileStem(const QDateTime& utc) const;
    QString cameraKey() const;
    void message(const QString& text);
    void applySettings(const Settings& s);
    // remote
    void showRemoteMenu();
    void ensureDiscovery();
    void onRemoteState();
    QJsonObject remoteCameraState() const;   ///< server: the camera as viewers see it, with the UI's current values
    QJsonObject remoteStatus() const;
    void updateRemoteUi();
    void updateClientStats();
    bool isRemote() const { return remote_ != nullptr; }
    // full view
    void showFullViewMenu();
    void syncFullBar();

    // state
    std::unique_ptr<ICamera> cam_;
    QString camSpec_;
    std::unique_ptr<CaptureSession> session_;
    LatestFrame tap_;
    bool recording_ = false;
    QString recordingPath_, outDir_;
    QDateTime recStartUtc_;
    std::map<std::string, double> recControls_;
    std::optional<QPoint> roiCentre_;
    Settings settings_;
    FocusController* focus_ = nullptr;
    devices::Mount* mount_ = nullptr;
    MountSection* mountSection_ = nullptr;
    RawHistogramSection* rawHist_ = nullptr;
    QElapsedTimer rawHistClock_;
    int statsLogTick_ = 0;   // stats lines go to the session log every 40 ticks
    // the automatic target (Target: Auto)
    QElapsedTimer identifyClock_;
    QString autoName_, pendingAutoName_, autoDetail_, autoNotes_;
    bool pendingSweep_ = false;
    int pendingRange_ = 0, pendingStep_ = 0, pendingFrames_ = 0;
    Mode mode_ = Capture;

    // remote viewing
    LatestFrame remoteTap_;                 ///< frames for the stream (server)
    net::RemoteServer* server_ = nullptr;   ///< sharing this camera
    net::RemoteClient* remote_ = nullptr;   ///< watching another SlopCap's camera (cam_ is then a RemoteCamera)
    net::Discovery* discovery_ = nullptr;
    QJsonObject cameraState_;               ///< server: the camera at its last populateCameraUi
    int controlOrigin_ = 0;                 ///< server: the viewer whose request is being applied (not told again)
    bool roiCentreDirty_ = false;           ///< client: send the ROI centre with the next format request
    bool connectFirst_ = false;
    std::size_t lastViewers_ = 0;
    int statusTick_ = 0;

    // full view
    bool fullView_ = false, wasMaximized_ = false;
    QByteArray normalGeometry_;
    FullViewBar* fullBar_ = nullptr;
    QTimer fullIdle_;
    QShortcut* escape_ = nullptr;

    // header
    QWidget* header_ = nullptr;
    QButtonGroup* modes_ = nullptr;
    QComboBox* target_ = nullptr;
    QComboBox* filter_ = nullptr;
    QPushButton* cameraPill_ = nullptr;
    QLabel* cameraDot_ = nullptr;
    QPushButton* remotePill_ = nullptr;
    QLabel* headerInfo_ = nullptr;

    // columns
    QStackedWidget* left_ = nullptr;
    QStackedWidget* centre_ = nullptr;
    QStackedWidget* right_ = nullptr;
    preview::PreviewWidget* preview_ = nullptr;
    SweepPanel* sweepPanel_ = nullptr;
    FocusChartPanel* chartPanel_ = nullptr;
    FocusResultsPanel* resultsPanel_ = nullptr;
    CollimationPanel* collim_ = nullptr;

    // capture left
    QWidget* cameraBox_ = nullptr;
    QLabel* noCamera_ = nullptr;
    QLabel* gainValue_ = nullptr;
    QSlider* gain_ = nullptr;
    QWidget* gainPresets_ = nullptr;
    QLabel* expValue_ = nullptr;
    QSlider* exposure_ = nullptr;
    QLabel *expMaxFps_ = nullptr, *expRange_ = nullptr;
    double expMin_ = 32, expMax_ = 1e6;
    QLabel* roiSize_ = nullptr;
    QLabel* roiOffset_ = nullptr;
    QComboBox* roiPreset_ = nullptr;
    QPushButton* roiCentreBtn_ = nullptr;
    QPushButton* roiMax_ = nullptr;
    QWidget* bitsBox_ = nullptr;
    QButtonGroup* bits_ = nullptr;
    QCheckBox* highSpeed_ = nullptr;
    QComboBox* readout_ = nullptr;
    QPushButton* allControlsBtn_ = nullptr;
    QFrame* allControls_ = nullptr;
    QFormLayout* controlsForm_ = nullptr;
    std::map<std::string, QDoubleSpinBox*> controlBoxes_;
    QComboBox* limit_ = nullptr;
    QPushButton* folder_ = nullptr;
    QLabel* nextFile_ = nullptr;
    QPushButton* record_ = nullptr;

    // capture right
    QLabel *tCentre_ = nullptr, *tMargin_ = nullptr, *tDisc_ = nullptr, *tDisp_ = nullptr;
    QLabel* tAuto_ = nullptr;

    // preview page
    QWidget *toolbar_ = nullptr, *strip_ = nullptr, *previewHolder_ = nullptr;
    QButtonGroup* zoom_ = nullptr;
    QButtonGroup* stretch_ = nullptr;
    QCheckBox* targetBox_ = nullptr;
    QLabel* recBadge_ = nullptr;
    QLabel* remoteBadge_ = nullptr;
    preview::HistogramWidget* histogram_ = nullptr;
    QLabel* histInfo_ = nullptr;
    QLabel *sSharp_ = nullptr, *sDisp_ = nullptr, *sFrame_ = nullptr;

    // footer
    QWidget* footer_ = nullptr;
    QLabel* fRemote_ = nullptr;
    QLabel *fState_ = nullptr, *fFps_ = nullptr, *fDropped_ = nullptr, *fRing_ = nullptr, *fDisk_ = nullptr, *fFrames_ = nullptr;
    QLabel* fMessage_ = nullptr;
    QLabel* fDisplay_ = nullptr;
    QTimer statsTimer_, messageTimer_;
};

}  // namespace slopcap::app

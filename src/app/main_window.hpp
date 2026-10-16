// SlopCap's main window (M2): pick and connect a camera, live view on the GPU with histogram and levels, camera
// settings that apply mid-stream, and record to SER with live throughput / drop counters.
#pragma once

#include "camera/camera.hpp"
#include "pipeline/capture_session.hpp"
#include "preview/latest_frame.hpp"

#include <QMainWindow>
#include <QTimer>

#include <map>
#include <memory>
#include <string>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QVBoxLayout;

namespace slopcap::preview {
class HistogramWidget;
class PreviewWidget;
struct Levels;
}

namespace slopcap::app {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void connectCamera(const QString& spec);   ///< also what --camera does
    void setOutputDir(const QString& dir);
    void toggleRecording();
    void stopCapture() { stopSession(); }      ///< stop live view / recording (keeps the camera connected)
    void printSummary() const;                  ///< status line and stats to stdout (smoke tests)

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    QWidget* buildSidebar();
    void refreshCameraList();
    void disconnectCamera();
    void populateCameraControls();
    void startSession(bool record);
    void stopSession();
    void applyFormat();                          ///< ROI / bit depth / readout changed: restart the stream
    void setControl(const std::string& name, double value);
    void levelsFromSliders();
    void showLevels(const preview::Levels& lv);
    void updateStats();
    void setConfigEnabled(bool on);

    std::unique_ptr<ICamera> cam_;
    std::unique_ptr<CaptureSession> session_;
    LatestFrame tap_;
    bool recording_ = false;
    QString recordingPath_;

    preview::PreviewWidget* preview_ = nullptr;
    preview::HistogramWidget* histogram_ = nullptr;
    QComboBox* cameraSpec_ = nullptr;
    QPushButton* connect_ = nullptr;
    QLabel* cameraName_ = nullptr;
    QComboBox* roi_ = nullptr;
    QComboBox* bits_ = nullptr;
    QComboBox* readout_ = nullptr;
    QFormLayout* controlsForm_ = nullptr;
    QVBoxLayout* landmarks_ = nullptr;
    std::map<std::string, QDoubleSpinBox*> controlBoxes_;
    QCheckBox* autoStretch_ = nullptr;
    QSlider* black_ = nullptr;
    QSlider* white_ = nullptr;
    QSlider* gamma_ = nullptr;
    QLineEdit* outDir_ = nullptr;
    QSpinBox* limitSeconds_ = nullptr;
    QPushButton* record_ = nullptr;
    QLabel* statusLeft_ = nullptr;
    QLabel* statusRight_ = nullptr;
    QTimer statsTimer_;
};

}  // namespace slopcap::app

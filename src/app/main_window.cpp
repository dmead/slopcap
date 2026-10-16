#include "app/main_window.hpp"

#include "app/collimation_panel.hpp"
#include "app/focus_panel.hpp"
#include "camera/open_camera.hpp"
#include "preview/histogram_widget.hpp"
#include "preview/preview_widget.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTabWidget>
#include <QTextStream>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QShortcut>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStatusBar>
#include <QVBoxLayout>

#include <cmath>
#include <cstdio>
#include <format>

namespace slopcap::app {

namespace {

constexpr int kSliderMax = 1000;

const char* landmarkName(GainLandmark::Kind k) {
    switch (k) {
        case GainLandmark::Kind::Unity: return "Unity gain";
        case GainLandmark::Kind::LowestReadNoise: return "Lowest read noise";
        case GainLandmark::Kind::HighestDynamicRange: return "Highest dynamic range";
        case GainLandmark::Kind::HcgSwitch: return "HCG switch";
        case GainLandmark::Kind::Preset: return "Preset";
        case GainLandmark::Kind::VendorDefault: return "Vendor default";
    }
    return "";
}

QString sanitize(const std::string& s) {
    QString out;
    for (QChar c : QString::fromStdString(s)) out += (c.isLetterOrNumber() || c == '-') ? c : QChar('_');
    return out;
}

/// WinJUPOS time stamp: YYYY-MM-DD-HHMM_T, T = tenths of a minute.
QString winjupos(const QDateTime& utc) {
    return utc.toString("yyyy-MM-dd-HHmm") + "_" + QString::number(utc.time().second() / 6);
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("SlopCap");
    preview_ = new preview::PreviewWidget;
    auto* split = new QSplitter;
    split->addWidget(preview_);
    split->addWidget(buildSidebar());
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    split->setSizes({1020, 420});
    setCentralWidget(split);

    statusLeft_ = new QLabel("No camera");
    statusRight_ = new QLabel;
    statusBar()->addWidget(statusLeft_, 1);
    statusBar()->addPermanentWidget(statusRight_);

    connect(preview_, &preview::PreviewWidget::frameShown, this,
            [this](const std::array<uint32_t, 256>& h, const preview::Levels& lv, const FrameInfo&) {
                histogram_->setData(h, lv);
                if (preview_->autoStretch()) showLevels(lv);
                uint64_t total = 0;
                int top = 0;
                for (int b = 0; b < 256; ++b)
                    if (h[static_cast<std::size_t>(b)]) total += h[static_cast<std::size_t>(b)], top = b;
                const double clipped = total ? 100.0 * h[255] / static_cast<double>(total) : 0;
                histInfo_->setText(QString("max %1 %  ·  clipped %2 %").arg(top * 100 / 255).arg(clipped, 0, 'f', 2));
                histInfo_->setStyleSheet(clipped > 0.1 ? "color: #f06a5a" : "");
            });
    connect(preview_, &preview::PreviewWidget::rawFrame, this, [this](const LatestFrame::Frame& f) {
        if (!cam_) return;
        focus_->onFrame(f, cam_->info().bayer);
        collim_->onFrame(f, cam_->info().bayer);
    });
    connect(preview_, &preview::PreviewWidget::centreRequested, this, &MainWindow::centreRoiOn);
    connect(&statsTimer_, &QTimer::timeout, this, &MainWindow::updateStats);
    statsTimer_.start(250);
    refreshCameraList();
    setConfigEnabled(false);
    resize(1440, 900);

    QSettings st;
    if (st.contains("window/geometry")) restoreGeometry(st.value("window/geometry").toByteArray());
    if (st.contains("record/dir")) outDir_->setText(st.value("record/dir").toString());
    target_->setText(st.value("record/target", "Saturn").toString());
    limitSeconds_->setValue(st.value("record/limit", 0).toInt());
    if (st.contains("camera/last")) cameraSpec_->setEditText(st.value("camera/last").toString());
    focus_->setBacklash(st.value("focuser/backlash", 600).toInt(), st.value("focuser/outward", true).toBool());
    focus_->setMetric(st.value("focus/metric", "gradient").toString());
    focus_->setOutputDir(outDir_->text());
    connect(outDir_, &QLineEdit::textChanged, focus_, &FocusPanel::setOutputDir);
    if (st.value("focuser/connected", false).toBool()) focus_->connectFocuser(st.value("focuser/server", "localhost:7624").toString());
}

MainWindow::~MainWindow() { stopSession(); }

void MainWindow::saveSettings() const {
    QSettings st;
    st.setValue("window/geometry", saveGeometry());
    st.setValue("record/dir", outDir_->text());
    st.setValue("record/target", target_->text());
    st.setValue("record/limit", limitSeconds_->value());
    st.setValue("camera/last", cameraSpec_->currentData().isValid() ? cameraSpec_->currentData().toString() : cameraSpec_->currentText());
    st.setValue("focuser/server", focus_->focuserServer());
    st.setValue("focuser/connected", focus_->focuserPosition() >= 0);
    st.setValue("focuser/backlash", focus_->backlash());
    st.setValue("focuser/outward", focus_->approachOutward());
    st.setValue("focus/metric", focus_->metricName());
    if (!cam_) return;
    st.beginGroup("cameras/" + cameraKey());
    const Roi r = cam_->roi();
    st.setValue("roi", QRect(r.x, r.y, r.width, r.height));
    st.setValue("bits", cam_->bitDepth());
    st.setValue("readout", cam_->readoutMode());
    for (const auto& [name, box] : controlBoxes_) st.setValue("controls/" + QString::fromStdString(name), box->value());
    st.endGroup();
}

QString MainWindow::cameraKey() const {
    const auto& info = cam_->info();
    return sanitize(info.vendor + "-" + info.model + "-" + info.serial);
}

void MainWindow::restoreCameraSettings() {
    QSettings st;
    st.beginGroup("cameras/" + cameraKey());
    try {
        if (st.contains("readout")) cam_->setReadoutMode(st.value("readout").toInt());
        if (st.contains("bits")) cam_->setBitDepth(st.value("bits").toInt());
        if (st.contains("roi")) {
            const QRect r = st.value("roi").toRect();
            cam_->setRoi({r.x(), r.y(), r.width(), r.height(), 1});
            const auto& info = cam_->info();
            if (r.width() < info.sensorWidth || r.height() < info.sensorHeight) roiCentre_ = r.center();
        }
    } catch (const std::exception& e) {
        statusLeft_->setText(QString("Saved format not accepted: %1").arg(e.what()));
    }
    st.beginGroup("controls");
    for (const auto& key : st.childKeys()) {
        try {
            cam_->setControl(key.toStdString(), st.value(key).toDouble());
        } catch (const std::exception&) {
            // controls differ between drivers / SDK versions: skip what this one doesn't take
        }
    }
}

void MainWindow::centreRoiOn(QPointF img) {
    if (!cam_ || recording_) return;
    const Roi cur = cam_->roi();
    roiCentre_ = QPoint(cur.x + static_cast<int>(img.x()), cur.y + static_cast<int>(img.y()));
    const auto& info = cam_->info();
    if (cur.width >= info.sensorWidth && cur.height >= info.sensorHeight && roi_->count() > 1) {
        // full frame can't move: switch to a planetary-sized ROI
        int pick = 1;
        for (int i = 1; i < roi_->count(); ++i)
            if (roi_->itemData(i).toSize().width() <= 1024) {
                pick = i;
                break;
            }
        const QSignalBlocker b(roi_);
        roi_->setCurrentIndex(pick);
    }
    applyFormat();
}

void MainWindow::writeSidecar(const QString& serPath, const CaptureStats& s, const QDateTime& mid) const {
    QFile f(QFileInfo(serPath).path() + "/" + QFileInfo(serPath).completeBaseName() + ".txt");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream o(&f);
    const auto& info = cam_->info();
    const Roi r = cam_->roi();
    const QDateTime end = recStartUtc_.addMSecs(static_cast<qint64>(s.seconds * 1000));
    o << "SlopCap capture\n";
    o << "File=" << QFileInfo(serPath).fileName() << "\n";
    o << "Target=" << target_->text() << "\n";
    o << "Camera=" << QString::fromStdString(info.vendor + " " + info.model) << "\n";
    o << "Serial=" << QString::fromStdString(info.serial) << "\n";
    o << "Start(UT)=" << recStartUtc_.toString(Qt::ISODateWithMs) << "\n";
    o << "Mid(UT)=" << mid.toString(Qt::ISODateWithMs) << "\n";
    o << "End(UT)=" << end.toString(Qt::ISODateWithMs) << "\n";
    o << "Duration=" << QString::number(s.seconds, 'f', 3) << " s\n";
    o << "Frames=" << s.written << "\n";
    o << "FPS=" << QString::number(s.captureFps, 'f', 2) << "\n";
    o << "Dropped(camera)=" << s.cameraDropped << "\n";
    o << "Dropped(disk)=" << s.ringDropped << "\n";
    o << "ROI=" << r.width << "x" << r.height << "\n";
    o << "ROI(Offset)=" << r.x << "x" << r.y << "\n";
    o << "BitDepth=" << cam_->bitDepth() << "\n";
    o << "Bayer=" << bayerName(info.bayer) << "\n";
    o << "Readout=" << readout_->currentText() << "\n";
    for (const auto& [name, v] : recControls_) o << QString::fromStdString(name) << "=" << v << "\n";
    if (focus_->focuserPosition() >= 0) o << "Focuser position=" << focus_->focuserPosition() << "\n";
    if (!s.error.empty()) o << "Error=" << QString::fromStdString(s.error) << "\n";
}

QWidget* MainWindow::buildSidebar() {
    auto* tabs = tabs_ = new QTabWidget;
    tabs->addTab(buildCaptureTab(), "Capture");
    auto scrolled = [](QWidget* w) {
        auto* sa = new QScrollArea;
        sa->setWidget(w);
        sa->setWidgetResizable(true);
        sa->setFrameShape(QFrame::NoFrame);
        sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);   // panels reflow to the sidebar's width
        return sa;
    };
    focus_ = new FocusPanel(preview_);
    tabs->addTab(scrolled(focus_), "Focus");
    collim_ = new CollimationPanel(preview_);
    tabs->addTab(scrolled(collim_), "Collimate");
    tabs->setMinimumWidth(380);
    return tabs;
}

void MainWindow::showTab(const QString& name) {
    for (int i = 0; i < tabs_->count(); ++i)
        if (tabs_->tabText(i).compare(name, Qt::CaseInsensitive) == 0) tabs_->setCurrentIndex(i);
    if (name.compare("collimate", Qt::CaseInsensitive) == 0) collim_->setAnalysing(true);
}

void MainWindow::startFocuser(const QString& hostPort, bool sweep, int range, int step, int frames) {
    focus_->connectFocuser(hostPort);
    if (sweep) focus_->sweepWhenReady(range, step, frames);
}

QWidget* MainWindow::buildCaptureTab() {
    auto* side = new QWidget;
    auto* col = new QVBoxLayout(side);

    // Camera
    auto* camBox = new QGroupBox("Camera");
    auto* camLay = new QVBoxLayout(camBox);
    auto* row = new QHBoxLayout;
    cameraSpec_ = new QComboBox;
    cameraSpec_->setEditable(true);
    cameraSpec_->setToolTip("asi, asi:N, qhy, qhy:N, sim:FILE.ser, sim-fast:FILE.ser");
    auto* refresh = new QPushButton("↻");
    refresh->setFixedWidth(32);
    refresh->setToolTip("Look for cameras again");
    connect_ = new QPushButton("Connect");
    row->addWidget(cameraSpec_, 1);
    row->addWidget(refresh);
    row->addWidget(connect_);
    camLay->addLayout(row);
    cameraName_ = new QLabel;
    cameraName_->setWordWrap(true);
    camLay->addWidget(cameraName_);
    auto* fmt = new QFormLayout;
    roi_ = new QComboBox;
    bits_ = new QComboBox;
    readout_ = new QComboBox;
    fmt->addRow("ROI", roi_);
    fmt->addRow("Depth", bits_);
    fmt->addRow("Readout", readout_);
    camLay->addLayout(fmt);
    col->addWidget(camBox);
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshCameraList);
    connect(connect_, &QPushButton::clicked, this, [this] {
        if (cam_) disconnectCamera();
        else connectCamera(cameraSpec_->currentData().isValid() ? cameraSpec_->currentData().toString() : cameraSpec_->currentText());
    });
    for (auto* c : {roi_, bits_, readout_}) connect(c, &QComboBox::activated, this, &MainWindow::applyFormat);

    // Controls (built per camera)
    auto* ctlBox = new QGroupBox("Controls");
    auto* ctlLay = new QVBoxLayout(ctlBox);
    controlsForm_ = new QFormLayout;
    ctlLay->addLayout(controlsForm_);
    landmarks_ = new QVBoxLayout;
    ctlLay->addLayout(landmarks_);
    auto* scroll = new QScrollArea;
    scroll->setWidget(ctlBox);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    col->addWidget(scroll, 1);

    // Display
    auto* dispBox = new QGroupBox("Display");
    auto* dispLay = new QVBoxLayout(dispBox);
    histogram_ = new preview::HistogramWidget;
    dispLay->addWidget(histogram_);
    histInfo_ = new QLabel;
    dispLay->addWidget(histInfo_);
    autoStretch_ = new QCheckBox("Auto stretch");
    autoStretch_->setChecked(true);
    auto* wbRow = new QHBoxLayout;
    wbRow->addWidget(autoStretch_);
    auto* autoWb = new QCheckBox("Auto white balance");
    autoWb->setChecked(true);
    autoWb->setToolTip("Display only: recordings stay raw");
    wbRow->addWidget(autoWb);
    dispLay->addLayout(wbRow);
    connect(autoWb, &QCheckBox::toggled, this, [this](bool on) { preview_->setAutoWhiteBalance(on); });
    auto* lvForm = new QFormLayout;
    auto slider = [](int max) {
        auto* s = new QSlider(Qt::Horizontal);
        s->setRange(0, max);
        return s;
    };
    black_ = slider(kSliderMax);
    white_ = slider(kSliderMax);
    gamma_ = slider(400);
    gamma_->setMinimum(20);
    white_->setValue(kSliderMax);
    gamma_->setValue(100);
    lvForm->addRow("Black", black_);
    lvForm->addRow("White", white_);
    lvForm->addRow("Gamma", gamma_);
    dispLay->addLayout(lvForm);
    col->addWidget(dispBox);
    connect(autoStretch_, &QCheckBox::toggled, this, [this](bool on) {
        if (on) preview_->setAutoStretch(true);
        else levelsFromSliders();
    });
    for (auto* s : {black_, white_, gamma_}) connect(s, &QSlider::valueChanged, this, &MainWindow::levelsFromSliders);

    // Record
    auto* recBox = new QGroupBox("Record");
    auto* recLay = new QFormLayout(recBox);
    auto* dirRow = new QHBoxLayout;
    outDir_ = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + "/SlopCap");
    auto* browse = new QPushButton("…");
    browse->setFixedWidth(32);
    dirRow->addWidget(outDir_, 1);
    dirRow->addWidget(browse);
    recLay->addRow("Folder", dirRow);
    target_ = new QLineEdit;
    target_->setToolTip("Used in the file name (WinJUPOS style: 2026-10-17-0315_4-Saturn.ser) and the sidecar");
    recLay->addRow("Target", target_);
    limitSeconds_ = new QSpinBox;
    limitSeconds_->setRange(0, 24 * 3600);
    limitSeconds_->setSuffix(" s");
    limitSeconds_->setSpecialValueText("no limit");
    recLay->addRow("Length", limitSeconds_);
    record_ = new QPushButton("● Record");
    record_->setMinimumHeight(36);
    recLay->addRow(record_);
    col->addWidget(recBox);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto d = QFileDialog::getExistingDirectory(this, "Recording folder", outDir_->text());
        if (!d.isEmpty()) outDir_->setText(d);
    });
    connect(record_, &QPushButton::clicked, this, &MainWindow::toggleRecording);
    record_->setToolTip("Ctrl+R");
    auto* rec = new QShortcut(QKeySequence("Ctrl+R"), this);
    connect(rec, &QShortcut::activated, this, &MainWindow::toggleRecording);
    return side;
}

void MainWindow::setOutputDir(const QString& dir) { outDir_->setText(dir); }

void MainWindow::refreshCameraList() {
    const QString typed = cameraSpec_->currentText();
    cameraSpec_->clear();
    for (const auto& line : listCameras()) {
        const auto q = QString::fromStdString(line);
        const auto spec = q.section(' ', 0, 0, QString::SectionSkipEmpty);
        cameraSpec_->addItem(q.simplified(), spec.startsWith("(") ? QVariant() : QVariant(spec));
    }
    if (!typed.isEmpty() && cameraSpec_->findText(typed) < 0) cameraSpec_->setEditText(typed);
}

void MainWindow::connectCamera(const QString& spec) {
    try {
        cam_ = openCamera(spec.trimmed().toStdString());
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "SlopCap", QString("Could not open %1:\n%2").arg(spec, e.what()));
        return;
    }
    if (cameraSpec_->currentText().isEmpty() || cameraSpec_->currentData().toString() != spec) cameraSpec_->setEditText(spec);
    roiCentre_.reset();
    restoreCameraSettings();
    const auto& info = cam_->info();
    cameraName_->setText(QString::fromStdString(std::format("{} {} · {}×{} · {:.2f} µm · {}", info.vendor, info.model, info.sensorWidth,
                                                            info.sensorHeight, info.pixelUm, bayerName(info.bayer))));
    {
        const QSignalBlocker b1(roi_), b2(bits_), b3(readout_);
        roi_->clear();
        roi_->addItem(QString("Full %1×%2").arg(info.sensorWidth).arg(info.sensorHeight), QSize(info.sensorWidth, info.sensorHeight));
        for (QSize s : {QSize(1920, 1080), QSize(1280, 960), QSize(1024, 768), QSize(800, 600), QSize(640, 480), QSize(320, 240)})
            if (s.width() < info.sensorWidth && s.height() < info.sensorHeight) roi_->addItem(QString("%1×%2").arg(s.width()).arg(s.height()), s);
        const Roi cur = cam_->roi();
        for (int i = 0; i < roi_->count(); ++i)
            if (roi_->itemData(i).toSize() == QSize(cur.width, cur.height)) roi_->setCurrentIndex(i);
        bits_->clear();
        for (int b : info.bitDepths) bits_->addItem(QString("%1-bit").arg(b), b);
        bits_->setCurrentIndex(std::max(0, bits_->findData(cam_->bitDepth())));
        readout_->clear();
        for (const auto& m : cam_->readoutModes()) readout_->addItem(QString::fromStdString(m.name), m.index);
        if (readout_->count() == 0) readout_->addItem("Default", 0);
        readout_->setCurrentIndex(std::max(0, readout_->findData(cam_->readoutMode())));
    }
    populateCameraControls();
    connect_->setText("Disconnect");
    cameraSpec_->setEnabled(false);
    setConfigEnabled(true);
    startSession(false);
}

void MainWindow::disconnectCamera() {
    stopSession();
    saveSettings();
    cam_.reset();
    connect_->setText("Connect");
    cameraSpec_->setEnabled(true);
    cameraName_->clear();
    setConfigEnabled(false);
    statusLeft_->setText("No camera");
}

void MainWindow::populateCameraControls() {
    while (controlsForm_->rowCount() > 0) controlsForm_->removeRow(0);
    while (auto* item = landmarks_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    controlBoxes_.clear();
    for (const auto& c : cam_->controls()) {
        if (!c.writable) continue;
        auto* box = new QDoubleSpinBox;
        box->setRange(c.min, c.max);
        const double step = c.step > 0 ? c.step : 1;
        box->setSingleStep(step);
        box->setDecimals(step < 1 ? 3 : 0);
        box->setKeyboardTracking(false);   // apply on Enter / arrows, not every keystroke
        if (c.name == "Exposure") box->setSuffix(" µs");   // ZWO and QHY both take exposure in microseconds
        if (auto v = cam_->control(c.name)) box->setValue(*v);
        const std::string name = c.name;
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, name](double v) { setControl(name, v); });
        controlsForm_->addRow(QString::fromStdString(c.name), box);
        controlBoxes_[c.name] = box;
    }
    const auto marks = cam_->gainLandmarks();
    if (!marks.empty()) landmarks_->addWidget(new QLabel("<b>Gain landmarks</b>"));
    for (const auto& m : marks) {
        if (!m.gain) continue;
        QString text = QString("%1: gain %2").arg(m.label.empty() ? landmarkName(m.kind) : QString::fromStdString(m.label)).arg(*m.gain);
        if (m.offset) text += QString(", offset %1").arg(*m.offset);
        auto* b = new QPushButton(text);
        b->setFlat(true);
        b->setStyleSheet("text-align: left");
        b->setToolTip("Set gain (and offset) to this value");
        connect(b, &QPushButton::clicked, this, [this, g = *m.gain, o = m.offset] {
            if (auto it = controlBoxes_.find("Gain"); it != controlBoxes_.end()) it->second->setValue(g);
            if (o)
                if (auto it = controlBoxes_.find("Offset"); it != controlBoxes_.end()) it->second->setValue(*o);
        });
        landmarks_->addWidget(b);
    }
}

void MainWindow::setControl(const std::string& name, double value) {
    if (!cam_) return;
    if (session_) {
        session_->post([name, value](ICamera& c) { c.setControl(name, value); });
        return;
    }
    try {
        cam_->setControl(name, value);
    } catch (const std::exception& e) {
        statusLeft_->setText(QString("%1: %2").arg(QString::fromStdString(name), e.what()));
    }
}

void MainWindow::applyFormat() {
    if (!cam_ || recording_) return;
    stopSession();
    try {
        if (const int m = readout_->currentData().toInt(); m != cam_->readoutMode()) cam_->setReadoutMode(m);
        cam_->setBitDepth(bits_->currentData().toInt());
        const QSize s = roi_->currentData().toSize();
        const auto& info = cam_->info();
        // centred (on the chosen point, else the sensor), on offsets every driver accepts (multiple of 8 across, even
        // down; keeps the Bayer phase)
        const QPoint c = roiCentre_.value_or(QPoint(info.sensorWidth / 2, info.sensorHeight / 2));
        const int x = std::clamp(c.x() - s.width() / 2, 0, info.sensorWidth - s.width()) / 8 * 8;
        const int y = std::clamp(c.y() - s.height() / 2, 0, info.sensorHeight - s.height()) / 2 * 2;
        cam_->setRoi({x, y, s.width(), s.height(), 1});
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "SlopCap", e.what());
    }
    startSession(false);
}

void MainWindow::startSession(bool record) {
    if (!cam_) return;
    CaptureOptions opt;
    opt.preview = &tap_;
    if (record) {
        QDir dir(outDir_->text());
        if (!dir.mkpath(".")) {
            QMessageBox::warning(this, "SlopCap", "Can't create " + outDir_->text());
            record = false;
        } else {
            recStartUtc_ = QDateTime::currentDateTimeUtc();
            recordingPath_ = dir.filePath(winjupos(recStartUtc_) + "-" + sanitize(target_->text().isEmpty() ? "capture" : target_->text().toStdString()) + ".ser");
            recControls_.clear();
            for (const auto& [name, box] : controlBoxes_) recControls_[name] = box->value();
            opt.output = recordingPath_.toStdString();
            opt.directIO = true;
            if (limitSeconds_->value() > 0) opt.duration = std::chrono::seconds(limitSeconds_->value());
        }
    }
    session_ = std::make_unique<CaptureSession>(*cam_, opt);
    try {
        session_->start();
    } catch (const std::exception& e) {
        session_.reset();
        QMessageBox::warning(this, "SlopCap", QString("Could not start: %1").arg(e.what()));
        record = false;
    }
    recording_ = record && session_;
    if (session_) preview_->setSource(&tap_, cam_->info().bayer);
    record_->setText(recording_ ? "■ Stop" : "● Record");
    setConfigEnabled(!recording_);
}

void MainWindow::stopSession() {
    preview_->setSource(nullptr, Bayer::None);
    if (!session_) return;
    session_->stop();
    const auto s = session_->stats();
    if (recording_ && s.written > 0) {
        // WinJUPOS wants the mid-capture time in the name: rename now that the length is known.
        const QDateTime mid = recStartUtc_.addMSecs(static_cast<qint64>(s.seconds * 500));
        const QString named = QFileInfo(recordingPath_).path() + "/" + winjupos(mid) + "-" +
                              sanitize(target_->text().isEmpty() ? "capture" : target_->text().toStdString()) + ".ser";
        if (named != recordingPath_ && !QFileInfo::exists(named) && QFile::rename(recordingPath_, named)) recordingPath_ = named;
        writeSidecar(recordingPath_, s, mid);
    }
    if (recording_)
        statusLeft_->setText(QString::fromStdString(std::format("Saved {} frames ({:.1f} s, {:.0f} MB/s), dropped {} by camera, {} by disk{}",
                                                                s.written, s.seconds, s.writeMBps, s.cameraDropped, s.ringDropped,
                                                                s.error.empty() ? "" : " — " + s.error)) +
                             " → " + recordingPath_);
    session_.reset();
    recording_ = false;
}

void MainWindow::toggleRecording() {
    if (!cam_) return;
    const bool start = !recording_;
    stopSession();
    startSession(start);
}

void MainWindow::printSummary() const {
    std::string line = statusLeft_->text().toStdString() + " | " + statusRight_->text().toStdString();
    std::fputs((line + "\n").c_str(), stdout);
    std::fflush(stdout);
}

void MainWindow::levelsFromSliders() {
    if (autoStretch_->isChecked()) {
        const QSignalBlocker b(autoStretch_);
        autoStretch_->setChecked(false);
    }
    preview::Levels lv;
    lv.black = black_->value() / double(kSliderMax);
    lv.white = std::max(lv.black + 0.001, white_->value() / double(kSliderMax));
    lv.gamma = gamma_->value() / 100.0;
    preview_->setLevels(lv);
}

void MainWindow::showLevels(const preview::Levels& lv) {
    const QSignalBlocker b1(black_), b2(white_), b3(gamma_);
    black_->setValue(static_cast<int>(std::lround(lv.black * kSliderMax)));
    white_->setValue(static_cast<int>(std::lround(lv.white * kSliderMax)));
    gamma_->setValue(static_cast<int>(std::lround(lv.gamma * 100)));
}

void MainWindow::updateStats() {
    if (!session_) {
        statusRight_->clear();
        return;
    }
    const auto s = session_->stats();
    if (!s.running && recording_) {   // hit the length limit, or failed: back to live view
        stopSession();
        startSession(false);
        return;
    }
    if (!s.error.empty()) statusLeft_->setText("Error: " + QString::fromStdString(s.error));
    else if (!s.notice.empty()) statusLeft_->setText(QString::fromStdString(s.notice));
    else if (recording_) {
        const QStorageInfo disk(outDir_->text());
        const double freeGB = static_cast<double>(disk.bytesAvailable()) / 1e9;
        const double minutes = s.writeMBps > 1 ? freeGB * 1000 / s.writeMBps / 60 : 0;
        statusLeft_->setText(QString::fromStdString(std::format("● REC {:.1f} s · {} frames · {:.0f} MB/s · buffer {}/{} · disk {:.0f} GB ({:.0f} min)",
                                                                s.seconds, s.written, s.writeMBps, s.ringQueued, s.ringCapacity, freeGB, minutes)));
    } else {
        const QStorageInfo disk(QDir(outDir_->text()).exists() ? outDir_->text() : QDir::homePath());
        statusLeft_->setText(QString("Live · %1 GB free for recordings").arg(static_cast<double>(disk.bytesAvailable()) / 1e9, 0, 'f', 0));
    }
    const uint64_t dropped = s.cameraDropped + s.ringDropped;
    statusRight_->setText(QString::fromStdString(std::format("camera {:.1f} fps · display {:.0f} fps{}", s.captureFps, preview_->displayFps(),
                                                             dropped ? std::format(" · dropped {} cam / {} disk", s.cameraDropped, s.ringDropped) : "")));
}

void MainWindow::setConfigEnabled(bool on) {
    for (QWidget* w : std::initializer_list<QWidget*>{roi_, bits_, readout_}) w->setEnabled(on && cam_);
    record_->setEnabled(static_cast<bool>(cam_));
    connect_->setEnabled(!recording_);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (recording_ && QMessageBox::question(this, "SlopCap", "A recording is running. Stop it and quit?") != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    saveSettings();
    stopSession();
    e->accept();
}

}  // namespace slopcap::app

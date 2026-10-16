#include "app/main_window.hpp"

#include "app/collimation_panel.hpp"
#include "app/focus_controller.hpp"
#include "app/focus_views.hpp"
#include "app/mount_section.hpp"
#include "app/theme.hpp"
#include "camera/open_camera.hpp"
#include "preview/histogram_widget.hpp"
#include "preview/preview_widget.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTextStream>
#include <QVBoxLayout>

#include <cmath>
#include <cstdio>
#include <format>

namespace slopcap::app {

namespace {

const char* landmarkShort(GainLandmark::Kind k) {
    switch (k) {
        case GainLandmark::Kind::Unity: return "Unity";
        case GainLandmark::Kind::LowestReadNoise: return "Low noise";
        case GainLandmark::Kind::HighestDynamicRange: return "Max DR";
        case GainLandmark::Kind::HcgSwitch: return "HCG";
        case GainLandmark::Kind::Preset: return "Preset";
        case GainLandmark::Kind::VendorDefault: return "Default";
    }
    return "";
}

QString sanitize(const QString& s) {
    QString out;
    for (QChar c : s) out += (c.isLetterOrNumber() || c == '-') ? c : QChar('_');
    return out;
}

/// WinJUPOS time stamp: YYYY-MM-DD-HHMM_T, T = tenths of a minute.
QString winjupos(const QDateTime& utc) { return utc.toString("yyyy-MM-dd-HHmm") + "_" + QString::number(utc.time().second() / 6); }

QString thousands(double v) { return QLocale(QLocale::English).toString(static_cast<qlonglong>(std::lround(v))); }

QString formatExposure(double us) {
    if (us < 1000) return QString("%1 ms").arg(us / 1000.0, 0, 'f', 3);
    if (us < 1e6) return QString("%1 ms").arg(us / 1000.0, 0, 'f', us < 1e4 ? 3 : 1);
    return QString("%1 s").arg(us / 1e6, 0, 'f', 2);
}

QString mmss(double seconds) {
    const int s = static_cast<int>(seconds);
    return QString("%1:%2").arg(s / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

QWidget* scrolled(QWidget* w) {
    auto* sa = new QScrollArea;
    sa->setWidget(w);
    sa->setWidgetResizable(true);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sa->setFrameShape(QFrame::NoFrame);
    return sa;
}

/// A side-column page: panel-coloured, fixed width range as in the design.
QWidget* sideColumn(QWidget* content, int minW, int maxW) {
    auto* f = new QFrame;
    f->setProperty("role", "panel");
    auto* l = new QVBoxLayout(f);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(scrolled(content));
    f->setMinimumWidth(minW);
    f->setMaximumWidth(maxW);
    return f;
}

QPixmap logo(double dpr) {
    QPixmap pm(static_cast<int>(22 * dpr), static_cast<int>(22 * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(theme::kAccent, 1.8));
    p.translate(11, 11);
    p.scale(22.0 / 24, 22.0 / 24);
    p.drawEllipse(QPointF(0, 0), 5, 5);
    p.rotate(-20);
    p.drawEllipse(QPointF(0, 0), 10.5, 3.2);
    return pm;
}

}  // namespace

// ================================================================ construction

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("SlopCap");
    focus_ = new FocusController(this);
    mount_ = new devices::Mount(focus_->indi(), this);   // same INDI server as the focuser
    preview_ = new preview::PreviewWidget;
    collim_ = new CollimationPanel(preview_, this);

    auto* root = new QWidget;
    root->setObjectName("ground");
    auto* v = new QVBoxLayout(root);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(buildHeader());

    // three columns separated by 1 px lines (the body's background shows through the spacing)
    auto* body = new QFrame;
    body->setStyleSheet(QString("QFrame#body { background: %1; }").arg(theme::kLine.name()));
    body->setObjectName("body");
    auto* h = new QHBoxLayout(body);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(1);
    left_ = new QStackedWidget;
    centre_ = new QStackedWidget;
    right_ = new QStackedWidget;
    sweepPanel_ = new SweepPanel(*focus_);
    chartPanel_ = new FocusChartPanel(*focus_);
    resultsPanel_ = new FocusResultsPanel(*focus_);
    left_->addWidget(sideColumn(buildCaptureLeft(), 280, 320));   // Capture
    left_->addWidget(sideColumn(sweepPanel_, 250, 300));          // Focus
    left_->addWidget(sideColumn(collim_->left(), 260, 300));      // Collimate
    centre_->addWidget(buildPreviewPage());
    centre_->addWidget(chartPanel_);
    right_->addWidget(sideColumn(buildCaptureRight(), 280, 320));
    right_->addWidget(sideColumn(resultsPanel_, 280, 330));
    right_->addWidget(sideColumn(collim_->right(), 280, 320));
    for (auto* s : {left_, right_}) s->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    h->addWidget(left_);
    h->addWidget(centre_, 1);
    h->addWidget(right_);
    v->addWidget(body, 1);
    v->addWidget(buildFooter());
    setCentralWidget(root);

    connect(preview_, &preview::PreviewWidget::frameShown, this, [this](const std::array<uint32_t, 256>& hist, const preview::Levels& lv, const FrameInfo&) {
        histogram_->setData(hist, lv);
        uint64_t total = 0;
        int top = 0;
        for (int b = 0; b < 256; ++b)
            if (hist[static_cast<std::size_t>(b)]) total += hist[static_cast<std::size_t>(b)], top = b;
        const double clipped = total ? 100.0 * hist[255] / static_cast<double>(total) : 0;
        histInfo_->setText(QString("peak %1% · clipped %2%").arg(top * 100 / 255).arg(clipped, 0, 'f', 2));
        histInfo_->setStyleSheet(clipped > 0.1 ? QString("color: %1").arg(theme::kRecText.name()) : "");
    });
    connect(preview_, &preview::PreviewWidget::rawFrame, this, [this](const LatestFrame::Frame& f) {
        if (!cam_) return;
        focus_->onFrame(f, cam_->info().bayer);
        if (mode_ == Collimate) collim_->onFrame(f, cam_->info().bayer);
    });
    connect(preview_, &preview::PreviewWidget::centreRequested, this, &MainWindow::centreRoiOn);
    connect(focus_, &FocusController::liveUpdated, this, &MainWindow::updateTargetInfo);
    connect(focus_, &FocusController::sweepUpdated, this, &MainWindow::updateFocusMode);
    connect(focus_, &FocusController::sweepStateChanged, this, [this](bool running) {
        updateFocusMode();
        if (running) setMode(Focus);
    });
    connect(focus_, &FocusController::focuserChanged, this, [this] {
        updateFocusMode();
        if (pendingSweep_ && focus_->focuser().ready()) {
            pendingSweep_ = false;
            const int at = focus_->focuser().position(), range = pendingRange_ ? pendingRange_ : 350;
            SweepSettings s;
            s.from = std::max(0, at - range), s.to = at + range, s.step = pendingStep_ ? pendingStep_ : 50, s.frames = pendingFrames_ ? pendingFrames_ : 60;
            QTimer::singleShot(500, this, [this, s] { focus_->startSweep(s); });
        }
    });
    connect(sweepPanel_, &SweepPanel::displayChanged, this, &MainWindow::updateFocusMode);
    connect(resultsPanel_, &FocusResultsPanel::targetChanged, this, &MainWindow::updateFocusMode);
    connect(resultsPanel_, &FocusResultsPanel::metricPicked, this, [this](analysis::Metric m) {
        focus_->setLiveMetric(m);
        updateFocusMode();
    });
    connect(&statsTimer_, &QTimer::timeout, this, &MainWindow::updateStats);
    statsTimer_.start(250);
    messageTimer_.setSingleShot(true);
    connect(&messageTimer_, &QTimer::timeout, this, [this] { fMessage_->clear(); });
    auto* rec = new QShortcut(QKeySequence("Ctrl+R"), this);
    connect(rec, &QShortcut::activated, this, &MainWindow::toggleRecording);

    // settings
    QSettings st;
    resize(1440, 900);
    if (st.contains("window/geometry")) restoreGeometry(st.value("window/geometry").toByteArray());
    outDir_ = st.value("record/dir", QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + "/SlopCap").toString();
    target_->setCurrentText(st.value("record/target", "Saturn").toString());
    filter_->setCurrentText(st.value("record/filter", "").toString());
    limit_->setCurrentIndex(std::max(0, limit_->findData(st.value("record/limit", 0).toInt())));
    mountSection_->setPulseMs(st.value("mount/pulseMs", 300).toInt());
    camSpec_ = st.value("camera/last").toString();
    Settings s;
    s.indiServer = st.value("focuser/server", s.indiServer).toString();
    s.focuserAutoConnect = st.value("focuser/autoConnect", false).toBool();
    s.backlash = st.value("focuser/backlash", s.backlash).toInt();
    s.approachOutward = st.value("focuser/outward", true).toBool();
    s.focalRatio = st.value("optics/focalRatio", 0.0).toDouble();
    s.micronsPerStep = st.value("optics/micronsPerStep", 0.0).toDouble();
    applySettings(s);
    focus_->setOutputDir(outDir_);
    if (s.focuserAutoConnect) focus_->connectFocuser(s.indiServer);
    populateCameraUi();
    setMode(Capture);
}

MainWindow::~MainWindow() { stopSession(); }

void MainWindow::applySettings(const Settings& s) {
    settings_ = s;
    focus_->focuser().setBacklash(s.backlash);
    focus_->focuser().setApproachOutward(s.approachOutward);
    focus_->setOptics(s.focalRatio, s.micronsPerStep);
    if (s.indiServer != focus_->server() && focus_->indi().isConnected()) {
        focus_->disconnectFocuser();
        focus_->connectFocuser(s.indiServer);
    } else {
        focus_->setServer(s.indiServer);
    }
    emit focus_->focuserChanged();
    updateFocusMode();
}

// ================================================================ header / footer

QWidget* MainWindow::buildHeader() {
    auto* f = new QFrame;
    f->setProperty("role", "header");
    auto* h = new QHBoxLayout(f);
    h->setContentsMargins(16, 10, 16, 10);
    h->setSpacing(16);
    auto* brand = new QHBoxLayout;
    brand->setSpacing(10);
    auto* icon = new QLabel;
    icon->setPixmap(logo(devicePixelRatioF()));
    brand->addWidget(icon);
    brand->addWidget(theme::label("SlopCap", "brand"));
    h->addLayout(brand);

    auto* nav = new QFrame;
    nav->setProperty("role", "nav");
    auto* nl = new QHBoxLayout(nav);
    nl->setContentsMargins(3, 3, 3, 3);
    nl->setSpacing(4);
    modes_ = new QButtonGroup(this);
    const char* names[] = {"Capture", "Focus", "Collimate", "Align", "Sequence"};
    for (int i = 0; i < 5; ++i) {
        auto* b = theme::button(names[i], "nav");
        b->setCheckable(true);
        if (i >= Align) {
            b->setEnabled(false);
            b->setToolTip(i == Align ? "Drift alignment with the target outline: designed, not built yet" : "Timed WinJUPOS sets: designed, not built yet");
        }
        modes_->addButton(b, i);
        nl->addWidget(b);
    }
    connect(modes_, &QButtonGroup::idClicked, this, [this](int id) { setMode(static_cast<Mode>(id)); });
    h->addWidget(nav);
    headerInfo_ = theme::label("", "muted");
    h->addWidget(headerInfo_, 1);

    auto combo = [&](const QString& caption, QComboBox*& c, const QStringList& items) {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        row->addWidget(theme::label(caption, "muted"));
        c = new QComboBox;
        c->setEditable(true);
        c->addItems(items);
        c->setMinimumWidth(caption == "Filter" ? 70 : 110);
        row->addWidget(c);
        h->addLayout(row);
    };
    combo("Target", target_, {"Saturn", "Jupiter", "Mars", "Moon", "Sun", "Venus", "Uranus", "Neptune", "Star"});
    combo("Filter", filter_, {"", "L", "R", "G", "B", "IR", "CH4", "UV"});
    target_->setToolTip("Used in file names (WinJUPOS style) and sidecars");
    filter_->setToolTip("Optional: goes into the file name, e.g. 2026-10-17-0315_4-L-Saturn.ser");
    for (auto* c : {target_, filter_}) connect(c, &QComboBox::currentTextChanged, this, &MainWindow::updateStats);

    cameraPill_ = new QPushButton;
    cameraPill_->setCursor(Qt::PointingHandCursor);
    cameraPill_->setStyleSheet(QString("QPushButton { border: 1px solid %1; border-radius: 13px; padding: 4px 12px 4px 24px; color: %2; font-family: '%3'; "
                                       "font-size: 12px; background: transparent; } QPushButton:hover { border-color: %4; }")
                                   .arg(theme::kLine.name(), theme::kMuted.name(), theme::monoFont().family(), theme::kDim.name()));
    cameraDot_ = new QLabel(cameraPill_);
    cameraDot_->setFixedSize(7, 7);
    cameraDot_->move(11, 10);
    theme::setDot(cameraDot_, theme::kGrey);
    connect(cameraPill_, &QPushButton::clicked, this, &MainWindow::showCameraMenu);
    h->addWidget(cameraPill_);

    auto* gear = theme::button("⚙");
    gear->setFixedSize(34, 34);
    gear->setToolTip("Settings");
    connect(gear, &QPushButton::clicked, this, [this] {
        SettingsDialog d(settings_, this);
        if (d.exec() == QDialog::Accepted) {
            applySettings(d.settings());
            saveSettings();
        }
    });
    h->addWidget(gear);
    return f;
}

QWidget* MainWindow::buildFooter() {
    auto* f = new QFrame;
    f->setProperty("role", "footer");
    auto* h = new QHBoxLayout(f);
    h->setContentsMargins(16, 7, 16, 7);
    h->setSpacing(20);
    auto mk = [&](QLabel*& l) {
        l = theme::label("", "monoMuted");
        h->addWidget(l);
    };
    mk(fState_);
    mk(fFps_);
    mk(fDropped_);
    mk(fRing_);
    mk(fDisk_);
    mk(fFrames_);
    fMessage_ = theme::label("", "muted");
    h->addWidget(fMessage_, 1);
    mk(fDisplay_);
    return f;
}

void MainWindow::message(const QString& text) {
    fMessage_->setText(text);
    messageTimer_.start(12000);
}

// ================================================================ capture: left column

QWidget* MainWindow::buildCaptureLeft() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(16, 16, 16, 16);
    v->setSpacing(18);

    auto* cam = new QVBoxLayout;
    cam->setSpacing(12);
    cam->addWidget(theme::section("Camera"));
    noCamera_ = theme::label("No camera connected. Click the camera pill at the top right to pick one.", "muted");
    noCamera_->setWordWrap(true);
    cam->addWidget(noCamera_);
    cameraBox_ = new QWidget;
    auto* cb = new QVBoxLayout(cameraBox_);
    cb->setContentsMargins(0, 0, 0, 0);
    cb->setSpacing(12);

    auto sliderRow = [&](const QString& name, QLabel*& value, QSlider*& slider) {
        auto* box = new QVBoxLayout;
        box->setSpacing(6);
        auto* top = new QHBoxLayout;
        top->addWidget(new QLabel(name));
        top->addStretch(1);
        value = theme::label("—", "mono");
        top->addWidget(value);
        box->addLayout(top);
        slider = new QSlider(Qt::Horizontal);
        box->addWidget(slider);
        cb->addLayout(box);
        return box;
    };
    sliderRow("Gain", gainValue_, gain_);
    gainPresets_ = new QWidget;
    new theme::FlowLayout(gainPresets_);
    cb->addWidget(gainPresets_);
    connect(gain_, &QSlider::valueChanged, this, [this](int g) {
        gainValue_->setText(QString::number(g));
        setControl("Gain", g);
    });

    auto* expBox = sliderRow("Exposure", expValue_, exposure_);
    exposure_->setRange(0, 1000);   // log scale between the camera's limits (capped at 60 s)
    auto* hint = new QHBoxLayout;
    expMaxFps_ = theme::label("", "monoDim");
    expRange_ = theme::label("", "monoDim");
    hint->addWidget(expMaxFps_);
    hint->addStretch(1);
    hint->addWidget(expRange_);
    expBox->addLayout(hint);
    connect(exposure_, &QSlider::valueChanged, this, [this](int pos) {
        const double us = std::round(expMin_ * std::pow(expMax_ / expMin_, pos / 1000.0));
        expValue_->setText(formatExposure(us));
        expMaxFps_->setText(QString("max %1 fps").arg(1e6 / us, 0, 'f', us < 1e5 ? 0 : 2));
        setControl("Exposure", us);
    });

    auto* roiCard = theme::card();
    auto* rc = new QVBoxLayout(roiCard);
    rc->setContentsMargins(10, 10, 10, 10);
    rc->setSpacing(8);
    auto* rtop = new QHBoxLayout;
    rtop->addWidget(new QLabel("ROI"));
    rtop->addStretch(1);
    roiSize_ = theme::label("—", "mono");
    rtop->addWidget(roiSize_);
    rc->addLayout(rtop);
    roiOffset_ = theme::label("", "monoDim");
    rc->addWidget(roiOffset_);
    auto* rbtn = new QHBoxLayout;
    rbtn->setSpacing(6);
    roiPreset_ = new QComboBox;
    roiCentreBtn_ = theme::button("Center");
    roiMax_ = theme::button("Max");
    roiCentreBtn_->setToolTip("Centre the ROI on the sensor (right-click the preview to centre it on a point)");
    rbtn->addWidget(roiPreset_, 1);
    rbtn->addWidget(roiCentreBtn_);
    rbtn->addWidget(roiMax_);
    rc->addLayout(rbtn);
    cb->addWidget(roiCard);
    connect(roiPreset_, &QComboBox::activated, this, [this] { applyFormat(roiPreset_->currentData().toSize()); });
    connect(roiCentreBtn_, &QPushButton::clicked, this, [this] {
        roiCentre_.reset();
        applyFormat(roiPreset_->currentData().toSize());
    });
    connect(roiMax_, &QPushButton::clicked, this, [this] {
        if (!cam_) return;
        roiCentre_.reset();
        applyFormat(QSize(cam_->info().sensorWidth, cam_->info().sensorHeight));
    });

    bitsBox_ = new QWidget;
    auto* bl = new QHBoxLayout(bitsBox_);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(6);
    bits_ = new QButtonGroup(this);
    for (int b : {8, 16}) {
        auto* btn = theme::button(QString("%1-bit").arg(b));
        btn->setCheckable(true);
        bits_->addButton(btn, b);
        bl->addWidget(btn);
    }
    cb->addWidget(bitsBox_);
    connect(bits_, &QButtonGroup::idClicked, this, [this](int b) { applyFormat(std::nullopt, b); });

    highSpeed_ = new QCheckBox("High-speed readout");
    readout_ = new QComboBox;
    cb->addWidget(highSpeed_);
    cb->addWidget(readout_);
    connect(highSpeed_, &QCheckBox::toggled, this, [this](bool on) { applyFormat(std::nullopt, std::nullopt, on ? 1 : 0); });
    connect(readout_, &QComboBox::activated, this, [this] { applyFormat(std::nullopt, std::nullopt, readout_->currentData().toInt()); });

    allControlsBtn_ = theme::button("All controls (WB, USB traffic, offset…)");
    allControlsBtn_->setStyleSheet(QString("QPushButton { background: transparent; border: none; color: %1; text-align: left; padding: 0; font-size: 12px; }")
                                       .arg(theme::kAccent.name()));
    cb->addWidget(allControlsBtn_);
    allControls_ = theme::card();
    controlsForm_ = new QFormLayout(allControls_);
    controlsForm_->setContentsMargins(10, 10, 10, 10);
    allControls_->setVisible(false);
    cb->addWidget(allControls_);
    connect(allControlsBtn_, &QPushButton::clicked, this, [this] { allControls_->setVisible(!allControls_->isVisible()); });
    cam->addWidget(cameraBox_);
    v->addLayout(cam);
    v->addStretch(1);

    auto* capture = new QVBoxLayout;
    capture->setSpacing(12);
    capture->addWidget(theme::section("Capture"));
    auto* grid = new QGridLayout;
    grid->setSpacing(8);
    limit_ = new QComboBox;
    for (auto [label, seconds] : std::initializer_list<std::pair<const char*, int>>{
             {"None", 0}, {"30 s", 30}, {"1 min", 60}, {"2 min", 120}, {"3 min", 180}, {"4 min", 240}, {"5 min", 300}, {"10 min", 600}})
        limit_->addItem(label, seconds);
    folder_ = theme::button("Folder…");
    auto lab = [](const QString& t, QWidget* field) {
        auto* b = new QWidget;
        auto* l = new QVBoxLayout(b);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(4);
        l->addWidget(theme::label(t, "muted"));
        l->addWidget(field);
        return b;
    };
    grid->addWidget(lab("Limit", limit_), 0, 0);
    grid->addWidget(lab("Folder", folder_), 0, 1);
    capture->addLayout(grid);
    connect(folder_, &QPushButton::clicked, this, [this] {
        const auto d = QFileDialog::getExistingDirectory(this, "Recording folder", outDir_);
        if (!d.isEmpty()) setOutputDir(d);
    });
    connect(limit_, &QComboBox::currentIndexChanged, this, &MainWindow::updateStats);
    auto* pathCard = theme::card();
    auto* pl = new QVBoxLayout(pathCard);
    pl->setContentsMargins(10, 8, 10, 8);
    nextFile_ = theme::label("", "monoMuted");
    nextFile_->setWordWrap(true);
    pl->addWidget(nextFile_);
    capture->addWidget(pathCard);
    record_ = theme::button("●  Record", "record");
    record_->setMinimumHeight(44);
    record_->setToolTip("Ctrl+R");
    connect(record_, &QPushButton::clicked, this, &MainWindow::toggleRecording);
    capture->addWidget(record_);
    v->addLayout(capture);
    return w;
}

// ================================================================ capture: right column

QWidget* MainWindow::buildCaptureRight() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(16, 16, 16, 16);
    v->setSpacing(18);
    v->addWidget(theme::section("Target"));
    auto* grid = new QGridLayout;
    grid->setSpacing(8);
    grid->addWidget(theme::stat("Centre (frame)", tCentre_), 0, 0);
    grid->addWidget(theme::stat("Edge margin", tMargin_), 0, 1);
    grid->addWidget(theme::stat("Size", tDisc_), 1, 0);
    grid->addWidget(theme::stat("Dispersion R−B", tDisp_), 1, 1);
    v->addLayout(grid);
    auto* focuser = new FocuserSection(*focus_);
    connect(focuser, &FocuserSection::openFocusMode, this, [this] { setMode(Focus); });
    connect(focuser, &FocuserSection::openSettings, this, [this] {
        SettingsDialog d(settings_, this);
        if (d.exec() == QDialog::Accepted) {
            applySettings(d.settings());
            saveSettings();
        }
    });
    v->addWidget(focuser);
    mountSection_ = new MountSection(*mount_, *focus_);
    connect(mountSection_, &MountSection::message, this, &MainWindow::message);
    v->addWidget(mountSection_);
    v->addStretch(1);
    return w;
}

// ================================================================ centre: preview

QWidget* MainWindow::buildPreviewPage() {
    auto* page = new QWidget;
    page->setObjectName("ground");
    page->setAttribute(Qt::WA_StyledBackground);
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* bar = new QFrame;
    bar->setProperty("role", "toolbar");
    auto* bh = new QHBoxLayout(bar);
    bh->setContentsMargins(12, 8, 12, 8);
    bh->setSpacing(8);
    bh->addWidget(theme::segmented({"Fit", "100%", "200%"}, zoom_, true));
    zoom_->button(0)->setChecked(true);
    connect(zoom_, &QButtonGroup::idClicked, this, [this](int id) {
        if (id == 0) preview_->fitToWindow();
        else preview_->zoomTo(id == 1 ? 1.0 : 2.0);
        zoom_->button(id)->setChecked(true);
    });
    connect(preview_, &preview::PreviewWidget::zoomChanged, this, [this] {
        if (preview_->fitted()) zoom_->button(0)->setChecked(true);
    });
    bh->addWidget(theme::segmented({"Linear", "Auto stretch"}, stretch_, true));
    stretch_->button(1)->setChecked(true);
    connect(stretch_, &QButtonGroup::idClicked, this, [this](int id) {
        if (id == 1) {
            preview_->setAutoStretch(true);
        } else {
            preview_->setLevels({});
        }
    });
    auto check = [&](const QString& t, bool on) {
        auto* c = new QCheckBox(t);
        c->setChecked(on);
        bh->addWidget(c);
        return c;
    };
    auto* debayer = check("Debayer", true);
    auto* wb = check("Auto WB", true);
    targetBox_ = check("Target box", true);
    auto* reticle = check("Reticle", false);
    wb->setToolTip("Display-only white balance; recordings stay raw");
    connect(debayer, &QCheckBox::toggled, this, [this](bool on) { preview_->setDebayer(on); });
    connect(wb, &QCheckBox::toggled, this, [this](bool on) { preview_->setAutoWhiteBalance(on); });
    connect(targetBox_, &QCheckBox::toggled, this, [this](bool on) {
        if (!on) preview_->setOverlay("target", {});
    });
    connect(reticle, &QCheckBox::toggled, this, [this](bool on) { preview_->setCrosshair(on); });
    bh->addStretch(1);
    v->addWidget(bar);

    auto* holder = new QWidget;
    auto* hl = new QVBoxLayout(holder);
    hl->setContentsMargins(20, 20, 20, 20);
    hl->addWidget(preview_);
    v->addWidget(holder, 1);
    recBadge_ = new QLabel(preview_);
    recBadge_->setStyleSheet(QString("background: rgba(13,15,18,210); border: 1px solid %1; border-radius: 4px; color: %2; padding: 3px 8px; "
                                     "font-family: '%3'; font-size: 12px; font-weight: 600;")
                                 .arg(theme::kRec.name(), theme::kRecText.name(), theme::monoFont().family()));
    recBadge_->move(12, 12);
    recBadge_->hide();

    auto* strip = new QFrame;
    strip->setProperty("role", "strip");
    auto* sh = new QHBoxLayout(strip);
    sh->setContentsMargins(16, 10, 16, 14);
    sh->setSpacing(16);
    auto* hist = new QVBoxLayout;
    hist->setSpacing(6);
    auto* hh = new QHBoxLayout;
    hh->addWidget(theme::label("Histogram", "muted"));
    hh->addStretch(1);
    histInfo_ = theme::label("", "monoMuted");
    hh->addWidget(histInfo_);
    hist->addLayout(hh);
    histogram_ = new preview::HistogramWidget;
    hist->addWidget(histogram_);
    sh->addLayout(hist, 1);
    auto* stats = new QGridLayout;
    stats->setHorizontalSpacing(20);
    stats->setVerticalSpacing(2);
    auto statCol = [&](int col, const QString& cap, QLabel*& val) {
        stats->addWidget(theme::label(cap, "dim"), 0, col);
        val = theme::label("—", "mono");
        stats->addWidget(val, 1, col);
    };
    statCol(0, "Sharpness (G)", sSharp_);
    statCol(1, "Dispersion", sDisp_);
    statCol(2, "Frame", sFrame_);
    sh->addLayout(stats);
    v->addWidget(strip);
    return page;
}

// ================================================================ modes

void MainWindow::setMode(Mode m) {
    if (m >= Align) return;
    mode_ = m;
    modes_->button(m)->setChecked(true);
    left_->setCurrentIndex(m);
    right_->setCurrentIndex(m);
    // a stacked widget is as wide as its widest page: size the side columns to the page that's showing
    for (auto* stack : {left_, right_}) {
        stack->setMinimumWidth(stack->currentWidget()->minimumWidth());
        stack->setMaximumWidth(stack->currentWidget()->maximumWidth());
    }
    centre_->setCurrentIndex(m == Focus ? 1 : 0);
    if (m != Collimate) collim_->setAnalysing(false);
    updateFocusMode();
    updateStats();
}

void MainWindow::showTab(const QString& name) {
    if (name.compare("focus", Qt::CaseInsensitive) == 0) setMode(Focus);
    else if (name.compare("collimate", Qt::CaseInsensitive) == 0) {
        setMode(Collimate);
        collim_->setAnalysing(true);
    } else setMode(Capture);
}

void MainWindow::startFocuser(const QString& hostPort, bool sweep, int range, int step, int frames) {
    pendingSweep_ = sweep;
    pendingRange_ = range, pendingStep_ = step, pendingFrames_ = frames;
    settings_.indiServer = hostPort;
    focus_->connectFocuser(hostPort);
}

void MainWindow::updateFocusMode() {
    const auto m = focus_->liveMetric();
    const auto chosen = resultsPanel_->chosen(m);
    chartPanel_->update(m, sweepPanel_->channelsShown(), chosen);
    resultsPanel_->update(m);
}

// ================================================================ camera

void MainWindow::showCameraMenu() {
    QMenu menu(this);
    if (cam_) {
        menu.addAction(QString("Disconnect %1").arg(QString::fromStdString(cam_->info().model)), this, &MainWindow::disconnectCamera);
        menu.addSeparator();
    }
    const auto cams = listCameras();
    for (const auto& line : cams) {
        const auto q = QString::fromStdString(line);
        const auto spec = q.section(' ', 0, 0, QString::SectionSkipEmpty);
        auto* a = menu.addAction(q.simplified());
        if (spec.startsWith("(") || cam_) a->setEnabled(false);
        else connect(a, &QAction::triggered, this, [this, spec] { connectCamera(spec); });
    }
    if (cams.empty()) menu.addAction("No cameras found")->setEnabled(false);
    menu.addSeparator();
    if (!cam_) {
        menu.addAction("Replay a recording…", this, [this] {
            const auto f = QFileDialog::getOpenFileName(this, "Replay a SER recording as a camera", outDir_, "SER video (*.ser)");
            if (!f.isEmpty()) connectCamera("sim:" + f);
        });
    }
    menu.exec(cameraPill_->mapToGlobal(QPoint(0, cameraPill_->height() + 4)));
}

void MainWindow::connectCamera(const QString& spec) {
    if (cam_) disconnectCamera();
    try {
        cam_ = openCamera(spec.trimmed().toStdString());
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "SlopCap", QString("Could not open %1:\n%2").arg(spec, e.what()));
        return;
    }
    camSpec_ = spec;
    roiCentre_.reset();
    restoreCameraSettings();
    populateCameraUi();
    startSession(false);
}

void MainWindow::disconnectCamera() {
    if (recording_ && QMessageBox::question(this, "SlopCap", "A recording is running. Stop it and disconnect?") != QMessageBox::Yes) return;
    stopSession();
    saveSettings();
    cam_.reset();
    populateCameraUi();
}

void MainWindow::populateCameraUi() {
    const bool have = static_cast<bool>(cam_);
    noCamera_->setVisible(!have);
    cameraBox_->setVisible(have);
    record_->setEnabled(have);
    theme::setDot(cameraDot_, have ? theme::kGood : theme::kGrey);
    if (!have) {
        cameraPill_->setText("No camera · connect…");
        return;
    }
    const auto& info = cam_->info();
    QString pill = QString::fromStdString(info.model);
    if (auto t = cam_->control("Temperature")) pill += QString(" · %1 °C").arg(info.vendor == "ZWO" ? *t / 10 : *t, 0, 'f', 1);
    cameraPill_->setText(pill.size() > 34 ? pill.left(32) + "…" : pill);
    cameraPill_->setToolTip(QString::fromStdString(std::format("{} {} · {}×{} · {:.2f} µm · {}", info.vendor, info.model, info.sensorWidth,
                                                               info.sensorHeight, info.pixelUm, bayerName(info.bayer))));
    {
        const QSignalBlocker b1(roiPreset_), b2(highSpeed_), b3(readout_);
        roiPreset_->clear();
        roiPreset_->addItem(QString("Full %1 × %2").arg(info.sensorWidth).arg(info.sensorHeight), QSize(info.sensorWidth, info.sensorHeight));
        for (QSize s : {QSize(1920, 1080), QSize(1280, 960), QSize(1040, 560), QSize(1024, 768), QSize(800, 600), QSize(640, 480), QSize(320, 240)})
            if (s.width() < info.sensorWidth && s.height() < info.sensorHeight) roiPreset_->addItem(QString("%1 × %2").arg(s.width()).arg(s.height()), s);
        const Roi cur = cam_->roi();
        bool listed = false;
        for (int i = 0; i < roiPreset_->count(); ++i)
            if (roiPreset_->itemData(i).toSize() == QSize(cur.width, cur.height)) roiPreset_->setCurrentIndex(i), listed = true;
        if (!listed) {
            roiPreset_->addItem(QString("%1 × %2").arg(cur.width).arg(cur.height), QSize(cur.width, cur.height));
            roiPreset_->setCurrentIndex(roiPreset_->count() - 1);
        }
        for (int b : {8, 16}) bits_->button(b)->setVisible(std::find(info.bitDepths.begin(), info.bitDepths.end(), b) != info.bitDepths.end());
        if (auto* b = bits_->button(cam_->bitDepth())) b->setChecked(true);
        const auto modes = cam_->readoutModes();
        highSpeed_->setVisible(modes.size() == 2);
        readout_->setVisible(modes.size() > 2);
        if (modes.size() == 2) {
            highSpeed_->setText(QString::fromStdString(modes[1].name));
            highSpeed_->setChecked(cam_->readoutMode() == 1);
        }
        readout_->clear();
        for (const auto& m : modes) readout_->addItem(QString::fromStdString(m.name), m.index);
        readout_->setCurrentIndex(std::max(0, readout_->findData(cam_->readoutMode())));
    }
    populateControls();
    updateRoiInfo();
}

void MainWindow::populateControls() {
    while (controlsForm_->rowCount() > 0) controlsForm_->removeRow(0);
    controlBoxes_.clear();
    std::optional<ControlInfo> gainInfo, expInfo;
    for (const auto& c : cam_->controls()) {
        if (!c.writable) continue;
        if (c.name == "Gain") gainInfo = c;
        if (c.name == "Exposure") expInfo = c;
        auto* box = new QDoubleSpinBox;
        box->setRange(c.min, c.max);
        const double step = c.step > 0 ? c.step : 1;
        box->setSingleStep(step);
        box->setDecimals(step < 1 ? 3 : 0);
        box->setKeyboardTracking(false);
        box->setProperty("role", "mono");
        if (c.name == "Exposure") box->setSuffix(" µs");   // ZWO and QHY both take exposure in microseconds
        if (auto val = cam_->control(c.name)) box->setValue(*val);
        const std::string name = c.name;
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, name](double val) {
            setControl(name, val);
            syncControlWidgets(name, val);
        });
        controlsForm_->addRow(theme::label(QString::fromStdString(c.name), "muted"), box);
        controlBoxes_[c.name] = box;
    }
    gain_->setEnabled(gainInfo.has_value());
    if (gainInfo) {
        const QSignalBlocker b(gain_);
        gain_->setRange(static_cast<int>(gainInfo->min), static_cast<int>(gainInfo->max));
        const int g = static_cast<int>(cam_->control("Gain").value_or(gainInfo->min));
        gain_->setValue(g);
        gainValue_->setText(QString::number(g));
    }
    exposure_->setEnabled(expInfo.has_value());
    if (expInfo) {
        expMin_ = std::max(1.0, expInfo->min);
        expMax_ = std::clamp(expInfo->max, expMin_ * 2, 60e6);
        expRange_->setText(QString("%1 – %2").arg(formatExposure(expMin_), formatExposure(expMax_)));
        syncControlWidgets("Exposure", cam_->control("Exposure").value_or(10000));
    }
    // gain landmarks as one-click presets under the gain slider
    auto* gl = gainPresets_->layout();
    while (auto* item = gl->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    std::vector<std::pair<double, std::optional<double>>> seen;   // drivers repeat values under different names
    for (const auto& m : cam_->gainLandmarks()) {
        if (!m.gain) continue;
        if (std::find(seen.begin(), seen.end(), std::pair{*m.gain, m.offset}) != seen.end()) continue;
        seen.emplace_back(*m.gain, m.offset);
        auto* b = theme::button(QString("%1 %2").arg(landmarkShort(m.kind)).arg(*m.gain));
        b->setStyleSheet(QString("QPushButton { background: transparent; border: 1px solid %1; border-radius: 10px; padding: 2px 8px; color: %2; "
                                 "font-size: 11px; min-height: 0; }")
                             .arg(theme::kControlLine.name(), theme::kMuted.name()));
        b->setToolTip(QString::fromStdString(m.label) + (m.offset ? QString(" · offset %1").arg(*m.offset) : QString()));
        connect(b, &QPushButton::clicked, this, [this, g = *m.gain, o = m.offset] {
            gain_->setValue(static_cast<int>(g));
            if (o)
                if (auto it = controlBoxes_.find("Offset"); it != controlBoxes_.end()) it->second->setValue(*o);
        });
        gl->addWidget(b);
    }
}

/// Keep the slider, its label and the "All controls" box showing the same value.
void MainWindow::syncControlWidgets(const std::string& name, double value) {
    if (name == "Gain") {
        const QSignalBlocker b(gain_);
        gain_->setValue(static_cast<int>(value));
        gainValue_->setText(QString::number(static_cast<int>(value)));
    } else if (name == "Exposure") {
        const QSignalBlocker b(exposure_);
        exposure_->setValue(static_cast<int>(std::lround(1000 * std::log(std::max(value, expMin_) / expMin_) / std::log(expMax_ / expMin_))));
        expValue_->setText(formatExposure(value));
        expMaxFps_->setText(QString("max %1 fps").arg(1e6 / value, 0, 'f', value < 1e5 ? 0 : 2));
    }
    if (auto it = controlBoxes_.find(name); it != controlBoxes_.end() && it->second->value() != value) {
        const QSignalBlocker b(it->second);
        it->second->setValue(value);
    }
}

void MainWindow::setControl(const std::string& name, double value) {
    if (!cam_) return;
    syncControlWidgets(name, value);
    if (session_) {
        session_->post([name, value](ICamera& c) { c.setControl(name, value); });
        return;
    }
    try {
        cam_->setControl(name, value);
    } catch (const std::exception& e) {
        message(QString("%1: %2").arg(QString::fromStdString(name), e.what()));
    }
}

void MainWindow::applyFormat(std::optional<QSize> size, std::optional<int> bits, std::optional<int> readout) {
    if (!cam_ || recording_) return;
    stopSession();
    try {
        if (readout && *readout != cam_->readoutMode()) cam_->setReadoutMode(*readout);
        if (bits) cam_->setBitDepth(*bits);
        const auto& info = cam_->info();
        const Roi cur = cam_->roi();
        const QSize s = size.value_or(QSize(cur.width, cur.height));
        // centred (on the chosen point, else the sensor), on offsets every driver accepts (multiple of 8 across, even
        // down; keeps the Bayer phase)
        const QPoint c = roiCentre_.value_or(QPoint(info.sensorWidth / 2, info.sensorHeight / 2));
        const int x = std::clamp(c.x() - s.width() / 2, 0, info.sensorWidth - s.width()) / 8 * 8;
        const int y = std::clamp(c.y() - s.height() / 2, 0, info.sensorHeight - s.height()) / 2 * 2;
        cam_->setRoi({x, y, s.width(), s.height(), 1});
    } catch (const std::exception& e) {
        message(e.what());
    }
    populateCameraUi();
    startSession(false);
}

void MainWindow::centreRoiOn(QPointF img) {
    if (!cam_ || recording_) return;
    const Roi cur = cam_->roi();
    roiCentre_ = QPoint(cur.x + static_cast<int>(img.x()), cur.y + static_cast<int>(img.y()));
    const auto& info = cam_->info();
    QSize size(cur.width, cur.height);
    if (cur.width >= info.sensorWidth && cur.height >= info.sensorHeight) size = QSize(1040, 560);   // full frame can't move
    applyFormat(size);
}

void MainWindow::updateRoiInfo() {
    if (!cam_) return;
    const Roi r = cam_->roi();
    const auto& info = cam_->info();
    roiSize_->setText(QString("%1 × %2").arg(r.width).arg(r.height));
    roiOffset_->setText(QString("offset %1, %2 · sensor %3 × %4").arg(r.x).arg(r.y).arg(info.sensorWidth).arg(info.sensorHeight));
    sFrame_->setText(QString("%1×%2 · %3-bit").arg(r.width).arg(r.height).arg(cam_->bitDepth()));
}

void MainWindow::updateTargetInfo() {
    const auto& l = focus_->live();
    const auto& s = l.last;
    if (s.channels == 0) {
        for (auto* lab : {tCentre_, tMargin_, tDisc_, tDisp_}) lab->setText("—");
        sSharp_->setText("—");
        sDisp_->setText("—");
        preview_->setOverlay("target", {});
        return;
    }
    const int c = s.channels == 3 ? 1 : 0;
    const double cx = s.ch[static_cast<std::size_t>(c)].cx, cy = s.ch[static_cast<std::size_t>(c)].cy;
    tCentre_->setText(QString("%1, %2").arg(cx, 0, 'f', 0).arg(cy, 0, 'f', 0));
    const int margin = std::min({s.targetX0, s.targetY0, l.frameWidth - s.targetX1, l.frameHeight - s.targetY1});
    const double frac = static_cast<double>(margin) / std::max(1, std::min(l.frameWidth, l.frameHeight));
    tMargin_->setText(QString("%1 px").arg(std::max(0, margin)));
    tMargin_->setStyleSheet(QString("color: %1").arg((frac > 0.1 ? theme::kGood : frac > 0.03 ? theme::kAccent : theme::kRecText).name()));
    tDisc_->setText(QString("%1 × %2 px").arg(s.targetX1 - s.targetX0).arg(s.targetY1 - s.targetY0));
    const QString disp = s.channels == 3 ? QString("%1, %2 px").arg(s.ch[0].cx - s.ch[2].cx, 0, 'f', 1).arg(s.ch[0].cy - s.ch[2].cy, 0, 'f', 1) : "—";
    tDisp_->setText(disp);
    sDisp_->setText(disp);
    sSharp_->setText(QString("%1").arg(l.fraction[static_cast<std::size_t>(c)], 0, 'f', 2));
    if (mode_ == Collimate || !targetBox_->isChecked()) {
        preview_->setOverlay("target", {});
    } else {
        const QRectF box(s.targetX0, s.targetY0, s.targetX1 - s.targetX0, s.targetY1 - s.targetY0);
        const QString text = QString("%1 · %2, %3 · %4 × %5 px")
                                 .arg(target_->currentText())
                                 .arg(cx, 0, 'f', 0)
                                 .arg(cy, 0, 'f', 0)
                                 .arg(s.targetX1 - s.targetX0)
                                 .arg(s.targetY1 - s.targetY0);
        preview_->setOverlay("target", [box, text](QPainter& p, double k) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(theme::kAccent, 1 / k, Qt::DashLine));
            p.drawRoundedRect(box, 4 / k, 4 / k);
            // label above the box, constant size on screen
            p.save();
            const QPointF at = p.transform().map(box.topLeft());
            p.resetTransform();
            QFont f = theme::monoFont();
            f.setPixelSize(11);
            p.setFont(f);
            const QFontMetricsF fm(f);
            const QRectF tag(at.x(), at.y() - 22, fm.horizontalAdvance(text) + 12, 18);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(13, 15, 18, 210));
            p.drawRoundedRect(tag, 3, 3);
            p.setPen(theme::kAccent);
            p.drawText(tag, Qt::AlignCenter, text);
            p.restore();
        });
    }
}

// ================================================================ settings

QString MainWindow::cameraKey() const {
    const auto& info = cam_->info();
    return sanitize(QString::fromStdString(info.vendor + "-" + info.model + "-" + info.serial));
}

void MainWindow::saveSettings() const {
    QSettings st;
    st.setValue("window/geometry", saveGeometry());
    st.setValue("record/dir", outDir_);
    st.setValue("record/target", target_->currentText());
    st.setValue("record/filter", filter_->currentText());
    st.setValue("record/limit", limit_->currentData().toInt());
    if (!camSpec_.isEmpty()) st.setValue("camera/last", camSpec_);
    st.setValue("focuser/server", settings_.indiServer);
    st.setValue("focuser/autoConnect", settings_.focuserAutoConnect);
    st.setValue("focuser/backlash", settings_.backlash);
    st.setValue("focuser/outward", settings_.approachOutward);
    st.setValue("mount/pulseMs", mountSection_->pulseMs());
    st.setValue("optics/focalRatio", settings_.focalRatio);
    st.setValue("optics/micronsPerStep", settings_.micronsPerStep);
    st.setValue("focus/metric", analysis::metricName(focus_->liveMetric()));
    if (!cam_) return;
    st.beginGroup("cameras/" + cameraKey());
    const Roi r = cam_->roi();
    st.setValue("roi", QRect(r.x, r.y, r.width, r.height));
    st.setValue("bits", cam_->bitDepth());
    st.setValue("readout", cam_->readoutMode());
    for (const auto& [name, box] : controlBoxes_) st.setValue("controls/" + QString::fromStdString(name), box->value());
    st.endGroup();
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
        message(QString("Saved format not accepted: %1").arg(e.what()));
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

void MainWindow::setOutputDir(const QString& dir) {
    outDir_ = dir;
    focus_->setOutputDir(dir);
    updateStats();
}

// ================================================================ recording

QString MainWindow::fileStem(const QDateTime& utc) const {
    QString stem = winjupos(utc);
    if (!filter_->currentText().trimmed().isEmpty()) stem += "-" + sanitize(filter_->currentText().trimmed());
    return stem + "-" + sanitize(target_->currentText().trimmed().isEmpty() ? "capture" : target_->currentText().trimmed());
}

void MainWindow::startSession(bool record) {
    if (!cam_) return;
    CaptureOptions opt;
    opt.preview = &tap_;
    if (record) {
        QDir dir(outDir_);
        if (!dir.mkpath(".")) {
            QMessageBox::warning(this, "SlopCap", "Can't create " + outDir_);
            record = false;
        } else {
            recStartUtc_ = QDateTime::currentDateTimeUtc();
            recordingPath_ = dir.filePath(fileStem(recStartUtc_) + ".ser");
            recControls_.clear();
            for (const auto& [name, box] : controlBoxes_) recControls_[name] = box->value();
            opt.output = recordingPath_.toStdString();
            opt.directIO = true;
            if (const int lim = limit_->currentData().toInt(); lim > 0) opt.duration = std::chrono::seconds(lim);
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
    for (auto* w : std::initializer_list<QWidget*>{roiPreset_, roiCentreBtn_, roiMax_, bitsBox_, highSpeed_, readout_}) w->setEnabled(!recording_);
    record_->setProperty("variant", recording_ ? "stop" : "record");
    record_->setText(recording_ ? "■  Stop · 00:00" : "●  Record");
    theme::restyle(record_);
    updateRoiInfo();
    updateStats();
}

void MainWindow::stopSession() {
    preview_->setSource(nullptr, Bayer::None);
    if (!session_) return;
    session_->stop();
    const auto s = session_->stats();
    if (recording_ && s.written > 0) {
        // WinJUPOS wants the mid-capture time in the name: rename now that the length is known.
        const QDateTime mid = recStartUtc_.addMSecs(static_cast<qint64>(s.seconds * 500));
        const QString named = QFileInfo(recordingPath_).path() + "/" + fileStem(mid) + ".ser";
        if (named != recordingPath_ && !QFileInfo::exists(named) && QFile::rename(recordingPath_, named)) recordingPath_ = named;
        writeSidecar(recordingPath_, s, mid);
        message(QString::fromStdString(std::format("Saved {} frames ({:.1f} s), dropped {} / {}{} → ", s.written, s.seconds, s.cameraDropped,
                                                   s.ringDropped, s.error.empty() ? "" : " — " + s.error)) +
                QFileInfo(recordingPath_).fileName());
    }
    session_.reset();
    recording_ = false;
    record_->setProperty("variant", "record");
    record_->setText("●  Record");
    theme::restyle(record_);
    recBadge_->hide();
}

void MainWindow::toggleRecording() {
    if (!cam_) return;
    const bool start = !recording_;
    stopSession();
    startSession(start);
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
    o << "Target=" << target_->currentText() << "\n";
    if (!filter_->currentText().isEmpty()) o << "Filter=" << filter_->currentText() << "\n";
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
    const auto modes = cam_->readoutModes();
    if (!modes.empty()) o << "Readout=" << QString::fromStdString(modes[static_cast<std::size_t>(std::clamp(cam_->readoutMode(), 0, static_cast<int>(modes.size()) - 1))].name) << "\n";
    for (const auto& [name, val] : recControls_) o << QString::fromStdString(name) << "=" << val << "\n";
    if (focus_->focuser().ready()) o << "Focuser position=" << focus_->focuser().position() << "\n";
    if (mount_->ready())
        o << "Mount RA(JNow)=" << QString::number(mount_->ra(), 'f', 5) << " h\nMount Dec(JNow)=" << QString::number(mount_->dec(), 'f', 4) << " deg\n";
    if (!s.error.empty()) o << "Error=" << QString::fromStdString(s.error) << "\n";
}

// ================================================================ status

void MainWindow::updateStats() {
    if (!fState_) return;
    // next file name, as it will be at the start (renamed to mid-capture when it ends)
    if (nextFile_ && !recording_) {
        // ~ for home, and a break opportunity after each separator so a long path wraps inside the column
        QString path = QDir::toNativeSeparators(QDir(outDir_).filePath(fileStem(QDateTime::currentDateTimeUtc()) + ".ser"));
        const QString home = QDir::toNativeSeparators(QDir::homePath());
        if (path.startsWith(home)) path = "~" + path.mid(home.size());
        nextFile_->setText(path.replace(QDir::separator(), QString(QDir::separator()) + QChar(0x200B)));
    }
    if (folder_) folder_->setText(QFileInfo(outDir_).fileName().isEmpty() ? "Folder…" : QFileInfo(outDir_).fileName());
    headerInfo_->setText(mode_ == Focus ? (focus_->sweeping() ? "Sweep running · scored per colour channel" : "Focus sweep · scored per colour channel")
                         : mode_ == Collimate ? "Defocused-star collimation"
                                              : "");
    const QStorageInfo disk(QDir(outDir_).exists() ? outDir_ : QDir::homePath());
    const double freeTB = static_cast<double>(disk.bytesAvailable()) / 1e12;
    const QString freeText = freeTB >= 1 ? QString("%1 TB free").arg(freeTB, 0, 'f', 1) : QString("%1 GB free").arg(freeTB * 1000, 0, 'f', 0);
    if (!session_) {
        fState_->setText(cam_ ? "○ Stopped" : "○ No camera");
        fState_->setStyleSheet("");
        for (auto* l : {fFps_, fDropped_, fRing_, fFrames_, fDisplay_}) l->clear();
        fDisk_->setText(freeText);
        return;
    }
    const auto s = session_->stats();
    if (!s.running && recording_) {   // hit the length limit, or failed: back to live view
        stopSession();
        startSession(false);
        return;
    }
    if (!s.error.empty()) message("Error: " + QString::fromStdString(s.error));
    else if (!s.notice.empty() && fMessage_->text().isEmpty()) message(QString::fromStdString(s.notice));
    fState_->setText(recording_ ? "● Recording" : "○ Previewing");
    fState_->setStyleSheet(recording_ ? QString("color: %1; font-weight: 600;").arg(theme::kRecText.name()) : "");
    fFps_->setText(QString("<span style='color:%1'>%2</span> fps").arg(theme::kText.name()).arg(s.captureFps, 0, 'f', 1));
    const uint64_t dropped = s.cameraDropped + s.ringDropped;
    fDropped_->setText(dropped ? QString("%1 dropped (%2 camera, %3 disk)").arg(dropped).arg(s.cameraDropped).arg(s.ringDropped) : "0 dropped");
    fDropped_->setStyleSheet(QString("color: %1").arg((dropped ? theme::kRecText : theme::kGood).name()));
    if (recording_) {
        const double minutes = s.writeMBps > 1 ? static_cast<double>(disk.bytesAvailable()) / 1e6 / s.writeMBps / 60 : 0;
        fRing_->setText(QString("buffer %1% · %2 / %3")
                            .arg(s.ringCapacity ? 100 * s.ringQueued / s.ringCapacity : 0)
                            .arg(thousands(static_cast<double>(s.ringQueued)), thousands(static_cast<double>(s.ringCapacity))));
        fDisk_->setText(QString("disk %1 MB/s · %2 · %3 min").arg(s.writeMBps, 0, 'f', 0).arg(freeText).arg(minutes, 0, 'f', 0));
        fFrames_->setText(QString("%1 frames").arg(thousands(static_cast<double>(s.written))));
        const int lim = limit_->currentData().toInt();
        const QString t = mmss(s.seconds) + (lim > 0 ? " / " + mmss(lim) : QString());
        record_->setText("■  Stop · " + mmss(s.seconds));
        recBadge_->setText("● REC " + t);
        recBadge_->adjustSize();
        recBadge_->show();
        recBadge_->raise();
    } else {
        fRing_->clear();
        fDisk_->setText(freeText);
        fFrames_->clear();
    }
    fDisplay_->setText(QString("preview %1 fps · GPU debayer").arg(preview_->displayFps(), 0, 'f', 0));
}

void MainWindow::printSummary() const {
    std::string line = (fState_->text() + " · " + fFps_->text() + " · " + fDropped_->text() + " · " + fDisk_->text() + " · " + fMessage_->text())
                           .toStdString();
    std::fputs((line + "\n").c_str(), stdout);
    std::fflush(stdout);
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

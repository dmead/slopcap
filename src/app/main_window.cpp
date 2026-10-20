#include "app/main_window.hpp"

#include "analysis/identify.hpp"
#include "app/collimation_panel.hpp"
#include "app/focus_controller.hpp"
#include "app/focus_views.hpp"
#include "app/full_view_bar.hpp"
#include "app/mount_section.hpp"
#include "app/raw_histogram.hpp"
#include "app/theme.hpp"
#include "camera/open_camera.hpp"
#include "net/discovery.hpp"
#include "net/frame_codec.hpp"
#include "net/remote_client.hpp"
#include "net/remote_server.hpp"
#include "preview/histogram_widget.hpp"
#include "preview/preview_widget.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCursor>
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
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
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

/// The exposure sliders' 0..1000 position <-> microseconds, log scale between the camera's limits.
double sliderToExposure(int pos, double lo, double hi) { return std::round(lo * std::pow(hi / lo, pos / 1000.0)); }
int exposureToSlider(double us, double lo, double hi) { return static_cast<int>(std::lround(1000 * std::log(std::max(us, lo) / lo) / std::log(hi / lo))); }

QString badgeStyle(const QColor& border, const QColor& text) {
    return QString("background: rgba(13,15,18,210); border: 1px solid %1; border-radius: 4px; color: %2; padding: 3px 8px; "
                   "font-family: '%3'; font-size: 12px; font-weight: 600;")
        .arg(border.name(), text.name(), theme::monoFont().family());
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
    logDevices();
    preview_ = new preview::PreviewWidget;
    collim_ = new CollimationPanel(preview_, this);

    auto* root = new QWidget;
    root->setObjectName("ground");
    auto* v = new QVBoxLayout(root);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    header_ = buildHeader();
    v->addWidget(header_);

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
    footer_ = buildFooter();
    v->addWidget(footer_);
    setCentralWidget(root);

    // full view: the preview over a whole screen, with a reduced control bar that hides while the mouse rests
    fullBar_ = new FullViewBar(preview_);
    connect(fullBar_->gain, &QSlider::valueChanged, this, [this](int g) { setControl("Gain", g); });
    connect(fullBar_->exposure, &QSlider::valueChanged, this, [this](int pos) { setControl("Exposure", sliderToExposure(pos, expMin_, expMax_)); });
    connect(fullBar_->stretch, &QButtonGroup::idClicked, this, [this](int id) { stretch_->button(id)->click(); });
    connect(fullBar_->record, &QPushButton::clicked, this, &MainWindow::toggleRecording);
    connect(fullBar_->exit, &QPushButton::clicked, this, [this] { setFullView(false); });
    fullIdle_.setSingleShot(true);
    connect(&fullIdle_, &QTimer::timeout, this, [this] {
        if (!fullView_) return;
        if (fullBar_->underMouse()) {
            fullIdle_.start(3000);
            return;
        }
        fullBar_->hide();
        preview_->setCursor(Qt::BlankCursor);
    });
    escape_ = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escape_->setEnabled(false);
    connect(escape_, &QShortcut::activated, this, [this] { setFullView(false); });
    auto* f11 = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(f11, &QShortcut::activated, this, [this] { setFullView(!fullView_); });
    preview_->installEventFilter(this);

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
        // the readout histogram: up to 10 times a second, while the Capture sidebar shows it
        if (mode_ == Capture && (!rawHistClock_.isValid() || rawHistClock_.elapsed() >= 100)) {
            rawHistClock_.start();
            rawHist_->setData(preview::rawHistogram(f.pixels, f.info.roi.width, f.info.roi.height, f.info.bitDepth,
                                                    cam_->info().bayer, cam_->info().adcBits));
        }
        // the automatic target: once a second, and not while recording (the file keeps the name it started with)
        if (isAutoTarget() && !recording_ && (!identifyClock_.isValid() || identifyClock_.elapsed() >= 1000)) {
            identifyClock_.start();
            runAutoTarget(f);
        }
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
    connect(&statsTimer_, &QTimer::timeout, this, [this] {
        updateStats();
        syncFullBar();
        if (server_ && ++statusTick_ % 2 == 0) server_->sendStatus(remoteStatus());   // twice a second
    });
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
    target_->setCurrentText(st.value("record/target", "Auto").toString());
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
    s.focalLengthMm = st.value("optics/focalLengthMm", 0.0).toDouble();
    if (st.contains("site/latitude") && st.contains("site/longitude"))
        s.latitude = st.value("site/latitude").toDouble(), s.longitude = st.value("site/longitude").toDouble();
    applySettings(s);
    focus_->setOutputDir(outDir_);
    if (s.focuserAutoConnect) focus_->connectFocuser(s.indiServer);
    populateCameraUi();
    updateAutoTarget();
    setMode(Capture);
}

MainWindow::~MainWindow() {
    stopSession();
    cam_.reset();
    delete server_;   // before remoteTap_ goes: its encoder thread reads it
    server_ = nullptr;
    delete remote_;
    remote_ = nullptr;
}

void MainWindow::applySettings(const Settings& s) {
    settings_ = s;
    qInfo().noquote() << "settings: INDI" << s.indiServer << (s.focuserAutoConnect ? "(connect at start)" : "") << "· backlash" << s.backlash
                      << (s.approachOutward ? "outward" : "inward") << "· f/" << s.focalRatio << "·" << s.micronsPerStep << "um/step ·"
                      << s.focalLengthMm << "mm · site" << (s.latitude ? QString("%1, %2").arg(*s.latitude).arg(*s.longitude) : QString("unset"))
                      << "· folder" << outDir_;
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
    combo("Target", target_, {"Auto", "Saturn", "Jupiter", "Mars", "Moon", "Sun", "Venus", "Uranus", "Neptune", "Star"});
    combo("Filter", filter_, {"", "L", "R", "G", "B", "IR", "CH4", "UV"});
    target_->setToolTip("Used in file names (WinJUPOS style) and sidecars. Auto: the Moon, Mars or an outer planet, "
                        "identified from the image and the mount");
    connect(target_, &QComboBox::currentTextChanged, this, [this] {
        autoName_.clear();
        pendingAutoName_.clear();
        identifyClock_.invalidate();
        updateAutoTarget();
    });
    filter_->setToolTip("Optional: goes into the file name, e.g. 2026-10-17-0315_4-L-Saturn.ser");
    for (auto* c : {target_, filter_}) connect(c, &QComboBox::currentTextChanged, this, &MainWindow::updateStats);

    const QString pillStyle = QString("QPushButton { border: 1px solid %1; border-radius: 13px; padding: 4px 12px 4px %5px; color: %2; font-family: '%3'; "
                                      "font-size: 12px; background: transparent; } QPushButton:hover { border-color: %4; }")
                                  .arg(theme::kLine.name(), theme::kMuted.name(), theme::monoFont().family(), theme::kDim.name());
    remotePill_ = new QPushButton("⇄ Remote");
    remotePill_->setCursor(Qt::PointingHandCursor);
    remotePill_->setStyleSheet(pillStyle.arg(12));
    remotePill_->setToolTip("Share this camera with other SlopCaps on the network, or watch one that shares");
    connect(remotePill_, &QPushButton::clicked, this, &MainWindow::showRemoteMenu);
    h->addWidget(remotePill_);

    cameraPill_ = new QPushButton;
    cameraPill_->setCursor(Qt::PointingHandCursor);
    cameraPill_->setStyleSheet(pillStyle.arg(24));
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
    mk(fRemote_);
    fMessage_ = theme::label("", "muted");
    h->addWidget(fMessage_, 1);
    mk(fDisplay_);
    return f;
}

void MainWindow::message(const QString& text) {
    if (text != fMessage_->text()) qInfo().noquote() << "ui:" << text;   // (errors are re-shown every stats tick)
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
        const double us = sliderToExposure(pos, expMin_, expMax_);
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
        roiCentreDirty_ = true;
        applyFormat(roiPreset_->currentData().toSize());
    });
    connect(roiMax_, &QPushButton::clicked, this, [this] {
        if (!cam_) return;
        roiCentre_.reset();
        roiCentreDirty_ = true;
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
    tAuto_ = theme::label("", "muted");   // the automatic target's answer and why
    tAuto_->setWordWrap(true);
    v->addWidget(tAuto_);
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
    rawHist_ = new RawHistogramSection;
    v->addWidget(rawHist_);
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
    toolbar_ = bar;
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
    auto* full = theme::button("Full screen");
    full->setToolTip("The preview over a whole screen, e.g. a projector, with gain, exposure and record in a bar that hides "
                     "itself (F11; Esc to leave)");
    connect(full, &QPushButton::clicked, this, &MainWindow::showFullViewMenu);
    bh->addWidget(full);
    v->addWidget(bar);

    auto* holder = new QWidget;
    previewHolder_ = holder;
    auto* hl = new QVBoxLayout(holder);
    hl->setContentsMargins(20, 20, 20, 20);
    hl->addWidget(preview_);
    v->addWidget(holder, 1);
    recBadge_ = new QLabel(preview_);
    recBadge_->setStyleSheet(badgeStyle(theme::kRec, theme::kRecText));
    recBadge_->move(12, 12);
    recBadge_->hide();
    remoteBadge_ = new QLabel(preview_);   // top right: a remote link is active (placed in eventFilter)
    remoteBadge_->hide();

    auto* strip = new QFrame;
    strip_ = strip;
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
    if (remote_) {
        menu.addAction(QString("Disconnect from %1").arg(remote_->serverName()), this, &MainWindow::disconnectRemote);
        menu.exec(cameraPill_->mapToGlobal(QPoint(0, cameraPill_->height() + 4)));
        return;
    }
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
        qWarning().noquote() << "camera: could not open" << spec << "-" << e.what();
        QMessageBox::warning(this, "SlopCap", QString("Could not open %1:\n%2").arg(spec, e.what()));
        return;
    }
    {
        const auto& i = cam_->info();
        qInfo().noquote() << "camera: opened" << spec << "·" << QString::fromStdString(i.vendor + " " + i.model) << "serial"
                          << QString::fromStdString(i.serial) << "·" << i.sensorWidth << "x" << i.sensorHeight << "·" << i.pixelUm << "um ·"
                          << "ADC" << (i.adcBits ? QString::number(*i.adcBits) : QString("?")) << "bits ·" << bayerName(i.bayer);
    }
    camSpec_ = spec;
    roiCentre_.reset();
    restoreCameraSettings();
    populateCameraUi();
    startSession(false);
}

void MainWindow::disconnectCamera() {
    if (remote_) {
        disconnectRemote();
        return;
    }
    if (recording_ && QMessageBox::question(this, "SlopCap", "A recording is running. Stop it and disconnect?") != QMessageBox::Yes) return;
    stopSession();
    saveSettings();
    cam_.reset();
    rawHist_->clear();
    autoName_.clear();
    autoDetail_.clear();
    updateAutoTarget();
    populateCameraUi();
}

void MainWindow::populateCameraUi() {
    const bool have = static_cast<bool>(cam_);
    noCamera_->setVisible(!have);
    cameraBox_->setVisible(have);
    record_->setEnabled(have);
    folder_->setEnabled(!remote_);
    limit_->setEnabled(!remote_);
    theme::setDot(cameraDot_, have ? theme::kGood : theme::kGrey);
    // what viewers of this camera are told (read here: the camera isn't streaming while the UI is rebuilt)
    cameraState_ = have && !remote_ ? net::cameraStateToJson(net::snapshot(*cam_)) : QJsonObject();
    if (server_) server_->setCameraName(have && !remote_ ? QString::fromStdString(cam_->info().model) : QString());
    noCamera_->setText(remote_ ? QString("%1 has no camera connected.").arg(remote_->serverName())
                               : "No camera connected. Click the camera pill at the top right to pick one.");
    if (!have) {
        cameraPill_->setText(remote_ ? QString("%1 · no camera").arg(remote_->serverName()) : "No camera · connect…");
        if (server_) server_->setState(remoteCameraState());
        return;
    }
    const auto& info = cam_->info();
    QString pill = (remote_ ? remote_->serverName() + " · " : QString()) + QString::fromStdString(info.model);
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
    if (server_) server_->setState(remoteCameraState());   // after populateControls: the values come from its boxes
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
    fullBar_->gain->setEnabled(gainInfo.has_value());
    fullBar_->exposure->setEnabled(expInfo.has_value());
    if (gainInfo) {
        const QSignalBlocker b(gain_), b2(fullBar_->gain);
        gain_->setRange(static_cast<int>(gainInfo->min), static_cast<int>(gainInfo->max));
        fullBar_->gain->setRange(gain_->minimum(), gain_->maximum());
        const int g = static_cast<int>(cam_->control("Gain").value_or(gainInfo->min));
        gain_->setValue(g);
        gainValue_->setText(QString::number(g));
        fullBar_->gain->setValue(g);
        fullBar_->gainValue->setText(QString::number(g));
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
        const QSignalBlocker b(gain_), b2(fullBar_->gain);
        gain_->setValue(static_cast<int>(value));
        gainValue_->setText(QString::number(static_cast<int>(value)));
        fullBar_->gain->setValue(static_cast<int>(value));
        fullBar_->gainValue->setText(gainValue_->text());
    } else if (name == "Exposure") {
        const QSignalBlocker b(exposure_), b2(fullBar_->exposure);
        exposure_->setValue(exposureToSlider(value, expMin_, expMax_));
        fullBar_->exposure->setValue(exposure_->value());
        expValue_->setText(formatExposure(value));
        fullBar_->expValue->setText(expValue_->text());
        expMaxFps_->setText(QString("max %1 fps").arg(1e6 / value, 0, 'f', value < 1e5 ? 0 : 2));
    }
    if (auto it = controlBoxes_.find(name); it != controlBoxes_.end() && it->second->value() != value) {
        const QSignalBlocker b(it->second);
        it->second->setValue(value);
    }
}

void MainWindow::setControl(const std::string& name, double value) {
    if (!cam_) return;
    qInfo().noquote() << "control:" << QString::fromStdString(name) << "=" << value << (controlOrigin_ ? "(from a viewer)" : "");
    syncControlWidgets(name, value);
    if (server_ && !remote_) server_->broadcastControl(QString::fromStdString(name), value, controlOrigin_);
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
    if (remote_) {   // the server applies it and sends the new State
        QJsonObject req;
        if (size) req["width"] = size->width(), req["height"] = size->height();
        if (bits) req["bits"] = *bits;
        if (readout) req["readout"] = *readout;
        if (roiCentreDirty_) req["centre"] = roiCentre_ ? QJsonValue(QJsonArray{roiCentre_->x(), roiCentre_->y()}) : QJsonValue(QJsonValue::Null);
        roiCentreDirty_ = false;
        qInfo().noquote() << "format: asking" << remote_->serverName() << "for" << QJsonDocument(req).toJson(QJsonDocument::Compact);
        remote_->sendFormat(req);
        return;
    }
    qInfo().noquote() << "format:" << (size ? QString("%1x%2").arg(size->width()).arg(size->height()) : QString("size same")) << "·"
                      << (bits ? QString("%1-bit").arg(*bits) : QString("bits same")) << "·"
                      << (readout ? QString("readout %1").arg(*readout) : QString("readout same"));
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
    roiCentreDirty_ = true;
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

// ================================================================ session log: devices

void MainWindow::logDevices() {
    auto& indi = focus_->indi();
    auto text = [&indi](const QString& dev, const QString& name) {
        QStringList parts;
        if (const auto* p = indi.property(dev, name))
            for (const auto& e : p->order) parts << e + "=" + p->elements.at(e).text;
        return parts.join(' ');
    };
    connect(&indi, &devices::IndiClient::serverConnected, this, [this] { qInfo().noquote() << "indi: connected to" << focus_->server(); });
    connect(&indi, &devices::IndiClient::serverDisconnected, this, [] { qWarning().noquote() << "indi: disconnected"; });
    connect(&indi, &devices::IndiClient::errorOccurred, this, [](const QString& e) { qWarning().noquote() << "indi: error:" << e; });
    // Driver messages carry the reasons things fail ("Failed to connect to port /dev/ttyUSB0", "Error reading ...").
    connect(&indi, &devices::IndiClient::message, this, [](const QString& dev, const QString& m) { qInfo().noquote() << "indi:" << dev << "says:" << m; });
    connect(&indi, &devices::IndiClient::propertyDefined, this, [&indi, text](const QString& dev, const QString& name) {
        if (name == "DRIVER_INFO" || name == "DEVICE_PORT" || name == "CONNECTION")
            qInfo().noquote() << "indi:" << dev << name << text(dev, name);
    });
    connect(&indi, &devices::IndiClient::propertyUpdated, this, [&indi, text](const QString& dev, const QString& name) {
        const auto* p = indi.property(dev, name);
        if (!p) return;
        if (p->state == "Alert") qWarning().noquote() << "indi:" << dev << name << "Alert" << text(dev, name);
        else if (name == "DEVICE_PORT" || name == "CONNECTION") qInfo().noquote() << "indi:" << dev << name << p->state << text(dev, name);
    });

    auto& f = focus_->focuser();
    connect(&f, &devices::Focuser::readyChanged, this, [this](bool ok) {
        const auto& fo = focus_->focuser();
        qInfo().noquote() << "focuser:" << (ok ? "ready" : "not ready") << fo.device() << "at" << fo.position() << "of" << fo.maxPosition()
                          << "· backlash" << fo.backlash() << (fo.approachOutward() ? "outward" : "inward");
    });
    connect(&f, &devices::Focuser::arrived, this, [](int pos) { qInfo().noquote() << "focuser: arrived at" << pos; });
    connect(&f, &devices::Focuser::failed, this, [](const QString& why) { qWarning().noquote() << "focuser: failed:" << why; });
    connect(focus_, &FocusController::sweepProgress, this, [](const QString& t) { qInfo().noquote() << "sweep:" << t; });
    connect(focus_, &FocusController::sweepStateChanged, this, [this](bool running) {
        const auto& s = focus_->sweepSettings();
        qInfo().noquote() << "sweep:" << (running ? "started" : "ended") << "· from" << s.from << "to" << s.to << "step" << s.step << "·" << s.frames
                          << "frames, settle" << s.settleMs << "ms · start position" << focus_->sweepStart();
    });

    connect(mount_, &devices::Mount::readyChanged, this, [this](bool ok) {
        qInfo().noquote() << "mount:" << (ok ? "ready" : "not ready") << mount_->device() << "· RA" << mount_->ra() << "Dec" << mount_->dec()
                          << "· pulses" << (mount_->canPulse() ? "yes" : "no");
    });
    connect(mount_, &devices::Mount::failed, this, [](const QString& why) { qWarning().noquote() << "mount: failed:" << why; });
}

// ================================================================ automatic target

bool MainWindow::isAutoTarget() const { return target_->currentText().trimmed().compare("Auto", Qt::CaseInsensitive) == 0; }

QString MainWindow::targetName() const {
    if (!isAutoTarget()) return target_->currentText().trimmed();
    return autoName_.isEmpty() ? "Target" : autoName_;
}

void MainWindow::runAutoTarget(const LatestFrame::Frame& f) {
    analysis::IdentifyInput in;
    in.image = analysis::measureTarget(f.pixels, f.info.roi.width, f.info.roi.height, f.info.bitDepth, cam_->info().bayer);
    in.jdUtc = analysis::julianDate(f.info.timestamp);
    if (settings_.focalLengthMm > 0 && cam_->info().pixelUm > 0)
        in.arcsecPerPixel = 206.265 * cam_->info().pixelUm * std::max(1, f.info.roi.bin) / settings_.focalLengthMm;
    // Not the mount: it may not be connected, and an unaligned mount's coordinates mislead.
    if (settings_.latitude && settings_.longitude) in.site = analysis::Site{*settings_.latitude, *settings_.longitude};
    const auto id = analysis::identifyTarget(in);

    // The name: the body when it's decided, "Target" when it isn't, nothing without a target.
    const QString name = id.body ? QString(analysis::bodyName(*id.body)) : id.candidates.empty() || !in.image.found ? QString() : "Target";
    QString detail = id.body ? QString::fromStdString(id.summary) : QString::fromStdString(analysis::candidatesText(id.candidates));
    if (!id.body && in.image.found) {
        QStringList need;
        if (!in.arcsecPerPixel) need << "the focal length";
        if (!in.site) need << "the site";
        if (!need.isEmpty()) need.back() += " (Settings)";
        if (!need.isEmpty()) detail += " — " + need.join(" or ") + " would help";
    }
    QStringList notes;
    for (const auto& n : id.notes) notes << QString::fromStdString(n);
    autoNotes_ = notes.join("\n");

    // A new answer must hold for two runs in a row before it replaces the old one (seeing, a passing cloud).
    const QString before = autoName_;
    if (name == autoName_ || autoName_.isEmpty()) {
        autoName_ = name;
        autoDetail_ = detail;
        pendingAutoName_.clear();
    } else if (name == pendingAutoName_) {
        autoName_ = name;
        autoDetail_ = detail;
        pendingAutoName_.clear();
    } else {
        pendingAutoName_ = name;
    }
    if (autoName_ != before)
        qInfo().noquote() << "target: auto" << (autoName_.isEmpty() ? "(none)" : autoName_) << "·" << autoDetail_ << "· features: diameter"
                          << in.image.diameterPx << "px, elongation" << in.image.elongation << ", symmetry" << in.image.symmetry << ", limb"
                          << in.image.limbRatio << ", clipped" << in.image.clipped << ", ln R/B"
                          << (analysis::colourIndex(in.image) ? QString::number(*analysis::colourIndex(in.image), 'f', 2) : "-") << "· steps:"
                          << notes.join(" | ");
    updateAutoTarget();
}

void MainWindow::updateAutoTarget() {
    if (!tAuto_) return;
    tAuto_->setVisible(isAutoTarget());
    if (!isAutoTarget()) return;
    if (!cam_) tAuto_->setText("Auto: connect a camera");
    else if (autoName_.isEmpty()) tAuto_->setText("Auto: no target yet");
    else if (autoName_ == "Target") tAuto_->setText("Auto: undecided · " + autoDetail_.toHtmlEscaped());
    else tAuto_->setText(QString("Auto: <b>%1</b>%2").arg(autoName_, autoDetail_.isEmpty() ? "" : " · " + autoDetail_.toHtmlEscaped()));
    tAuto_->setToolTip(autoNotes_);
    updateStats();
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
                                 .arg(targetName())
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
    st.setValue("optics/focalLengthMm", settings_.focalLengthMm);
    if (settings_.latitude && settings_.longitude) {
        st.setValue("site/latitude", *settings_.latitude);
        st.setValue("site/longitude", *settings_.longitude);
    } else {
        st.remove("site");
    }
    st.setValue("focus/metric", analysis::metricName(focus_->liveMetric()));
    if (!cam_ || remote_) return;
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
    return stem + "-" + sanitize(targetName().isEmpty() ? "capture" : targetName());
}

void MainWindow::startSession(bool record) {
    if (!cam_) return;
    if (remote_) record = false;   // raw files stay on the server
    CaptureOptions opt;
    opt.preview = &tap_;
    if (!remote_) opt.remote = &remoteTap_;   // copies only when a viewer's encoder asks
    if (server_) server_->setBayer(remote_ ? Bayer::None : cam_->info().bayer);
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
    if (session_) {
        const Roi r = cam_->roi();
        QStringList controls;
        for (const auto& [name, box] : controlBoxes_) controls << QString("%1=%2").arg(QString::fromStdString(name)).arg(box->value());
        qInfo().noquote() << (recording_ ? "record: start" : "preview: start") << (recording_ ? recordingPath_ : QString()) << "· ROI"
                          << r.width << "x" << r.height << "+" << r.x << "," << r.y << "bin" << r.bin << "·" << cam_->bitDepth() << "-bit ·"
                          << "target" << targetName() << "· filter" << filter_->currentText() << "·" << controls.join(' ');
        statsLogTick_ = 0;
        preview_->setSource(&tap_, cam_->info().bayer);
    }
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
    qInfo().noquote() << (recording_ ? "record: stop" : "preview: stop") << QString::fromStdString(std::format(
                             "· {} delivered, {} written, {:.1f} s, {:.1f} fps, dropped camera {} disk {}, ring peak {}/{}, {:.0f} MB/s, "
                             "write p50 {} us p99 {} us max {} us{}",
                             s.delivered, s.written, s.seconds, s.captureFps, s.cameraDropped, s.ringDropped, s.ringHighWater,
                             s.ringCapacity, s.writeMBps, s.writeP50Us, s.writeP99Us, s.writeMaxUs, s.error.empty() ? "" : " · error: " + s.error));
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
    if (remote_) {
        const bool on = !remote_->status()["recording"].toBool();
        qInfo().noquote() << "record: asking" << remote_->serverName() << "to" << (on ? "start" : "stop");
        remote_->sendRecord(on);
        return;
    }
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
    o << "Target=" << targetName() << "\n";
    if (isAutoTarget()) o << "TargetAuto=" << autoDetail_ << "\n";
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
    updateRemoteUi();
    if (remote_) {
        updateClientStats();
        return;
    }
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
    if (++statsLogTick_ % 40 == 0) {   // every 10 s (stats run every 250 ms): the night's timeline
        const auto& l = focus_->live();
        qInfo().noquote() << QString::fromStdString(std::format(
                                 "stats: {} {:.0f} s · {:.1f} fps · display {:.0f} fps · {} written · dropped camera {} disk {} · ring {}/{} "
                                 "(peak {}) · {:.0f} MB/s · write p99 {} us",
                                 recording_ ? "recording" : "preview", s.seconds, s.captureFps, preview_->displayFps(), s.written,
                                 s.cameraDropped, s.ringDropped, s.ringQueued, s.ringCapacity, s.ringHighWater, s.writeMBps, s.writeP99Us))
                          << "·" << freeText << "· focuser" << (focus_->focuser().ready() ? QString::number(focus_->focuser().position()) : "-")
                          << "· sharpness R/G/B" << QString("%1/%2/%3").arg(l.median[0], 0, 'g', 4).arg(l.median[1], 0, 'g', 4).arg(l.median[2], 0, 'g', 4)
                          << "· target" << targetName();
    }
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
    QString controls;
    for (const char* n : {"Gain", "Exposure"})
        if (auto it = controlBoxes_.find(n); it != controlBoxes_.end()) controls += QString(" · %1=%2").arg(n).arg(it->second->value());
    std::string line = (fState_->text() + " · " + fFps_->text() + " · " + fDropped_->text() + " · " + fDisk_->text() + controls +
                        (fRemote_->text().isEmpty() ? QString() : " · " + fRemote_->text()) +
                        (remoteBadge_->isVisible() ? " · badge: " + remoteBadge_->text() : QString()) + " · " + fMessage_->text())
                           .toStdString();
    std::fputs((line + "\n").c_str(), stdout);
    std::fflush(stdout);
}

// ================================================================ remote viewing

void MainWindow::setNamedControl(const QString& name, double value) {
    if (name == "Record") {   // a pseudo-control for tests: 1 starts recording, 0 stops it (on the server, if remote)
        const bool on = remote_ ? remote_->status()["recording"].toBool() : recording_;
        if ((value != 0) != on) toggleRecording();
        return;
    }
    if (!cam_) {
        qWarning().noquote() << "control:" << name << "- no camera";
        return;
    }
    setControl(name.toStdString(), value);
}

void MainWindow::ensureDiscovery() {
    if (discovery_) return;
    discovery_ = new net::Discovery(this);
    connect(discovery_, &net::Discovery::serversChanged, this, [this] {
        if (!connectFirst_) return;
        for (const auto& s : discovery_->servers()) {
            if (server_ && s.local && s.port == server_->port()) continue;   // ourselves
            if (s.protocol != net::kProtocol) continue;
            connectFirst_ = false;
            connectRemote(s.address.toString(), s.port);
            break;
        }
    });
    discovery_->start();
}

void MainWindow::startSharing(const QString& address) {
    if (server_) return;
    if (remote_) disconnectRemote();
    server_ = new net::RemoteServer(remoteTap_, this);
    if (!server_->start(address.isEmpty() ? QHostAddress(QHostAddress::Any) : QHostAddress(address))) {
        const QString why = server_->error();
        delete server_;
        server_ = nullptr;
        qWarning().noquote() << "remote: can't share:" << why;
        QMessageBox::warning(this, "SlopCap", "Can't share this camera:\n" + why);
        return;
    }
    connect(server_, &net::RemoteServer::controlRequested, this, [this](int id, const QString& name, double value) {
        if (!cam_) return;
        controlOrigin_ = id;
        setControl(name.toStdString(), value);
        controlOrigin_ = 0;
    });
    connect(server_, &net::RemoteServer::formatRequested, this, [this](int id, const QJsonObject& o) {
        if (!cam_) return;
        if (recording_) {
            server_->sendNotice(id, "it's recording: the ROI and format can't change until the recording stops");
            return;
        }
        if (o.contains("centre")) {
            const auto c = o["centre"].toArray();
            if (c.size() == 2) roiCentre_ = QPoint(c[0].toInt(), c[1].toInt());
            else roiCentre_.reset();
        }
        std::optional<QSize> size;
        std::optional<int> bits, readout;
        if (o.contains("width")) size = QSize(o["width"].toInt(), o["height"].toInt());
        if (o.contains("bits")) bits = o["bits"].toInt();
        if (o.contains("readout")) readout = o["readout"].toInt();
        qInfo().noquote() << "format: asked by a viewer";
        applyFormat(size, bits, readout);
    });
    connect(server_, &net::RemoteServer::recordRequested, this, [this](int, bool on) {
        qInfo().noquote() << "record:" << (on ? "start" : "stop") << "asked by a viewer";
        if (cam_ && on != recording_) toggleRecording();
    });
    connect(server_, &net::RemoteServer::viewersChanged, this, [this] {
        const auto v = server_->viewers();
        if (v.size() != lastViewers_) {
            QStringList names;
            for (const auto& x : v) names << (x.host.isEmpty() ? x.address : x.host);
            message(v.empty() ? "No one is watching now" : QString("Watching: %1").arg(names.join(", ")));
            lastViewers_ = v.size();
        }
        updateRemoteUi();
    });
    if (cam_) {
        server_->setCameraName(QString::fromStdString(cam_->info().model));
        server_->setBayer(cam_->info().bayer);
    }
    server_->setState(remoteCameraState());
    message(QString("Sharing on port %1: SlopCaps on this network can watch").arg(server_->port()));
    updateRemoteUi();
}

void MainWindow::stopSharing() {
    if (!server_) return;
    delete server_;
    server_ = nullptr;
    lastViewers_ = 0;
    message("Stopped sharing");
    updateRemoteUi();
}

void MainWindow::connectRemote(const QString& host, quint16 port) {
    if (server_ && !server_->viewers().empty() &&
        QMessageBox::question(this, "SlopCap", "Others are watching this camera. Stop sharing it and watch another?") != QMessageBox::Yes)
        return;
    stopSharing();
    if (remote_) disconnectRemote();
    if (cam_) {
        disconnectCamera();
        if (cam_) return;   // a recording is running and stays
    }
    remote_ = new net::RemoteClient(host, port, this);
    connect(remote_, &net::RemoteClient::stateChanged, this, &MainWindow::onRemoteState);
    connect(remote_, &net::RemoteClient::controlChanged, this, [this](const QString& name, double value) {
        qInfo().noquote() << "control:" << name << "=" << value << "(set on" << remote_->serverName() + ")";
        syncControlWidgets(name.toStdString(), value);
    });
    connect(remote_, &net::RemoteClient::statusChanged, this, &MainWindow::updateRemoteUi);
    connect(remote_, &net::RemoteClient::linkChanged, this, [this] {
        if (remote_->link() == net::RemoteClient::Link::Reconnecting) message(QString("Lost %1: reconnecting…").arg(remote_->serverName()));
        else if (remote_->link() == net::RemoteClient::Link::Connected) message(QString("Watching %1").arg(remote_->serverName()));
        populateCameraUi();
        updateStats();
    });
    connect(remote_, &net::RemoteClient::notice, this, [this](const QString& text) { message(remote_->serverName() + ": " + text); });
    remote_->start();
    QSettings().setValue("remote/last", port && port != net::kStreamPort ? QString("%1:%2").arg(host).arg(port) : host);
    populateCameraUi();
    updateStats();
}

void MainWindow::connectFirstRemote() {
    connectFirst_ = true;
    ensureDiscovery();
}

void MainWindow::disconnectRemote() {
    if (!remote_) return;
    qInfo().noquote() << "remote: disconnected from" << remote_->serverName();
    stopSession();
    cam_.reset();
    remote_->disconnect(this);
    remote_->deleteLater();
    remote_ = nullptr;
    rawHist_->clear();
    autoName_.clear();
    autoDetail_.clear();
    recBadge_->hide();
    updateAutoTarget();
    populateCameraUi();
    updateStats();
}

void MainWindow::onRemoteState() {
    stopSession();
    cam_.reset();
    if (remote_->hasCamera()) {
        cam_ = std::make_unique<net::RemoteCamera>(*remote_);
        const auto& i = cam_->info();
        const Roi r = cam_->roi();
        qInfo().noquote() << "camera: remote" << QString::fromStdString(i.vendor + " " + i.model) << "on" << remote_->serverName() << "·" << r.width << "x"
                          << r.height << "·" << cam_->bitDepth() << "-bit ·" << bayerName(i.bayer);
    } else {
        qInfo().noquote() << "camera: none on" << remote_->serverName();
    }
    roiCentre_.reset();
    roiCentreDirty_ = false;
    populateCameraUi();
    if (cam_) startSession(false);
    updateAutoTarget();
}

QJsonObject MainWindow::remoteCameraState() const {
    if (cameraState_.isEmpty()) return {{"camera", QJsonValue(QJsonValue::Null)}};
    QJsonObject cam = cameraState_;
    auto values = cam["values"].toObject();
    for (const auto& [name, box] : controlBoxes_) values[QString::fromStdString(name)] = box->value();
    cam["values"] = values;
    return {{"camera", cam}};
}

QJsonObject MainWindow::remoteStatus() const {
    QJsonObject s{{"recording", recording_}, {"limit", limit_->currentData().toInt()}, {"target", targetName()}};
    if (session_) {
        const auto st = session_->stats();
        s["fps"] = st.captureFps;
        s["dropped"] = static_cast<double>(st.cameraDropped + st.ringDropped);
        if (recording_) s["seconds"] = st.seconds, s["frames"] = static_cast<double>(st.written);
    }
    s["file"] = recording_ ? QFileInfo(recordingPath_).fileName() : fileStem(QDateTime::currentDateTimeUtc()) + ".ser";
    const QStorageInfo disk(QDir(outDir_).exists() ? outDir_ : QDir::homePath());
    const double freeTB = static_cast<double>(disk.bytesAvailable()) / 1e12;
    s["disk"] = freeTB >= 1 ? QString("%1 TB free").arg(freeTB, 0, 'f', 1) : QString("%1 GB free").arg(freeTB * 1000, 0, 'f', 0);
    return s;
}

void MainWindow::showRemoteMenu() {
    ensureDiscovery();
    if (discovery_->servers().empty()) {   // the first look: give the answers a moment
        discovery_->probeNow();
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 400) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    QMenu menu(this);
    if (remote_) {
        menu.addAction(QString("Disconnect from %1").arg(remote_->serverName()), this, &MainWindow::disconnectRemote);
    } else if (server_) {
        menu.addAction("Stop sharing this camera", this, &MainWindow::stopSharing);
        const auto v = server_->viewers();
        if (v.empty()) menu.addAction(QString("No one is watching yet (port %1)").arg(server_->port()))->setEnabled(false);
        for (const auto& x : v)
            menu.addAction(QString("%1 (%2) · %3 Mbps · %4 fps · %5%6")
                               .arg(x.host, x.address)
                               .arg(x.mbps, 0, 'f', 1)
                               .arg(x.fps, 0, 'f', 0)
                               .arg(net::qualityName(x.quality))
                               .arg(x.scale > 1 ? QString(" · 1/%1 size").arg(x.scale) : QString()))
                ->setEnabled(false);
    } else {
        menu.addAction("Share this camera on the network", this, [this] { startSharing(); });
    }
    menu.addSeparator();
    menu.addAction("Watch a SlopCap on this network:")->setEnabled(false);
    int listed = 0;
    for (const auto& srv : discovery_->servers()) {
        if (server_ && srv.local && srv.port == server_->port()) continue;   // ourselves
        const QString addr = srv.address.toString() + (srv.port != net::kStreamPort ? QString(":%1").arg(srv.port) : QString());
        QString text = QString("    %1 · %2 (%3)").arg(srv.name, srv.camera.isEmpty() ? "no camera" : srv.camera, addr);
        if (srv.viewers) text += QString(" · %1 watching").arg(srv.viewers);
        const bool compatible = srv.protocol == net::kProtocol;
        if (!compatible) text += QString(" · a different SlopCap version (protocol %1, this one %2): update one").arg(srv.protocol).arg(net::kProtocol);
        auto* a = menu.addAction(text);
        a->setEnabled(compatible && !(remote_ && remote_->host() == srv.address.toString() && remote_->port() == srv.port));
        connect(a, &QAction::triggered, this, [this, host = srv.address.toString(), port = srv.port] { connectRemote(host, port); });
        ++listed;
    }
    if (!listed) menu.addAction(QString("    none found (same subnet; UDP %1 must be open on the server)").arg(net::kDiscoveryPort))->setEnabled(false);
    menu.addAction("    Watch an address…", this, [this] {
        bool ok = false;
        const QString text = QInputDialog::getText(this, "SlopCap", "Watch the SlopCap at (host or host:port):", QLineEdit::Normal,
                                                   QSettings().value("remote/last").toString(), &ok)
                                 .trimmed();
        if (!ok || text.isEmpty()) return;
        // host, host:port, or [v6]:port
        QString host = text;
        quint16 port = 0;
        const int colon = text.lastIndexOf(':');
        if (colon > 0 && text.count(':') == 1) host = text.left(colon), port = static_cast<quint16>(text.mid(colon + 1).toUInt());
        else if (text.startsWith('[') && text.contains("]:")) host = text.mid(1, text.indexOf(']') - 1), port = static_cast<quint16>(text.mid(text.indexOf("]:") + 2).toUInt());
        connectRemote(host, port);
    });
    menu.exec(remotePill_->mapToGlobal(QPoint(0, remotePill_->height() + 4)));
}

void MainWindow::updateRemoteUi() {
    if (!remotePill_) return;
    QString pill = "⇄ Remote", badge, foot;
    QColor colour = theme::kGood;
    if (remote_) {
        const QString name = remote_->serverName();
        switch (remote_->link()) {
            case net::RemoteClient::Link::Connected:
                pill = "⇄ " + name;
                badge = "⇄ LIVE FROM " + name.toUpper();
                foot = QString("⇄ %1 Mbps · %2 fps · %3%4")
                           .arg(remote_->mbps(), 0, 'f', 1)
                           .arg(remote_->fps(), 0, 'f', 0)
                           .arg(net::qualityName(remote_->quality()))
                           .arg(remote_->scale() > 1 ? QString(" · 1/%1 size").arg(remote_->scale()) : QString());
                break;
            case net::RemoteClient::Link::Connecting:
                pill = "⇄ " + name + " · connecting…";
                badge = "⇄ CONNECTING TO " + name.toUpper() + "…";
                colour = theme::kAccent;
                break;
            case net::RemoteClient::Link::Reconnecting:
                pill = "⇄ " + name + " · reconnecting…";
                badge = "⇄ LINK LOST · RECONNECTING TO " + name.toUpper() + "…";
                foot = "⇄ reconnecting";
                colour = theme::kRecText;
                break;
        }
    } else if (server_) {
        const auto v = server_->viewers();
        if (v.empty()) {
            pill = "⇄ Sharing";
            badge = "⇄ SHARING · NO VIEWERS";
            colour = theme::kMuted;
            foot = QString("⇄ sharing on port %1").arg(server_->port());
        } else {
            QStringList names;
            for (const auto& x : v) names << (x.host.isEmpty() ? x.address : x.host);
            pill = QString("⇄ Sharing · %1 watching").arg(v.size());
            badge = "⇄ SHARED WITH " + names.join(", ").toUpper();
            foot = QString("⇄ %1 Mbps to %2").arg(server_->mbps(), 0, 'f', 1).arg(v.size() == 1 ? names[0] : QString("%1 viewers").arg(v.size()));
            if (v.size() == 1)
                foot += QString(" · %1 fps · %2%3").arg(v[0].fps, 0, 'f', 0).arg(net::qualityName(v[0].quality)).arg(v[0].scale > 1 ? QString(" · 1/%1 size").arg(v[0].scale) : QString());
        }
    }
    remotePill_->setText(pill);
    fRemote_->setText(foot);
    fRemote_->setVisible(!foot.isEmpty());
    remoteBadge_->setVisible(!badge.isEmpty());
    if (badge.isEmpty()) return;
    if (remoteBadge_->property("colour").toString() != colour.name()) {
        remoteBadge_->setStyleSheet(badgeStyle(colour, colour));
        remoteBadge_->setProperty("colour", colour.name());
    }
    remoteBadge_->setText(badge);
    remoteBadge_->adjustSize();
    remoteBadge_->move(preview_->width() - remoteBadge_->width() - 12, 12);
    remoteBadge_->raise();
}

/// The footer, record button and REC badge of a remote camera: the server's recording, the link.
void MainWindow::updateClientStats() {
    const auto& st = remote_->status();
    const QString name = remote_->serverName();
    const bool up = remote_->link() == net::RemoteClient::Link::Connected && cam_;
    const bool rec = up && st["recording"].toBool();
    fState_->setText(rec ? "● Recording on " + name : up ? "⇄ Watching " + name : "⇄ " + name + "…");
    fState_->setStyleSheet(rec ? QString("color: %1; font-weight: 600;").arg(theme::kRecText.name()) : "");
    const double secs = st["seconds"].toDouble();
    const int lim = st["limit"].toInt();
    record_->setText(rec ? "■  Stop · " + mmss(secs) : "●  Record on " + name);
    const char* variant = rec ? "stop" : "record";
    if (record_->property("variant").toString() != variant) {
        record_->setProperty("variant", variant);
        theme::restyle(record_);
    }
    if (!up) {
        for (auto* l : {fFps_, fDropped_, fRing_, fFrames_, fDisplay_, fDisk_}) l->clear();
        recBadge_->hide();
        return;
    }
    fFps_->setText(QString("<span style='color:%1'>%2</span> fps at the camera").arg(theme::kText.name()).arg(st["fps"].toDouble(), 0, 'f', 1));
    const auto dropped = static_cast<qint64>(st["dropped"].toDouble());
    fDropped_->setText(QString("%1 dropped").arg(dropped));
    fDropped_->setStyleSheet(QString("color: %1").arg((dropped ? theme::kRecText : theme::kGood).name()));
    fRing_->clear();
    fDisk_->setText(QString("%1: %2").arg(name, st["disk"].toString()));
    fFrames_->setText(rec ? QString("%1 frames").arg(thousands(st["frames"].toDouble())) : QString());
    fDisplay_->setText(QString("preview %1 fps").arg(preview_->displayFps(), 0, 'f', 0));
    nextFile_->setText(QString("Recordings stay on %1: %2").arg(name, st["file"].toString()));
    if (rec) {
        recBadge_->setText(QString("● REC on %1 · %2").arg(name, mmss(secs) + (lim > 0 ? " / " + mmss(lim) : QString())));
        recBadge_->adjustSize();
        recBadge_->show();
        recBadge_->raise();
    } else {
        recBadge_->hide();
    }
}

// ================================================================ full view

void MainWindow::showFullViewMenu() {
    if (fullView_) {
        setFullView(false);
        return;
    }
    const auto screens = QGuiApplication::screens();
    if (screens.size() < 2) {
        setFullView(true);
        return;
    }
    QMenu menu(this);
    for (QScreen* sc : screens) {
        QString label = (sc->manufacturer() + " " + sc->model()).trimmed();
        if (label.isEmpty()) label = sc->name();
        menu.addAction(QString("Full screen on %1 · %2 × %3%4")
                           .arg(label)
                           .arg(sc->geometry().width())
                           .arg(sc->geometry().height())
                           .arg(sc == screen() ? "  (this window's screen)" : ""),
                       this, [this, sc] { setFullView(true, sc); });
    }
    menu.exec(QCursor::pos());
}

void MainWindow::setFullView(bool on, QScreen* target) {
    if (on == fullView_) return;
    if (on && mode_ == Focus) setMode(Capture);   // the full view is the live preview
    fullView_ = on;
    for (QWidget* w : {header_, footer_, toolbar_, strip_, static_cast<QWidget*>(left_), static_cast<QWidget*>(right_)}) w->setVisible(!on);
    previewHolder_->layout()->setContentsMargins(on ? QMargins() : QMargins(20, 20, 20, 20));
    escape_->setEnabled(on);
    preview_->setMouseTracking(on);   // moves without a button wake the control bar
    if (on) {
        normalGeometry_ = saveGeometry();
        wasMaximized_ = isMaximized();
        if (target && target != screen()) {
            if (isMaximized() || isFullScreen()) showNormal();
            setGeometry(target->availableGeometry().adjusted(40, 40, -40, -40));
        }
        showFullScreen();
        fullBar_->show();
        syncFullBar();
        fullBar_->place();
        fullIdle_.start(3000);
        qApp->installEventFilter(this);
        qInfo().noquote() << "view: full screen on" << screen()->name();
    } else {
        qApp->removeEventFilter(this);
        fullIdle_.stop();
        fullBar_->hide();
        preview_->unsetCursor();
        showNormal();
        restoreGeometry(normalGeometry_);
        if (wasMaximized_) showMaximized();
        qInfo().noquote() << "view: windowed";
    }
}

void MainWindow::syncFullBar() {
    if (!fullView_) return;
    auto* r = fullBar_->record;
    r->setText(record_->text());
    r->setEnabled(record_->isEnabled());
    if (r->property("variant") != record_->property("variant")) {
        r->setProperty("variant", record_->property("variant"));
        theme::restyle(r);
    }
    fullBar_->link->setText(fRemote_->text());
    fullBar_->link->setVisible(!fRemote_->text().isEmpty());
    if (auto* b = fullBar_->stretch->button(stretch_->checkedId())) b->setChecked(true);
    if (fullBar_->isVisible()) fullBar_->place();
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
    if (o == preview_ && e->type() == QEvent::Resize) {
        if (remoteBadge_->isVisible()) remoteBadge_->move(preview_->width() - remoteBadge_->width() - 12, 12);
        if (fullBar_->isVisible()) fullBar_->place();
    }
    if (fullView_ && e->type() == QEvent::MouseMove) {
        if (!fullBar_->isVisible()) {
            fullBar_->show();
            syncFullBar();
            fullBar_->place();
            preview_->unsetCursor();
        }
        fullIdle_.start(3000);
    }
    return QMainWindow::eventFilter(o, e);
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

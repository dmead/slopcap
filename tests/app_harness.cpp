#include "app_harness.hpp"

#include "io/ser.hpp"
#ifdef SLOPCAP_FAKE_ASI
#include "fakes/fake_sdk.hpp"
#endif
#include "preview/preview_widget.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <random>

namespace slopcap::test {

namespace {

QString describe(QWidget* w) {
    if (auto* m = qobject_cast<QMenu*>(w)) {
        QStringList items;
        for (auto* a : m->actions())
            if (!a->isSeparator()) items << (a->isEnabled() ? "" : "(off) ") + a->text().simplified();
        return "menu: " + items.join(" | ");
    }
    if (auto* mb = qobject_cast<QMessageBox*>(w)) return "message: " + mb->text();
    if (qobject_cast<QFileDialog*>(w)) return "file dialog: " + w->windowTitle();
    if (auto* id = qobject_cast<QInputDialog*>(w)) return "input: " + id->labelText();
    return "dialog: " + w->windowTitle();
}

void closeIt(QWidget* w) {
    if (auto* d = qobject_cast<QDialog*>(w)) d->reject();
    else w->hide();
}

}  // namespace

Modals& Modals::get() {
    static Modals* m = new Modals;   // lives as long as the application (QTimer needs it)
    return *m;
}

Modals::Modals() {
    connect(&timer_, &QTimer::timeout, this, &Modals::poll);
    timer_.start(20);
}

void Modals::poll() {
    QWidget* w = QApplication::activePopupWidget();
    if (!w) w = QApplication::activeModalWidget();
    if (!w || !w->isVisible()) {
        unexpectedSince_.invalidate();
        return;
    }
    if (handling_ == w) return;
    if (steps_.empty()) {
        // nothing expected: close it, but give a test that queues late a moment
        if (!unexpectedSince_.isValid()) {
            unexpectedSince_.start();
            return;
        }
        if (unexpectedSince_.elapsed() < 1500) return;
        unexpected_ << describe(w);
        seen_ << describe(w);
        handling_ = w;
        QTimer::singleShot(0, w, [w] { closeIt(w); });
        return;
    }
    Pending p = std::move(steps_.front());
    steps_.pop_front();
    handling_ = w;
    seen_ << describe(w);
    // From a fresh timer event: a step that opens another dialog runs a nested event loop, and this poll timer has to
    // keep running inside it.
    QTimer::singleShot(0, w, [step = std::move(p.step), w] { step(w); });
}

void Modals::expect(const QString& what, Step step) { steps_.push_back({what, std::move(step)}); }

void Modals::pickMenu(const QString& text) {
    expect("menu " + text, [text](QWidget* w) {
        auto* m = qobject_cast<QMenu*>(w);
        if (!m) return closeIt(w);
        QAction* hit = nullptr;
        for (auto* a : m->actions())
            if (!text.isEmpty() && !a->isSeparator() && a->isEnabled() && a->text().contains(text)) {
                hit = a;
                break;
            }
        m->hide();   // as a click does: the menu goes, then the action runs
        if (hit) hit->trigger();
    });
}

void Modals::answer(QMessageBox::StandardButton b) {
    expect("message box", [b](QWidget* w) {
        if (auto* mb = qobject_cast<QMessageBox*>(w))
            if (auto* btn = mb->button(b)) return btn->click();
        closeIt(w);
    });
}

void Modals::chooseFile(const QString& path) {
    expect("file dialog", [path](QWidget* w) {
        auto* fd = qobject_cast<QFileDialog*>(w);
        if (!fd || path.isEmpty()) return closeIt(w);
        const QFileInfo fi(path);
        // the name field decides (selectFile() leaves it alone while it has the focus, as it does in a dialog on screen)
        fd->setDirectory(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath());
        if (auto* name = fd->findChild<QLineEdit*>("fileNameEdit")) name->setText(fi.absoluteFilePath());
        else fd->selectFile(fi.absoluteFilePath());
        QMetaObject::invokeMethod(fd, "accept");   // protected in QFileDialog
    });
}

void Modals::enterText(const QString& text) {
    expect("input dialog", [text](QWidget* w) {
        auto* id = qobject_cast<QInputDialog*>(w);
        if (!id || text.isEmpty()) return closeIt(w);
        id->setTextValue(text);
        id->accept();
    });
}

void Modals::settings(std::function<void(app::SettingsDialog&)> edit, bool accept) {
    expect("settings", [edit = std::move(edit), accept](QWidget* w) {
        auto* d = qobject_cast<app::SettingsDialog*>(w);
        if (!d) return closeIt(w);
        if (edit) edit(*d);
        if (accept) d->accept();
        else d->reject();
    });
}

bool Modals::done(int ms) {
    const bool ok = waitFor([this] { return steps_.empty(); }, ms);
    pump(50);
    return ok;
}

QStringList Modals::takeUnexpected() {
    QStringList u;
    u.swap(unexpected_);
    return u;
}

void Modals::clear() {
    steps_.clear();
    seen_.clear();
    unexpected_.clear();
}

// ---------------------------------------------------------------- INDI

SimServer::SimServer(const QStringList& drivers) {
    if (QStandardPaths::findExecutable("indiserver").isEmpty()) return;
    for (const auto& d : drivers)
        if (QStandardPaths::findExecutable(d).isEmpty()) return;
    std::random_device rd;
    port = static_cast<quint16>(20000 + rd() % 20000);
    QStringList args{"-p", QString::number(port)};
#ifdef __linux__
    // Linux indiserver also listens on a local socket: a private one keeps clear of a running server's
    args = QStringList{"-u", QString("/tmp/slopcap-app-test-%1").arg(port)} + args;
#endif
    proc.start("indiserver", args + drivers);
    ok = proc.waitForStarted(3000);
    pump(700);   // let it bind
}

SimServer::~SimServer() {
    proc.kill();
    proc.waitForFinished(2000);
}

// ---------------------------------------------------------------- files and settings

TempDir::TempDir() {
    std::random_device rd;
    path = std::filesystem::temp_directory_path() / ("slopcap-app-test-" + std::to_string(rd()) + std::to_string(rd()));
    std::filesystem::create_directories(path);
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

QString makeSer(const QString& path, int width, int height, int frames) {
    ser::Header hd;
    hd.colorId = ser::ColorId::BayerRGGB;
    hd.width = width;
    hd.height = height;
    hd.bitDepth = 8;
    ser::Writer wr(path.toStdString(), hd);
    std::vector<std::byte> f(hd.frameBytes());
    const double r = std::min(width, height) / 6.0;
    for (int i = 0; i < frames; ++i) {
        const double cx = width / 2.0 + 2 * std::sin(i * 0.3), cy = height / 2.0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const double d = std::hypot(x - cx, y - cy);
                const int v = d < r ? 150 + ((x / 4 + y / 4) % 2) * 40 : 12;   // a chequered disc: detail to focus on
                f[static_cast<std::size_t>(y * width + x)] = static_cast<std::byte>(v);
            }
        wr.writeFrame(f, 639270383770360000 + int64_t{i} * 400'000);   // 25 fps
    }
    return path;
}

void clearSettings() {
    QSettings st;
    st.clear();
    st.sync();
}

bool useFakeAsi([[maybe_unused]] int width, [[maybe_unused]] int height) {
#ifdef SLOPCAP_FAKE_ASI
    auto& f = fake::asi();
    f.reset();
    f.cam().maxWidth = width;
    f.cam().maxHeight = height;
    return true;
#else
    return false;
#endif
}

void drawFocusFrames([[maybe_unused]] const std::atomic<int>* position, [[maybe_unused]] std::array<int, 3> best) {
#ifdef SLOPCAP_FAKE_ASI
    fake::asi().draw = [position, best](uint8_t* px, int w, int h, int bps, int) {
        const double r = std::min(w, h) / 6.0, cx = w / 2.0, cy = h / 2.0;
        const int pos = position->load();
        std::array<double, 3> edge{};
        for (std::size_t c = 0; c < 3; ++c) edge[c] = 1 + std::abs(pos - best[c]) / 15.0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int site = (y & 1) * 2 + (x & 1);   // RGGB
                const std::size_t c = site == 0 ? 0 : site == 3 ? 2 : 1;
                const double d = std::hypot(x - cx, y - cy);
                const double v = std::clamp((r - d) / edge[c] + 0.5, 0.0, 1.0);
                const double band = 1 + 0.25 * std::sin(y / (2.0 * edge[c]));   // belts, softer when defocused
                const double out = 0.05 + 0.75 * v * band;
                const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                if (bps == 2) {
                    const auto s = static_cast<uint16_t>(out * 60000);
                    px[2 * i] = static_cast<uint8_t>(s & 0xff), px[2 * i + 1] = static_cast<uint8_t>(s >> 8);
                } else {
                    px[i] = static_cast<uint8_t>(out * 250);
                }
            }
    };
#endif
}

void drawDonut([[maybe_unused]] double offset) {
#ifdef SLOPCAP_FAKE_ASI
    fake::asi().draw = [offset](uint8_t* px, int w, int h, int bps, int) {
        const double cx = w / 2.0, cy = h / 2.0, outer = 45, inner = 18;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double d = std::hypot(x - cx, y - cy), s = std::hypot(x - cx - offset, y - cy);
                const double v = d < outer && s > inner ? 0.8 : 0.03;
                const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                if (bps == 2) {
                    const auto q = static_cast<uint16_t>(v * 60000);
                    px[2 * i] = static_cast<uint8_t>(q & 0xff), px[2 * i + 1] = static_cast<uint8_t>(q >> 8);
                } else {
                    px[i] = static_cast<uint8_t>(v * 250);
                }
            }
    };
#endif
}

QPushButton* button(QWidget* root, const QString& text) {
    const auto all = root->findChildren<QPushButton*>();
    for (auto* b : all)
        if (b->text() == text) return b;
    for (auto* b : all)
        if (b->text().contains(text)) return b;
    return nullptr;
}

QCheckBox* checkBox(QWidget* root, const QString& text) {
    for (auto* b : root->findChildren<QCheckBox*>())
        if (b->text().contains(text)) return b;
    return nullptr;
}

QLabel* labelWith(QWidget* root, const QString& text) {
    for (auto* l : root->findChildren<QLabel*>())
        if (l->text().contains(text)) return l;
    return nullptr;
}

}  // namespace slopcap::test

namespace slopcap::app {

bool MainWindowProbe::waitForFrames(int frames, int ms) {
    int n = 0;
    auto c = QObject::connect(preview, &preview::PreviewWidget::frameShown, preview, [&n] { ++n; });
    const bool ok = test::waitFor([&] { return n >= frames; }, ms);
    QObject::disconnect(c);
    return ok;
}

}  // namespace slopcap::app

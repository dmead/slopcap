#include "app/mount_settings_dialog.hpp"

#include "app/theme.hpp"
#include "mounts/mount_log.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace slopcap::app {

namespace {

using Kind = mounts::MountSetting::Kind;

/// The status reads every poll makes: hidden from the log unless asked for
bool isPoll(const std::string& line) {
    for (const char* p : {":GU#", ":GR#", ":GD#", ":GRH#", ":GDH#", ":D#", ":Gm#", ":GT#", "\\x06"})
        if (line.rfind(p, 0) == 0) return true;
    return false;
}

}  // namespace

MountSettingsDialog::MountSettingsDialog(devices::Mount& mount, QWidget* parent) : QDialog(parent), mount_(mount) {
    setWindowTitle("Mount settings and log");
    resize(620, 720);
    auto* v = new QVBoxLayout(this);
    tabs_ = new QTabWidget;
    v->addWidget(tabs_);

    // ---- settings
    auto* settingsTab = new QWidget;
    auto* sv = new QVBoxLayout(settingsTab);
    auto* top = new QHBoxLayout;
    status_ = theme::label("", "muted");
    status_->setWordWrap(true);
    auto* reread = theme::button("Read from the mount");
    auto* applyButton = theme::button("Apply changes", "accent");
    applyButton->setToolTip("Write the settings you changed to the mount (and nothing else)");
    top->addWidget(status_, 1);
    top->addWidget(reread);
    top->addWidget(applyButton);
    sv->addLayout(top);
    scroll_ = new QScrollArea;
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    sv->addWidget(scroll_, 1);
    tabs_->addTab(settingsTab, "Settings");
    connect(reread, &QPushButton::clicked, this, [this] {
        status_->setText("Reading from the mount…");
        mount_.readSettings();
    });
    connect(applyButton, &QPushButton::clicked, this, &MountSettingsDialog::apply);
    connect(&mount_, &devices::Mount::settingsRead, this, [this] {
        read_ = mount_.settings();
        rebuild();
        status_->setText("Read from the mount at " + QTime::currentTime().toString("HH:mm:ss") +
                         ". Changes are written only when you press Apply.");
    });
    connect(&mount_, &devices::Mount::failed, this, [this](const QString& why) {
        status_->setText("<span style='color:#ff6b6b'>" + why.toHtmlEscaped() + "</span>");
    });

    // ---- log
    auto* logTab = new QWidget;
    auto* lv = new QVBoxLayout(logTab);
    auto* filters = new QHBoxLayout;
    hidePolls_ = new QCheckBox("Hide status polls");
    hidePolls_->setChecked(true);
    hidePolls_->setToolTip("The position and status reads made every second (:GR#, :GD#, :GU#, …)");
    errorsOnly_ = new QCheckBox("Errors only");
    auto* copy = theme::button("Copy all");
    auto* clear = theme::button("Clear");
    filters->addWidget(hidePolls_);
    filters->addWidget(errorsOnly_);
    filters->addStretch(1);
    filters->addWidget(copy);
    filters->addWidget(clear);
    lv->addLayout(filters);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setFont(theme::monoFont());
    log_->setLineWrapMode(QPlainTextEdit::NoWrap);
    log_->setMaximumBlockCount(static_cast<int>(mounts::MountLog::kMax));
    lv->addWidget(log_, 1);
    lv->addWidget(theme::label("Every command SlopCap's own mount driver sent (>) and the reply (<), errors (!!) and notes (--). "
                               "The session log has the errors too; the commands only at debug level.",
                               "muted"));
    tabs_->addTab(logTab, "Log");
    auto refilter = [this] {
        logSeq_ = 0;
        log_->clear();
        pullLog();
    };
    connect(hidePolls_, &QCheckBox::toggled, this, refilter);
    connect(errorsOnly_, &QCheckBox::toggled, this, refilter);
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(log_->toPlainText()); });
    connect(clear, &QPushButton::clicked, this, [this] {
        mounts::MountLog::instance().clear();
        log_->clear();
    });
    logTimer_ = new QTimer(this);
    logTimer_->setInterval(500);
    connect(logTimer_, &QTimer::timeout, this, &MountSettingsDialog::pullLog);
    logTimer_->start();
    pullLog();

    auto* close = theme::button("Close");
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    bottom->addWidget(close);
    v->addLayout(bottom);

    rebuild();
    if (mount_.hasSettings()) {
        status_->setText("Reading from the mount…");
        mount_.readSettings();
    }
}

void MountSettingsDialog::showLog() { tabs_->setCurrentIndex(1); }

void MountSettingsDialog::rebuild() {
    editors_.clear();
    auto* page = new QWidget;
    auto* pv = new QVBoxLayout(page);
    pv->setSpacing(6);
    if (!mount_.hasSettings()) {
        auto* none = theme::label(mount_.ready() ? "This mount's own settings can't be changed from SlopCap (an OnStep's can, on its own driver: onstep:PORT)."
                                                 : "Connect the mount to see its settings. The Log tab says what happened so far.",
                                  "muted");
        none->setWordWrap(true);
        pv->addWidget(none);
    }
    QString group;
    QFormLayout* form = nullptr;
    for (const auto& s : read_) {
        if (QString::fromStdString(s.group) != group || !form) {
            group = QString::fromStdString(s.group);
            pv->addSpacing(8);
            pv->addWidget(theme::section(group));
            form = new QFormLayout;
            form->setLabelAlignment(Qt::AlignLeft);
            pv->addLayout(form);
        }
        const QString key = QString::fromStdString(s.key), label = QString::fromStdString(s.label), help = QString::fromStdString(s.help);
        const QString value = QString::fromStdString(s.value);
        QWidget* w = nullptr;
        switch (s.kind) {
            case Kind::Number: {
                auto* b = new QDoubleSpinBox;
                b->setRange(s.min, s.max);
                b->setDecimals(s.decimals);
                if (!s.unit.empty()) b->setSuffix(" " + QString::fromStdString(s.unit));
                b->setValue(value.toDouble());
                w = b;
                break;
            }
            case Kind::Choice: {
                auto* c = new QComboBox;
                for (const auto& [val, text] : s.choices) c->addItem(QString::fromStdString(text), QString::fromStdString(val));
                c->setCurrentIndex(std::max(0, c->findData(value)));
                w = c;
                break;
            }
            case Kind::Toggle: {
                auto* c = new QCheckBox;
                c->setChecked(value == "1");
                w = c;
                break;
            }
            case Kind::Text: {
                auto* e = new QLineEdit(value);
                w = e;
                break;
            }
            case Kind::Info: {
                auto* l = theme::label(value, "mono");
                l->setWordWrap(true);
                l->setTextInteractionFlags(Qt::TextSelectableByMouse);
                w = l;
                break;
            }
            case Kind::Action: {
                auto* b = theme::button(label);
                connect(b, &QPushButton::clicked, this, [this, key, help] {
                    if (help.contains('?')) {
                        const auto q = help.left(help.lastIndexOf('.') + 1).trimmed(), ask = help.mid(help.lastIndexOf('.') + 1).trimmed();
                        if (QMessageBox::question(this, "Mount", q + "\n\n" + ask) != QMessageBox::Yes) return;
                    }
                    act(key);
                });
                w = b;
                break;
            }
        }
        w->setToolTip(help);
        w->setAccessibleName(label);
        editors_[s.key] = w;
        if (s.kind == Kind::Action) form->addRow(QString(), w);
        else form->addRow(label, w);
    }
    pv->addStretch(1);
    scroll_->setWidget(page);
}

QString MountSettingsDialog::valueOf(const QString& key) const {
    const auto it = editors_.find(key.toStdString());
    if (it == editors_.end()) return {};
    if (auto* b = qobject_cast<QDoubleSpinBox*>(it->second)) return QString::number(b->value(), 'f', b->decimals());
    if (auto* c = qobject_cast<QComboBox*>(it->second)) return c->currentData().toString();
    if (auto* t = qobject_cast<QCheckBox*>(it->second)) return t->isChecked() ? "1" : "0";
    if (auto* e = qobject_cast<QLineEdit*>(it->second)) return e->text().trimmed();
    if (auto* l = qobject_cast<QLabel*>(it->second)) return l->text();
    return {};
}

void MountSettingsDialog::edit(const QString& key, const QString& value) {
    const auto it = editors_.find(key.toStdString());
    if (it == editors_.end()) return;
    if (auto* b = qobject_cast<QDoubleSpinBox*>(it->second)) b->setValue(value.toDouble());
    else if (auto* c = qobject_cast<QComboBox*>(it->second)) c->setCurrentIndex(std::max(0, c->findData(value)));
    else if (auto* t = qobject_cast<QCheckBox*>(it->second)) t->setChecked(value == "1");
    else if (auto* e = qobject_cast<QLineEdit*>(it->second)) e->setText(value);
}

void MountSettingsDialog::apply() {
    int changes = 0;
    for (const auto& s : read_) {
        if (s.kind == Kind::Info || s.kind == Kind::Action) continue;
        const QString now = valueOf(QString::fromStdString(s.key)), was = QString::fromStdString(s.value);
        const bool same = s.kind == Kind::Number ? std::abs(now.toDouble() - was.toDouble()) < 0.5 * std::pow(10.0, -s.decimals) : now == was;
        if (same) continue;
        mount_.changeSetting(QString::fromStdString(s.key), now);
        ++changes;
    }
    status_->setText(changes ? QString("Sent %1 change%2; reading them back…").arg(changes).arg(changes == 1 ? "" : "s") : "Nothing changed");
}

void MountSettingsDialog::act(const QString& key) {
    mount_.changeSetting(key, {});
    status_->setText("Sent; reading back…");
}

void MountSettingsDialog::pullLog() {
    const auto entries = mounts::MountLog::instance().since(logSeq_);
    if (entries.empty()) return;
    QStringList lines;
    for (const auto& e : entries) {
        logSeq_ = e.seq;
        using K = mounts::MountLog::Kind;
        if (errorsOnly_->isChecked() && e.kind != K::Error) continue;
        if (hidePolls_->isChecked() && e.kind == K::Exchange && isPoll(e.text)) continue;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(e.at.time_since_epoch()).count();
        const QString t = QDateTime::fromMSecsSinceEpoch(ms).toString("HH:mm:ss.zzz");
        const char* mark = e.kind == K::Exchange ? ">  " : e.kind == K::Error ? "!! " : "-- ";
        lines << t + "  " + mark + QString::fromStdString(e.text);
    }
    if (lines.isEmpty()) return;
    const bool atEnd = !log_->verticalScrollBar() || log_->verticalScrollBar()->value() == log_->verticalScrollBar()->maximum();
    log_->appendPlainText(lines.join('\n'));
    if (atEnd) log_->moveCursor(QTextCursor::End);
}

QString MountSettingsDialog::logText() const { return log_->toPlainText(); }

}  // namespace slopcap::app

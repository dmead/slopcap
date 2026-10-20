#include "app/theme.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QDir>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPushButton>
#include <QStyle>
#include <QStyleFactory>
#include <QVBoxLayout>

namespace slopcap::app::theme {

namespace {

QString hex(const QColor& c) { return c.name(QColor::HexRgb); }

QString family(const QString& wanted, QFontDatabase::SystemFont fallback) {
    return QFontDatabase::families().contains(wanted) ? wanted : QFontDatabase::systemFont(fallback).family();
}

}  // namespace

QFont monoFont(double pointSize) {
    QFont f(family("IBM Plex Mono", QFontDatabase::FixedFont));
    f.setStyleHint(QFont::Monospace);
    if (pointSize > 0) f.setPointSizeF(pointSize);
    return f;
}

void apply(QApplication& app) {
    // Fonts embedded at build time (IBM Plex), so the look doesn't depend on what the machine has installed.
    for (const auto& f : QDir(":/fonts").entryList({"*.ttf"})) QFontDatabase::addApplicationFont(":/fonts/" + f);
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, kPanel);
    p.setColor(QPalette::WindowText, kText);
    p.setColor(QPalette::Base, kControl);
    p.setColor(QPalette::AlternateBase, kPanel);
    p.setColor(QPalette::Text, kText);
    p.setColor(QPalette::Button, kControl);
    p.setColor(QPalette::ButtonText, kText);
    p.setColor(QPalette::Highlight, kAccent);
    p.setColor(QPalette::HighlightedText, QColor(0x1a, 0x12, 0x05));
    p.setColor(QPalette::ToolTipBase, kControl);
    p.setColor(QPalette::ToolTipText, kText);
    p.setColor(QPalette::PlaceholderText, kDim);
    p.setColor(QPalette::Link, kAccent);
    for (auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText}) p.setColor(QPalette::Disabled, role, kDim);
    app.setPalette(p);

    QFont ui(family("IBM Plex Sans", QFontDatabase::GeneralFont));
    ui.setPixelSize(13);
    app.setFont(ui);
    const QString mono = monoFont().family();

    // clang-format off
    app.setStyleSheet(QString(R"(
QWidget { color: %1; }
QMainWindow, QWidget#ground { background: %2; }
QToolTip { background: %4; color: %1; border: 1px solid %5; padding: 4px 6px; }

QFrame[role="panel"] { background: %3; }
QFrame[role="header"] { background: %3; border-bottom: 1px solid %6; }
QFrame[role="footer"] { background: %3; border-top: 1px solid %6; }
QFrame[role="card"] { background: %4; border-radius: 6px; }
QFrame[role="note"] { background: %4; border-radius: 8px; }
QFrame[role="warn"] { background: %8; border: 1px solid %9; border-radius: 8px; }
QFrame[role="good"] { background: #13261b; border: 1px solid #245c3a; border-radius: 8px; }
QFrame[role="toolbar"] { background: %2; border-bottom: 1px solid %6; }
QFrame[role="strip"] { background: %2; border-top: 1px solid %6; }
QFrame[role="segments"] { background: %3; border: 1px solid %6; border-radius: 6px; }
QFrame[role="nav"] { background: %2; border: 1px solid %6; border-radius: 8px; }
QFrame[role="pill"] { border: 1px solid %6; border-radius: 12px; }

QLabel[role="section"] { color: %10; font-size: 11px; font-weight: 600; letter-spacing: 1px; }
QLabel[role="muted"] { color: %10; }
QLabel[role="dim"] { color: %11; font-size: 11px; }
QLabel[role="mono"] { font-family: "%12"; }
QLabel[role="monoDim"] { font-family: "%12"; color: %11; font-size: 11px; }
QLabel[role="monoMuted"] { font-family: "%12"; color: %10; font-size: 12px; }
QLabel[role="monoLarge"] { font-family: "%12"; font-size: 22px; font-weight: 500; }
QLabel[role="monoHuge"] { font-family: "%12"; font-size: 34px; font-weight: 500; }
QLabel[role="title"] { font-size: 18px; font-weight: 600; }
QLabel[role="brand"] { font-size: 16px; font-weight: 600; letter-spacing: 0.3px; }
QLabel[role="rec"] { color: %13; font-family: "%12"; font-weight: 600; }
QLabel[role="good"] { color: %14; font-family: "%12"; }
QLabel[role="accent"] { color: %7; font-weight: 600; }

QPushButton { background: %4; color: %1; border: 1px solid %5; border-radius: 6px; padding: 6px 10px; min-height: 20px; }
QPushButton:hover { border-color: %11; }
QPushButton:pressed { background: %6; }
QPushButton:disabled { color: %11; border-color: %6; }
QPushButton:checked { background: %8; color: %7; border-color: %9; font-weight: 600; }
QPushButton[variant="accent"] { background: %7; color: #1a1205; border: none; border-radius: 8px; font-weight: 600; font-size: 14px; min-height: 30px; }
QPushButton[variant="accent"]:hover { background: #ffb85c; }
QPushButton[variant="accent"]:disabled { background: %6; color: %11; }
QPushButton[variant="record"] { background: %15; color: white; border: none; border-radius: 8px; font-weight: 600; font-size: 14px; min-height: 30px; }
QPushButton[variant="record"]:hover { background: #db4247; }
QPushButton[variant="stop"] { background: %16; color: %13; border: 1px solid %15; border-radius: 8px; font-weight: 600; font-size: 14px; min-height: 30px; }
QPushButton[variant="nav"] { background: transparent; color: %10; border: none; border-radius: 6px; padding: 6px 14px; }
QPushButton[variant="nav"]:checked { background: %8; color: %7; font-weight: 600; }
QPushButton[variant="nav"]:disabled { color: #4a525b; }
QPushButton[variant="segment"] { background: transparent; color: %10; border: none; border-radius: 4px; padding: 4px 10px; min-height: 18px; }
QPushButton[variant="segment"]:checked { background: %6; color: %1; font-weight: 400; }
QPushButton[variant="mono"] { font-family: "%12"; }
QPushButton[variant="option"] { text-align: left; padding: 6px 10px; }

QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox { background: %4; color: %1; border: 1px solid %5; border-radius: 6px; padding: 5px 8px; selection-background-color: %7; selection-color: #1a1205; }
QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: %9; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox::down-arrow { width: 0; height: 0; border-left: 4px solid transparent; border-right: 4px solid transparent; border-top: 5px solid %10; }
QComboBox QAbstractItemView { background: %4; border: 1px solid %5; selection-background-color: %8; selection-color: %7; }
QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 0; border: none; }
QLineEdit[role="mono"], QSpinBox[role="mono"], QDoubleSpinBox[role="mono"] { font-family: "%12"; }

QCheckBox, QRadioButton { color: %10; spacing: 8px; }
QCheckBox::indicator, QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid %5; background: %4; }
QCheckBox::indicator { border-radius: 3px; }
QRadioButton::indicator { border-radius: 7px; }
QCheckBox::indicator:checked, QRadioButton::indicator:checked { background: %7; border-color: %7; }

QSlider::groove:horizontal { height: 4px; background: %6; border-radius: 2px; }
QSlider::sub-page:horizontal { background: %7; border-radius: 2px; }
QSlider::handle:horizontal { background: %7; width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; }

QScrollArea { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 8px; }
QScrollBar::handle:vertical { background: %5; border-radius: 4px; min-height: 30px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QTableWidget { background: transparent; border: none; gridline-color: %6; }
QHeaderView::section { background: transparent; color: %11; border: none; border-bottom: 1px solid %6; padding: 4px 2px; font-weight: 500; }
QSplitter::handle { background: %6; }
)")
        .arg(hex(kText), hex(kGround), hex(kPanel), hex(kControl), hex(kControlLine), hex(kLine), hex(kAccent), hex(kAccentBg),
             hex(kAccentLine))
        .arg(hex(kMuted), hex(kDim), mono, hex(kRecText), hex(kGood), hex(kRec), hex(kRecBg)));
    // clang-format on
}

void restyle(QWidget* w) {
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

QLabel* section(const QString& text) { return label(text.toUpper(), "section"); }

QLabel* label(const QString& text, const char* role) {
    auto* l = new QLabel(text);
    if (role) l->setProperty("role", role);
    return l;
}

QFrame* card() {
    auto* f = new QFrame;
    f->setProperty("role", "card");
    return f;
}

QWidget* stat(const QString& caption, QLabel*& value) {
    auto* c = card();
    auto* v = new QVBoxLayout(c);
    v->setContentsMargins(10, 8, 10, 8);
    v->setSpacing(2);
    v->addWidget(label(caption, "dim"));
    value = label("—", "mono");
    v->addWidget(value);
    return c;
}

QPushButton* button(const QString& text, const char* variant) {
    auto* b = new QPushButton(text);
    if (variant) b->setProperty("variant", variant);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

QWidget* segmented(const QStringList& items, QButtonGroup*& group, bool compact) {
    auto* box = new QFrame;
    box->setProperty("role", "segments");
    auto* h = new QHBoxLayout(box);
    h->setContentsMargins(2, 2, 2, 2);
    h->setSpacing(2);
    group = new QButtonGroup(box);
    group->setExclusive(true);
    for (int i = 0; i < items.size(); ++i) {
        auto* b = button(items[i], "segment");
        b->setCheckable(true);
        if (compact) b->setMinimumHeight(22);
        group->addButton(b, i);
        h->addWidget(b);
    }
    return box;
}

QWidget* pill(QLabel*& text, QLabel*& dot) {
    auto* f = new QFrame;
    f->setProperty("role", "pill");
    auto* h = new QHBoxLayout(f);
    h->setContentsMargins(10, 4, 10, 4);
    h->setSpacing(6);
    dot = new QLabel;
    dot->setFixedSize(7, 7);
    setDot(dot, kGrey);
    text = label("", "monoMuted");
    h->addWidget(dot);
    h->addWidget(text);
    return f;
}

void setDot(QLabel* dot, const QColor& c) { dot->setStyleSheet(QString("background: %1; border-radius: 3px;").arg(c.name())); }

FlowLayout::~FlowLayout() {
    while (auto* item = takeAt(0)) delete item;
}

QSize FlowLayout::minimumSize() const {
    QSize s;
    for (auto* item : items_) s = s.expandedTo(item->minimumSize());
    return s;
}

int FlowLayout::arrange(const QRect& r, bool apply) const {
    int x = r.x(), y = r.y(), lineH = 0;
    for (auto* item : items_) {
        const QSize sz = item->sizeHint();
        if (x + sz.width() > r.right() + 1 && lineH > 0) {
            x = r.x();
            y += lineH + spacing_;
            lineH = 0;
        }
        if (apply) item->setGeometry(QRect(QPoint(x, y), sz));
        x += sz.width() + spacing_;
        lineH = std::max(lineH, sz.height());
    }
    return y + lineH - r.y();
}

void drawChannelMarker(QPainter& p, int channel, double x, double y, double s) {
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(kChannel[channel]);
    if (channel == 0) {
        p.drawEllipse(QPointF(x, y), s / 2, s / 2);
    } else if (channel == 1) {
        p.drawRect(QRectF(x - s / 2, y - s / 2, s, s));
    } else {
        QPainterPath t;
        t.moveTo(x, y - s * 0.6);
        t.lineTo(x + s * 0.55, y + s * 0.45);
        t.lineTo(x - s * 0.55, y + s * 0.45);
        t.closeSubpath();
        p.drawPath(t);
    }
    p.restore();
}

}  // namespace slopcap::app::theme

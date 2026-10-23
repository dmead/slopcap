// SlopCap's look, from the "SlopCap UI" design canvas: a dark ground, one amber accent, IBM Plex Sans / Mono, and
// colour-blind-safe channel markers (R circle + dashed, G square + solid, B triangle + dotted).
// Widgets opt into variants with dynamic properties that the style sheet matches:
//   QPushButton[variant="accent" | "record" | "stop" | "ghost" | "segment"]
//   QLabel[role="section" | "muted" | "dim" | "mono" | "monoLarge" | "value"]
//   QFrame[role="card" | "panel" | "header" | "footer" | "note" | "good" | "warn"]
#pragma once

#include <QColor>
#include <QFont>
#include <QString>
#include <QStringList>
#include <QLayout>
#include <QList>
#include <QWidget>

#include <functional>

class QApplication;
class QButtonGroup;
class QLabel;
class QPushButton;
class QFrame;

namespace slopcap::app::theme {

// Tokens
inline const QColor kGround{0x0d, 0x0f, 0x12};
inline const QColor kPanel{0x15, 0x18, 0x1c};
inline const QColor kLine{0x23, 0x27, 0x2d};
inline const QColor kControl{0x1c, 0x20, 0x26};
inline const QColor kControlLine{0x2c, 0x31, 0x38};
inline const QColor kText{0xe7, 0xe9, 0xec};
inline const QColor kMuted{0xa3, 0xac, 0xb7};
inline const QColor kDim{0x7d, 0x87, 0x92};
inline const QColor kAccent{0xf2, 0xa5, 0x41};
inline const QColor kAccentBg{0x2a, 0x24, 0x18};
inline const QColor kAccentLine{0x5a, 0x45, 0x23};
inline const QColor kGood{0x3f, 0xb9, 0x7a};
inline const QColor kRec{0xc9, 0x35, 0x3a};
inline const QColor kRecText{0xff, 0x8a, 0x8d};
inline const QColor kRecBg{0x3a, 0x14, 0x16};
inline const QColor kGrey{0x5d, 0x66, 0x70};
inline const QColor kChannel[3] = {QColor(0xff, 0x6b, 0x6b), QColor(0x4c, 0xd0, 0x7d), QColor(0x6a, 0xa8, 0xff)};

void apply(QApplication& app);        ///< palette, fonts, style sheet
QFont monoFont(double pointSize = 0); ///< IBM Plex Mono (or the system monospace)

// Building blocks
QLabel* section(const QString& text);                 ///< small uppercase panel heading
QLabel* label(const QString& text, const char* role); ///< a label with a style role
QFrame* card();                                       ///< rounded control-coloured box
QWidget* stat(const QString& caption, QLabel*& value); ///< caption over a mono value, in a card
/// Keep a readout at least as wide as `widest` in its own font (empty: no reserve), so digits coming and going every
/// frame don't push what's beside it, or resize the panel it's in.
void reserveWidth(QLabel* label, const QString& widest);
/// Set a style sheet only when it differs: every new one re-polishes the widget and re-lays out its window.
void setStyleIfChanged(QWidget* w, const QString& css);
QPushButton* button(const QString& text, const char* variant = nullptr);
/// Exclusive row of buttons styled as one segmented control. Returns the container; `group` gets the buttons with ids
/// 0..n-1.
QWidget* segmented(const QStringList& items, QButtonGroup*& group, bool compact = false);
/// A status pill: coloured dot + mono text.
QWidget* pill(QLabel*& text, QLabel*& dot);
void setDot(QLabel* dot, const QColor& c);
/// Re-evaluate the style sheet after changing a widget's role/variant property.
void restyle(QWidget* w);

/// Lays children out left to right, wrapping to new lines (like CSS flex-wrap) so a row of chips never forces its
/// column wider.
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int spacing = 6) : QLayout(parent), spacing_(spacing) { setContentsMargins(0, 0, 0, 0); }
    ~FlowLayout() override;
    void addItem(QLayoutItem* item) override { items_.append(item); }
    int count() const override { return static_cast<int>(items_.size()); }
    QLayoutItem* itemAt(int i) const override { return items_.value(i); }
    QLayoutItem* takeAt(int i) override { return i >= 0 && i < items_.size() ? items_.takeAt(i) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return arrange(QRect(0, 0, w, 0), false); }
    void setGeometry(const QRect& r) override {
        QLayout::setGeometry(r);
        arrange(r, true);
    }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override;

private:
    int arrange(const QRect& r, bool apply) const;
    QList<QLayoutItem*> items_;
    int spacing_;
};

/// Draw a channel's marker (0 = R circle, 1 = G square, 2 = B triangle) centred at (x, y).
void drawChannelMarker(class QPainter& p, int channel, double x, double y, double size);

}  // namespace slopcap::app::theme

// The reduced controls over the preview in full view (the preview stretched over a whole screen, e.g. a projector at
// an outreach event): gain, exposure, stretch, record and the remote link in one translucent bar. MainWindow wires the
// widgets like their sidebar twins and hides the bar (and the cursor) while the mouse rests.
#pragma once

#include <QFrame>

class QButtonGroup;
class QLabel;
class QPushButton;
class QSlider;

namespace slopcap::app {

class FullViewBar : public QFrame {
    Q_OBJECT
public:
    explicit FullViewBar(QWidget* parent);
    /// Bottom centre of the parent.
    void place();

    QSlider* gain = nullptr;
    QLabel* gainValue = nullptr;
    QSlider* exposure = nullptr;   ///< same 0..1000 log scale as the sidebar's
    QLabel* expValue = nullptr;
    QButtonGroup* stretch = nullptr;   ///< 0 linear, 1 auto
    QPushButton* record = nullptr;
    QLabel* link = nullptr;
    QPushButton* exit = nullptr;
};

}  // namespace slopcap::app

// Capture sidebar, under the focuser: the filter wheel. One button per slot, named (Names... renames them, kept per
// wheel), the one in the light path lit; a click turns the wheel there. The header's Filter box follows the wheel and
// turns it too (MainWindow). Connect opens the first ZWO EFW; once connected it connects again at the next start.
#pragma once

#include <QWidget>

#include <vector>

class QGridLayout;
class QLabel;
class QPushButton;

namespace slopcap::devices {
class FilterWheel;
}

namespace slopcap::app {

class FilterWheelSection : public QWidget {
    Q_OBJECT
public:
    explicit FilterWheelSection(devices::FilterWheel& wheel, QWidget* parent = nullptr);
    QPushButton* slotButton(int slot) const { return slot >= 0 && slot < static_cast<int>(slots_.size()) ? slots_[slot] : nullptr; }
    void editNames();   ///< (also the tests')

signals:
    void message(const QString& text);   ///< for the status footer

private:
    void rebuild();     ///< the slot buttons, after the wheel or its names change
    void refresh();
    devices::FilterWheel& wheel_;
    QWidget* offline_ = nullptr;
    QLabel* offlineText_ = nullptr;
    QPushButton* connect_ = nullptr;
    QWidget* online_ = nullptr;
    QLabel* state_ = nullptr;
    QGridLayout* grid_ = nullptr;
    std::vector<QPushButton*> slots_;
};

}  // namespace slopcap::app

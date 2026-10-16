// Settings: focuser connection and behaviour, and the optics the critical focus zone needs.
#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

namespace slopcap::app {

struct Settings {
    QString indiServer = "localhost:7624";
    bool focuserAutoConnect = true;
    int backlash = 600;
    bool approachOutward = true;
    double focalRatio = 0;       ///< 0 = unknown
    double micronsPerStep = 0;   ///< 0 = unknown
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(const Settings& s, QWidget* parent = nullptr);
    Settings settings() const;

private:
    QLineEdit* server_ = nullptr;
    QCheckBox* auto_ = nullptr;
    QSpinBox* backlash_ = nullptr;
    QComboBox* approach_ = nullptr;
    QDoubleSpinBox* fRatio_ = nullptr;
    QDoubleSpinBox* umStep_ = nullptr;
};

}  // namespace slopcap::app

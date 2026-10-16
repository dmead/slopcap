// Settings: focuser connection and behaviour, the optics (critical focus zone, image scale) and the site (which
// targets are up, for the automatic target).
#pragma once

#include <QDialog>

#include <optional>

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
    double focalLengthMm = 0;    ///< effective (with any Barlow); 0 = unknown
    std::optional<double> latitude, longitude;   ///< the site, when the mount doesn't say (east positive)
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
    QDoubleSpinBox* focalLength_ = nullptr;
    QDoubleSpinBox* latitude_ = nullptr;
    QDoubleSpinBox* longitude_ = nullptr;
};

}  // namespace slopcap::app

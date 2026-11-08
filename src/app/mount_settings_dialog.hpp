// Mount > Settings and log...: the mount's own settings (OnStep: site and time, tracking, limits, meridian flips,
// guide and goto speeds, backlash, home and park, as its web server has them), read from the mount and written only
// when Apply (or an action's button) is pressed; and the mount's log: every command and reply of SlopCap's own
// drivers, and every error, fault and note (mounts::MountLog), newest last, kept whatever the log level.
#pragma once

#include "devices/mount.hpp"

#include <QDialog>

#include <cstdint>
#include <map>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QScrollArea;
class QTabWidget;
class QTimer;

namespace slopcap::app {

class MountSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit MountSettingsDialog(devices::Mount& mount, QWidget* parent = nullptr);
    void showLog();                       ///< open on the Log tab
    /// (tests) the value a setting's widget holds now, and setting it as a user would
    QString valueOf(const QString& key) const;
    void edit(const QString& key, const QString& value);
    void apply();                         ///< write the settings whose widgets differ from the mount's
    void act(const QString& key);         ///< an action, without asking
    QString logText() const;

private:
    void rebuild();
    void pullLog();
    devices::Mount& mount_;
    QTabWidget* tabs_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QLabel* status_ = nullptr;
    std::vector<mounts::MountSetting> read_;
    std::map<std::string, QWidget*> editors_;
    QPlainTextEdit* log_ = nullptr;
    QCheckBox* hidePolls_ = nullptr;
    QCheckBox* errorsOnly_ = nullptr;
    std::uint64_t logSeq_ = 0;
    QTimer* logTimer_ = nullptr;
};

}  // namespace slopcap::app

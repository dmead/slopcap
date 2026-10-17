// One log file per run, for studying a night afterwards: every Qt message (qInfo / qWarning / ..., including Qt's own
// and the device code's) with a UTC timestamp, written and flushed line by line so a crash keeps everything before
// it, and echoed to stderr. Files go to <app data>/logs/slopcap-<UTC start>.log (Linux:
// ~/.local/share/SlopCap/SlopCap/logs; Windows: %LOCALAPPDATA%\SlopCap\SlopCap\logs); the newest 100 are kept.
#pragma once

#include <QString>

namespace slopcap::app::sessionlog {

/// Install the handler and open this run's file. Returns its path ("" if it couldn't be opened: stderr only).
QString start();
QString path();

}  // namespace slopcap::app::sessionlog

#include "app/session_log.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QStandardPaths>

#include <cstdio>

namespace slopcap::app::sessionlog {

namespace {

QFile* file = nullptr;
QString filePath;
QMutex mutex;   // messages come from any thread (capture, writer, Qt's)

void handler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    const char level = type == QtDebugMsg ? 'D' : type == QtInfoMsg ? 'I' : type == QtWarningMsg ? 'W' : type == QtCriticalMsg ? 'E' : 'F';
    const QByteArray line = QString("%1 %2 %3\n").arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm:ss.zzzZ")).arg(level).arg(msg).toUtf8();
    QMutexLocker lock(&mutex);
    if (file) {
        file->write(line);
        file->flush();
    }
    std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stderr);
    if (type == QtFatalMsg) std::abort();
}

}  // namespace

QString start() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
    QDir d(dir);
    if (d.mkpath(".")) {
        // keep the newest 100
        const auto old = d.entryInfoList({"slopcap-*.log"}, QDir::Files, QDir::Name | QDir::Reversed);
        for (qsizetype i = 99; i < old.size(); ++i) QFile::remove(old[i].absoluteFilePath());
        filePath = d.filePath("slopcap-" + QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd-HHmmss") + ".log");
        file = new QFile(filePath);
        if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            delete file;
            file = nullptr;
            filePath.clear();
        }
    }
    qInstallMessageHandler(handler);
    return filePath;
}

QString path() { return filePath; }

}  // namespace slopcap::app::sessionlog

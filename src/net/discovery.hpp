// Finding SlopCap servers on the local subnet (protocol.hpp): a probe to every interface's broadcast address and to
// localhost every couple of seconds; servers answer the sender directly, which firewalls let back in as a reply.
#pragma once

#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QTimer>

#include <vector>

class QUdpSocket;

namespace slopcap::net {

struct ServerInfo {
    QString name;          ///< the server's host name
    QHostAddress address;
    quint16 port = 0;
    QString camera;        ///< model, empty without one
    int viewers = 0;
    qint64 seenMs = 0;     ///< when it last answered (QDateTime ms)
    bool local = false;    ///< answered on a loopback address: this machine
};

class Discovery : public QObject {
    Q_OBJECT
public:
    explicit Discovery(QObject* parent = nullptr);
    void start(int intervalMs = 2000);
    void stop();
    void probeNow();
    /// Servers that answered in the last few seconds, by name.
    std::vector<ServerInfo> servers() const;

signals:
    void serversChanged();

private:
    void read();
    QUdpSocket* sock_ = nullptr;
    QTimer timer_;
    std::vector<ServerInfo> servers_;
};

}  // namespace slopcap::net

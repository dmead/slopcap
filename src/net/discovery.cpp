#include "net/discovery.hpp"

#include "net/protocol.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QUdpSocket>

#include <algorithm>

namespace slopcap::net {

namespace {
constexpr qint64 kForgetMs = 7000;
}

Discovery::Discovery(QObject* parent) : QObject(parent) {
    sock_ = new QUdpSocket(this);
    sock_->bind(QHostAddress::AnyIPv4, 0);
    connect(sock_, &QUdpSocket::readyRead, this, &Discovery::read);
    connect(&timer_, &QTimer::timeout, this, [this] {
        probeNow();
        // forget servers that stopped answering
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const auto n = servers_.size();
        std::erase_if(servers_, [now](const ServerInfo& s) { return now - s.seenMs > kForgetMs; });
        if (servers_.size() != n) emit serversChanged();
    });
}

void Discovery::start(int intervalMs) {
    probeNow();
    timer_.start(intervalMs);
}

void Discovery::stop() { timer_.stop(); }

void Discovery::probeNow() {
    QList<QHostAddress> targets{QHostAddress::Broadcast, QHostAddress::LocalHost};
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsUp) || !(iface.flags() & QNetworkInterface::CanBroadcast)) continue;
        for (const auto& e : iface.addressEntries())
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol && !e.broadcast().isNull() && !targets.contains(e.broadcast()))
                targets << e.broadcast();
    }
    for (const auto& t : targets) sock_->writeDatagram(QByteArray(kProbe), t, kDiscoveryPort);
}

void Discovery::read() {
    bool changed = false;
    while (sock_->hasPendingDatagrams()) {
        const QNetworkDatagram d = sock_->receiveDatagram();
        const auto o = QJsonDocument::fromJson(d.data()).object();
        if (o["slopcap"].toInt() != kProtocol) continue;
        ServerInfo s;
        s.name = o["name"].toString();
        s.port = static_cast<quint16>(o["port"].toInt());
        s.camera = o["camera"].toString();
        s.viewers = o["viewers"].toInt();
        s.seenMs = QDateTime::currentMSecsSinceEpoch();
        // an IPv4-mapped IPv6 sender (some stacks) back to plain IPv4
        bool ok = false;
        const quint32 v4 = d.senderAddress().toIPv4Address(&ok);
        s.address = ok ? QHostAddress(v4) : d.senderAddress();
        s.local = s.address.isLoopback();
        auto it = std::find_if(servers_.begin(), servers_.end(), [&](const ServerInfo& x) { return x.name == s.name && x.port == s.port; });
        if (it == servers_.end()) {
            servers_.push_back(s);
            changed = true;
            continue;
        }
        // the same server heard on loopback and on the LAN: show the LAN address, remember it's this machine
        const bool local = it->local || s.local;
        if (s.local && !it->address.isLoopback()) s.address = it->address;
        s.local = local;
        changed |= it->camera != s.camera || it->viewers != s.viewers || it->address != s.address;
        *it = s;
    }
    if (changed) emit serversChanged();
}

std::vector<ServerInfo> Discovery::servers() const {
    auto out = servers_;
    std::sort(out.begin(), out.end(), [](const ServerInfo& a, const ServerInfo& b) { return a.name < b.name; });
    return out;
}

}  // namespace slopcap::net

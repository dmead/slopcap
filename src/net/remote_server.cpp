#include "net/remote_server.hpp"

#include "net/frame_codec.hpp"
#include "net/protocol.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

#include <chrono>

namespace slopcap::net {

namespace {

QJsonObject hello(const char* role) {
    return {{"proto", kProtocol}, {"app", "SlopCap"}, {"version", QCoreApplication::applicationVersion()},
            {"host", QSysInfo::machineHostName()}, {"role", role}};
}

QString plainAddress(const QHostAddress& a) {
    bool ok = false;
    const quint32 v4 = a.toIPv4Address(&ok);
    return ok ? QHostAddress(v4).toString() : a.toString();
}

}  // namespace

RemoteServer::RemoteServer(LatestFrame& tap, QObject* parent) : QObject(parent), tap_(tap) {
    connect(&secondTimer_, &QTimer::timeout, this, [this] {
        for (auto& [id, c] : clients_) {
            c.mbps = static_cast<double>(c.bytesThisSecond) * 8 / 1e6;
            c.bytesThisSecond = 0;
        }
        if (!clients_.empty()) emit viewersChanged();
    });
}

RemoteServer::~RemoteServer() { stop(); }

bool RemoteServer::running() const { return tcp_ && tcp_->isListening(); }

quint16 RemoteServer::port() const { return running() ? tcp_->serverPort() : 0; }

bool RemoteServer::start(const QHostAddress& address, quint16 port) {
    if (running()) return true;
    if (!jpegAvailable()) {
        error_ = "this build has no JPEG encoder (Qt's jpeg image plugin is missing)";
        return false;
    }
    tcp_ = new QTcpServer(this);
    const quint16 first = port ? port : kStreamPort;
    bool ok = false;
    for (quint16 p = first; p < first + 10 && !ok; ++p) ok = tcp_->listen(address, p);
    if (!ok) {
        error_ = tcp_->errorString();
        delete tcp_;
        tcp_ = nullptr;
        return false;
    }
    connect(tcp_, &QTcpServer::newConnection, this, &RemoteServer::onConnection);
    udp_ = new QUdpSocket(this);
    const QHostAddress udpAddress = address == QHostAddress::Any ? QHostAddress(QHostAddress::AnyIPv4) : address;
    if (udp_->bind(udpAddress, kDiscoveryPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint))
        connect(udp_, &QUdpSocket::readyRead, this, &RemoteServer::answerProbes);
    else
        qWarning().noquote() << "remote: discovery port" << kDiscoveryPort << "unavailable (" << udp_->errorString()
                             << "): clients must connect by address";
    stop_ = false;
    encoder_ = std::thread([this] { encoderLoop(); });
    secondTimer_.start(1000);
    qInfo().noquote() << "remote: sharing on" << plainAddress(address) << "port" << tcp_->serverPort();
    return true;
}

void RemoteServer::stop() {
    if (encoder_.joinable()) {
        stop_ = true;
        wake_.notify_all();
        encoder_.join();
    }
    secondTimer_.stop();
    for (auto& [id, c] : clients_) {
        c.socket->disconnect(this);
        c.socket->abort();
        c.socket->deleteLater();
    }
    const bool had = !clients_.empty();
    clients_.clear();
    {
        std::lock_guard lock(mutex_);
        flows_.clear();
    }
    delete udp_;
    udp_ = nullptr;
    if (tcp_) {
        tcp_->close();
        delete tcp_;
        tcp_ = nullptr;
        qInfo().noquote() << "remote: sharing stopped";
    }
    if (had) emit viewersChanged();
}

void RemoteServer::setBayer(Bayer b) {
    std::lock_guard lock(mutex_);
    bayer_ = b;
}

// ---------------------------------------------------------------- discovery

void RemoteServer::answerProbes() {
    while (udp_ && udp_->hasPendingDatagrams()) {
        const QNetworkDatagram d = udp_->receiveDatagram();
        if (!d.data().startsWith("SLOPCAP?")) continue;
        const QJsonObject a{{"slopcap", kProtocol},
                            {"name", QSysInfo::machineHostName()},
                            {"port", tcp_ ? tcp_->serverPort() : 0},
                            {"camera", cameraName_},
                            {"viewers", static_cast<int>(viewers().size())}};
        udp_->writeDatagram(QJsonDocument(a).toJson(QJsonDocument::Compact), d.senderAddress(), static_cast<quint16>(d.senderPort()));
    }
}

// ---------------------------------------------------------------- connections

void RemoteServer::onConnection() {
    while (QTcpSocket* s = tcp_->nextPendingConnection()) {
        const int id = nextId_++;
        s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        Client c;
        c.socket = s;
        c.reader = std::make_unique<MessageReader>();
        c.flow = std::make_shared<Flow>();
        c.flow->id = id;
        c.address = plainAddress(s->peerAddress());
        connect(s, &QTcpSocket::readyRead, this, [this, id] { onReadyRead(id); });
        connect(s, &QTcpSocket::disconnected, this, [this, id] { onDisconnected(id); });
        connect(s, &QTcpSocket::bytesWritten, this, [this, id](qint64 n) {
            if (auto it = clients_.find(id); it != clients_.end()) it->second.bytesThisSecond += n;
        });
        {
            std::lock_guard lock(mutex_);
            flows_.push_back(c.flow);
        }
        qInfo().noquote() << "remote: connection from" << c.address;
        clients_.emplace(id, std::move(c));
    }
}

void RemoteServer::onDisconnected(int id) {
    auto it = clients_.find(id);
    if (it == clients_.end()) return;
    qInfo().noquote() << "remote: viewer" << (it->second.host.isEmpty() ? it->second.address : it->second.host) << "left";
    {
        std::lock_guard lock(mutex_);
        std::erase(flows_, it->second.flow);
    }
    it->second.socket->deleteLater();
    clients_.erase(it);
    emit viewersChanged();
}

void RemoteServer::onReadyRead(int id) {
    auto it = clients_.find(id);
    if (it == clients_.end()) return;
    Client& c = it->second;
    c.reader->append(c.socket->readAll());
    while (auto m = c.reader->next()) {
        handle(id, c, static_cast<uint8_t>(m->type), m->json());
        if (!clients_.count(id)) return;   // a handler can end the connection
    }
    if (c.reader->error()) {
        qWarning().noquote() << "remote: garbled stream from" << c.address << "- closing it";
        c.socket->abort();
    }
}

void RemoteServer::handle(int id, Client& c, uint8_t type, const QJsonObject& o) {
    switch (static_cast<Msg>(type)) {
        case Msg::Hello:
            if (o["proto"].toInt() != kProtocol) {
                c.socket->write(packJson(Msg::Notice, {{"text", QString("This server speaks SlopCap remote protocol %1, the viewer %2: update both")
                                                                    .arg(kProtocol)
                                                                    .arg(o["proto"].toInt())}}));
                c.socket->disconnectFromHost();
                return;
            }
            c.host = o["host"].toString();
            c.flow->name = c.host.isEmpty() ? c.address : c.host;
            c.socket->write(packJson(Msg::Hello, hello("server")));
            c.socket->write(packJson(Msg::State, state_));
            c.flow->ready = true;
            wake_.notify_all();
            qInfo().noquote() << "remote: viewer" << c.host << "(" << c.address << ", SlopCap" << o["version"].toString() << ") joined";
            emit viewersChanged();
            break;
        case Msg::Ack:
            if (c.flow->inFlight.fetch_sub(1) <= 0) c.flow->inFlight = 0;
            wake_.notify_all();
            break;
        case Msg::Control: emit controlRequested(id, o["name"].toString(), o["value"].toDouble()); break;
        case Msg::Format: emit formatRequested(id, o); break;
        case Msg::Record: emit recordRequested(id, o["on"].toBool()); break;
        default: break;   // newer clients may send more; ignore what we don't know
    }
}

void RemoteServer::send(int id, const QByteArray& bytes) {
    if (auto it = clients_.find(id); it != clients_.end() && it->second.socket->state() == QAbstractSocket::ConnectedState)
        it->second.socket->write(bytes);
}

// ---------------------------------------------------------------- state to clients

void RemoteServer::setState(const QJsonObject& state) {
    state_ = state;
    const QByteArray msg = packJson(Msg::State, state_);
    for (auto& [id, c] : clients_)
        if (c.flow->ready) c.socket->write(msg);
}

void RemoteServer::broadcastControl(const QString& name, double value, int except) {
    // keep the state current for clients that join later
    if (state_["camera"].isObject()) {
        auto cam = state_["camera"].toObject();
        auto values = cam["values"].toObject();
        values[name] = value;
        cam["values"] = values;
        state_["camera"] = cam;
    }
    const QByteArray msg = packJson(Msg::Control, {{"name", name}, {"value", value}});
    for (auto& [id, c] : clients_)
        if (c.flow->ready && id != except) c.socket->write(msg);
}

void RemoteServer::sendStatus(const QJsonObject& status) {
    if (clients_.empty()) return;
    QJsonObject s = status;
    s["viewers"] = static_cast<int>(viewers().size());
    const QByteArray msg = packJson(Msg::Status, s);
    for (auto& [id, c] : clients_)
        if (c.flow->ready) c.socket->write(msg);
}

void RemoteServer::sendNotice(int client, const QString& text) { send(client, packJson(Msg::Notice, {{"text", text}})); }

std::vector<RemoteServer::Viewer> RemoteServer::viewers() const {
    std::vector<Viewer> out;
    for (const auto& [id, c] : clients_) {
        if (!c.flow->ready) continue;
        out.push_back({id, c.host, c.address, c.mbps, c.flow->fps.load(), c.flow->shownQuality.load(), c.flow->shownScale.load()});
    }
    return out;
}

double RemoteServer::mbps() const {
    double sum = 0;
    for (const auto& [id, c] : clients_) sum += c.mbps;
    return sum;
}

// ---------------------------------------------------------------- the encoder thread

void RemoteServer::encoderLoop() {
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / kMaxStreamFps));
    auto nextDue = Clock::now();
    auto secondStart = Clock::now();
    int taken = 0;
    while (!stop_) {
        std::vector<std::shared_ptr<Flow>> flows;
        Bayer bayer;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(4));
            if (stop_) break;
            flows = flows_;
            bayer = bayer_;
        }
        const auto now = Clock::now();
        if (now - secondStart >= std::chrono::seconds(1)) {
            const double secs = std::chrono::duration<double>(now - secondStart).count();
            for (auto& f : flows) adapt(*f, secs, std::min(kMaxStreamFps, taken / secs));
            taken = 0;
            secondStart = now;
        }
        bool anyReady = false;
        for (auto& f : flows) anyReady |= f->ready.load();
        if (!anyReady || now < nextDue) continue;
        // Take the newest frame even when every window is full: the frame goes by unsent (counted as skipped) and the
        // tap stays fresh for when an ack arrives.
        const LatestFrame::Frame* frame = tap_.take();
        if (!frame) continue;
        nextDue = std::max(nextDue + period, now - period);
        ++taken;
        std::map<std::pair<int, int>, QByteArray> encoded;   // (quality, scale) → message: clients alike share one
        for (auto& f : flows) {
            if (!f->ready) continue;
            if (f->inFlight.load() >= kWindow) {
                ++f->second.skipped;
                continue;
            }
            const auto key = std::make_pair(f->rate.quality, f->rate.scale);
            auto it = encoded.find(key);
            if (it == encoded.end()) {
                const auto t0 = Clock::now();
                const auto e = encodeFrame(std::span(frame->pixels).first(frame->info.bytes), frame->info, bayer, {key.first, key.second});
                it = encoded.emplace(key, packFrame(e.header, e.data)).first;
                f->second.encodeMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            }
            ++f->inFlight;
            ++f->second.sent;
            f->second.bytes += static_cast<double>(it->second.size());
            QMetaObject::invokeMethod(this, [this, id = f->id, msg = it->second] { send(id, msg); }, Qt::QueuedConnection);
        }
    }
}

void RemoteServer::adapt(Flow& f, double seconds, double availableFps) {
    StreamSecond sec = f.second;
    sec.seconds = seconds;
    f.second = {};
    f.fps = sec.sent / seconds;
    const int q0 = f.rate.quality, s0 = f.rate.scale;
    if (const char* why = f.rate.update(sec, availableFps))
        qInfo().noquote() << QString("remote: %1 %2 → %3, size 1/%4 → 1/%5 (%6: sent %7 of %8 fps, %9 skipped, %10 kB and "
                                     "%11 ms a frame; next step up after %12 s)")
                                 .arg(f.name)
                                 .arg(qualityName(q0))
                                 .arg(qualityName(f.rate.quality))
                                 .arg(s0)
                                 .arg(f.rate.scale)
                                 .arg(why)
                                 .arg(sec.sent / seconds, 0, 'f', 0)
                                 .arg(availableFps, 0, 'f', 0)
                                 .arg(sec.skipped)
                                 .arg(sec.sent ? sec.bytes / sec.sent / 1000 : 0, 0, 'f', 0)
                                 .arg(sec.sent ? sec.encodeMs / sec.sent : 0, 0, 'f', 1)
                                 .arg(f.rate.upgradeWait());
    f.shownQuality = f.rate.quality;
    f.shownScale = f.rate.scale;
}

}  // namespace slopcap::net

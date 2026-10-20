#include "net/remote_client.hpp"

#include "net/frame_codec.hpp"

#include <QCoreApplication>
#include <QTcpSocket>

namespace slopcap::net {

// ---------------------------------------------------------------- feed

void RemoteFeed::push(const QJsonObject& header, const QByteArray& data) {
    {
        std::lock_guard lock(m_);
        if (fresh_) ++replaced_;
        header_ = header;
        data_ = data;
        fresh_ = true;
    }
    cv_.notify_one();
}

bool RemoteFeed::wait(QJsonObject& header, QByteArray& data, std::chrono::milliseconds timeout) {
    std::unique_lock lock(m_);
    if (!cv_.wait_for(lock, timeout, [this] { return fresh_; })) return false;
    header = header_;
    data.swap(data_);
    fresh_ = false;
    return true;
}

void RemoteFeed::setValue(const std::string& name, double value) {
    std::lock_guard lock(m_);
    values_[name] = value;
}

void RemoteFeed::setValues(const std::map<std::string, double, std::less<>>& values) {
    std::lock_guard lock(m_);
    values_ = values;
}

std::optional<double> RemoteFeed::value(std::string_view name) const {
    std::lock_guard lock(m_);
    if (auto it = values_.find(name); it != values_.end()) return it->second;
    return std::nullopt;
}

uint64_t RemoteFeed::replaced() const {
    std::lock_guard lock(m_);
    return replaced_;
}

// ---------------------------------------------------------------- client

RemoteClient::RemoteClient(const QString& host, quint16 port, QObject* parent)
    : QObject(parent), host_(host), port_(port ? port : kStreamPort) {
    retry_.setSingleShot(true);
    retry_.setInterval(2000);
    connect(&retry_, &QTimer::timeout, this, &RemoteClient::connectNow);
    connect(&second_, &QTimer::timeout, this, [this] {
        mbps_ = static_cast<double>(bytes_) * 8 / 1e6;
        fps_ = frames_;
        bytes_ = 0;
        frames_ = 0;
        emit statusChanged();
    });
    second_.start(1000);
}

RemoteClient::~RemoteClient() {
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
    }
}

void RemoteClient::start() {
    qInfo().noquote() << "remote: connecting to" << host_ << "port" << port_;
    connectNow();
}

void RemoteClient::connectNow() {
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
    }
    socket_ = new QTcpSocket(this);
    reader_ = std::make_unique<MessageReader>();
    connect(socket_, &QTcpSocket::connected, this, &RemoteClient::onConnected);
    connect(socket_, &QTcpSocket::readyRead, this, &RemoteClient::onReadyRead);
    connect(socket_, &QTcpSocket::disconnected, this, &RemoteClient::onDropped);
    connect(socket_, &QTcpSocket::errorOccurred, this, &RemoteClient::onDropped);
    socket_->connectToHost(host_, port_);
}

void RemoteClient::onConnected() {
    socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    socket_->write(packJson(Msg::Hello, {{"proto", kProtocol}, {"app", "SlopCap"}, {"version", QCoreApplication::applicationVersion()},
                                          {"host", machineName()}, {"role", "client"}}));
}

void RemoteClient::onDropped() {
    if (retry_.isActive()) return;   // disconnected + errorOccurred both fire
    if (link_ == Link::Connected) qWarning().noquote() << "remote: lost" << serverName() << "-" << socket_->errorString() << "- reconnecting";
    setLink(link_ == Link::Connecting ? Link::Connecting : Link::Reconnecting);
    retry_.start();
}

void RemoteClient::setLink(Link l) {
    if (l == link_) return;
    link_ = l;
    emit linkChanged();
}

void RemoteClient::onReadyRead() {
    const QByteArray bytes = socket_->readAll();
    bytes_ += bytes.size();
    reader_->append(bytes);
    while (auto m = reader_->next()) {
        switch (m->type) {
            case Msg::Hello:
                serverName_ = m->json()["host"].toString();
                qInfo().noquote() << "remote: connected to" << serverName() << "(" << host_ << ", SlopCap" << m->json()["version"].toString() << ")";
                setLink(Link::Connected);
                break;
            case Msg::State: {
                const QJsonValue cam = m->json()["camera"];   // a value: json() is a temporary
                if (cam.isObject()) {
                    state_ = cameraStateFromJson(cam.toObject());
                    feed_->setValues(state_->values);
                } else {
                    state_.reset();
                }
                emit stateChanged();
                break;
            }
            case Msg::Control: {
                const auto o = m->json();
                const auto name = o["name"].toString().toStdString();
                const double v = o["value"].toDouble();
                feed_->setValue(name, v);
                if (state_) state_->values[name] = v;
                emit controlChanged(o["name"].toString(), v);
                break;
            }
            case Msg::Status:
                status_ = m->json();
                emit statusChanged();
                break;
            case Msg::Notice: emit notice(m->json()["text"].toString()); break;
            case Msg::Frame: {
                QJsonObject header;
                QByteArray pixels;
                if (!splitFrame(m->payload, header, pixels)) break;
                socket_->write(packJson(Msg::Ack, {{"seq", header["seq"]}}));   // at once: acks pace the server
                ++frames_;
                quality_ = header["q"].toInt();
                scale_ = header["scale"].toInt(1);
                feed_->push(header, pixels);
                break;
            }
            default: break;
        }
    }
    if (reader_->error()) {
        qWarning().noquote() << "remote: garbled stream from" << serverName() << "- reconnecting";
        socket_->abort();
    }
}

void RemoteClient::sendControl(const QString& name, double value) {
    if (socket_ && link_ == Link::Connected) socket_->write(packJson(Msg::Control, {{"name", name}, {"value", value}}));
}

void RemoteClient::sendFormat(const QJsonObject& request) {
    if (socket_ && link_ == Link::Connected) socket_->write(packJson(Msg::Format, request));
}

void RemoteClient::sendRecord(bool on) {
    if (socket_ && link_ == Link::Connected) socket_->write(packJson(Msg::Record, {{"on", on}}));
}

// ---------------------------------------------------------------- camera

RemoteCamera::RemoteCamera(RemoteClient& client) : state_(client.state()), feed_(client.feed()), client_(&client) {}

void RemoteCamera::setControl(std::string_view name, double value) {
    feed_->setValue(std::string(name), value);
    if (RemoteClient* c = client_.data())
        QMetaObject::invokeMethod(c, [c, n = QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())), value] { c->sendControl(n, value); },
                                  Qt::QueuedConnection);
}

ReadResult RemoteCamera::readFrame(std::span<std::byte> dst, FrameInfo& info, std::chrono::milliseconds timeout) {
    QJsonObject header;
    QByteArray data;
    if (!feed_->wait(header, data, timeout)) return ReadResult::Timeout;
    const FrameInfo fi = frameInfoFrom(header);
    // a frame from before a format change: the new State is on its way
    if (fi.roi.width != state_.roi.width || fi.roi.height != state_.roi.height || fi.bitDepth != (state_.bits > 8 ? 16 : 8) || dst.size() < fi.bytes)
        return ReadResult::Timeout;
    if (!decodeFrame(header, data, dst)) return ReadResult::Timeout;
    info = fi;
    return ReadResult::Ok;
}

}  // namespace slopcap::net

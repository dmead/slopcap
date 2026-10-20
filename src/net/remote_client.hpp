// The watching side of the remote protocol (protocol.hpp). RemoteClient owns the connection (on the GUI thread):
// it says Hello, acknowledges every frame, keeps the server's camera state and status, and reconnects by itself
// after a dropped link. RemoteCamera is the server's camera as an ICamera, so the rest of SlopCap (capture session,
// preview, analysis, the camera controls) treats a remote camera like a local one: its frames are the decoded stream,
// its settings go to the server. It never records: Record asks the server to, and the raw files stay there.
#pragma once

#include "camera/camera.hpp"
#include "net/protocol.hpp"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

class QTcpSocket;

namespace slopcap::net {

/// Hands the newest compressed frame and the control values from the connection (GUI thread) to RemoteCamera
/// (capture thread). Older undelivered frames are replaced: the newest one wins.
class RemoteFeed {
public:
    void push(const QJsonObject& header, const QByteArray& data);
    bool wait(QJsonObject& header, QByteArray& data, std::chrono::milliseconds timeout);
    void setValue(const std::string& name, double value);
    std::optional<double> value(std::string_view name) const;
    void setValues(const std::map<std::string, double, std::less<>>& values);
    uint64_t replaced() const;   ///< frames that arrived but were overtaken before decoding

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    QJsonObject header_;
    QByteArray data_;
    bool fresh_ = false;
    uint64_t replaced_ = 0;
    std::map<std::string, double, std::less<>> values_;
};

class RemoteClient : public QObject {
    Q_OBJECT
public:
    enum class Link { Connecting, Connected, Reconnecting };

    RemoteClient(const QString& host, quint16 port, QObject* parent = nullptr);
    ~RemoteClient() override;

    void start();
    QString host() const { return host_; }
    quint16 port() const { return port_; }
    /// The server's name (its host name once it said Hello).
    QString serverName() const { return serverName_.isEmpty() ? host_ : serverName_; }
    Link link() const { return link_; }
    bool hasCamera() const { return state_.has_value(); }
    const CameraState& state() const { return *state_; }   ///< only while hasCamera()
    const QJsonObject& status() const { return status_; }
    std::shared_ptr<RemoteFeed> feed() const { return feed_; }

    double mbps() const { return mbps_; }   ///< received, last second
    double fps() const { return fps_; }     ///< frames received, last second
    int quality() const { return quality_; }
    int scale() const { return scale_; }

    void sendControl(const QString& name, double value);
    void sendFormat(const QJsonObject& request);
    void sendRecord(bool on);

signals:
    void stateChanged();                              ///< the camera (or its format) changed, appeared or went away
    void controlChanged(const QString& name, double value);
    void statusChanged();
    void linkChanged();
    void notice(const QString& text);

private:
    void connectNow();
    void onConnected();
    void onReadyRead();
    void onDropped();
    void setLink(Link l);

    QString host_, serverName_;
    quint16 port_;
    QTcpSocket* socket_ = nullptr;
    std::unique_ptr<MessageReader> reader_;
    Link link_ = Link::Connecting;
    QTimer retry_, second_;
    std::optional<CameraState> state_;
    QJsonObject status_;
    std::shared_ptr<RemoteFeed> feed_ = std::make_shared<RemoteFeed>();
    qint64 bytes_ = 0;
    int frames_ = 0;
    double mbps_ = 0, fps_ = 0;
    int quality_ = 0, scale_ = 1;
};

/// The server's camera, as of its last State. Rebuild it whenever RemoteClient::stateChanged fires (with the capture
/// session stopped): format and controls are fixed per instance; values follow the feed.
class RemoteCamera final : public ICamera {
public:
    explicit RemoteCamera(RemoteClient& client);

    const CameraInfo& info() const override { return state_.info; }
    std::vector<ControlInfo> controls() const override { return state_.controls; }
    std::optional<double> control(std::string_view name) const override { return feed_->value(name); }
    void setControl(std::string_view name, double value) override;   ///< any thread: forwarded to the server
    std::vector<ReadoutMode> readoutModes() const override { return state_.readoutModes; }
    int readoutMode() const override { return state_.readout; }
    void setReadoutMode(int) override { throw CameraError("remote camera: change the format through the server"); }
    std::vector<GainLandmark> gainLandmarks() const override { return state_.landmarks; }
    Roi roi() const override { return state_.roi; }
    void setRoi(const Roi&) override { throw CameraError("remote camera: change the format through the server"); }
    int bitDepth() const override { return state_.bits; }
    void setBitDepth(int) override { throw CameraError("remote camera: change the format through the server"); }

    void startStream() override { streaming_ = true; }
    void stopStream() override { streaming_ = false; }
    bool streaming() const override { return streaming_; }
    ReadResult readFrame(std::span<std::byte> dst, FrameInfo& info, std::chrono::milliseconds timeout) override;

private:
    CameraState state_;
    std::shared_ptr<RemoteFeed> feed_;
    QPointer<RemoteClient> client_;
    bool streaming_ = false;
};

}  // namespace slopcap::net

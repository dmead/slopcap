// The sharing side of the remote protocol (protocol.hpp): accepts clients, answers discovery probes, and streams the
// live view to every client from an encoder thread, each at the quality its link can carry.
//
// The capture session publishes frames into the tap (CaptureOptions::remote) only when the encoder asks, so sharing
// costs nothing while nobody watches. Per client, at most kWindow frames are in flight; when the window is full the
// client skips frames. Once a second each client's quality and size adapt to what it took (rate_control.hpp).
//
// Everything here runs on the thread that owns the server (the GUI thread) except the encoder.
#pragma once

#include "camera/camera.hpp"
#include "net/rate_control.hpp"
#include "preview/latest_frame.hpp"

#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QTcpServer;
class QTcpSocket;
class QUdpSocket;

namespace slopcap::net {

class MessageReader;

class RemoteServer : public QObject {
    Q_OBJECT
public:
    struct Viewer {
        int id = 0;
        QString host;       ///< what the client called itself
        QString address;
        double mbps = 0;    ///< video sent, last second
        double fps = 0;
        int quality = 0, scale = 1;
    };

    /// `tap`: where the capture session publishes frames for the stream (CaptureOptions::remote).
    explicit RemoteServer(LatestFrame& tap, QObject* parent = nullptr);
    ~RemoteServer() override;

    /// Listen on `port` (or the next free one of ten) and answer discovery probes. False (and error()) if neither works.
    bool start(const QHostAddress& address = QHostAddress::Any, quint16 port = 0);
    void stop();
    bool running() const;
    quint16 port() const;
    QString error() const { return error_; }

    void setBayer(Bayer b);

    /// The camera as clients should see it ({"camera": null} without one); sent now to every client and later to new ones.
    void setState(const QJsonObject& state);
    void setCameraName(const QString& model) { cameraName_ = model; }
    /// A control was set (on this machine, or by client `except`, which already knows).
    void broadcastControl(const QString& name, double value, int except = 0);
    void sendStatus(const QJsonObject& status);
    void sendNotice(int client, const QString& text);

    std::vector<Viewer> viewers() const;
    double mbps() const;   ///< all clients, last second

signals:
    void viewersChanged();
    void controlRequested(int client, const QString& name, double value);
    void formatRequested(int client, const QJsonObject& request);
    void recordRequested(int client, bool on);

private:
    /// Shared between this thread and the encoder.
    struct Flow {
        int id = 0;
        std::atomic<int> inFlight{0};
        std::atomic<bool> ready{false};   ///< said Hello
        QString name;                     ///< written before ready is set, read by the encoder after
        // encoder thread only
        RateControl rate;
        StreamSecond second;   ///< this second so far
        // stats for the UI (written by the encoder, read here)
        std::atomic<double> fps{0};
        std::atomic<int> shownQuality{75}, shownScale{1};
    };
    struct Client {
        QTcpSocket* socket = nullptr;
        std::unique_ptr<MessageReader> reader;
        std::shared_ptr<Flow> flow;
        QString host, address;
        qint64 bytesThisSecond = 0;
        double mbps = 0;
    };

    void onConnection();
    void onReadyRead(int id);
    void onDisconnected(int id);
    void handle(int id, Client& c, uint8_t type, const QJsonObject& o);
    void answerProbes();
    void send(int id, const QByteArray& bytes);
    void encoderLoop();
    void adapt(Flow& f, double seconds, double availableFps);

    QTcpServer* tcp_ = nullptr;
    QUdpSocket* udp_ = nullptr;
    QString error_, cameraName_;
    QJsonObject state_;
    std::map<int, Client> clients_;
    int nextId_ = 1;
    QTimer secondTimer_;

    LatestFrame& tap_;
    std::thread encoder_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;              ///< flows_ and bayer_
    std::condition_variable wake_;
    std::vector<std::shared_ptr<Flow>> flows_;
    Bayer bayer_ = Bayer::None;
};

}  // namespace slopcap::net

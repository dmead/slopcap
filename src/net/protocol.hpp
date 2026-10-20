// SlopCap's remote protocol: one SlopCap shares its camera (the server: it has the USB connection and keeps every raw
// file), others watch and steer it (clients: e.g. a laptop on a projector at an outreach event). Plain TCP plus UDP
// discovery on the local subnet, the same on Windows, Linux and macOS: Qt Network, big-endian framing, JSON for
// everything except pixels.
//
// Discovery (UDP kDiscoveryPort): a client sends kProbe to the broadcast addresses (and to localhost); every server
// answers the sender with one JSON datagram:
//   {"slopcap":1, "name":"observatory", "port":45720, "camera":"QHY533M", "viewers":0}
// Stream (TCP kStreamPort, or the next free one; the discovery answer says which): length-prefixed messages,
//   u32 length (big-endian; counts the type byte and the payload) | u8 type | payload
// JSON payloads unless noted:
//   Hello    both ways, first   {"proto":1, "app":"SlopCap", "version":"9a82bf3", "host":"desktop", "role":"client"}
//   State    server → client    the camera (cameraStateToJson; {"camera":null} without one). After Hello, and
//                               whenever the camera, ROI, bit depth or readout mode changes.
//   Control  both ways          {"name":"Gain", "value":250}: please set (client) / was set (server → other clients)
//   Format   client → server    {"width","height","bits","readout"} plus "centre": [x, y] (sensor pixel), null
//                               (centre on the sensor) or absent (keep the server's): ROI size, bit depth, readout
//   Record   client → server    {"on":true}: start or stop recording ON THE SERVER (raw files never leave it)
//   Status   server → client    twice a second: recording, file, seconds, frames, fps, control values, viewers
//   Notice   server → client    {"text":"..."}: something the user should read (e.g. a refused request)
//   Frame    server → client    binary: u32 header length | JSON header | compressed pixels (frame_codec.hpp)
//   Ack      client → server    {"seq":N} for every Frame. The server keeps at most kWindow frames unacknowledged
//                               per client, so a slow link lowers the frame rate (then the quality) instead of
//                               queueing delay.
#pragma once

#include "camera/camera.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace slopcap::net {

inline constexpr int kProtocol = 1;
inline constexpr quint16 kStreamPort = 45720;     ///< TCP; a server tries the next 9 if it's taken
inline constexpr quint16 kDiscoveryPort = 45721;  ///< UDP
inline constexpr char kProbe[] = "SLOPCAP?1";
inline constexpr int kWindow = 3;                 ///< frames in flight per client (rides out a viewer's ~100 ms stalls)
inline constexpr double kMaxStreamFps = 30;       ///< plenty for a projector; spares the link and the server's CPU
inline constexpr std::size_t kMaxMessage = 64u << 20;

enum class Msg : uint8_t { Hello = 1, State = 2, Control = 3, Format = 4, Record = 5, Status = 6, Notice = 7, Frame = 8, Ack = 9 };

/// Everything a client needs to show and steer the server's camera.
struct CameraState {
    CameraInfo info;
    std::vector<ControlInfo> controls;
    std::map<std::string, double, std::less<>> values;
    std::vector<ReadoutMode> readoutModes;
    int readout = 0;
    std::vector<GainLandmark> landmarks;
    Roi roi;
    int bits = 8;
};

/// Read it from a camera (call while the camera isn't streaming: cameras aren't thread-safe).
CameraState snapshot(const ICamera& cam);
QJsonObject cameraStateToJson(const CameraState& s);
CameraState cameraStateFromJson(const QJsonObject& o);

QString bayerToString(Bayer b);
Bayer bayerFromString(const QString& s);

QByteArray pack(Msg type, const QByteArray& payload);
QByteArray packJson(Msg type, const QJsonObject& o);
/// A Frame message: header JSON + compressed pixels.
QByteArray packFrame(const QJsonObject& header, const QByteArray& pixels);

struct Message {
    Msg type;
    QByteArray payload;
    QJsonObject json() const;   ///< the payload as a JSON object ({} if it isn't one)
};

/// Splits a TCP byte stream back into messages.
class MessageReader {
public:
    void append(const QByteArray& bytes) { buf_.append(bytes); }
    /// The next whole message, if one has arrived. Sets error() (and returns nothing more) on a malformed stream.
    std::optional<Message> next();
    bool error() const { return error_; }

private:
    QByteArray buf_;
    qsizetype pos_ = 0;
    bool error_ = false;
};

/// A Frame payload split into its JSON header and pixel bytes (false if malformed).
bool splitFrame(const QByteArray& payload, QJsonObject& header, QByteArray& pixels);

}  // namespace slopcap::net

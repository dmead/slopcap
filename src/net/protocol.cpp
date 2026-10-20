#include "net/protocol.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>

#include <array>

namespace slopcap::net {

namespace {

constexpr std::array<std::pair<GainLandmark::Kind, const char*>, 6> kKinds{{
    {GainLandmark::Kind::Unity, "unity"},
    {GainLandmark::Kind::LowestReadNoise, "lowestReadNoise"},
    {GainLandmark::Kind::HighestDynamicRange, "highestDynamicRange"},
    {GainLandmark::Kind::HcgSwitch, "hcgSwitch"},
    {GainLandmark::Kind::Preset, "preset"},
    {GainLandmark::Kind::VendorDefault, "vendorDefault"},
}};

QJsonArray ints(const std::vector<int>& v) {
    QJsonArray a;
    for (int i : v) a.append(i);
    return a;
}

std::vector<int> ints(const QJsonValue& v) {
    std::vector<int> out;
    for (const auto& x : v.toArray()) out.push_back(x.toInt());
    return out;
}

QJsonObject roiJson(const Roi& r) { return {{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}, {"bin", r.bin}}; }

Roi roiFrom(const QJsonObject& o) {
    return {o["x"].toInt(), o["y"].toInt(), o["width"].toInt(), o["height"].toInt(), std::max(1, o["bin"].toInt(1))};
}

void putU32(QByteArray& b, uint32_t v) {
    char raw[4];
    qToBigEndian(v, raw);
    b.append(raw, 4);
}

}  // namespace

QString bayerToString(Bayer b) {
    switch (b) {
        case Bayer::RGGB: return "RGGB";
        case Bayer::GRBG: return "GRBG";
        case Bayer::GBRG: return "GBRG";
        case Bayer::BGGR: return "BGGR";
        case Bayer::None: break;
    }
    return "mono";
}

Bayer bayerFromString(const QString& s) {
    if (s == "RGGB") return Bayer::RGGB;
    if (s == "GRBG") return Bayer::GRBG;
    if (s == "GBRG") return Bayer::GBRG;
    if (s == "BGGR") return Bayer::BGGR;
    return Bayer::None;
}

CameraState snapshot(const ICamera& cam) {
    CameraState s;
    s.info = cam.info();
    s.controls = cam.controls();
    for (const auto& c : s.controls)
        if (auto v = cam.control(c.name)) s.values[c.name] = *v;
    s.readoutModes = cam.readoutModes();
    s.readout = cam.readoutMode();
    s.landmarks = cam.gainLandmarks();
    s.roi = cam.roi();
    s.bits = cam.bitDepth();
    return s;
}

QJsonObject cameraStateToJson(const CameraState& s) {
    const auto& i = s.info;
    QJsonObject info{{"vendor", QString::fromStdString(i.vendor)},
                     {"model", QString::fromStdString(i.model)},
                     {"serial", QString::fromStdString(i.serial)},
                     {"sensorWidth", i.sensorWidth},
                     {"sensorHeight", i.sensorHeight},
                     {"pixelUm", i.pixelUm},
                     {"bayer", bayerToString(i.bayer)},
                     {"bins", ints(i.bins)},
                     {"bitDepths", ints(i.bitDepths)}};
    if (i.adcBits) info["adcBits"] = *i.adcBits;
    QJsonArray controls;
    for (const auto& c : s.controls) {
        QJsonObject o{{"name", QString::fromStdString(c.name)}, {"min", c.min},         {"max", c.max},
                      {"step", c.step},                         {"writable", c.writable}, {"auto", c.autoSupported}};
        if (c.defaultValue) o["default"] = *c.defaultValue;
        controls.append(o);
    }
    QJsonObject values;
    for (const auto& [name, v] : s.values) values[QString::fromStdString(name)] = v;
    QJsonArray modes;
    for (const auto& m : s.readoutModes) {
        QJsonObject o{{"index", m.index}, {"name", QString::fromStdString(m.name)}};
        if (m.width) o["width"] = *m.width;
        if (m.height) o["height"] = *m.height;
        modes.append(o);
    }
    QJsonArray landmarks;
    for (const auto& l : s.landmarks) {
        QJsonObject o{{"label", QString::fromStdString(l.label)}, {"source", static_cast<int>(l.source)}};
        for (const auto& [k, name] : kKinds)
            if (k == l.kind) o["kind"] = name;
        if (l.gain) o["gain"] = *l.gain;
        if (l.offset) o["offset"] = *l.offset;
        landmarks.append(o);
    }
    return {{"info", info},           {"controls", controls}, {"values", values}, {"readoutModes", modes},
            {"readout", s.readout},   {"landmarks", landmarks}, {"roi", roiJson(s.roi)}, {"bits", s.bits}};
}

CameraState cameraStateFromJson(const QJsonObject& o) {
    CameraState s;
    const auto info = o["info"].toObject();
    auto& i = s.info;
    i.vendor = info["vendor"].toString().toStdString();
    i.model = info["model"].toString().toStdString();
    i.serial = info["serial"].toString().toStdString();
    i.sensorWidth = info["sensorWidth"].toInt();
    i.sensorHeight = info["sensorHeight"].toInt();
    i.pixelUm = info["pixelUm"].toDouble();
    if (info.contains("adcBits")) i.adcBits = info["adcBits"].toInt();
    i.bayer = bayerFromString(info["bayer"].toString());
    i.bins = ints(info["bins"]);
    i.bitDepths = ints(info["bitDepths"]);
    for (const auto& v : o["controls"].toArray()) {
        const auto c = v.toObject();
        ControlInfo ci;
        ci.name = c["name"].toString().toStdString();
        ci.min = c["min"].toDouble();
        ci.max = c["max"].toDouble();
        ci.step = c["step"].toDouble(1);
        ci.writable = c["writable"].toBool(true);
        ci.autoSupported = c["auto"].toBool();
        if (c.contains("default")) ci.defaultValue = c["default"].toDouble();
        s.controls.push_back(ci);
    }
    const auto values = o["values"].toObject();
    for (auto it = values.begin(); it != values.end(); ++it) s.values[it.key().toStdString()] = it.value().toDouble();
    for (const auto& v : o["readoutModes"].toArray()) {
        const auto m = v.toObject();
        ReadoutMode rm;
        rm.index = m["index"].toInt();
        rm.name = m["name"].toString().toStdString();
        if (m.contains("width")) rm.width = m["width"].toInt();
        if (m.contains("height")) rm.height = m["height"].toInt();
        s.readoutModes.push_back(rm);
    }
    s.readout = o["readout"].toInt();
    for (const auto& v : o["landmarks"].toArray()) {
        const auto l = v.toObject();
        GainLandmark g{GainLandmark::Kind::Preset, l["label"].toString().toStdString(), std::nullopt, std::nullopt,
                       static_cast<GainLandmark::Source>(l["source"].toInt())};
        for (const auto& [k, name] : kKinds)
            if (l["kind"].toString() == name) g.kind = k;
        if (l.contains("gain")) g.gain = l["gain"].toDouble();
        if (l.contains("offset")) g.offset = l["offset"].toDouble();
        s.landmarks.push_back(g);
    }
    s.roi = roiFrom(o["roi"].toObject());
    s.bits = o["bits"].toInt(8);
    return s;
}

QByteArray pack(Msg type, const QByteArray& payload) {
    QByteArray b;
    b.reserve(payload.size() + 5);
    putU32(b, static_cast<uint32_t>(payload.size() + 1));
    b.append(static_cast<char>(type));
    b.append(payload);
    return b;
}

QByteArray packJson(Msg type, const QJsonObject& o) { return pack(type, QJsonDocument(o).toJson(QJsonDocument::Compact)); }

QByteArray packFrame(const QJsonObject& header, const QByteArray& pixels) {
    const QByteArray h = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray payload;
    payload.reserve(4 + h.size() + pixels.size());
    putU32(payload, static_cast<uint32_t>(h.size()));
    payload.append(h);
    payload.append(pixels);
    return pack(Msg::Frame, payload);
}

QJsonObject Message::json() const { return QJsonDocument::fromJson(payload).object(); }

std::optional<Message> MessageReader::next() {
    if (error_ || buf_.size() - pos_ < 4) return std::nullopt;
    const uint32_t len = qFromBigEndian<uint32_t>(buf_.constData() + pos_);
    if (len == 0 || len > kMaxMessage) {
        error_ = true;
        return std::nullopt;
    }
    if (buf_.size() - pos_ - 4 < static_cast<qsizetype>(len)) return std::nullopt;
    Message m{static_cast<Msg>(static_cast<uint8_t>(buf_[pos_ + 4])), buf_.mid(pos_ + 5, len - 1)};
    pos_ += 4 + len;
    // compact now and then, not after every message
    if (pos_ > (1 << 20) || pos_ == buf_.size()) {
        buf_.remove(0, pos_);
        pos_ = 0;
    }
    return m;
}

bool splitFrame(const QByteArray& payload, QJsonObject& header, QByteArray& pixels) {
    if (payload.size() < 4) return false;
    const uint32_t hl = qFromBigEndian<uint32_t>(payload.constData());
    if (static_cast<qsizetype>(hl) > payload.size() - 4) return false;
    QJsonParseError err;
    const auto doc = QJsonDocument::fromJson(payload.mid(4, hl), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    header = doc.object();
    pixels = payload.mid(4 + hl);
    return true;
}

}  // namespace slopcap::net

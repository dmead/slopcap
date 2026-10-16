#include "devices/indi_client.hpp"

namespace slopcap::devices {

namespace {

std::optional<IndiProperty::Type> vectorType(QStringView tag) {
    // defNumberVector / setNumberVector / ...
    if (tag.endsWith(u"NumberVector")) return IndiProperty::Type::Number;
    if (tag.endsWith(u"SwitchVector")) return IndiProperty::Type::Switch;
    if (tag.endsWith(u"TextVector")) return IndiProperty::Type::Text;
    if (tag.endsWith(u"LightVector")) return IndiProperty::Type::Light;
    if (tag.endsWith(u"BLOBVector")) return IndiProperty::Type::Blob;
    return std::nullopt;
}

bool isElementTag(QStringView tag) {
    return tag == u"defNumber" || tag == u"oneNumber" || tag == u"defSwitch" || tag == u"oneSwitch" || tag == u"defText" ||
           tag == u"oneText" || tag == u"defLight" || tag == u"oneLight" || tag == u"defBLOB" || tag == u"oneBLOB";
}

QString escape(const QString& s) { return s.toHtmlEscaped(); }

}  // namespace

IndiClient::IndiClient(QObject* parent) : QObject(parent) {
    connect(&socket_, &QTcpSocket::connected, this, [this] {
        xml_.clear();
        xml_.addData(QByteArray("<indi>"));
        props_.clear();
        send(R"(<getProperties version="1.7"/>)");
        emit serverConnected();
    });
    connect(&socket_, &QTcpSocket::disconnected, this, &IndiClient::serverDisconnected);
    connect(&socket_, &QTcpSocket::errorOccurred, this, [this] { emit errorOccurred(socket_.errorString()); });
    connect(&socket_, &QTcpSocket::readyRead, this, &IndiClient::onReadyRead);
}

IndiClient::~IndiClient() {
    // The socket is destroyed after this object's own members; don't let its last signals reach a half-gone client.
    socket_.disconnect(this);
    socket_.abort();
}

void IndiClient::connectToServer(const QString& host, quint16 port) {
    socket_.abort();
    socket_.connectToHost(host, port);
}

void IndiClient::disconnectFromServer() { socket_.disconnectFromHost(); }

void IndiClient::onReadyRead() {
    xml_.addData(socket_.readAll());
    parse();
}

void IndiClient::parse() {
    while (!xml_.atEnd()) {
        const auto t = xml_.readNext();
        if (t == QXmlStreamReader::Invalid) {
            if (xml_.error() == QXmlStreamReader::PrematureEndOfDocumentError) return;   // wait for more data
            emit errorOccurred("INDI stream: " + xml_.errorString());
            socket_.abort();
            return;
        }
        if (t == QXmlStreamReader::StartElement) {
            const auto tag = xml_.name();
            const auto a = xml_.attributes();
            if (auto type = vectorType(tag)) {
                inVector_ = true;
                isDef_ = tag.startsWith(u"def");
                const QString dev = a.value("device").toString(), name = a.value("name").toString();
                auto& devProps = props_[dev];
                auto it = devProps.find(name);
                if (isDef_ || it == devProps.end()) {
                    current_ = IndiProperty{};
                    current_.type = *type;
                    current_.device = dev;
                    current_.name = name;
                } else {
                    current_ = it->second;
                }
                if (a.hasAttribute("label")) current_.label = a.value("label").toString();
                if (a.hasAttribute("group")) current_.group = a.value("group").toString();
                if (a.hasAttribute("state")) current_.state = a.value("state").toString();
                if (a.hasAttribute("perm")) current_.perm = a.value("perm").toString();
            } else if (inVector_ && isElementTag(tag)) {
                elemName_ = a.value("name").toString();
                auto it = current_.elements.find(elemName_);
                elem_ = it != current_.elements.end() ? it->second : IndiElement{};
                if (a.hasAttribute("label")) elem_.label = a.value("label").toString();
                if (a.hasAttribute("min")) elem_.min = a.value("min").toDouble();
                if (a.hasAttribute("max")) elem_.max = a.value("max").toDouble();
                if (a.hasAttribute("step")) elem_.step = a.value("step").toDouble();
                elemText_.clear();
            } else if (tag == u"delProperty") {
                const QString dev = a.value("device").toString(), name = a.value("name").toString();
                if (name.isEmpty()) {
                    props_.erase(dev);
                    emit propertyDeleted(dev, {});
                } else if (auto d = props_.find(dev); d != props_.end()) {
                    d->second.erase(name);
                    emit propertyDeleted(dev, name);
                }
            } else if (tag == u"message") {
                if (a.hasAttribute("message")) emit message(a.value("device").toString(), a.value("message").toString());
            }
        } else if (t == QXmlStreamReader::Characters) {
            if (!elemName_.isEmpty()) elemText_ += xml_.text();
        } else if (t == QXmlStreamReader::EndElement) {
            const auto tag = xml_.name();
            if (inVector_ && isElementTag(tag)) {
                elem_.text = elemText_.trimmed();
                if (!current_.elements.count(elemName_)) current_.order << elemName_;
                current_.elements[elemName_] = elem_;
                elemName_.clear();
            } else if (vectorType(tag) && inVector_) {
                inVector_ = false;
                props_[current_.device][current_.name] = current_;
                if (isDef_) emit propertyDefined(current_.device, current_.name);
                else emit propertyUpdated(current_.device, current_.name);
            }
        }
    }
}

const IndiProperty* IndiClient::property(const QString& device, const QString& name) const {
    auto d = props_.find(device);
    if (d == props_.end()) return nullptr;
    auto p = d->second.find(name);
    return p == d->second.end() ? nullptr : &p->second;
}

std::optional<double> IndiClient::number(const QString& device, const QString& name, const QString& element) const {
    const auto* p = property(device, name);
    if (!p) return std::nullopt;
    auto e = p->elements.find(element);
    if (e == p->elements.end()) return std::nullopt;
    return e->second.number();
}

QStringList IndiClient::devices() const {
    QStringList out;
    for (const auto& [d, _] : props_) out << d;
    return out;
}

void IndiClient::send(const QString& xml) {
    if (isConnected()) socket_.write(xml.toUtf8());
}

void IndiClient::sendNumbers(const QString& device, const QString& name, const std::map<QString, double>& values) {
    QString x = QString(R"(<newNumberVector device="%1" name="%2">)").arg(escape(device), escape(name));
    for (const auto& [e, v] : values) x += QString(R"(<oneNumber name="%1">%2</oneNumber>)").arg(escape(e)).arg(v, 0, 'g', 12);
    send(x + "</newNumberVector>");
}

void IndiClient::sendSwitch(const QString& device, const QString& name, const QString& element, bool on) {
    send(QString(R"(<newSwitchVector device="%1" name="%2"><oneSwitch name="%3">%4</oneSwitch></newSwitchVector>)")
             .arg(escape(device), escape(name), escape(element), on ? "On" : "Off"));
}

void IndiClient::sendText(const QString& device, const QString& name, const QString& element, const QString& value) {
    send(QString(R"(<newTextVector device="%1" name="%2"><oneText name="%3">%4</oneText></newTextVector>)")
             .arg(escape(device), escape(name), escape(element), escape(value)));
}

}  // namespace slopcap::devices

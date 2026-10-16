// Minimal INDI client (protocol 1.7) over Qt sockets: keeps a cache of every device's properties and sends number /
// switch / text changes. Enough for focusers now and mounts / filter wheels later, with no libindi dependency, so it
// builds the same on Windows (talking to an INDI server elsewhere).
//
// The INDI stream has no root element, so the parser is fed a synthetic "<indi>" first and then parses incrementally
// as data arrives.
#pragma once

#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QXmlStreamReader>

#include <map>
#include <optional>

namespace slopcap::devices {

struct IndiElement {
    QString label;
    QString text;          ///< raw value (number as text, "On"/"Off" for switches)
    double min = 0, max = 0, step = 0;
    double number() const { return text.trimmed().toDouble(); }
    bool on() const { return text.trimmed() == "On"; }
};

struct IndiProperty {
    enum class Type { Number, Switch, Text, Light, Blob };
    Type type = Type::Number;
    QString device, name, label, group;
    QString state;         ///< Idle, Ok, Busy, Alert
    QString perm;          ///< ro, wo, rw
    std::map<QString, IndiElement> elements;
    bool busy() const { return state == "Busy"; }
};

class IndiClient : public QObject {
    Q_OBJECT
public:
    explicit IndiClient(QObject* parent = nullptr);
    ~IndiClient() override;
    void connectToServer(const QString& host, quint16 port);
    void disconnectFromServer();
    bool isConnected() const { return socket_.state() == QAbstractSocket::ConnectedState; }

    const IndiProperty* property(const QString& device, const QString& name) const;
    std::optional<double> number(const QString& device, const QString& name, const QString& element) const;
    QStringList devices() const;

    void sendNumbers(const QString& device, const QString& name, const std::map<QString, double>& values);
    void sendSwitch(const QString& device, const QString& name, const QString& element, bool on = true);
    void sendText(const QString& device, const QString& name, const QString& element, const QString& value);

signals:
    void serverConnected();
    void serverDisconnected();
    void errorOccurred(const QString& message);
    void propertyDefined(const QString& device, const QString& name);
    void propertyUpdated(const QString& device, const QString& name);
    void propertyDeleted(const QString& device, const QString& name);
    void message(const QString& device, const QString& text);

private:
    void onReadyRead();
    void parse();
    void send(const QString& xml);

    QTcpSocket socket_;
    QXmlStreamReader xml_;
    std::map<QString, std::map<QString, IndiProperty>> props_;   // device -> name -> property
    // element being parsed
    IndiProperty current_;
    bool inVector_ = false, isDef_ = false;
    QString elemName_, elemText_;
    IndiElement elem_;
};

}  // namespace slopcap::devices

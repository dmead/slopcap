#include "mounts/transport.hpp"

#include <QTcpSocket>
#include <QUdpSocket>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace slopcap::mounts {

namespace {

#ifdef _WIN32

std::string lastError() {
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, GetLastError(), 0, buf, sizeof buf, nullptr);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s;
}

class SerialPort : public ITransport {
public:
    SerialPort(const std::string& path, int baud) : name_(path + " at " + std::to_string(baud) + " baud") {
        h_ = CreateFileA(("\\\\.\\" + path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) throw MountError("can't open " + path + ": " + lastError());
        DCB dcb{};
        dcb.DCBlength = sizeof dcb;
        GetCommState(h_, &dcb);
        dcb.BaudRate = static_cast<DWORD>(baud);
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = dcb.fDsrSensitivity = FALSE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;   // DTR rising resets an Arduino (OnStep)
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutX = dcb.fInX = dcb.fNull = dcb.fAbortOnError = FALSE;
        if (!SetCommState(h_, &dcb)) {
            CloseHandle(h_);
            throw MountError("can't set up " + path + ": " + lastError());
        }
        PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
    }
    ~SerialPort() override { CloseHandle(h_); }

    void write(std::string_view bytes) override {
        timeouts(1);
        DWORD n = 0;
        if (!WriteFile(h_, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr) || n != bytes.size())
            throw MountError("the link went away (" + name_ + ")");
    }
    std::string read(int ms) override {
        // MAXDWORD / MAXDWORD / ms: back as soon as anything has arrived, or after ms with nothing
        timeouts(std::max(1, ms));
        char buf[512];
        DWORD n = 0;
        if (!ReadFile(h_, buf, sizeof buf, &n, nullptr)) throw MountError("the link went away (" + name_ + ")");
        return std::string(buf, n);
    }
    void drain() override { PurgeComm(h_, PURGE_RXCLEAR); }
    std::string name() const override { return name_; }

private:
    void timeouts(int readMs) {
        if (readMs == readMs_) return;
        COMMTIMEOUTS t{MAXDWORD, MAXDWORD, static_cast<DWORD>(readMs), 0, 2000};
        SetCommTimeouts(h_, &t);
        readMs_ = readMs;
    }
    HANDLE h_ = INVALID_HANDLE_VALUE;
    int readMs_ = -1;
    std::string name_;
};

#else

speed_t speedFor(int baud) {
    switch (baud) {
        case 1200: return B1200;
        case 2400: return B2400;
        case 4800: return B4800;
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
#ifdef B460800
        case 460800: return B460800;
#endif
    }
    return 0;
}

class SerialPort : public ITransport {
public:
    SerialPort(const std::string& path, int baud) : name_(path + " at " + std::to_string(baud) + " baud") {
        const speed_t speed = speedFor(baud);
        if (!speed) throw MountError("serial ports don't do " + std::to_string(baud) + " baud");
        fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) throw MountError("can't open " + path + ": " + std::strerror(errno));
        termios t{};
        if (tcgetattr(fd_, &t) != 0) {
            ::close(fd_);
            throw MountError(path + " isn't a serial port");
        }
        cfmakeraw(&t);
        t.c_cflag = (t.c_cflag & ~static_cast<tcflag_t>(CSIZE | CSTOPB | PARENB | CRTSCTS | HUPCL)) | CS8 | CLOCAL | CREAD;
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        cfsetispeed(&t, speed);
        cfsetospeed(&t, speed);
        tcsetattr(fd_, TCSANOW, &t);
        int lines = TIOCM_DTR | TIOCM_RTS;   // low, as on Windows (a pseudo-terminal has none: ignored)
        ::ioctl(fd_, TIOCMBIC, &lines);
        tcflush(fd_, TCIOFLUSH);
    }
    ~SerialPort() override { ::close(fd_); }

    void write(std::string_view bytes) override {
        while (!bytes.empty()) {
            const auto n = ::write(fd_, bytes.data(), bytes.size());
            if (n > 0) {
                bytes.remove_prefix(static_cast<std::size_t>(n));
                continue;
            }
            pollfd p{fd_, POLLOUT, 0};
            if ((n < 0 && errno != EAGAIN) || ::poll(&p, 1, 2000) <= 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
                throw MountError("the link went away (" + name_ + ")");
        }
    }
    std::string read(int ms) override {
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, std::max(0, ms)) <= 0) return {};
        char buf[512];
        const auto n = ::read(fd_, buf, sizeof buf);
        if (n > 0) return std::string(buf, static_cast<std::size_t>(n));
        if (n < 0 && errno == EAGAIN) return {};
        throw MountError("the link went away (" + name_ + ")");   // unplugged: EOF or EIO
    }
    void drain() override { tcflush(fd_, TCIFLUSH); }
    std::string name() const override { return name_; }

private:
    int fd_ = -1;
    std::string name_;
};

#endif

class SocketLink : public ITransport {
public:
    SocketLink(bool udp, const std::string& host, int port)
        : name_((udp ? "udp://" : "tcp://") + host + ":" + std::to_string(port)), udp_(udp) {
        if (udp) s_ = std::make_unique<QUdpSocket>();
        else s_ = std::make_unique<QTcpSocket>();
        s_->connectToHost(QString::fromStdString(host), static_cast<quint16>(port));
        if (!s_->waitForConnected(5000)) throw MountError("can't reach " + name_ + ": " + s_->errorString().toStdString());
        if (!udp) s_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    }
    void write(std::string_view bytes) override {
        const auto n = static_cast<qint64>(bytes.size());
        if (s_->write(bytes.data(), n) != n || (!udp_ && !s_->waitForBytesWritten(2000)))
            throw MountError("the link went away (" + name_ + ")");
    }
    std::string read(int ms) override {
        if (udp_) {
            auto* u = static_cast<QUdpSocket*>(s_.get());
            if (!u->hasPendingDatagrams() && !u->waitForReadyRead(ms)) return {};
            QByteArray d(static_cast<qsizetype>(std::max<qint64>(0, u->pendingDatagramSize())), Qt::Uninitialized);
            const auto n = u->readDatagram(d.data(), d.size());
            return n > 0 ? std::string(d.constData(), static_cast<std::size_t>(n)) : std::string();
        }
        if (s_->bytesAvailable() == 0 && !s_->waitForReadyRead(ms)) {
            if (s_->state() != QAbstractSocket::ConnectedState) throw MountError("the link went away (" + name_ + ")");
            return {};
        }
        return s_->readAll().toStdString();
    }
    void drain() override {
        if (udp_) {
            auto* u = static_cast<QUdpSocket*>(s_.get());
            while (u->hasPendingDatagrams()) u->readDatagram(nullptr, 0);
        } else {
            s_->readAll();
        }
    }
    bool datagrams() const override { return udp_; }
    std::string name() const override { return name_; }

private:
    std::string name_;
    bool udp_;
    std::unique_ptr<QAbstractSocket> s_;
};

}  // namespace

std::unique_ptr<ITransport> openTransport(const std::string& where, int baud, int port) {
    for (const char* scheme : {"tcp://", "udp://"}) {
        if (where.rfind(scheme, 0) != 0) continue;
        std::string host = where.substr(6);
        if (const auto colon = host.rfind(':'); colon != std::string::npos && host.find(']') == std::string::npos) {
            port = std::atoi(host.c_str() + colon + 1);
            host.resize(colon);
        }
        if (host.empty() || port <= 0 || port > 65535) throw MountError("not a network address: " + where);
        return std::make_unique<SocketLink>(scheme[0] == 'u', host, port);
    }
    std::string path = where;
    if (const auto at = path.rfind('@'); at != std::string::npos) {
        baud = std::atoi(path.c_str() + at + 1);
        path.resize(at);
    }
    if (path.empty()) throw MountError("no port given (a serial port such as COM3 or /dev/ttyUSB0, or tcp://HOST:PORT)");
    return std::make_unique<SerialPort>(path, baud);
}

std::vector<std::string> listSerialPorts() {
    std::vector<std::string> out;
#ifdef _WIN32
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            char name[256], data[64];
            DWORD nameLen = sizeof name, dataLen = sizeof data, type = 0;
            if (RegEnumValueA(key, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(data), &dataLen) != ERROR_SUCCESS) break;
            if (type == REG_SZ) out.emplace_back(data, strnlen(data, dataLen));
        }
        RegCloseKey(key);
    }
#else
    namespace fs = std::filesystem;
    std::error_code ec;
    // by-id names first: they stay the same whichever USB socket the adapter is in
    for (const auto& e : fs::directory_iterator("/dev/serial/by-id", ec)) out.push_back(e.path().string());
    std::sort(out.begin(), out.end());
    std::vector<std::string> dev;
    for (const auto& e : fs::directory_iterator("/dev", ec)) {
        const auto n = e.path().filename().string();
        for (const char* p : {"ttyUSB", "ttyACM", "rfcomm", "cu."})
            if (n.rfind(p, 0) == 0) dev.push_back(e.path().string());
    }
    std::sort(dev.begin(), dev.end());
    out.insert(out.end(), dev.begin(), dev.end());
#endif
    return out;
}

}  // namespace slopcap::mounts

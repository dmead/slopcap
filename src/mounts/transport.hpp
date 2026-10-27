// The links a built-in mount driver talks over: a serial port (USB adapters, the mount's own USB, Bluetooth serial),
// TCP (OnStep's and SkyFi's Wi-Fi bridges) or UDP (Sky-Watcher's Wi-Fi, one datagram per command). All blocking, for
// the mount's worker thread. Serial is a small class of SlopCap's own (Win32 / termios) rather than Qt SerialPort, to
// keep the executable small; it leaves DTR and RTS low, since Arduino-based controllers (OnStep) reset when DTR rises.
//
// Where a mount is:  COM3  COM3@115200  /dev/ttyUSB0  /dev/cu.usbserial-1410@9600  tcp://HOST:PORT  udp://HOST[:PORT]
#pragma once

#include "mounts/mount_driver.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace slopcap::mounts {

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual void write(std::string_view bytes) = 0;   ///< throws MountError when the link is gone
    /// What has arrived, waiting up to `ms` for the first byte ("" on a timeout). A datagram link returns one
    /// datagram.
    virtual std::string read(int ms) = 0;
    virtual void drain() = 0;                         ///< drop anything waiting to be read
    virtual bool datagrams() const { return false; }  ///< UDP: one write is a command, one read its whole reply
    virtual std::string name() const = 0;             ///< for messages: "COM3 at 9600 baud", "udp://192.168.4.1:11880"
};

/// Open `where` (above). `baud` and `port` are the protocol's defaults when `where` doesn't give them.
std::unique_ptr<ITransport> openTransport(const std::string& where, int baud, int port);

/// Serial ports on this computer (COM ports from the registry; /dev/serial/by-id, ttyUSB, ttyACM, rfcomm; /dev/cu.*).
std::vector<std::string> listSerialPorts();

}  // namespace slopcap::mounts

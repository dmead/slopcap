// Picture Transfer Protocol (ISO 15740), what the stills-camera backend needs of it: transactions over a Transport (USB
// bulk pipes on Linux and macOS, Windows Portable Devices' MTP-extension commands on Windows), little-endian datasets,
// and the DeviceInfo and ObjectInfo datasets. Vendor extensions (Sony's) build on this: src/camera/sony_ptp.hpp.
#pragma once

#include "camera/camera.hpp"

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slopcap::ptp {

// operations
inline constexpr uint16_t kGetDeviceInfo = 0x1001, kOpenSession = 0x1002, kCloseSession = 0x1003, kGetObjectInfo = 0x1008,
                          kGetObject = 0x1009, kGetDevicePropDesc = 0x1014;
// responses
inline constexpr uint16_t kOk = 0x2001, kGeneralError = 0x2002, kSessionNotOpen = 0x2003, kOperationNotSupported = 0x2005,
                          kParameterNotSupported = 0x2006, kInvalidObjectHandle = 0x2009, kAccessDenied = 0x200F, kDeviceBusy = 0x2019,
                          kSessionAlreadyOpen = 0x201E;
// data types of device properties
inline constexpr uint16_t kInt8 = 1, kUint8 = 2, kInt16 = 3, kUint16 = 4, kInt32 = 5, kUint32 = 6, kInt64 = 7, kUint64 = 8,
                          kString = 0xFFFF;
// object formats
inline constexpr uint16_t kFormatJpeg = 0x3801, kFormatSonyArw = 0xB101;
/// More data than any picture: a length past it is a damaged answer, not something to make room for.
inline constexpr uint64_t kMaxData = uint64_t{1} << 30;

/// "0x2019 device busy": the code and, where it is a standard one, its name.
std::string responseText(uint16_t code);
/// "JPEG", "ARW", or the code in hex.
std::string formatName(uint16_t objectFormat);

/// A PTP failure that isn't a response code (those come back in Response): a dataset that doesn't parse, a command
/// the system refused.
struct Error : CameraError {
    using CameraError::CameraError;
};
/// The link itself failed: the camera is gone (unplugged, switched off) or doesn't answer.
struct LinkError : Error {
    using Error::Error;
};

struct Response {
    uint16_t code = 0;
    std::vector<uint32_t> params;
    bool ok() const { return code == kOk; }
};

enum class Data { None, In, Out };

/// One of the camera's events (its interrupt pipe): when it came, since the transport opened, its code and parameters.
struct Event {
    double atMs = 0;
    uint16_t code = 0;
    std::vector<uint32_t> params;
};

/// One PTP session with a camera. Not thread-safe: callers serialise transactions.
class Transport {
public:
    virtual ~Transport() = default;
    /// One transaction: operation `op` with up to five parameters, then its data phase: none, `out` to the camera, or
    /// the camera's data into *in (replaced). Throws Error when the link fails.
    virtual Response transact(uint16_t op, std::span<const uint32_t> params, Data phase, std::span<const uint8_t> out,
                              std::vector<uint8_t>* in) = 0;
    /// What carries it, for reports: "WPD" or "USB".
    virtual std::string kind() const = 0;
    /// The camera's events so far, where the transport reads them (PTP over USB: its interrupt pipe, all the time).
    virtual std::vector<Event> events() const { return {}; }
    /// After a transaction got stuck: a fresh session (PTP's Device Reset request, cleared pipes, OpenSession). False
    /// where the transport can't. A vendor's own state (Sony's handshake) has to be set up again.
    virtual bool recover() { return false; }
};

/// Transactions that must succeed (they throw CameraError naming `what` and the response otherwise).
void command(Transport& t, uint16_t op, std::initializer_list<uint32_t> params, std::string_view what);
std::vector<uint8_t> receive(Transport& t, uint16_t op, std::initializer_list<uint32_t> params, std::string_view what);
void send(Transport& t, uint16_t op, std::initializer_list<uint32_t> params, std::span<const uint8_t> data, std::string_view what);

/// Reads a little-endian dataset; throws Error when it runs out.
class Reader {
public:
    explicit Reader(std::span<const uint8_t> data) : d_(data) {}
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    /// A value of a property data type (signed types sign-extended); strings are skipped and read as 0.
    int64_t value(uint16_t type);
    std::string str();   ///< PTP string: a count of UTF-16 units (with the terminator), then the units; as UTF-8
    std::vector<uint16_t> u16array();
    std::vector<uint32_t> u32array();
    void skip(std::size_t n);
    std::size_t left() const { return d_.size() - pos_; }
    std::size_t pos() const { return pos_; }

private:
    void need(std::size_t n) const;
    std::span<const uint8_t> d_;
    std::size_t pos_ = 0;
};

class Writer {
public:
    Writer& u8(uint8_t v);
    Writer& u16(uint16_t v);
    Writer& u32(uint32_t v);
    Writer& u64(uint64_t v);
    Writer& value(uint16_t type, int64_t v);   ///< as Reader::value
    Writer& str(std::string_view utf8);         ///< ASCII as PTP string (what cameras send)
    Writer& u16array(std::span<const uint16_t> v);
    Writer& bytes(std::span<const uint8_t> v);
    const std::vector<uint8_t>& data() const { return d_; }
    std::vector<uint8_t> take() { return std::move(d_); }

private:
    std::vector<uint8_t> d_;
};

/// Size in bytes of one value of a property data type (0 for strings and arrays).
std::size_t typeSize(uint16_t type);

struct DeviceInfo {
    uint16_t standardVersion = 0;
    uint32_t vendorExtensionId = 0;
    uint16_t vendorExtensionVersion = 0;
    std::string vendorExtensionDesc;
    uint16_t functionalMode = 0;
    std::vector<uint16_t> operations, events, properties, captureFormats, imageFormats;
    std::string manufacturer, model, deviceVersion, serial;
    bool supports(uint16_t op) const;
};
DeviceInfo parseDeviceInfo(std::span<const uint8_t> data);
std::vector<uint8_t> packDeviceInfo(const DeviceInfo& info);   ///< for fakes and tests

struct ObjectInfo {
    uint32_t storageId = 0;
    uint16_t format = 0;
    uint32_t compressedSize = 0;
    uint32_t imageWidth = 0, imageHeight = 0, imageBitDepth = 0;
    std::string filename;
};
ObjectInfo parseObjectInfo(std::span<const uint8_t> data);
std::vector<uint8_t> packObjectInfo(const ObjectInfo& info);

}  // namespace slopcap::ptp

#include "camera/ptp.hpp"

#include <algorithm>
#include <format>

namespace slopcap::ptp {

namespace {

const char* responseName(uint16_t code) {
    switch (code) {
        case kOk: return "OK";
        case kGeneralError: return "general error";
        case kSessionNotOpen: return "session not open";
        case 0x2004: return "invalid transaction id";
        case kOperationNotSupported: return "operation not supported";
        case kParameterNotSupported: return "parameter not supported";
        case 0x2007: return "incomplete transfer";
        case kInvalidObjectHandle: return "invalid object handle";
        case 0x200A: return "device property not supported";
        case kAccessDenied: return "access denied";
        case 0x2014: return "specification by format unsupported";
        case 0x2017: return "self-test failed";
        case kDeviceBusy: return "device busy";
        case 0x201B: return "invalid device property format";
        case 0x201C: return "invalid device property value";
        case 0x201D: return "invalid parameter";
        case kSessionAlreadyOpen: return "session already open";
        case 0x201F: return "transaction cancelled";
        default: return nullptr;
    }
}

}  // namespace

std::string responseText(uint16_t code) {
    const char* n = responseName(code);
    return n ? std::format("0x{:04X} {}", code, n) : std::format("0x{:04X}", code);
}

std::string formatName(uint16_t f) {
    switch (f) {
        case kFormatJpeg: return "JPEG";
        case kFormatSonyArw: return "ARW";
        case 0x3000: return "undefined";
        case 0x3808: return "JFIF";
        case 0x380D: return "TIFF";
        case 0xB110: return "HEIF";
        default: return std::format("0x{:04X}", f);
    }
}

void command(Transport& t, uint16_t op, std::initializer_list<uint32_t> params, std::string_view what) {
    const auto r = t.transact(op, std::span(params.begin(), params.size()), Data::None, {}, nullptr);
    if (!r.ok()) throw CameraError(std::format("{}: camera answered {}", what, responseText(r.code)));
}

std::vector<uint8_t> receive(Transport& t, uint16_t op, std::initializer_list<uint32_t> params, std::string_view what) {
    std::vector<uint8_t> in;
    const auto r = t.transact(op, std::span(params.begin(), params.size()), Data::In, {}, &in);
    if (!r.ok()) throw CameraError(std::format("{}: camera answered {}", what, responseText(r.code)));
    return in;
}

void send(Transport& t, uint16_t op, std::initializer_list<uint32_t> params, std::span<const uint8_t> data, std::string_view what) {
    const auto r = t.transact(op, std::span(params.begin(), params.size()), Data::Out, data, nullptr);
    if (!r.ok()) throw CameraError(std::format("{}: camera answered {}", what, responseText(r.code)));
}

// ---------------------------------------------------------------- Reader / Writer

void Reader::need(std::size_t n) const {
    if (n > left()) throw Error(std::format("PTP dataset ends early (needs {} more bytes at {}, has {})", n, pos_, left()));
}

uint8_t Reader::u8() {
    need(1);
    return d_[pos_++];
}

uint16_t Reader::u16() {
    need(2);
    const uint16_t v = static_cast<uint16_t>(d_[pos_] | d_[pos_ + 1] << 8);
    pos_ += 2;
    return v;
}

uint32_t Reader::u32() {
    const uint32_t lo = u16();
    return lo | static_cast<uint32_t>(u16()) << 16;
}

uint64_t Reader::u64() {
    const uint64_t lo = u32();
    return lo | static_cast<uint64_t>(u32()) << 32;
}

int64_t Reader::value(uint16_t type) {
    switch (type) {
        case kInt8: return static_cast<int8_t>(u8());
        case kUint8: return u8();
        case kInt16: return static_cast<int16_t>(u16());
        case kUint16: return u16();
        case kInt32: return static_cast<int32_t>(u32());
        case kUint32: return u32();
        case kInt64:
        case kUint64: return static_cast<int64_t>(u64());
        case kString: str(); return 0;
        default: throw Error(std::format("PTP data type 0x{:04X} not handled", type));
    }
}

std::string Reader::str() {
    const uint8_t n = u8();
    need(std::size_t{n} * 2);
    std::string out;
    for (uint8_t i = 0; i < n; ++i) {
        const uint16_t c = u16();
        if (c == 0) continue;   // the terminator (and any stray padding)
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | c >> 6);
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {   // (surrogate pairs come out as two 3-byte sequences: no camera names need them)
            out += static_cast<char>(0xE0 | c >> 12);
            out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::vector<uint16_t> Reader::u16array() {
    const uint32_t n = u32();
    need(std::size_t{n} * 2);
    std::vector<uint16_t> v(n);
    for (auto& x : v) x = u16();
    return v;
}

std::vector<uint32_t> Reader::u32array() {
    const uint32_t n = u32();
    need(std::size_t{n} * 4);
    std::vector<uint32_t> v(n);
    for (auto& x : v) x = u32();
    return v;
}

void Reader::skip(std::size_t n) {
    need(n);
    pos_ += n;
}

Writer& Writer::u8(uint8_t v) {
    d_.push_back(v);
    return *this;
}

Writer& Writer::u16(uint16_t v) {
    d_.push_back(static_cast<uint8_t>(v));
    d_.push_back(static_cast<uint8_t>(v >> 8));
    return *this;
}

Writer& Writer::u32(uint32_t v) { return u16(static_cast<uint16_t>(v)).u16(static_cast<uint16_t>(v >> 16)); }

Writer& Writer::u64(uint64_t v) { return u32(static_cast<uint32_t>(v)).u32(static_cast<uint32_t>(v >> 32)); }

Writer& Writer::value(uint16_t type, int64_t v) {
    switch (type) {
        case kInt8:
        case kUint8: return u8(static_cast<uint8_t>(v));
        case kInt16:
        case kUint16: return u16(static_cast<uint16_t>(v));
        case kInt32:
        case kUint32: return u32(static_cast<uint32_t>(v));
        case kInt64:
        case kUint64: return u64(static_cast<uint64_t>(v));
        case kString: return str("");
        default: throw Error(std::format("PTP data type 0x{:04X} not handled", type));
    }
}

Writer& Writer::str(std::string_view s) {
    if (s.empty()) return u8(0);
    const auto n = std::min<std::size_t>(s.size(), 254);
    u8(static_cast<uint8_t>(n + 1));
    for (std::size_t i = 0; i < n; ++i) u16(static_cast<uint8_t>(s[i]));
    return u16(0);
}

Writer& Writer::u16array(std::span<const uint16_t> v) {
    u32(static_cast<uint32_t>(v.size()));
    for (auto x : v) u16(x);
    return *this;
}

Writer& Writer::bytes(std::span<const uint8_t> v) {
    d_.insert(d_.end(), v.begin(), v.end());
    return *this;
}

std::size_t typeSize(uint16_t type) {
    switch (type) {
        case kInt8:
        case kUint8: return 1;
        case kInt16:
        case kUint16: return 2;
        case kInt32:
        case kUint32: return 4;
        case kInt64:
        case kUint64: return 8;
        default: return 0;
    }
}

// ---------------------------------------------------------------- datasets

bool DeviceInfo::supports(uint16_t op) const { return std::ranges::find(operations, op) != operations.end(); }

DeviceInfo parseDeviceInfo(std::span<const uint8_t> data) {
    Reader r(data);
    DeviceInfo d;
    d.standardVersion = r.u16();
    d.vendorExtensionId = r.u32();
    d.vendorExtensionVersion = r.u16();
    d.vendorExtensionDesc = r.str();
    d.functionalMode = r.u16();
    d.operations = r.u16array();
    d.events = r.u16array();
    d.properties = r.u16array();
    d.captureFormats = r.u16array();
    d.imageFormats = r.u16array();
    d.manufacturer = r.str();
    d.model = r.str();
    d.deviceVersion = r.str();
    d.serial = r.str();
    return d;
}

std::vector<uint8_t> packDeviceInfo(const DeviceInfo& d) {
    Writer w;
    w.u16(d.standardVersion).u32(d.vendorExtensionId).u16(d.vendorExtensionVersion).str(d.vendorExtensionDesc).u16(d.functionalMode);
    w.u16array(d.operations).u16array(d.events).u16array(d.properties).u16array(d.captureFormats).u16array(d.imageFormats);
    w.str(d.manufacturer).str(d.model).str(d.deviceVersion).str(d.serial);
    return w.take();
}

ObjectInfo parseObjectInfo(std::span<const uint8_t> data) {
    Reader r(data);
    ObjectInfo o;
    o.storageId = r.u32();
    o.format = r.u16();
    r.u16();   // protection status
    o.compressedSize = r.u32();
    r.skip(2 + 4 + 4 + 4);   // thumbnail format, size, width, height
    o.imageWidth = r.u32();
    o.imageHeight = r.u32();
    o.imageBitDepth = r.u32();
    r.skip(4 + 2 + 4 + 4);   // parent, association type and description, sequence number
    o.filename = r.str();
    return o;   // (capture and modification dates, keywords: not needed)
}

std::vector<uint8_t> packObjectInfo(const ObjectInfo& o) {
    Writer w;
    w.u32(o.storageId).u16(o.format).u16(0).u32(o.compressedSize);
    w.u16(0).u32(0).u32(0).u32(0);
    w.u32(o.imageWidth).u32(o.imageHeight).u32(o.imageBitDepth);
    w.u32(0).u16(0).u32(0).u32(0);
    w.str(o.filename).str("20261112T220000").str("").str("");
    return w.take();
}

}  // namespace slopcap::ptp

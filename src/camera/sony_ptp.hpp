// Sony's PTP extension, as Alpha and NEX cameras speak it over USB in "PC Remote" mode: a handshake (SDIO connect 1, 2,
// the extension's device info, connect 3), all properties in one dataset (GetAllDevicePropData), values set directly
// (SetControlDeviceA) or as button presses and one-notch steps (SetControlDeviceB), and the picture taken by pressing
// the shutter button half way (S1) and fully (S2), then fetched from a fixed object handle once the camera says it has
// it (the ObjectInMemory property). Live view, on bodies that have it over USB, is another fixed handle.
// Codes and layouts follow public descriptions of the protocol (libgphoto2's documentation of it among them); none of
// its code is used here. tools/sonyprobe checks them against a real camera.
#pragma once

#include "camera/ptp.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <optional>

namespace slopcap::sony {

inline constexpr uint16_t kUsbVendor = 0x054C;

// operations
inline constexpr uint16_t kSdioConnect = 0x9201, kGetSdioExtDeviceInfo = 0x9202, kSetControlDeviceA = 0x9205,
                          kSetControlDeviceB = 0x9207, kGetAllDevicePropData = 0x9209;
inline constexpr uint16_t kProtocol200 = 0x00C8;   // what we say we speak (the a6000 generation)
// properties
inline constexpr uint16_t kCompression = 0x5004, kFNumber = 0x5007, kFocusMode = 0x500A, kExposureProgram = 0x500E,
                          kImageSize = 0xD203, kShutterSpeed = 0xD20D, kAspectRatio = 0xD211, kFocusIndication = 0xD213,
                          kObjectInMemory = 0xD215, kBatteryLevel = 0xD218, kIso = 0xD21E, kSaveDestination = 0xD222,
                          kS1Button = 0xD2C1, kS2Button = 0xD2C2;
inline constexpr int64_t kButtonUp = 1, kButtonDown = 2;
inline constexpr int64_t kStepUp = 0x01, kStepDown = 0xFF;   ///< SetControlDeviceB's one-notch steps (direction learned)
inline constexpr int64_t kManualExposure = 0x0001;           ///< kExposureProgram: M
inline constexpr int64_t kQualityFine = 0x03, kQualityRaw = 0x10, kQualityRawJpeg = 0x13;   ///< kCompression
inline constexpr uint32_t kIsoAuto = 0xFFFFFF;
// objects
inline constexpr uint32_t kShotObject = 0xFFFFC001, kLiveViewObject = 0xFFFFC002;

struct Prop {
    enum class Form : uint8_t { None = 0, Range = 1, Enum = 2 };
    uint16_t code = 0, type = 0;
    uint8_t getSet = 0;    ///< 1: can be set
    uint8_t enabled = 0;   ///< 1: yes, 2: shown only, 0: not now (e.g. the mode dial doesn't allow it)
    int64_t factory = 0, current = 0;
    Form form = Form::None;
    int64_t min = 0, max = 0, step = 0;
    std::vector<int64_t> values;   ///< Enum: what the camera offers
    bool settable() const { return getSet == 1 && enabled == 1; }
};

struct ExtInfo {
    uint16_t version = 0;                     ///< the camera's protocol version (0xC8: 2.00)
    std::vector<std::vector<uint16_t>> lists; ///< the code lists it sends (properties; on some bodies a second list)
    std::vector<uint16_t> codes() const;      ///< all of them
};

ExtInfo parseExtInfo(std::span<const uint8_t> data);
std::vector<uint8_t> packExtInfo(const ExtInfo& info);
/// GetAllDevicePropData's dataset. From protocol version 3.00 an Enum form has two lists (settable, then all): the
/// first is kept. Stops at a property it can't read (`*stoppedAt` then says where; else it is set to 0).
std::vector<Prop> parseAllProps(std::span<const uint8_t> data, uint16_t version, std::size_t* stoppedAt = nullptr);
std::vector<uint8_t> packAllProps(const std::vector<Prop>& props, uint16_t version);

/// A short name for the properties we know ("Shutter speed"), else nullptr.
const char* propName(uint16_t code);
/// A value as the camera means it: "1/250", "2.5\"", "ISO 800", "M", ... (else the number).
std::string formatValue(uint16_t code, int64_t v);
/// Shutter speed values are numerator << 16 | denominator (seconds); 0 is bulb.
std::optional<double> shutterSeconds(int64_t v);
int64_t shutterValue(int numerator, int denominator);
/// ISO: the low 24 bits (kIsoAuto: auto); the high byte marks the multi-frame noise reduction variants.
std::optional<int> isoNumber(int64_t v);
/// The width of the model's sensor in millimetres, for the pixel size (0 if unknown).
double sensorWidthMm(std::string_view model);

/// The JPEG inside a blob (a live view frame comes with a header): from its first SOI marker to its last EOI marker.
std::span<const uint8_t> findJpeg(std::span<const uint8_t> blob);

struct ShotObject {
    ptp::ObjectInfo info;
    std::vector<uint8_t> data;
    double readyMs = 0;      ///< from the trigger to the camera saying it holds this object
    double downloadMs = 0;   ///< GetObjectInfo and GetObject
};

struct Shot {
    std::chrono::steady_clock::time_point triggered;   ///< S2 pressed: the exposure starts
    double pressMs = 0;                                ///< S1 down to S2 up
    std::vector<ShotObject> objects;                   ///< the picture (JPEG, and ARW too when set to RAW+JPEG)
    const ShotObject* jpeg() const;
};

/// The extension on one PTP transport. Not thread-safe.
class Session {
public:
    explicit Session(ptp::Transport& t, int pollMs = 30) : t_(t), pollMs_(pollMs) {}
    /// The handshake and the first read of the properties. Throws CameraError.
    const ExtInfo& connect();
    const ExtInfo& ext() const { return ext_; }

    void refresh();   ///< reads all properties again
    /// Right after the handshake a camera may report most values as 0 while it catches up (an a6000 did: ISO 0, the
    /// shutter speed 0): reads the properties again until the ISO (else the shutter speed) is non-zero, for up to
    /// `max`. True when it was.
    bool settle(std::chrono::milliseconds max);
    const std::vector<Prop>& props() const { return props_; }
    const Prop* prop(uint16_t code) const;
    const std::vector<uint8_t>& lastPropData() const { return propData_; }   ///< the dataset as read, for the probe
    std::size_t propsStoppedAt() const { return stoppedAt_; }

    /// SetControlDeviceA / B in the property's data type (B's steps and buttons: as given). The response code.
    uint16_t setA(uint16_t code, int64_t value);
    uint16_t setB(uint16_t code, uint16_t type, int64_t value);

    enum class SetMethod { None, Direct, Steps };
    /// A property to `target`: directly, else (older bodies take some only so) one notch at a time, ordered by
    /// `order` (e.g. the exposure time), stopping on the target or, if the camera skips it, on the nearer of the two
    /// values either side. Throws CameraError if the camera has the property but won't change it.
    SetMethod setValue(uint16_t code, int64_t target, const std::function<double(int64_t)>& order);
    /// How many objects the camera holds (ObjectInMemory, from the last refresh()).
    int objectsInMemory() const;

    /// Takes a picture and downloads it: drops what the camera still holds, S1 and S2 down, S2 and S1 up, then waits
    /// up to `timeout` for the camera to hold the picture and fetches everything it holds. `stop` is asked while waiting: true abandons the shot (nullopt).
    std::optional<Shot> shoot(std::chrono::milliseconds timeout, const std::function<bool()>& stop = {});
    /// Fetches and drops what the camera still holds (pictures from before we connected). How many.
    int drain();
    /// One live view frame, the JPEG in it; empty when the camera has none (`*response` says why).
    std::vector<uint8_t> liveView(uint16_t* response = nullptr);
    /// Fetches the object the camera holds first (its info and its data).
    ShotObject fetch();

private:
    bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds timeout, const std::function<bool()>& stop = {});
    ptp::Transport& t_;
    int pollMs_;
    ExtInfo ext_;
    std::vector<Prop> props_;
    std::vector<uint8_t> propData_;
    std::size_t stoppedAt_ = 0;
    std::map<uint16_t, int> stepSign_;   ///< per property: +1 if kStepUp raises order(), -1 if it lowers it
    std::optional<std::chrono::steady_clock::time_point> owed_;   ///< a picture still to come, until then
};

}  // namespace slopcap::sony

// PTP through Windows Portable Devices (Windows' own MTP/PTP driver, which binds to every PTP camera): WPD's
// MTP-extension commands (WPD_COMMAND_MTP_EXT_*) carry any operation, vendor ones included, so a camera needs no driver
// swap (Zadig, libusb). The driver opens the PTP session itself. The commands go through a seam, wpd::Device: the
// system's IPortableDevice in ptp_wpd_win.cpp, a fake in the tests; WpdTransport turns transactions into them.
#pragma once

#include "camera/ptp.hpp"

#include <memory>
#include <optional>

namespace slopcap::ptp::wpd {

/// The MTP-extension commands (their WPD command ids).
enum class Command : uint32_t {
    VendorOpcodes = 11,    ///< WPD_COMMAND_MTP_EXT_GET_SUPPORTED_VENDOR_OPCODES
    ExecuteNoData = 12,    ///< ..._EXECUTE_COMMAND_WITHOUT_DATA_PHASE
    ExecuteRead = 13,      ///< ..._EXECUTE_COMMAND_WITH_DATA_TO_READ
    ExecuteWrite = 14,     ///< ..._EXECUTE_COMMAND_WITH_DATA_TO_WRITE
    ReadData = 15,         ///< ..._READ_DATA
    WriteData = 16,        ///< ..._WRITE_DATA
    EndTransfer = 17,      ///< ..._END_DATA_TRANSFER
};

/// A command's parameters or results (the WPD_PROPERTY_MTP_EXT_* values), those that apply.
struct Values {
    std::optional<uint32_t> opCode;          ///< OPERATION_CODE
    std::vector<uint32_t> params;            ///< OPERATION_PARAMS in, RESPONSE_PARAMS or VENDOR_OPERATION_CODES out
    std::optional<uint32_t> responseCode;    ///< RESPONSE_CODE
    std::string context;                     ///< TRANSFER_CONTEXT
    std::optional<uint64_t> totalSize;       ///< TRANSFER_TOTAL_DATA_SIZE
    std::optional<uint32_t> bytes;           ///< NUM_BYTES_TO_READ / _TO_WRITE in, NUM_BYTES_READ / _WRITTEN out
    std::vector<uint8_t> data;               ///< TRANSFER_DATA: written (in), or read (out)
    std::optional<uint32_t> optimalBuffer;   ///< OPTIMAL_TRANSFER_BUFFER_SIZE
    int32_t hresult = 0;                     ///< the command's WPD_PROPERTY_COMMON_HRESULT
};

/// One opened WPD device.
class Device {
public:
    virtual ~Device() = default;
    /// IPortableDevice::SendCommand. Throws LinkError when the call itself fails; the command's own result is in hresult.
    virtual Values send(Command c, const Values& in) = 0;
};

/// "0x80070005 access denied": an HRESULT and, for the usual ones, what it means.
std::string hresultText(int32_t hr);

class WpdTransport final : public Transport {
public:
    explicit WpdTransport(std::unique_ptr<Device> device);
    Response transact(uint16_t op, std::span<const uint32_t> params, Data phase, std::span<const uint8_t> out,
                      std::vector<uint8_t>* in) override;
    std::string kind() const override { return "WPD"; }
    /// The vendor operations the driver lets through (it refuses the rest).
    std::vector<uint32_t> vendorOpcodes();

private:
    Values send(Command c, const Values& in, std::string_view what);   ///< throws on a failed HRESULT
    Response finish(const std::string& context, uint16_t op);          ///< END_DATA_TRANSFER
    std::unique_ptr<Device> dev_;
};

// ---------------------------------------------------------------- the system's devices (Windows only)

struct DeviceEntry {
    std::string id;                  ///< the WPD (PnP) device id
    std::string name, manufacturer, description;
};
/// Every WPD device (cameras, phones, players). Empty elsewhere than Windows. `problem`: why there are none when WPD
/// itself doesn't work (a Windows N edition without the Media Feature Pack has no WPD), else left empty.
std::vector<DeviceEntry> listDevices(std::string* problem = nullptr);
/// Opens one for MTP-extension commands. Throws CameraError (also on non-Windows systems).
std::unique_ptr<Device> openDevice(const std::string& id);
/// "vid_054c&pid_094e" from a device id: its USB vendor and product, without the serial number the id also holds.
std::string usbIdsOf(const std::string& id);

}  // namespace slopcap::ptp::wpd

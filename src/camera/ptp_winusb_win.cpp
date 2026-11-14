// PTP over USB on Windows for a camera whose driver is WinUSB (as Zadig installs it, or a camera Windows has no driver
// of its own for): WinUSB's API on the interface's device path, behind ptp_usb.hpp's UsbPipe. The device is found
// through SetupAPI, its interface GUID in its registry key (DeviceInterfaceGUIDs, as the installing INF set it).
#include "camera/ptp_usb.hpp"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <objbase.h>
#include <winusb.h>

#include <algorithm>
#include <cwctype>
#include <format>

namespace slopcap::ptp {

namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring property(HDEVINFO set, SP_DEVINFO_DATA& d, DWORD prop) {
    DWORD type = 0, size = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &d, prop, &type, nullptr, 0, &size);
    if (size < sizeof(wchar_t)) return {};
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(set, &d, prop, &type, reinterpret_cast<BYTE*>(s.data()), size, nullptr)) return {};
    s.resize(wcsnlen(s.c_str(), s.size()));   // (a multi-string: its first entry)
    return s;
}

/// The USB class its compatible ids give (6: still image, 0xFF: vendor-specific; 0: neither or unknown).
int usbClassOf(HDEVINFO set, SP_DEVINFO_DATA& d) {
    DWORD type = 0, size = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_COMPATIBLEIDS, &type, nullptr, 0, &size);
    if (size < sizeof(wchar_t)) return 0;
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_COMPATIBLEIDS, &type, reinterpret_cast<BYTE*>(s.data()), size, nullptr)) return 0;
    std::ranges::transform(s, s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    if (s.find(L"CLASS_06") != std::wstring::npos) return 6;
    if (s.find(L"CLASS_FF") != std::wstring::npos) return 0xFF;
    return 0;
}

/// "VID_054C&PID_094E" -> 0x054C, 0x094E
void idsOf(const std::wstring& hw, uint16_t& vid, uint16_t& pid) {
    std::wstring u = hw;
    std::ranges::transform(u, u.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    if (const auto v = u.find(L"VID_"); v != std::wstring::npos) vid = static_cast<uint16_t>(std::wcstoul(u.c_str() + v + 4, nullptr, 16));
    if (const auto p = u.find(L"PID_"); p != std::wstring::npos) pid = static_cast<uint16_t>(std::wcstoul(u.c_str() + p + 4, nullptr, 16));
}

/// The device's interface path, through the interface GUID its driver's INF registered.
std::wstring interfacePath(HDEVINFO set, SP_DEVINFO_DATA& d) {
    HKEY key = SetupDiOpenDevRegKey(set, &d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) return {};
    wchar_t buf[512] = {};
    DWORD size = sizeof buf - sizeof(wchar_t), type = 0;
    LONG r = RegQueryValueExW(key, L"DeviceInterfaceGUIDs", nullptr, &type, reinterpret_cast<BYTE*>(buf), &size);
    if (r != ERROR_SUCCESS) {
        size = sizeof buf - sizeof(wchar_t);
        r = RegQueryValueExW(key, L"DeviceInterfaceGUID", nullptr, &type, reinterpret_cast<BYTE*>(buf), &size);
    }
    RegCloseKey(key);
    GUID guid{};
    if (r != ERROR_SUCCESS || FAILED(CLSIDFromString(buf, &guid))) return {};
    wchar_t instance[MAX_DEVICE_ID_LEN] = {};
    if (CM_Get_Device_IDW(d.DevInst, instance, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return {};
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, &guid, instance, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len < 2) return {};
    std::wstring list(len, L'\0');
    if (CM_Get_Device_Interface_ListW(&guid, instance, list.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return {};
    return list.c_str();   // the first
}

std::string lastError(const char* what) {
    const DWORD e = GetLastError();
    switch (e) {
        case ERROR_SEM_TIMEOUT: return std::format("{}: no answer in time", what);
        case ERROR_ACCESS_DENIED: return std::format("{}: in use by another program", what);
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_FILE_NOT_FOUND:
        case ERROR_GEN_FAILURE:
        case ERROR_BAD_COMMAND: return std::format("{}: the camera is gone (unplugged or switched off)", what);
        default: return std::format("{}: Windows error {}", what, e);
    }
}

class WinUsbPipe final : public UsbPipe {
public:
    WinUsbPipe(HANDLE file, WINUSB_INTERFACE_HANDLE usb, UCHAR in, UCHAR out, UCHAR events, UCHAR ifc, std::size_t packet)
        : file_(file), usb_(usb), in_(in), out_(out), events_(events), ifc_(ifc), packet_(packet) {}
    ~WinUsbPipe() override {
        WinUsb_Free(usb_);
        CloseHandle(file_);
    }
    WinUsbPipe(const WinUsbPipe&) = delete;
    WinUsbPipe& operator=(const WinUsbPipe&) = delete;

    void write(std::span<const uint8_t> data) override {
        timeout(out_, 10000);
        ULONG done = 0;
        if (!WinUsb_WritePipe(usb_, out_, const_cast<UCHAR*>(data.data()), static_cast<ULONG>(data.size()), &done, nullptr))
            throw LinkError(lastError("USB write to the camera"));
    }
    std::size_t read(std::span<uint8_t> buf, std::chrono::milliseconds t) override {
        timeout(in_, static_cast<ULONG>(std::clamp<int64_t>(t.count(), 1, 600000)));
        ULONG got = 0;
        if (!WinUsb_ReadPipe(usb_, in_, buf.data(), static_cast<ULONG>(std::min<std::size_t>(buf.size(), 1u << 30)), &got, nullptr))
            throw LinkError(lastError("USB read from the camera"));
        return got;
    }
    std::size_t packetSize() const override { return packet_; }
    bool hasEvents() const override { return events_ != 0; }
    std::size_t readEvent(std::span<uint8_t> buf, std::chrono::milliseconds t) override {
        timeout(events_, static_cast<ULONG>(std::clamp<int64_t>(t.count(), 1, 600000)));
        ULONG got = 0;
        if (WinUsb_ReadPipe(usb_, events_, buf.data(), static_cast<ULONG>(std::min<std::size_t>(buf.size(), 65536)), &got, nullptr)) return got;
        const DWORD e = GetLastError();
        if (e == ERROR_DEVICE_NOT_CONNECTED || e == ERROR_FILE_NOT_FOUND || e == ERROR_BAD_COMMAND) throw LinkError("the camera is gone");
        if (e != ERROR_SEM_TIMEOUT) Sleep(static_cast<DWORD>(t.count()));   // (a stall: try again later)
        return 0;
    }
    void reset() override {
        // PTP's Device Reset Request (class-specific, to the interface), then the bulk pipes reset
        WinUsb_AbortPipe(usb_, in_);
        WinUsb_AbortPipe(usb_, out_);
        WINUSB_SETUP_PACKET setup{};
        setup.RequestType = 0x21;
        setup.Request = 0x66;
        setup.Index = ifc_;
        ULONG n = 0;
        WinUsb_ControlTransfer(usb_, setup, nullptr, 0, &n, nullptr);
        WinUsb_ResetPipe(usb_, in_);
        WinUsb_ResetPipe(usb_, out_);
    }

private:
    void timeout(UCHAR pipe, ULONG ms) { WinUsb_SetPipePolicy(usb_, pipe, PIPE_TRANSFER_TIMEOUT, sizeof ms, &ms); }
    HANDLE file_;
    WINUSB_INTERFACE_HANDLE usb_;
    UCHAR in_, out_, events_, ifc_;
    std::size_t packet_;
};

}  // namespace

std::vector<UsbCameraEntry> listUsbCameras(uint16_t vendor) {
    std::vector<UsbCameraEntry> out;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA d{};
    d.cbSize = sizeof d;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        std::wstring service = property(set, d, SPDRP_SERVICE);
        std::ranges::transform(service, service.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (service != L"winusb") continue;
        uint16_t vid = 0, pid = 0;
        idsOf(property(set, d, SPDRP_HARDWAREID), vid, pid);
        if (vendor && vid != vendor) continue;
        const std::wstring path = interfacePath(set, d);
        UsbCameraEntry e{"winusb:" + utf8(path), vid, pid, utf8(property(set, d, SPDRP_MFG)), {}, {}, usbClassOf(set, d)};
        e.model = utf8(property(set, d, SPDRP_FRIENDLYNAME));
        if (e.model.empty()) e.model = utf8(property(set, d, SPDRP_DEVICEDESC));
        if (path.empty()) e.problem = "on WinUSB, but no device interface registered for it (reinstall the driver)";
        out.push_back(std::move(e));
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

std::unique_ptr<UsbPipe> openUsbCamera(const std::string& id) {
    if (!id.starts_with("winusb:") || id.size() == 7) throw CameraError(std::format("no WinUSB camera {}", id));
    HANDLE file = CreateFileW(wide(id.substr(7)).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw CameraError(lastError("open the camera"));
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(file, &usb)) {
        const auto why = lastError("WinUSB");
        CloseHandle(file);
        throw CameraError(why);
    }
    USB_INTERFACE_DESCRIPTOR desc{};
    UCHAR in = 0, out = 0, events = 0;
    std::size_t packet = 512;
    if (WinUsb_QueryInterfaceSettings(usb, 0, &desc))
        for (UCHAR i = 0; i < desc.bNumEndpoints; ++i) {
            WINUSB_PIPE_INFORMATION p{};
            if (!WinUsb_QueryPipe(usb, 0, i, &p)) continue;
            if (p.PipeType == UsbdPipeTypeInterrupt && (p.PipeId & 0x80)) events = p.PipeId;
            if (p.PipeType != UsbdPipeTypeBulk) continue;
            if (p.PipeId & 0x80) in = p.PipeId;
            else out = p.PipeId;
            if (p.MaximumPacketSize) packet = p.MaximumPacketSize;
        }
    if (!in || !out) {
        WinUsb_Free(usb);
        CloseHandle(file);
        throw CameraError("the camera's USB interface has no pair of bulk pipes (not its PTP interface?)");
    }
    WinUsb_ResetPipe(usb, in);   // (as libusb's clear_halt: a pipe left stalled by an earlier program)
    WinUsb_ResetPipe(usb, out);
    return std::make_unique<WinUsbPipe>(file, usb, in, out, events, desc.bInterfaceNumber, packet);
}

std::string libusbProblem() { return {}; }

}  // namespace slopcap::ptp

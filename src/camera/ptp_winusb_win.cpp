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
#include <chrono>
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

/// Windows' own words for an error, in UTF-8.
std::string message(DWORD e) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                                   reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring w = n && buf ? std::wstring(buf, n) : std::wstring();
    if (buf) LocalFree(buf);
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r' || w.back() == L' ' || w.back() == L'.')) w.pop_back();
    return w.empty() ? "unknown error" : utf8(w);
}

/// What a Windows error on the camera's pipes means, with its number: a report from far away is all there is to go on.
std::string problem(const char* what, DWORD e) {
    switch (e) {
        case ERROR_SEM_TIMEOUT: return std::format("{}: no answer in time", what);
        case ERROR_ACCESS_DENIED: return std::format("{}: in use by another program (Windows error 5)", what);
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_FILE_NOT_FOUND: return std::format("{}: the camera is gone, unplugged or switched off (Windows error {})", what, e);
        case ERROR_GEN_FAILURE: return std::format("{}: the transfer failed, stopped by the camera or a USB error (Windows error 31)", what);
        default: return std::format("{}: {} (Windows error {})", what, message(e), e);
    }
}

std::string lastError(const char* what) { return problem(what, GetLastError()); }

DWORD ms(std::chrono::milliseconds t) { return static_cast<DWORD>(std::clamp<int64_t>(t.count(), 1, 600000)); }

/// An OVERLAPPED with an event of its own: one for each pipe, as each has at most one transfer out at a time.
struct Overlapped {
    OVERLAPPED ov{};
    Overlapped() { ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~Overlapped() { CloseHandle(ov.hEvent); }
    Overlapped(const Overlapped&) = delete;
    Overlapped& operator=(const Overlapped&) = delete;
    OVERLAPPED* fresh() {
        const HANDLE ev = ov.hEvent;
        ov = OVERLAPPED{};
        ov.hEvent = ev;
        ResetEvent(ev);
        return &ov;
    }
};

// Every transfer is overlapped and waited for here, aborted when it takes too long: WinUSB's own timeouts (a pipe
// policy) and its blocking calls aren't used, as the bulk pipes and the interrupt pipe are in use from two threads at
// once. Bulk-in transfers are 64 KB at most; the transport asks again for the rest.
class WinUsbPipe final : public UsbPipe {
public:
    WinUsbPipe(HANDLE file, WINUSB_INTERFACE_HANDLE usb, UCHAR in, UCHAR out, UCHAR events, UCHAR ifc, std::size_t packet)
        : file_(file), usb_(usb), in_(in), out_(out), events_(events), ifc_(ifc), packet_(packet) {}
    ~WinUsbPipe() override {
        if (eventPending_) {   // the interrupt read still out (the transport's event thread has stopped)
            WinUsb_AbortPipe(usb_, events_);
            ULONG n = 0;
            WinUsb_GetOverlappedResult(usb_, &interrupt_.ov, &n, TRUE);
        }
        WinUsb_Free(usb_);
        CloseHandle(file_);
    }
    WinUsbPipe(const WinUsbPipe&) = delete;
    WinUsbPipe& operator=(const WinUsbPipe&) = delete;

    void write(std::span<const uint8_t> data) override {
        ULONG done = 0;
        const auto start = [&](OVERLAPPED* ov) {
            return WinUsb_WritePipe(usb_, out_, const_cast<UCHAR*>(data.data()), static_cast<ULONG>(data.size()), nullptr, ov);
        };
        if (const DWORD e = transfer(out_, bulkOut_, start, 10000, done)) throw LinkError(problem("USB write to the camera", e));
    }
    std::size_t read(std::span<uint8_t> buf, std::chrono::milliseconds t) override {
        const ULONG n = static_cast<ULONG>(std::min<std::size_t>(buf.size(), kMaxTransfer));
        ULONG got = 0;
        const auto start = [&](OVERLAPPED* ov) { return WinUsb_ReadPipe(usb_, in_, buf.data(), n, nullptr, ov); };
        if (const DWORD e = transfer(in_, bulkIn_, start, ms(t), got)) throw LinkError(problem("USB read from the camera", e));
        return got;
    }
    std::size_t packetSize() const override { return packet_; }
    bool hasEvents() const override { return events_ != 0; }
    std::size_t readEvent(std::span<uint8_t> buf, std::chrono::milliseconds t) override {
        // one read kept out on the pipe from call to call, never cut short: no event is lost to a timeout
        if (!eventPending_) {
            if (!WinUsb_ReadPipe(usb_, events_, eventBuf_, sizeof eventBuf_, nullptr, interrupt_.fresh()) && GetLastError() != ERROR_IO_PENDING)
                return eventProblem(GetLastError(), t);
            eventPending_ = true;
        }
        if (WaitForSingleObject(interrupt_.ov.hEvent, ms(t)) == WAIT_TIMEOUT) return 0;
        eventPending_ = false;
        ULONG got = 0;
        if (!WinUsb_GetOverlappedResult(usb_, &interrupt_.ov, &got, FALSE)) return eventProblem(GetLastError(), t);
        const std::size_t n = std::min<std::size_t>(got, buf.size());
        std::copy_n(eventBuf_, n, buf.begin());
        return n;
    }
    void cancel(uint32_t tid) override {
        // PTP's Cancel Request (class-specific, to the interface): the cancellation code 0x4001 and the transaction
        uint8_t d[6] = {0x01, 0x40, static_cast<uint8_t>(tid), static_cast<uint8_t>(tid >> 8), static_cast<uint8_t>(tid >> 16),
                        static_cast<uint8_t>(tid >> 24)};
        ULONG n = 0;
        if (const DWORD e = control(0x21, 0x64, d, n)) throw LinkError(problem("USB cancel request", e));
    }
    uint16_t status() override {
        // PTP's Get Device Status Request: its length, then a response code (then the endpoints it stalled)
        uint8_t d[64] = {};
        ULONG n = 0;
        if (const DWORD e = control(0xA1, 0x67, d, n)) throw LinkError(problem("USB status request", e));
        return n >= 4 ? static_cast<uint16_t>(d[2] | d[3] << 8) : 0;
    }
    void reset() override {
        // PTP's Device Reset Request (class-specific, to the interface), then the bulk pipes reset (their halts cleared)
        ULONG n = 0;
        const DWORD e = control(0x21, 0x66, {}, n);
        WinUsb_ResetPipe(usb_, in_);
        WinUsb_ResetPipe(usb_, out_);
        if (e) throw LinkError(problem("USB reset request", e));
    }

private:
    static constexpr std::size_t kMaxTransfer = 64 * 1024;

    /// One transfer that `start` begins with an OVERLAPPED, waited for up to `wait` ms and aborted after that (pipe
    /// 0: the control pipe, cancelled instead): 0, or the Windows error.
    template <class Start>
    DWORD transfer(UCHAR pipe, Overlapped& o, Start&& start, DWORD wait, ULONG& done) {
        OVERLAPPED* ov = o.fresh();
        done = 0;
        if (!start(ov) && GetLastError() != ERROR_IO_PENDING) return GetLastError();
        if (WaitForSingleObject(ov->hEvent, wait) == WAIT_TIMEOUT) {
            if (pipe) WinUsb_AbortPipe(usb_, pipe);
            else CancelIoEx(file_, ov);
            // until the abort has landed (the buffer is in use till then); it may have finished just before
            return WinUsb_GetOverlappedResult(usb_, ov, &done, TRUE) ? ERROR_SUCCESS : ERROR_SEM_TIMEOUT;
        }
        return WinUsb_GetOverlappedResult(usb_, ov, &done, FALSE) ? ERROR_SUCCESS : GetLastError();
    }
    /// A class-specific request to the PTP interface, `data` its data stage (in or out, as `type` says).
    DWORD control(UCHAR type, UCHAR request, std::span<uint8_t> data, ULONG& done) {
        WINUSB_SETUP_PACKET setup{};
        setup.RequestType = type;
        setup.Request = request;
        setup.Index = ifc_;
        setup.Length = static_cast<USHORT>(data.size());
        const auto start = [&](OVERLAPPED* ov) { return WinUsb_ControlTransfer(usb_, setup, data.data(), setup.Length, nullptr, ov); };
        return transfer(0, control_, start, 2000, done);
    }
    std::size_t eventProblem(DWORD e, std::chrono::milliseconds t) {
        if (e == ERROR_DEVICE_NOT_CONNECTED || e == ERROR_FILE_NOT_FOUND) throw LinkError("the camera is gone");
        if (e == ERROR_GEN_FAILURE) WinUsb_ResetPipe(usb_, events_);   // stalled: cleared, then read again
        Sleep(ms(t));
        return 0;
    }

    HANDLE file_;
    WINUSB_INTERFACE_HANDLE usb_;
    UCHAR in_, out_, events_, ifc_;
    std::size_t packet_;
    Overlapped bulkIn_, bulkOut_, interrupt_, control_;
    bool eventPending_ = false;   ///< (the event thread's alone, then the destructor's)
    UCHAR eventBuf_[1024] = {};
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

// libusb for the PTP-over-USB transport (Linux, macOS), loaded at run time: SlopCap doesn't link against it, and the
// build needs no libusb headers. The declarations below are libusb 1.0's stable ABI, the parts this file uses.
#include "camera/ptp_usb.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <climits>
#include <format>
#include <mutex>
#include <optional>
#include <thread>

namespace slopcap::ptp {

namespace {

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

struct libusb_device_descriptor {
    uint8_t bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};
struct libusb_endpoint_descriptor {
    uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval, bRefresh, bSynchAddress;
    const unsigned char* extra;
    int extra_length;
};
struct libusb_interface_descriptor {
    uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints, bInterfaceClass, bInterfaceSubClass,
        bInterfaceProtocol, iInterface;
    const libusb_endpoint_descriptor* endpoint;
    const unsigned char* extra;
    int extra_length;
};
struct libusb_interface {
    const libusb_interface_descriptor* altsetting;
    int num_altsetting;
};
struct libusb_config_descriptor {
    uint8_t bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, MaxPower;
    const libusb_interface* interface;
    const unsigned char* extra;
    int extra_length;
};

constexpr int kErrAccess = -3, kErrNoDevice = -4, kErrBusy = -6, kErrTimeout = -7;
constexpr uint8_t kImageClass = 6;

/// The library and the functions used, looked up once; never unloaded.
struct Lib {
    void* handle = nullptr;
    libusb_context* ctx = nullptr;
    std::string problem;
    int (*init)(libusb_context**) = nullptr;
    long (*get_device_list)(libusb_context*, libusb_device***) = nullptr;
    void (*free_device_list)(libusb_device**, int) = nullptr;
    int (*get_device_descriptor)(libusb_device*, libusb_device_descriptor*) = nullptr;
    int (*get_active_config_descriptor)(libusb_device*, libusb_config_descriptor**) = nullptr;
    int (*get_config_descriptor)(libusb_device*, uint8_t, libusb_config_descriptor**) = nullptr;
    void (*free_config_descriptor)(libusb_config_descriptor*) = nullptr;
    uint8_t (*get_bus_number)(libusb_device*) = nullptr;
    uint8_t (*get_device_address)(libusb_device*) = nullptr;
    int (*open)(libusb_device*, libusb_device_handle**) = nullptr;
    void (*close)(libusb_device_handle*) = nullptr;
    int (*claim_interface)(libusb_device_handle*, int) = nullptr;
    int (*release_interface)(libusb_device_handle*, int) = nullptr;
    int (*set_auto_detach_kernel_driver)(libusb_device_handle*, int) = nullptr;
    int (*bulk_transfer)(libusb_device_handle*, unsigned char, unsigned char*, int, int*, unsigned int) = nullptr;
    int (*interrupt_transfer)(libusb_device_handle*, unsigned char, unsigned char*, int, int*, unsigned int) = nullptr;
    int (*control_transfer)(libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char*, uint16_t, unsigned int) = nullptr;
    int (*get_string_descriptor_ascii)(libusb_device_handle*, uint8_t, unsigned char*, int) = nullptr;
    int (*clear_halt)(libusb_device_handle*, unsigned char) = nullptr;
    const char* (*error_name)(int) = nullptr;

    std::string err(int code) const { return error_name ? error_name(code) : std::format("libusb error {}", code); }
};

template <class F>
void bind(Lib& l, F& f, const char* name) {
    f = reinterpret_cast<F>(dlsym(l.handle, name));
    if (!f && l.problem.empty()) l.problem = std::format("libusb has no {}", name);
}

Lib& lib() {
    static Lib l;
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef __APPLE__
        const char* names[] = {"@rpath/libusb-1.0.0.dylib", "libusb-1.0.0.dylib", "/opt/homebrew/lib/libusb-1.0.0.dylib",
                               "/usr/local/lib/libusb-1.0.0.dylib"};
#else
        const char* names[] = {"libusb-1.0.so.0", "libusb-1.0.so"};
#endif
        for (const char* n : names)
            if ((l.handle = dlopen(n, RTLD_NOW | RTLD_LOCAL))) break;
        if (!l.handle) {
            l.problem = "libusb 1.0 not found (Linux: install libusb-1.0-0; macOS: brew install libusb)";
            return;
        }
        bind(l, l.init, "libusb_init");
        bind(l, l.get_device_list, "libusb_get_device_list");
        bind(l, l.free_device_list, "libusb_free_device_list");
        bind(l, l.get_device_descriptor, "libusb_get_device_descriptor");
        bind(l, l.get_active_config_descriptor, "libusb_get_active_config_descriptor");
        bind(l, l.get_config_descriptor, "libusb_get_config_descriptor");
        bind(l, l.free_config_descriptor, "libusb_free_config_descriptor");
        bind(l, l.get_bus_number, "libusb_get_bus_number");
        bind(l, l.get_device_address, "libusb_get_device_address");
        bind(l, l.open, "libusb_open");
        bind(l, l.close, "libusb_close");
        bind(l, l.claim_interface, "libusb_claim_interface");
        bind(l, l.release_interface, "libusb_release_interface");
        bind(l, l.set_auto_detach_kernel_driver, "libusb_set_auto_detach_kernel_driver");
        bind(l, l.bulk_transfer, "libusb_bulk_transfer");
        bind(l, l.interrupt_transfer, "libusb_interrupt_transfer");
        bind(l, l.control_transfer, "libusb_control_transfer");
        bind(l, l.get_string_descriptor_ascii, "libusb_get_string_descriptor_ascii");
        bind(l, l.clear_halt, "libusb_clear_halt");
        bind(l, l.error_name, "libusb_error_name");
        if (!l.problem.empty()) return;
        if (const int e = l.init(&l.ctx); e != 0) l.problem = std::format("libusb_init: {}", l.err(e));
    });
    return l;
}

/// The PTP interface of a device and its bulk endpoints.
struct PtpInterface {
    int number = -1, cls = 0;
    uint8_t in = 0, out = 0, events = 0;   ///< bulk in and out; interrupt in (0: none)
    std::size_t packet = 512;
};

/// The Still Image interface; with `vendorClass` also, failing that, a vendor-specific one with a pair of bulk pipes (a
/// camera in a remote-control mode may describe itself so: gphoto2 finds such cameras by their IDs, not their class).
std::optional<PtpInterface> findInterface(Lib& l, libusb_device* dev, bool vendorClass) {
    libusb_config_descriptor* cfg = nullptr;
    if (l.get_active_config_descriptor(dev, &cfg) != 0 && l.get_config_descriptor(dev, 0, &cfg) != 0) return std::nullopt;
    std::optional<PtpInterface> found, fallback;
    for (int i = 0; i < cfg->bNumInterfaces && !found; ++i) {
        const auto& itf = cfg->interface[i];
        if (itf.num_altsetting < 1) continue;
        const auto& alt = itf.altsetting[0];
        if (alt.bInterfaceClass != kImageClass && !(vendorClass && alt.bInterfaceClass == 0xFF)) continue;
        PtpInterface p;
        p.number = alt.bInterfaceNumber;
        p.cls = alt.bInterfaceClass;
        for (int e = 0; e < alt.bNumEndpoints; ++e) {
            const auto& ep = alt.endpoint[e];
            if ((ep.bmAttributes & 3) == 3 && (ep.bEndpointAddress & 0x80)) p.events = ep.bEndpointAddress;   // interrupt in
            if ((ep.bmAttributes & 3) != 2) continue;
            if (ep.bEndpointAddress & 0x80) p.in = ep.bEndpointAddress;
            else p.out = ep.bEndpointAddress;
            if (ep.wMaxPacketSize) p.packet = ep.wMaxPacketSize & 0x7FF;
        }
        if (p.in && p.out) (p.cls == kImageClass ? found : fallback) = p;
    }
    l.free_config_descriptor(cfg);
    return found ? found : fallback;
}

std::string idOf(Lib& l, libusb_device* dev) {
    return std::format("usb:{:03}:{:03}", l.get_bus_number(dev), l.get_device_address(dev));
}

std::string openProblem(Lib& l, int e) {
    if (e == kErrAccess) return "no permission to open it (Linux: needs a udev rule, see the user guide)";
    if (e == kErrBusy) return "in use by another program (Linux: the desktop's gvfs often holds cameras; macOS: ptpcamerad)";
    return l.err(e);
}

class LibusbPipe final : public UsbPipe {
public:
    LibusbPipe(Lib& l, libusb_device_handle* h, PtpInterface p) : l_(l), h_(h), p_(p) {}
    ~LibusbPipe() override {
        l_.release_interface(h_, p_.number);
        l_.close(h_);
    }
    LibusbPipe(const LibusbPipe&) = delete;
    LibusbPipe& operator=(const LibusbPipe&) = delete;

    void write(std::span<const uint8_t> data) override {
        int done = 0;
        const int e = l_.bulk_transfer(h_, p_.out, const_cast<unsigned char*>(data.data()), static_cast<int>(data.size()), &done, 10000);
        if (e != 0) throw LinkError(std::format("USB write to the camera: {}", problem(e)));
    }
    std::size_t read(std::span<uint8_t> buf, std::chrono::milliseconds timeout) override {
        int got = 0;
        const int e = l_.bulk_transfer(h_, p_.in, buf.data(), static_cast<int>(std::min<std::size_t>(buf.size(), INT_MAX)), &got,
                                       static_cast<unsigned>(timeout.count()));
        if (e != 0) throw LinkError(std::format("USB read from the camera: {}", problem(e)));
        return static_cast<std::size_t>(got);
    }
    std::size_t packetSize() const override { return p_.packet; }
    bool hasEvents() const override { return p_.events != 0; }
    std::size_t readEvent(std::span<uint8_t> buf, std::chrono::milliseconds timeout) override {
        int got = 0;
        const int e = l_.interrupt_transfer(h_, p_.events, buf.data(), static_cast<int>(std::min<std::size_t>(buf.size(), INT_MAX)), &got,
                                            static_cast<unsigned>(timeout.count()));
        if (e == kErrNoDevice) throw LinkError("the camera is gone");
        if (e != 0 && e != kErrTimeout) std::this_thread::sleep_for(timeout);   // (a stall or an overflow: try again later)
        return e == 0 ? static_cast<std::size_t>(got) : 0;
    }
    void reset() override {
        // PTP's Device Reset Request (class-specific, to the interface), then the bulk pipes' halts cleared
        l_.control_transfer(h_, 0x21, 0x66, 0, static_cast<uint16_t>(p_.number), nullptr, 0, 2000);
        l_.clear_halt(h_, p_.in);
        l_.clear_halt(h_, p_.out);
    }

private:
    std::string problem(int e) const {
        if (e == kErrNoDevice) return "the camera is gone (unplugged or switched off)";
        if (e == kErrTimeout) return "no answer in time";
        return l_.err(e);
    }
    Lib& l_;
    libusb_device_handle* h_;
    PtpInterface p_;
};

}  // namespace

std::string libusbProblem() { return lib().problem; }

std::vector<UsbCameraEntry> listUsbCameras(uint16_t vendor) {
    std::vector<UsbCameraEntry> out;
    Lib& l = lib();
    if (!l.problem.empty()) return out;
    libusb_device** devs = nullptr;
    const long n = l.get_device_list(l.ctx, &devs);
    for (long i = 0; i < n; ++i) {
        libusb_device_descriptor d{};
        if (l.get_device_descriptor(devs[i], &d) != 0 || (vendor && d.idVendor != vendor)) continue;
        const auto itf = findInterface(l, devs[i], vendor != 0);
        if (!itf) continue;
        UsbCameraEntry e{idOf(l, devs[i]), d.idVendor, d.idProduct, {}, {}, {}, itf->cls};
        libusb_device_handle* h = nullptr;
        if (const int err = l.open(devs[i], &h); err != 0) {
            e.problem = openProblem(l, err);
        } else {
            unsigned char s[128];
            if (d.iManufacturer && l.get_string_descriptor_ascii(h, d.iManufacturer, s, sizeof s) > 0) e.manufacturer = reinterpret_cast<char*>(s);
            if (d.iProduct && l.get_string_descriptor_ascii(h, d.iProduct, s, sizeof s) > 0) e.model = reinterpret_cast<char*>(s);
            l.close(h);
        }
        out.push_back(std::move(e));
    }
    if (devs) l.free_device_list(devs, 1);
    return out;
}

std::unique_ptr<UsbPipe> openUsbCamera(const std::string& id) {
    Lib& l = lib();
    if (!l.problem.empty()) throw CameraError(l.problem);
    libusb_device** devs = nullptr;
    const long n = l.get_device_list(l.ctx, &devs);
    std::unique_ptr<UsbPipe> pipe;
    std::string problem = std::format("no USB camera {} (unplugged?)", id);
    for (long i = 0; i < n && !pipe; ++i) {
        if (idOf(l, devs[i]) != id) continue;
        const auto itf = findInterface(l, devs[i], true);
        if (!itf) {
            problem = std::format("{} has no PTP interface (is the camera in PC Remote mode?)", id);
            break;
        }
        libusb_device_handle* h = nullptr;
        if (const int e = l.open(devs[i], &h); e != 0) {
            problem = std::format("open {}: {}", id, openProblem(l, e));
            break;
        }
        l.set_auto_detach_kernel_driver(h, 1);   // (no kernel driver claims PTP interfaces; fails harmlessly where unsupported)
        if (const int e = l.claim_interface(h, itf->number); e != 0) {
            l.close(h);
            problem = std::format("claim {}: {}", id, openProblem(l, e));
            break;
        }
        l.clear_halt(h, itf->in);
        l.clear_halt(h, itf->out);
        pipe = std::make_unique<LibusbPipe>(l, h, *itf);
    }
    if (devs) l.free_device_list(devs, 1);
    if (!pipe) throw CameraError(problem);
    return pipe;
}

}  // namespace slopcap::ptp

// PTP over USB, the Still Image class's bulk-only transport (USB Still Image Capture Device Definition): every phase of
// a transaction is a container (length, type, code, transaction id, payload) on the bulk pipes. The pipes are a seam:
// libusb on Linux and macOS (ptp_libusb.cpp, loaded at run time, so nothing links against it), WinUSB on Windows for a
// camera given that driver (ptp_winusb_win.cpp: as Zadig installs it), a fake in the tests. (A camera on Windows' own
// portable-device driver goes through ptp_wpd.hpp instead.)
#pragma once

#include "camera/ptp.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace slopcap::ptp {

class UsbPipe {
public:
    virtual ~UsbPipe() = default;
    /// One bulk-out transfer of all of `data` (empty: a zero-length packet). Throws Error.
    virtual void write(std::span<const uint8_t> data) = 0;
    /// One bulk-in transfer into `buf`: the bytes received (0: a zero-length packet). Throws Error, also on a timeout.
    virtual std::size_t read(std::span<uint8_t> buf, std::chrono::milliseconds timeout) = 0;
    /// The bulk endpoints' packet size (512 at USB 2 high speed): a transfer that fills whole packets ends with an
    /// empty one.
    virtual std::size_t packetSize() const = 0;
    /// Whether there is an interrupt pipe, which carries the camera's events.
    virtual bool hasEvents() const { return false; }
    /// One transfer from the interrupt pipe: the bytes received, 0 when nothing came in `timeout`. Called from a
    /// thread of its own while the bulk pipes are in use. Throws LinkError when the camera is gone.
    virtual std::size_t readEvent(std::span<uint8_t> buf, std::chrono::milliseconds timeout) {
        (void)buf, (void)timeout;
        return 0;
    }
    /// PTP's Device Reset request (class-specific, 0x66) and the bulk pipes cleared: after a stuck transaction.
    virtual void reset() {}
};

class UsbTransport final : public Transport {
public:
    /// Takes the pipes and opens a session (one left open by an earlier program is closed and opened again). Reads
    /// the camera's events from the interrupt pipe all the time, on a thread of its own: a camera may hold back its
    /// answers while an event waits to be read.
    explicit UsbTransport(std::unique_ptr<UsbPipe> pipe, std::chrono::milliseconds timeout = std::chrono::seconds(10));
    ~UsbTransport() override;   ///< closes the session
    UsbTransport(const UsbTransport&) = delete;
    UsbTransport& operator=(const UsbTransport&) = delete;

    /// Throws LinkError naming the operation and the stage it stuck in (sending it, waiting for its answer).
    Response transact(uint16_t op, std::span<const uint32_t> params, Data phase, std::span<const uint8_t> out,
                      std::vector<uint8_t>* in) override;
    std::string kind() const override { return "USB"; }
    std::vector<Event> events() const override;
    bool recover() override;

private:
    struct Header {
        uint32_t length = 0;
        uint16_t type = 0, code = 0;
        uint32_t tid = 0;
    };
    void writeContainer(uint16_t type, uint16_t code, uint32_t tid, std::span<const uint8_t> payload);
    Header nextHeader();                           ///< the next container's header (empty transfers and events skipped)
    void readPayload(std::size_t n, std::vector<uint8_t>& dst);
    void fill(std::size_t n);                      ///< until pending_ holds n bytes
    Response doTransact(uint16_t op, std::span<const uint32_t> params, Data phase, std::span<const uint8_t> out,
                        std::vector<uint8_t>* in);
    void openSession();
    void addEvent(std::span<const uint8_t> container);
    void readEvents();                             ///< the event thread

    std::unique_ptr<UsbPipe> pipe_;
    std::chrono::milliseconds timeout_;
    uint32_t tid_ = 0;
    bool session_ = false;
    std::vector<uint8_t> pending_;   ///< bytes read but not yet used (a device that skips an empty packet sends more)
    std::size_t pendingPos_ = 0;
    std::chrono::steady_clock::time_point opened_ = std::chrono::steady_clock::now();
    mutable std::mutex eventsMutex_;
    std::vector<Event> events_;
    std::atomic<bool> stopEvents_{false};
    std::thread eventThread_;
};

// ---------------------------------------------------------------- libusb (Linux, macOS)

struct UsbCameraEntry {
    std::string id;              ///< "usb:BUS:ADDRESS" (libusb), "winusb:PATH" (Windows)
    uint16_t vendor = 0, product = 0;
    std::string manufacturer, model;   ///< from the device's strings, when it could be opened to read them
    std::string problem;         ///< why it couldn't be opened (permissions), empty if it could
    int interfaceClass = 6;      ///< 6: Still Image; 0xFF: vendor-specific (with bulk pipes)
};

/// Every connected USB device with a Still Image (PTP) interface; vendor != 0: only that vendor's, also those whose
/// interface is vendor-specific but has a pair of bulk pipes. Linux and macOS: through libusb (empty when it isn't
/// installed: libusbProblem() says so). Windows: the devices on the WinUSB driver.
std::vector<UsbCameraEntry> listUsbCameras(uint16_t vendor = 0);
/// The camera's bulk pipes, its PTP interface claimed. Throws CameraError (in use, no permission, gone).
std::unique_ptr<UsbPipe> openUsbCamera(const std::string& id);
/// Why libusb couldn't be loaded; empty when it could (and on Windows).
std::string libusbProblem();

}  // namespace slopcap::ptp

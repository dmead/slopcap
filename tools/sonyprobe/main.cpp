// slopcap-sonyprobe: what a Sony camera does over USB in PC Remote mode, for SlopCap's Sony support: which Sony
// operations and properties it has, whether it gives live view, whether the shutter speed and ISO can be set from the
// computer, how long a picture takes from the shutter to the computer, and what it sends (JPEG, ARW). It takes one
// picture and changes the shutter speed and ISO one notch and back. The report goes on screen and into two files,
// sonyprobe-<model>-<time>.txt and .json, with the picture(s) beside them; the camera's serial number is left out.
// Without a camera it still writes the report: on Windows, what Windows made of each Sony USB device and what to do.
//   slopcap-sonyprobe [--no-shot] [--shots N] [--no-set] [--live N] [--device N|TEXT] [--out DIR] [--keep-serial] [--list]
//                     [--all-usb]   (Windows: every USB device in the report, not only Sony's and still-image ones)
// Windows: through Windows' own camera driver (no driver to install), or WinUSB if the camera has that. Linux, macOS:
// libusb.
#include "camera/ptp_usb.hpp"
#include "camera/sony_ptp.hpp"
#include "diagnose.hpp"
#include "probe.hpp"
#ifdef _WIN32
#include "camera/ptp_wpd.hpp"

#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>

#ifndef SLOPCAP_GIT
#define SLOPCAP_GIT "unknown"
#endif

namespace ptp = slopcap::ptp;
using sonyprobe::Json;

namespace {

/// Everything said goes to the console, as it is said, and into the text report.
struct Tee : std::streambuf {
    int overflow(int c) override {
        if (c == EOF) return 0;
        const char ch = static_cast<char>(c);
        return xsputn(&ch, 1) == 1 ? c : EOF;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::cout.write(s, n);
        text.append(s, static_cast<std::size_t>(n));
        if (std::memchr(s, '\n', static_cast<std::size_t>(n))) std::cout.flush();
        return n;
    }
    std::string text;
};

struct Device {
    std::string id, shown, name, manufacturer, problem;
    bool sony = false;
    bool usb = false;   ///< through PTP over USB (libusb; WinUSB on Windows), not Windows Portable Devices
};

std::vector<Device> devices(std::string& wpdProblem) {
    std::vector<Device> out;
#ifdef _WIN32
    for (const auto& d : ptp::wpd::listDevices(&wpdProblem)) {
        const auto usb = ptp::wpd::usbIdsOf(d.id);
        out.push_back({d.id, usb.empty() ? "(not USB)" : usb, d.name.empty() ? d.description : d.name, d.manufacturer, {},
                       usb.starts_with("vid_054c"), false});
    }
#else
    (void)wpdProblem;
#endif
    // Linux, macOS: still-image devices, and Sony's also with a vendor-specific interface. Windows: Sony's on WinUSB.
#ifdef _WIN32
    auto usb = ptp::listUsbCameras(slopcap::sony::kUsbVendor);
#else
    auto usb = ptp::listUsbCameras();
    for (auto& d : ptp::listUsbCameras(slopcap::sony::kUsbVendor))
        if (std::ranges::find(usb, d.id, &ptp::UsbCameraEntry::id) == usb.end()) usb.push_back(d);
#endif
    for (const auto& d : usb) {
#ifdef _WIN32
        const std::string where = "winusb";   // (the id itself is the device path, which holds the serial number)
#else
        const std::string where = d.id;
#endif
        out.push_back({d.id,
                       std::format("{} {:04x}:{:04x}{}", where, d.vendor, d.product, d.interfaceClass == 0xFF ? " (vendor-specific interface)" : ""),
                       d.model, d.manufacturer, d.problem, d.vendor == slopcap::sony::kUsbVendor, true});
    }
    return out;
}

std::unique_ptr<ptp::Transport> open(const Device& d, Json& report) {
    if (d.usb) return std::make_unique<ptp::UsbTransport>(ptp::openUsbCamera(d.id));
#ifdef _WIN32
    auto t = std::make_unique<ptp::wpd::WpdTransport>(ptp::wpd::openDevice(d.id));
    try {
        Json ops = Json::array();
        for (auto op : t->vendorOpcodes()) ops.push(sonyprobe::hex4(op));
        report["wpdVendorOpcodes"] = std::move(ops);
    } catch (const std::exception& e) {
        report["wpdVendorOpcodes"] = std::string("error: ") + e.what();
    }
    return t;
#else
    (void)report;
    throw slopcap::CameraError("no such camera");
#endif
}

std::string stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return b;
}

std::string safe(std::string s) {
    for (auto& c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') c = '_';
    return s.empty() ? "camera" : s;
}

/// Started from Explorer (a console of its own): wait before the window closes.
void pauseIfOwnConsole() {
#ifdef _WIN32
    DWORD ids[2];
    if (GetConsoleProcessList(ids, 2) == 1) {
        std::cout << "\nPress Enter to close this window." << std::endl;
        std::string line;
        std::getline(std::cin, line);
    }
#endif
}

int usage() {
    std::cerr << "usage: slopcap-sonyprobe [--no-shot] [--shots N] [--no-set] [--live N] [--device N|TEXT] [--out DIR] [--keep-serial] [--list] "
                 "[--all-usb]\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    sonyprobe::Options o;
    std::string which, outDir = ".";
    bool listOnly = false;
    [[maybe_unused]] bool allUsb = false;   // (Windows only)
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--no-shot") o.shot = false;
        else if (a == "--shots") o.shots = std::max(1, std::atoi(next().c_str()));
        else if (a == "--no-set") o.setTest = false;
        else if (a == "--live") o.liveFrames = std::max(0, std::atoi(next().c_str()));
        else if (a == "--device") which = next();
        else if (a == "--out") outDir = next();
        else if (a == "--keep-serial") o.keepSerial = true;
        else if (a == "--list") listOnly = true;
        else if (a == "--all-usb") allUsb = true;
        else return usage();
    }

    Tee tee;
    std::ostream log(&tee);
    Json report = Json::object();
    report["tool"] = "slopcap-sonyprobe";
    report["version"] = SLOPCAP_GIT;
#ifdef _WIN32
    report["system"] = "Windows";
#elif defined(__APPLE__)
    report["system"] = "macOS";
#else
    report["system"] = "Linux";
#endif
    log << "SlopCap Sony probe (" << SLOPCAP_GIT << ")" << std::endl;

    std::string wpdProblem;
    const auto all = devices(wpdProblem);
    Json devs = Json::array();
    log << (all.empty() ? "No camera-like USB devices found.\n" : "Devices:\n");
    for (const auto& d : all) {
        Json j = Json::object();
        j["id"] = d.shown;
        j["name"] = d.name;
        j["manufacturer"] = d.manufacturer;
        if (!d.problem.empty()) j["problem"] = d.problem;
        j["sony"] = d.sony;
        j["via"] = d.usb ? "USB" : "WPD";
        devs.push(std::move(j));
        log << std::format("  {} {:<22} {} {}{}\n", d.sony ? "*" : " ", d.shown, d.manufacturer, d.name, d.problem.empty() ? "" : "  (" + d.problem + ")");
    }
    report["devices"] = std::move(devs);
    if (const auto p = ptp::libusbProblem(); !p.empty()) {
        report["libusb"] = p;
        log << "  " << p << "\n";
    }
#ifdef _WIN32
    // what Windows made of each Sony USB device (and any still-image one), whether or not it found a camera
    const auto rows = sonyprobe::usbDevices(allUsb);
    const auto edition = sonyprobe::windowsEdition();
    {
        Json w = Json::object();
        w["edition"] = edition;
        if (!wpdProblem.empty()) w["portableDevices"] = wpdProblem;
        Json usb = Json::array();
        for (const auto& r : rows) {
            Json j = Json::object();
            j["ids"] = r.ids;
            j["name"] = r.name;
            j["class"] = r.setupClass;
            j["driver"] = r.service;
            j["usbClass"] = r.usbClass;
            j["problem"] = r.problem;
            j["sony"] = r.sony;
            usb.push(std::move(j));
        }
        w["usbDevices"] = std::move(usb);
        report["windows"] = std::move(w);
    }
    log << "Windows " << edition << "\n";
    if (!wpdProblem.empty()) log << "  " << wpdProblem << "\n";
    for (const auto& r : rows)
        if (r.sony || allUsb)
            log << std::format("  USB {} {}: driver {}, class {}, {}{}\n", r.ids, r.name, r.service.empty() ? "none" : r.service,
                               r.setupClass.empty() ? "none" : r.setupClass, r.usbClass.empty() ? "USB class unknown" : r.usbClass,
                               r.problem ? std::format(", Device Manager problem {}", r.problem) : "");
#endif
    log.flush();

    std::vector<const Device*> sony;
    for (const auto& d : all)
        if (d.sony) sony.push_back(&d);
    const Device* pick = nullptr;
    for (std::size_t i = 0; i < sony.size() && !pick; ++i)
        if (which.empty() || which == std::to_string(i) || sony[i]->name.find(which) != std::string::npos) pick = sony[i];

    int rc = 0;
    std::vector<sonyprobe::File> files;
    if (!pick) {
        log << "\nNo Sony camera found.\n";
        Json why = Json::array();
#ifdef _WIN32
        for (const auto& line : sonyprobe::diagnose(rows, wpdProblem, edition)) {
            log << "  " << line << "\n";
            why.push(line);
        }
#else
        for (const char* line :
             {"On the camera: MENU > Setup > USB Connection > PC Remote; connect it by USB and switch it on.",
              "In WSL: attach the camera from Windows first (PowerShell as administrator: usbipd list, then usbipd bind --busid <id> "
              "and usbipd attach --wsl --busid <id>), and run the probe with sudo.",
              "A Sony device listed with a problem: no permission (sudo, or a udev rule) or in use (gphoto2, the desktop's gvfs)."}) {
            log << "  " << line << "\n";
            why.push(line);
        }
#endif
        report["noCamera"] = std::move(why);
        rc = 1;
    } else if (!listOnly) {
        log << "\nProbing " << pick->name << " (" << pick->shown << "):\n";
        log.flush();
        Json cam = Json::object();
        try {
            auto t = open(*pick, report);
            cam = sonyprobe::run(*t, o, log, files);
        } catch (const std::exception& e) {
            cam["error"] = e.what();
            log << "  could not open the camera: " << e.what() << "\n";
            rc = 1;
        }
        report["camera"] = std::move(cam);
    }

    if (!listOnly) {   // the report, also when no camera was found: it says what Windows made of it
        const std::string base = std::format("sonyprobe-{}-{}", pick ? safe(pick->name) : std::string("no-camera"), stamp());
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        for (const auto& f : files) {
            const auto path = std::filesystem::path(outDir) / (base + "-" + f.suffix);
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data.data()), static_cast<std::streamsize>(f.data.size()));
        }
        const auto json = std::filesystem::path(outDir) / (base + ".json");
        const auto txt = std::filesystem::path(outDir) / (base + ".txt");
        log << "\nReport: " << std::filesystem::absolute(json).lexically_normal().string() << "\n        "
            << std::filesystem::absolute(txt).lexically_normal().string() << "\n";
        if (!files.empty()) log << "Pictures beside it (" << base << "-*).\n";
        log << "Please send the two report files back.\n";
        log.flush();
        std::ofstream(json) << report.dump() << "\n";
        std::ofstream(txt) << tee.text;
    }
    log.flush();
    pauseIfOwnConsole();
    return rc;
}

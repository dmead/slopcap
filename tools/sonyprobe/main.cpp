// slopcap-sonyprobe: what a Sony camera does over USB in PC Remote mode, for SlopCap's Sony support: which Sony
// operations and properties it has, whether it gives live view, whether the shutter speed and ISO can be set from the
// computer, how long a picture takes from the shutter to the computer, and what it sends (JPEG, ARW). It takes one
// picture and changes the shutter speed and ISO one notch and back. The report goes on screen and into two files,
// sonyprobe-<model>-<time>.txt and .json, with the picture(s) beside them; the camera's serial number is left out.
//   slopcap-sonyprobe [--no-shot] [--shots N] [--no-set] [--live N] [--device N|TEXT] [--out DIR] [--keep-serial] [--list]
// Windows: through Windows' own camera driver (no driver to install). Linux, macOS: libusb.
#include "camera/sony_ptp.hpp"
#include "probe.hpp"
#ifdef _WIN32
#include "camera/ptp_wpd.hpp"

#include <windows.h>
#else
#include "camera/ptp_usb.hpp"
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
};

std::vector<Device> devices() {
    std::vector<Device> out;
#ifdef _WIN32
    for (const auto& d : ptp::wpd::listDevices()) {
        const auto usb = ptp::wpd::usbIdsOf(d.id);
        out.push_back({d.id, usb.empty() ? "(not USB)" : usb, d.name.empty() ? d.description : d.name, d.manufacturer, {},
                       usb.starts_with("vid_054c")});
    }
#else
    for (const auto& d : ptp::listUsbCameras()) {
        out.push_back({d.id, std::format("{} {:04x}:{:04x}", d.id, d.vendor, d.product), d.model, d.manufacturer, d.problem,
                       d.vendor == slopcap::sony::kUsbVendor});
    }
#endif
    return out;
}

std::unique_ptr<ptp::Transport> open(const Device& d, Json& report) {
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
    return std::make_unique<ptp::UsbTransport>(ptp::openUsbCamera(d.id));
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
    std::cerr << "usage: slopcap-sonyprobe [--no-shot] [--shots N] [--no-set] [--live N] [--device N|TEXT] [--out DIR] [--keep-serial] [--list]\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    sonyprobe::Options o;
    std::string which, outDir = ".";
    bool listOnly = false;
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

    const auto all = devices();
    Json devs = Json::array();
    log << (all.empty() ? "No camera-like USB devices found.\n" : "Devices:\n");
    for (const auto& d : all) {
        Json j = Json::object();
        j["id"] = d.shown;
        j["name"] = d.name;
        j["manufacturer"] = d.manufacturer;
        if (!d.problem.empty()) j["problem"] = d.problem;
        j["sony"] = d.sony;
        devs.push(std::move(j));
        log << std::format("  {} {:<22} {} {}{}\n", d.sony ? "*" : " ", d.shown, d.manufacturer, d.name, d.problem.empty() ? "" : "  (" + d.problem + ")");
    }
    report["devices"] = std::move(devs);
#ifndef _WIN32
    if (const auto p = ptp::libusbProblem(); !p.empty()) {
        report["libusb"] = p;
        log << "  " << p << "\n";
    }
#endif
    log.flush();

    std::vector<const Device*> sony;
    for (const auto& d : all)
        if (d.sony) sony.push_back(&d);
    const Device* pick = nullptr;
    for (std::size_t i = 0; i < sony.size() && !pick; ++i)
        if (which.empty() || which == std::to_string(i) || sony[i]->name.find(which) != std::string::npos) pick = sony[i];

    int rc = 0;
    if (listOnly) {
        rc = sony.empty() ? 1 : 0;
    } else if (!pick) {
        log << "\nNo Sony camera found. On the camera: Menu > Setup > USB Connection: PC Remote; connect it by USB and switch it on.\n";
        report["error"] = "no Sony camera found";
        rc = 1;
    } else {
        log << "\nProbing " << pick->name << " (" << pick->shown << "):\n";
        log.flush();
        std::vector<sonyprobe::File> files;
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

        const std::string base = std::format("sonyprobe-{}-{}", safe(pick->name), stamp());
        std::filesystem::create_directories(outDir);
        for (const auto& f : files) {
            const auto path = std::filesystem::path(outDir) / (base + "-" + f.suffix);
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data.data()), static_cast<std::streamsize>(f.data.size()));
        }
        log.flush();
        const auto json = std::filesystem::path(outDir) / (base + ".json");
        const auto txt = std::filesystem::path(outDir) / (base + ".txt");
        log << "\nReport: " << std::filesystem::absolute(json).string() << "\n        " << std::filesystem::absolute(txt).string() << "\n";
        if (!files.empty()) log << "Pictures beside it (" << base << "-*). Please send the two report files back.\n";
        log.flush();
        std::ofstream(json) << report.dump() << "\n";
        std::ofstream(txt) << tee.text;
    }
    log.flush();
    pauseIfOwnConsole();
    return rc;
}

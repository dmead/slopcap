#include "diagnose.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace sonyprobe {

namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool has(const std::string& s, const char* what) { return lower(s).find(what) != std::string::npos; }

const char* kGiveBack = "Device Manager > this device > Uninstall device, tick \"Delete the driver software\", then unplug the camera "
                        "and plug it in again";
const char* kPickMtp = "Device Manager > this device > Update driver > Browse my computer > Let me pick > Portable Devices > "
                       "MTP USB Device (untick \"Show compatible hardware\" if it isn't listed)";

}  // namespace

std::string advice(const UsbRow& r) {
    const std::string svc = lower(r.service);
    std::string vendorClass = has(r.usbClass, "class_ff") ? " Its USB class is vendor-specific, so Windows has no driver of its own for it." : "";
    if (svc == "usbccgp") return "the USB composite device: its parts follow";
    if (svc == "winusb") return "on the WinUSB driver: the probe talks to it through WinUSB.";
    if (svc == "libusbk" || svc == "libusb0")
        return std::format("on the {} driver (as Zadig installs it), which the probe can't use: give it back to Windows ({}), or "
                           "choose WinUSB for it in Zadig.",
                           r.service, kGiveBack);
    if (has(svc, "vboxusb") || has(svc, "usbip"))
        return "held for WSL or a virtual machine (usbipd): in PowerShell as administrator, usbipd list shows its bus id; then "
               "usbipd detach --busid <id> and usbipd unbind --busid <id>.";
    if (r.problem == 28 || (svc.empty() && r.problem == 0))
        return std::format("no driver.{} {}, or choose WinUSB for it in Zadig.", vendorClass, kPickMtp);
    if (r.problem == 10) return "Windows couldn't start it (problem 10): switch the camera off and on, try another USB port or cable.";
    if (r.problem == 43) return "Windows stopped it (problem 43): try another USB port or cable.";
    if (r.problem) return std::format("Device Manager reports problem {} for it: {}.", r.problem, kGiveBack);
    if (svc == "usbscan" || has(r.setupClass, "image"))
        return std::format("on Windows' scanner and camera (WIA) driver, which the probe can't use: {}.", kPickMtp);
    if (has(svc, "wpd") || has(r.setupClass, "wpd"))
        return "on Windows' portable-device driver: the probe should see it. Close Imaging Edge and the Photos app's import, "
               "unplug, plug in again.";
    return std::format("on the {} driver ({}), which the probe can't use: {}.", r.service, r.setupClass, kGiveBack);
}

std::vector<std::string> diagnose(const std::vector<UsbRow>& rows, const std::string& wpdProblem, const std::string& edition) {
    std::vector<std::string> out;
    if (!wpdProblem.empty()) out.push_back(wpdProblem + ".");
    const std::string ed = edition.substr(0, edition.find_first_of(", "));   // EditionID: "ProfessionalN", "EducationN"
    if (!ed.empty() && ed.back() == 'N')                                     // (but "Education" isn't one)
        out.push_back(std::format("This is an N edition of Windows ({}): it has no MTP / Portable Devices until the Media Feature "
                                  "Pack is added (Settings > Apps > Optional features > Add a feature > Media Feature Pack), "
                                  "then restart.",
                                  edition));
    const auto sony = std::ranges::count_if(rows, &UsbRow::sony);
    if (sony == 0)
        out.push_back("Windows sees no Sony USB device at all: check the camera is on and set to PC Remote (MENU > Setup > USB "
                      "Connection), that the cable carries data (some only charge), and that WSL isn't holding it (usbipd list).");
    for (const auto& r : rows)
        if (r.sony) out.push_back(std::format("{} {}: {}", r.ids, r.name, advice(r)));
    return out;
}

}  // namespace sonyprobe

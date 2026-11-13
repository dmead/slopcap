// Why the probe finds no camera, in words its owner can act on: what Windows made of each Sony USB device (its driver,
// USB class and Device Manager problem, gathered by windevices.cpp), whether Windows Portable Devices works at all, and
// the Windows edition. The rules are here, apart from Windows, so the tests check them everywhere.
#pragma once

#include <string>
#include <vector>

namespace sonyprobe {

/// A present USB device as Device Manager has it.
struct UsbRow {
    std::string ids;          ///< "VID_054C&PID_094E" (and "&MI_00" for one interface of a composite device); no serial
    std::string name;         ///< friendly name or description
    std::string setupClass;   ///< "WPD", "Image", "USBDevice", "libusbK USB Devices", ... (empty: none)
    std::string service;      ///< the driver: "WUDFWpdMtp", "WinUSB", "usbscan", "usbccgp", ... (empty: none)
    std::string usbClass;     ///< from its compatible ids: "Class_06&SubClass_01&Prot_01" (still image), "Class_ff", ...
    unsigned problem = 0;     ///< Device Manager's problem code (28: no driver, 10: can't start, ...), 0: none
    bool sony = false;
};

/// What this device's driver means for the probe: one line, an instruction where one helps.
std::string advice(const UsbRow& r);
/// Everything to say when no Sony camera was found: Portable Devices' trouble (`wpdProblem`), each Sony device, the
/// edition (an N edition lacks Portable Devices without the Media Feature Pack).
std::vector<std::string> diagnose(const std::vector<UsbRow>& rows, const std::string& wpdProblem, const std::string& edition);

/// Windows only (windevices.cpp): the present USB devices that are Sony's or of the still-image class (`all`: every
/// one), and the edition ("Professional, Windows 10 Pro, build 19045"; EditionID first: "ProfessionalN" is an N edition).
std::vector<UsbRow> usbDevices(bool all = false);
std::string windowsEdition();

}  // namespace sonyprobe

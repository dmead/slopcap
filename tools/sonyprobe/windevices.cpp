// diagnose.hpp's Windows half: the USB devices as Device Manager has them (SetupAPI, Configuration Manager), and the
// edition from the registry.
#include "diagnose.hpp"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <cwctype>
#include <format>

namespace sonyprobe {

namespace {

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

/// A property; a multi-string as its entries.
std::vector<std::wstring> property(HDEVINFO set, SP_DEVINFO_DATA& d, DWORD prop) {
    DWORD type = 0, size = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &d, prop, &type, nullptr, 0, &size);
    if (size < sizeof(wchar_t)) return {};
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(set, &d, prop, &type, reinterpret_cast<BYTE*>(s.data()), size, nullptr)) return {};
    std::vector<std::wstring> out;
    for (std::size_t at = 0; at < s.size() && s[at];) {
        const std::wstring one = s.c_str() + at;
        out.push_back(one);
        at += one.size() + 1;
    }
    return out;
}

std::string first(const std::vector<std::wstring>& v) { return v.empty() ? std::string() : utf8(v.front()); }

std::wstring upper(std::wstring s) {
    std::ranges::transform(s, s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return s;
}

std::wstring registry(const wchar_t* name) {
    wchar_t buf[256] = {};
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

}  // namespace

std::vector<UsbRow> usbDevices(bool all) {
    std::vector<UsbRow> out;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA d{};
    d.cbSize = sizeof d;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        const auto hw = property(set, d, SPDRP_HARDWAREID), compat = property(set, d, SPDRP_COMPATIBLEIDS);
        const bool sony = std::ranges::any_of(hw, [](const std::wstring& h) { return upper(h).find(L"VID_054C") != std::wstring::npos; });
        std::wstring cls;
        for (const auto& c : compat)
            if (const auto at = upper(c).find(L"CLASS_"); at != std::wstring::npos && (cls.empty() || c.size() > cls.size())) cls = c.substr(at);
        const bool image = upper(cls).starts_with(L"CLASS_06");
        if (!sony && !image && !all) continue;
        UsbRow r;
        // the shortest hardware id: vendor and product (and interface), no revision; the instance id (with the serial)
        // is left out
        std::wstring ids = hw.empty() ? L"" : *std::ranges::min_element(hw, {}, &std::wstring::size);
        if (const auto at = upper(ids).find(L"VID_"); at != std::wstring::npos) ids = ids.substr(at);
        r.ids = utf8(ids);
        r.name = first(property(set, d, SPDRP_FRIENDLYNAME));
        if (r.name.empty()) r.name = first(property(set, d, SPDRP_DEVICEDESC));
        r.setupClass = first(property(set, d, SPDRP_CLASS));
        r.service = first(property(set, d, SPDRP_SERVICE));
        r.usbClass = utf8(cls);
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, d.DevInst, 0) == CR_SUCCESS && (status & DN_HAS_PROBLEM)) r.problem = problem;
        r.sony = sony;
        out.push_back(std::move(r));
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

std::string windowsEdition() {
    const std::wstring id = registry(L"EditionID"), product = registry(L"ProductName"), build = registry(L"CurrentBuild");
    return std::format("{}, {}, build {}", utf8(id), utf8(product), utf8(build));
}

}  // namespace sonyprobe

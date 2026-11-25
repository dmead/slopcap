#include "devices/wheel.hpp"

#include "camera/open_camera.hpp"

#include <chrono>
#include <format>

#ifdef SLOPCAP_HAVE_EFW
#include <libasi/EFW_filter.h>

#include <cstring>
#include <thread>
#endif

namespace slopcap {

namespace {

/// A wheel that takes 0.3 s per slot to turn, one way round.
class SimWheel : public IFilterWheel {
public:
    explicit SimWheel(int slots) : slots_(slots) {}
    std::string model() const override { return std::format("Simulated wheel ({} slots)", slots_); }
    std::string serial() const override { return std::format("sim{}", slots_); }
    int slotCount() const override { return slots_; }
    int position() override {
        if (target_ != at_ && Clock::now() >= arrive_) at_ = target_;
        return target_ == at_ ? at_ : -1;
    }
    void setPosition(int slot) override {
        if (slot < 0 || slot >= slots_) throw WheelError(std::format("no slot {} (the wheel has {})", slot + 1, slots_));
        if (position() < 0) throw WheelError("the wheel is still turning");
        const int steps = (slot - at_ + slots_) % slots_;
        target_ = slot;
        arrive_ = Clock::now() + std::chrono::milliseconds(300 * steps);
    }

private:
    using Clock = std::chrono::steady_clock;
    int slots_, at_ = 0, target_ = 0;
    Clock::time_point arrive_{};
};

[[maybe_unused]] int indexOf(std::string_view spec, std::string_view prefix) {
    if (spec.size() == prefix.size()) return 0;
    if (spec[prefix.size()] != ':') throw WheelError(std::format("bad filter wheel spec '{}'", spec));
    try {
        return std::stoi(std::string(spec.substr(prefix.size() + 1)));
    } catch (const std::exception&) {
        throw WheelError(std::format("bad filter wheel spec '{}'", spec));
    }
}

#ifdef SLOPCAP_HAVE_EFW
std::string efwError(EFW_ERROR_CODE code) {
    switch (code) {
        case EFW_SUCCESS: return "success";
        case EFW_ERROR_INVALID_INDEX: return "no such wheel";
        case EFW_ERROR_INVALID_ID: return "invalid wheel id";
        case EFW_ERROR_INVALID_VALUE: return "invalid slot";
        case EFW_ERROR_REMOVED: return "wheel removed (unplugged?)";
        case EFW_ERROR_MOVING: return "the wheel is still turning";
        case EFW_ERROR_ERROR_STATE: return "the wheel is in an error state (stuck? power?)";
        case EFW_ERROR_GENERAL_ERROR: return "general error (open in another program?)";
        case EFW_ERROR_NOT_SUPPORTED: return "not supported";
        case EFW_ERROR_CLOSED: return "wheel not open";
        default: return std::format("error {}", static_cast<int>(code));
    }
}

void check(EFW_ERROR_CODE code, std::string_view what) {
    if (code != EFW_SUCCESS) throw WheelError(std::format("ZWO EFW {}: {}", what, efwError(code)));
}

class ZwoWheel : public IFilterWheel {
public:
    explicit ZwoWheel(int index) {
        const int n = EFWGetNum();
        if (index < 0 || index >= n) throw WheelError(std::format("no ZWO filter wheel at index {} ({} found)", index, n));
        check(EFWGetID(index, &id_), "id");
        check(EFWOpen(id_), "open");
        // Right after it's plugged in (or opened) the wheel counts its slots, answering "moving" meanwhile
        EFW_INFO info{};
        EFW_ERROR_CODE e = EFW_ERROR_MOVING;
        for (auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
             (e = EFWGetProperty(id_, &info)) == EFW_ERROR_MOVING && std::chrono::steady_clock::now() < until;)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (e != EFW_SUCCESS) {
            EFWClose(id_);
            check(e, "property");
        }
        slots_ = info.slotNum;
        name_ = std::string(info.Name, strnlen(info.Name, sizeof info.Name));
        EFW_SN sn{};
        if (EFWGetSerialNumber(id_, &sn) == EFW_SUCCESS) {
            for (unsigned char b : sn.id) serial_ += std::format("{:02x}", b);
        }
        if (serial_.empty() || serial_.find_first_not_of('0') == std::string::npos) serial_ = std::format("{}#{}", name_, id_);
    }
    ~ZwoWheel() override { EFWClose(id_); }

    std::string model() const override { return std::format("ZWO {} ({} slots)", name_, slots_); }
    std::string serial() const override { return serial_; }
    int slotCount() const override { return slots_; }
    int position() override {
        int p = -1;
        check(EFWGetPosition(id_, &p), "position");
        return p;
    }
    void setPosition(int slot) override {
        if (slot < 0 || slot >= slots_) throw WheelError(std::format("no slot {} (the wheel has {})", slot + 1, slots_));
        check(EFWSetPosition(id_, slot), "move");
    }

private:
    int id_ = -1, slots_ = 0;
    std::string name_, serial_;
};
#endif

}  // namespace

std::unique_ptr<IFilterWheel> openWheel(std::string_view spec) {
    if (spec.starts_with("sim")) {
        const int n = spec == "sim" ? 5 : indexOf(spec, "sim");
        if (n < 2 || n > 20) throw WheelError(std::format("bad filter wheel spec '{}' (sim:2 to sim:20)", spec));
        return std::make_unique<SimWheel>(n);
    }
    if (spec.starts_with("efw")) {
#ifdef SLOPCAP_HAVE_EFW
        if (!sdkAvailable("EFW_filter.dll")) throw WheelError("ZWO filter wheel SDK not found (EFW_filter.dll)");
        return std::make_unique<ZwoWheel>(indexOf(spec, "efw"));
#else
        throw WheelError("this build has no ZWO filter wheel support (SDK not found at build time)");
#endif
    }
    throw WheelError(std::format("unknown filter wheel '{}' (efw, efw:N, sim:N)", spec));
}

std::vector<std::string> listWheels() {
    std::vector<std::string> out;
#ifdef SLOPCAP_HAVE_EFW
    if (!sdkAvailable("EFW_filter.dll")) {
        out.push_back("(ZWO filter wheel SDK not found: EFW_filter.dll)");
        return out;
    }
    const int n = EFWGetNum();
    for (int i = 0; i < n; ++i) {
        int id = -1;
        EFW_INFO info{};
        if (EFWGetID(i, &id) != EFW_SUCCESS) continue;
        // slotNum is 0 until the wheel is opened, so the name is all there is to show without opening it
        const std::string name = EFWGetProperty(id, &info) == EFW_SUCCESS ? std::string(info.Name, strnlen(info.Name, sizeof info.Name)) : "EFW";
        out.push_back(i == 0 ? std::format("efw      ZWO {}", name) : std::format("efw:{}    ZWO {}", i, name));
    }
    if (n == 0) out.push_back("(no ZWO filter wheel found)");
#else
    out.push_back("(no ZWO filter wheel support in this build)");
#endif
    return out;
}

}  // namespace slopcap

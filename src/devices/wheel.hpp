// Filter wheels, without Qt: the small part every wheel shares (how many slots, where it is, go to a slot) and a way
// to open one from a short spec, for the app (devices/filter_wheel.hpp) and the tools:
//   "efw" / "efw:N"  ZWO EFW N (default 0), through ZWO's SDK (libEFWFilter / EFW_filter.dll)
//   "sim:N"          a simulated wheel with N slots that takes a moment to turn (tests, demos)
#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace slopcap {

struct WheelError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class IFilterWheel {
public:
    virtual ~IFilterWheel() = default;
    virtual std::string model() const = 0;     ///< e.g. "EFW (7 slots)"
    virtual std::string serial() const = 0;    ///< stable per wheel (slot names are kept per wheel)
    virtual int slotCount() const = 0;
    /// The slot in the light path, 0-based; -1 while the wheel turns. Throws WheelError (removed, error state).
    virtual int position() = 0;
    virtual void setPosition(int slot) = 0;    ///< starts the move and returns; throws WheelError
};

std::unique_ptr<IFilterWheel> openWheel(std::string_view spec);   ///< throws WheelError

/// Every connected wheel as "spec  model" lines (backends that weren't built are noted).
std::vector<std::string> listWheels();

}  // namespace slopcap

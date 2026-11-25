// A fake of ZWO's filter wheel SDK (libEFWFilter / EFW_filter.dll), like the camera fakes (fake_sdk.hpp): the C
// functions src/devices/wheel.cpp calls, defined in the test program, over a scripted wheel that turns one slot per
// position() call.
#pragma once

#include <string>

namespace fake {

struct Efw {
    int count = 1;               ///< wheels connected
    int slotCount = 7;
    std::string name = "EFW";
    int at = 0, target = 0;      ///< target != at: turning
    int busyProperty = 2;        ///< EFWGetProperty answers "moving" this many times first (counting slots)
    bool open = false, removed = false, errorState = false;
    int opens = 0, closes = 0, moves = 0;
    void reset() { *this = Efw{}; }
};
Efw& efw();

}  // namespace fake

// The fake ToupTek SDK (fake_toup.inc) for ToupTek's own symbols, and the fake state both brands share the shape of.
#include "fakes/fake_sdk.hpp"

#include <libtoupcam/toupcam.h>

namespace fake {

void Toup::reset() {
    *this = Toup{};
    ToupCam c;
    c.flags = TOUPCAM_FLAG_CMOS | TOUPCAM_FLAG_RAW8 | TOUPCAM_FLAG_RAW12 | TOUPCAM_FLAG_CG | TOUPCAM_FLAG_CGHDR | TOUPCAM_FLAG_TEC_ONOFF |
              TOUPCAM_FLAG_GETTEMPERATURE | TOUPCAM_FLAG_FAN | TOUPCAM_FLAG_BLACKLEVEL | TOUPCAM_FLAG_LOW_NOISE | TOUPCAM_FLAG_HIGH_FULLWELL;
    c.fourcc = 'R' | 'G' << 8 | 'G' << 16 | static_cast<unsigned>('B') << 24;
    cams = {c};
}

Toup& touptek() {
    static Toup t = [] {
        Toup x;
        x.reset();
        return x;
    }();
    return t;
}

Toup& mallincam() {
    static Toup t = [] {
        Toup x;
        x.reset();
        x.cam().name = "MallinCam DS10c";
        x.cam().id = "mc-0001";
        x.cam().sn = "MC0123456789";
        return x;
    }();
    return t;
}

}  // namespace fake

#define FN(x) Toupcam_##x
#define TY(x) Toupcam##x
#define CO(x) TOUPCAM_##x
#define HANDLE HToupcam
#define EVENT_CB PTOUPCAM_EVENT_CALLBACK
#define STATE fake::touptek
#include "fakes/fake_toup.inc"

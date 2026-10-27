// MallinCam cameras through libmallincam / mallincam.dll: ToupTek's SDK under MallinCam's name (see touptek_camera.hpp).
#include <libmallincam/mallincam.h>

#include "camera/touptek_family.inc"

namespace slopcap {

template <>
struct ToupSdk<ToupBrand::MallinCam> {
    SLOPCAP_TOUP_SDK(Mallincam_, Mallincam, MALLINCAM_, "MallinCam")
};

template class ToupCamera<ToupBrand::MallinCam>;

}  // namespace slopcap

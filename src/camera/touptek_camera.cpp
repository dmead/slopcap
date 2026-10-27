// ToupTek cameras through libtoupcam / toupcam.dll (see touptek_camera.hpp).
#include <libtoupcam/toupcam.h>

#include "camera/touptek_family.inc"

namespace slopcap {

template <>
struct ToupSdk<ToupBrand::ToupTek> {
    SLOPCAP_TOUP_SDK(Toupcam_, Toupcam, TOUPCAM_, "ToupTek")
};

template class ToupCamera<ToupBrand::ToupTek>;

}  // namespace slopcap

#include "camera/camera.hpp"

namespace slopcap {

const char* bayerName(Bayer b) {
    switch (b) {
        case Bayer::None: return "mono";
        case Bayer::RGGB: return "RGGB";
        case Bayer::GRBG: return "GRBG";
        case Bayer::GBRG: return "GBRG";
        case Bayer::BGGR: return "BGGR";
    }
    return "?";
}

std::size_t ICamera::frameBytes() const {
    const Roi r = roi();
    return static_cast<std::size_t>(r.width) * static_cast<std::size_t>(r.height) * (bitDepth() > 8 ? 2u : 1u);
}

}  // namespace slopcap

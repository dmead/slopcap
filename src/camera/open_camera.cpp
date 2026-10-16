#include "camera/open_camera.hpp"

#include "camera/sim_camera.hpp"
#ifdef SLOPCAP_HAVE_ASI
#include "camera/asi_camera.hpp"
#endif
#ifdef SLOPCAP_HAVE_QHY
#include "camera/qhy_camera.hpp"
#endif

#include <format>

namespace slopcap {

namespace {

int indexOf(std::string_view spec, std::string_view prefix) {
    if (spec.size() == prefix.size()) return 0;
    if (spec[prefix.size()] != ':') throw CameraError(std::format("bad camera spec '{}'", spec));
    try {
        return std::stoi(std::string(spec.substr(prefix.size() + 1)));
    } catch (const std::exception&) {
        throw CameraError(std::format("bad camera index in '{}'", spec));
    }
}

}  // namespace

std::unique_ptr<ICamera> openCamera(std::string_view spec) {
    if (spec.starts_with("sim:")) return std::make_unique<SimCamera>(std::string(spec.substr(4)));
    if (spec.starts_with("sim-fast:")) return std::make_unique<SimCamera>(std::string(spec.substr(9)), SimOptions{SimPace::AsFastAsPossible});
    if (spec.starts_with("asi")) {
#ifdef SLOPCAP_HAVE_ASI
        return std::make_unique<AsiCamera>(indexOf(spec, "asi"));
#else
        throw CameraError("this build has no ZWO support (SDK not found at build time)");
#endif
    }
    if (spec.starts_with("qhy")) {
#ifdef SLOPCAP_HAVE_QHY
        return std::make_unique<QhyCamera>(indexOf(spec, "qhy"));
#else
        throw CameraError("this build has no QHY support (SDK not found at build time)");
#endif
    }
    throw CameraError(std::format("unknown camera spec '{}' (use asi[:N], qhy[:N], sim:FILE.ser)", spec));
}

std::vector<std::string> listCameras() {
    std::vector<std::string> out;
#ifdef SLOPCAP_HAVE_ASI
    for (const auto& c : AsiCamera::list()) out.push_back(std::format("asi:{}  {}", c.index, c.model));
#else
    out.push_back("(ZWO support not built)");
#endif
#ifdef SLOPCAP_HAVE_QHY
    for (const auto& c : QhyCamera::list()) out.push_back(std::format("qhy:{}  {}  [{}]", c.index, c.model, c.id));
#else
    out.push_back("(QHY support not built)");
#endif
    return out;
}

}  // namespace slopcap

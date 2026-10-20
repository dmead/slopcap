#include "camera/open_camera.hpp"

#include "camera/sim_camera.hpp"
#ifdef SLOPCAP_HAVE_ASI
#include "camera/asi_camera.hpp"
#endif
#ifdef SLOPCAP_HAVE_QHY
#include "camera/qhy_camera.hpp"
#endif

#include <format>

#ifdef _WIN32
#include <windows.h>
#endif

namespace slopcap {

namespace {

/// On Windows the SDK DLLs are delay-loaded (cmake/DelayImport.cmake): calling into one that can't be loaded would
/// crash, so check first. A DLL the GUI already loaded from its embedded copy is found by name.
/// (Both helpers go unused in a build with no camera SDK at all, e.g. macOS without the vendor SDKs.)
[[maybe_unused]] bool sdkLoads([[maybe_unused]] const char* dll) {
#ifdef _WIN32
    return LoadLibraryA(dll) != nullptr;   // the loader keeps one reference per module; this just bumps it
#else
    return true;
#endif
}

[[maybe_unused]] int indexOf(std::string_view spec, std::string_view prefix) {
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
    if (spec.starts_with("sim-loop:")) return std::make_unique<SimCamera>(std::string(spec.substr(9)), SimOptions{SimPace::Realtime, true});
    if (spec.starts_with("sim-fast:")) return std::make_unique<SimCamera>(std::string(spec.substr(9)), SimOptions{SimPace::AsFastAsPossible});
    if (spec.starts_with("asi")) {
#ifdef SLOPCAP_HAVE_ASI
        if (!sdkLoads("ASICamera2.dll")) throw CameraError("ZWO SDK not found (ASICamera2.dll)");
        return std::make_unique<AsiCamera>(indexOf(spec, "asi"));
#else
        throw CameraError("this build has no ZWO support (SDK not found at build time)");
#endif
    }
    if (spec.starts_with("qhy")) {
#ifdef SLOPCAP_HAVE_QHY
        if (!sdkLoads("qhyccd.dll")) throw CameraError("QHY SDK not found (qhyccd.dll)");
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
    if (!sdkLoads("ASICamera2.dll")) out.push_back("(ZWO SDK not found: ASICamera2.dll)");
    else
        for (const auto& c : AsiCamera::list()) out.push_back(std::format("asi:{}  {}", c.index, c.model));
#else
    out.push_back("(ZWO support not built)");
#endif
#ifdef SLOPCAP_HAVE_QHY
    if (!sdkLoads("qhyccd.dll")) out.push_back("(QHY SDK not found: qhyccd.dll)");
    else
        for (const auto& c : QhyCamera::list()) out.push_back(std::format("qhy:{}  {}  [{}]", c.index, c.model, c.id));
#else
    out.push_back("(QHY support not built)");
#endif
    return out;
}

}  // namespace slopcap

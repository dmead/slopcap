// Open any camera from a short spec, for tools and tests:
//   "sim:FILE.ser"  replay a recording in real time      "sim-fast:FILE.ser"  as fast as possible
//   "sim-loop:FILE.ser"  in real time, over and over (demos, long tests)
//   "asi" / "asi:N" ZWO camera N (default 0)            "qhy" / "qhy:N"       QHY camera N
#pragma once

#include "camera/camera.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace slopcap {

std::unique_ptr<ICamera> openCamera(std::string_view spec);   ///< throws CameraError

/// Every connected real camera as "spec  model" lines (backends that weren't built are noted).
std::vector<std::string> listCameras();

/// Slow one-time camera set-up, best started off the GUI thread at launch (macOS: QHY firmware upload, a few
/// seconds). listCameras() and openCamera() wait for it if it is still running. Does nothing on other platforms.
void prepareCameras();

/// Windows: before calling into a camera SDK, its DLL must load (LoadLibrary), or the delay-loaded call crashes. The
/// tests' fake SDKs live in the test program itself, so they replace that check. nullptr restores it.
using SdkLoadCheck = bool (*)(const char* dll);
void setSdkLoadCheck(SdkLoadCheck check);

}  // namespace slopcap

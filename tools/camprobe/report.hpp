// Camera capability report shared by the vendor probes: identity, sensor, formats, controls,
// readout modes and "gain landmarks" (unity, lowest read noise / HCG, highest dynamic range, ...).
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace camprobe {

struct Control {
    std::string name;
    std::string description;
    double min = 0, max = 0, step = 0;
    std::optional<double> defaultValue;
    std::optional<double> current;
    bool writable = true;
    bool autoSupported = false;
    bool autoOn = false;
};

struct ReadoutMode {
    int index = 0;
    std::string name;
    std::optional<int> width, height;   // some modes change the usable resolution
    bool current = false;
};

struct GainLandmark {
    std::string name;                   // "Unity gain", "Lowest read noise (HCG)", ...
    std::optional<double> gain;
    std::optional<double> offset;
    std::string source;                 // which SDK call it came from, or "estimate: ..."
};

struct CameraReport {
    std::string vendor, sdkVersion, model, id;
    int sensorWidth = 0, sensorHeight = 0;
    double pixelUm = 0;
    int adcBits = 0;
    bool color = false;
    std::string bayer;
    std::vector<std::string> formats;
    std::vector<int> bins;
    std::vector<Control> controls;
    std::vector<ReadoutMode> readoutModes;
    std::vector<std::string> triggerModes;
    std::vector<GainLandmark> gainLandmarks;
    std::vector<std::string> capabilities;  // vendor feature flags that are present
    std::vector<std::string> notes;
};

struct ProbeResult {
    std::string vendor;
    bool sdkAvailable = false;
    std::string sdkVersion;
    std::vector<CameraReport> cameras;
    std::vector<std::string> errors;
};

void printText(const ProbeResult& r);
std::string toJson(const std::vector<ProbeResult>& results);

}  // namespace camprobe

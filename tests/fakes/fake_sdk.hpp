// Fakes of the ZWO and QHY camera SDKs for the tests: the SDKs' C functions, defined in the test executable, take the
// real libraries' place there, so src/camera/asi_camera.cpp and qhy_camera.cpp run unmodified against a scripted
// camera. Each fake is a plain struct the tests set up (cameras, controls, read modes) and can make fail (any SDK call
// by name), and that records what the backend asked of it. reset() restores a typical camera.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fake {

/// Draw a test frame: dark sky and a bright disc (diameter about a fifth of the frame), 8 or 16 bits, row-major.
void drawDisc(uint8_t* dst, int width, int height, int bytesPerSample, int frameNumber);

struct AsiControl {
    std::string name;
    int type = 0;            ///< ASI_CONTROL_TYPE
    long min = 0, max = 100, def = 0, value = 0;
    bool autoSupported = false, writable = true, autoOn = false;
};

struct AsiCam {
    std::string name = "ZWO ASI585MC";
    int id = 0;
    long maxWidth = 1920, maxHeight = 1080;
    bool color = true;
    int bayer = 0;                       ///< ASI_BAYER_PATTERN (0 = RG)
    std::vector<int> bins{1, 2};
    std::vector<int> formats{0, 2};      ///< ASI_IMG_RAW8 = 0, ASI_IMG_RAW16 = 2
    double pixelUm = 2.9;
    int bitDepth = 12;
    std::vector<AsiControl> controls;
    bool serialOk = true, gainOffsetOk = true, lmhOk = true;
};

struct Asi {
    std::vector<AsiCam> cams;
    std::map<std::string, int> fail;     ///< SDK function name -> error code it returns (until erased)
    std::map<std::string, int> failOnce; ///< the same, for the next call only
    int timeouts = 0;                    ///< ASIGetVideoData times out this many times before delivering
    int dropped = 0;                     ///< what ASIGetDroppedFrames reports
    int frameMs = 10;                    ///< time each frame takes
    /// Draws the frames instead of drawDisc (pixels, width, height, bytes per sample, frame number). Runs on the
    /// capture thread: change it while a camera streams with setAsiDraw().
    std::function<void(uint8_t*, int, int, int, int)> draw;
    // what the backend did
    int openId = -1;
    bool capturing = false;
    int width = 0, height = 0, bin = 0, imgType = -1, startX = 0, startY = 0, frames = 0;
    void reset();
    AsiCam& cam() { return cams.front(); }
    AsiControl* control(const std::string& name);
};
Asi& asi();
/// Replace Asi::draw safely while the capture thread may be calling it.
void setAsiDraw(std::function<void(uint8_t*, int, int, int, int)> draw);

struct QhyMode {
    std::string name;
    uint32_t width = 0, height = 0;      ///< 0: the mode doesn't change the area
};

struct QhyCam {
    std::string id = "QHY533M-0123456789abcdef";
    std::string model = "QHY533M";
    std::vector<QhyMode> modes{{"STD Mode", 0, 0}, {"High Gain Mode", 0, 0}, {"Extend Fullwell Mode", 3008, 3000}};
    uint32_t currentMode = 0;
    uint32_t width = 3008, height = 3008;
    double pixelUm = 3.76;
    uint32_t bayer = 0;                  ///< 0 = mono, else BAYER_GB / GR / BG / RG
    std::set<int> available;             ///< CONTROL_IDs IsQHYCCDControlAvailable answers OK for
    std::map<int, double> params;
    std::map<int, std::vector<double>> ranges;   ///< CONTROL_ID -> {min, max, step}
    bool readNoiseCurve = true, systemGainCurve = true;
};

struct Qhy {
    std::vector<QhyCam> cams;
    std::map<std::string, int> fail;     ///< SDK function name -> return value (non-zero = failure)
    std::map<std::string, int> failOnce; ///< the same, for the next call only
    int notReady = 0;                    ///< GetQHYCCDLiveFrame says "not yet" this many times before a frame
    bool wrongSize = false;              ///< deliver a frame of the wrong size
    bool openFails = false;              ///< OpenQHYCCD returns null
    int frameMs = 0;                     ///< time each frame takes
    // what the backend did
    bool open = false, live = false, initialised = false;
    uint32_t bits = 0, bin = 0, x = 0, y = 0, w = 0, h = 0, streamMode = 9, readMode = 0;
    int resourceUsers = 0, frames = 0;
    void reset();
    QhyCam& cam() { return cams.front(); }
};
Qhy& qhy();
int qhyFirmwareUploads();                ///< OSXInitQHYCCDFirmwareArray calls in this process (macOS)

}  // namespace fake

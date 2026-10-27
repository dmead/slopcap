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

struct PoaConfig {
    std::string name;
    int id = 0;                          ///< POAConfig
    int type = 0;                        ///< POAValueType: 0 int, 1 float, 2 bool
    double min = 0, max = 100, def = 0, value = 0;
    bool autoSupported = false, writable = true, autoOn = false;
};

struct PoaCam {
    std::string name = "Uranus-C", sn = "c0ffee0123";
    int id = 0, maxWidth = 3856, maxHeight = 2180, bitDepth = 12;
    bool color = true;
    int bayer = 0;                       ///< POABayerPattern (0 = RG)
    double pixelUm = 2.9;
    std::vector<int> bins{1, 2, 4};
    std::vector<int> formats{0, 1};      ///< POA_RAW8 = 0, POA_RAW16 = 1
    std::vector<std::string> modes{"Normal", "LowNoise"};
    std::vector<PoaConfig> configs;
    bool gainsOk = true;
};

struct Poa {
    std::vector<PoaCam> cams;
    std::map<std::string, int> fail;     ///< SDK function name -> POAErrors it returns (until erased)
    std::map<std::string, int> failOnce; ///< the same, for the next call only
    int timeouts = 0;                    ///< POAGetImageData times out this many times before delivering
    int dropped = 0;                     ///< what POAGetDroppedImagesCount reports
    int frameMs = 5;
    // what the backend did
    int openId = -1, mode = 0;
    bool initialised = false, exposing = false;
    int width = 0, height = 0, bin = 0, startX = 0, startY = 0, format = -1, frames = 0;
    void reset();
    PoaCam& cam() { return cams.front(); }
    PoaConfig* config(const std::string& name);
};
Poa& poa();

struct ToupCam {
    std::string name = "ATR585C", id = "tp-0001", sn = "TP00000000000001";
    unsigned long long flags = 0;        ///< TOUPCAM_FLAG_*: reset() sets a colour camera with everything
    unsigned width = 3840, height = 2160;
    float pixelUm = 2.9f;
    int maxBits = 12;
    unsigned fourcc = 0;                 ///< 0: RGGB (reset() fills it); 'YYYY' for mono
    int maxSpeed = 3, maxFan = 2;
    int tecRange = 0;                    ///< TECTARGET_RANGE (0: not answered)
    bool zeroPadding = true;             ///< takes OPTION_ZERO_PADDING (an older SDK doesn't: samples come LSB-aligned)
};

/// One per brand: the same fake is compiled for ToupTek's symbols and for MallinCam's.
struct Toup {
    std::vector<ToupCam> cams;
    std::map<std::string, int> fail;     ///< function name without the brand prefix ("put_Roi") -> HRESULT
    std::map<std::string, int> failOnce;
    bool openFails = false;
    int timeouts = 0;                    ///< WaitImageV3 times out this many times before delivering
    unsigned seqJump = 0;                ///< the next frame's sequence number skips this many (frames lost)
    bool wrongSize = false;              ///< deliver a frame of the wrong size
    int frameMs = 5;
    // what the backend did
    bool open = false, running = false;
    std::map<unsigned, int> options;
    unsigned expo = 10000, roiX = 0, roiY = 0, roiW = 0, roiH = 0, seq = 0;
    unsigned short gain = 100, speed = 0;
    int autoExpo = 0, frames = 0;
    short temperature = 215;             ///< 0.1 °C
    void reset();
    ToupCam& cam() { return cams.front(); }
};
Toup& touptek();
Toup& mallincam();

}  // namespace fake

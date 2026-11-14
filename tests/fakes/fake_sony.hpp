// A fake Sony camera in PC Remote mode, at the PTP level: device info, Sony's handshake, all properties in one dataset,
// values set directly or a notch at a time, the shutter buttons, pictures (JPEG from tests/fakes/jpeg_writer, ARW too
// when set to RAW+JPEG) appearing a little after the shutter is pressed, live view if it has it. Tests set it up and can
// make it fail; it records what it was asked. It sits behind the same seams the real camera does: straight as a
// ptp::Transport, as a WPD device (the MTP-extension commands' contexts and chunks), as USB bulk pipes (containers,
// packets, empty packets), and as a SonyDeviceSource for openCamera("sony").
#pragma once

#include "camera/ptp_usb.hpp"
#include "camera/ptp_wpd.hpp"
#include "camera/sony_camera.hpp"
#include "camera/sony_ptp.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace fake {

namespace ptp = slopcap::ptp;
namespace sony = slopcap::sony;

class SonyCam {
public:
    SonyCam();

    // ---- set up by the tests (change them while no stream runs, except through the setters below)
    std::string manufacturer = "Sony Corporation", model = "ILCE-6000", firmware = "3.21", serial = "0123456789abcdef";
    std::vector<uint16_t> operations;   ///< DeviceInfo's (Sony's 0x92xx included)
    uint16_t version = 200;             ///< Sony protocol version it reports
    int width = 320, height = 200;      ///< picture size
    bool liveView = false;
    int liveWidth = 160, liveHeight = 100;
    enum class Quality { Jpeg, RawOnly, RawPlusJpeg, RawFirst } quality = Quality::Jpeg;
    int jpegLagMs = 0;                  ///< RawFirst: the JPEG comes this much later than the ARW
    bool listValues = true;             ///< shutter speed and ISO come with their lists of values
    enum class Direct { Works, Refused, Ignored } direct = Direct::Works;   ///< SetControlDeviceA on shutter / ISO
    int stepSign = 1;                   ///< +1: kStepUp moves to the next value in the list (longer exposure / higher ISO)
    bool manual = true;                 ///< mode dial on M: shutter speed and ISO settable
    int shotMs = 20;                    ///< shutter to picture ready
    bool noPicture = false;             ///< the shutter fires but no picture ever comes
    int leftover = 0;                   ///< pictures waiting when we connect
    int busyAfterHandshake = 0;         ///< GetAllDevicePropData answers busy this many times
    bool oddProps = true;               ///< an array-typed and a string-typed property in the dataset
    // as a real a6000 did (its first report, 2026-11-13)
    bool liveViewSilent = false;        ///< GetObject 0xFFFFC002 gets no answer at all
    int zeroReads = 0;                  ///< the first this many property reads after the handshake show shutter and ISO as 0
    bool flaggedFixed = false;          ///< shutter speed and ISO flagged not settable (getSet 0) though notches work
    bool qualitySettable = true;
    std::map<uint16_t, uint16_t> fail;      ///< operation -> response code it answers
    std::map<uint16_t, int> failTimes;      ///< operation -> how many more times it answers General Error
    std::map<uint16_t, int> linkFailTimes;  ///< operation -> how many more times the link breaks on it

    int64_t shutter = sony::shutterValue(1, 250), iso = 800, program = 1;
    std::vector<int64_t> shutterList, isoList;

    /// The scene (luminance, width, height, picture number); default a disc. Change it with setDraw() while streaming.
    void setDraw(std::function<void(uint8_t*, int, int, int)> draw);
    void setUnplugged(bool gone);       ///< every transaction from now on fails as if the cable were pulled
    /// The next event for the interrupt pipe (a whole PTP event container), if one waits; how many wait.
    std::optional<std::vector<uint8_t>> takeEvent();
    int eventsWaiting() const;
    void resetPtp();                    ///< PTP's Device Reset request: the session and Sony's handshake are gone

    // ---- what happened
    std::atomic<int> shots{0}, stepsTaken{0}, directSets{0};
    std::vector<uint16_t> ops() const;  ///< the operations asked for, in order
    bool sessionOpen = false;
    int sdioStep = 0;
    std::atomic<int> liveFrames{0};

    /// One PTP transaction, as the camera answers it.
    ptp::Response handle(uint16_t op, std::span<const uint32_t> params, ptp::Data phase, std::span<const uint8_t> out,
                         std::vector<uint8_t>* in);
    std::vector<uint8_t> jpegOf(int w, int h, int n);   ///< a picture as it would send it

private:
    struct Object {
        uint16_t format;
        std::vector<uint8_t> data;
        int w, h;
        std::chrono::steady_clock::time_point readyAt;
        int shot;   ///< which press made it (its file name: DSC0000N)
    };
    std::vector<uint8_t> props();
    void step(int64_t& value, const std::vector<int64_t>& list, int64_t dir);
    void addShot(std::chrono::steady_clock::time_point at);
    void event(uint16_t code, uint32_t param);   ///< (m_ held)
    std::deque<std::vector<uint8_t>> events_;
    int readsSinceHandshake_ = 0;
    mutable std::mutex m_;
    std::function<void(uint8_t*, int, int, int)> draw_;
    bool unplugged_ = false;
    std::vector<uint16_t> ops_;
    std::deque<Object> objects_;
    int busyLeft_ = 0;
};

/// The camera straight as a transport.
std::unique_ptr<ptp::Transport> directTransport(SonyCam& cam);

/// The camera behind WPD's MTP-extension commands.
class WpdDevice final : public ptp::wpd::Device {
public:
    explicit WpdDevice(SonyCam& cam, uint32_t optimal = 64 * 1024) : cam_(cam), optimal_(optimal) {}
    ptp::wpd::Values send(ptp::wpd::Command c, const ptp::wpd::Values& in) override;
    std::map<ptp::wpd::Command, int32_t> hresult;   ///< a command -> the HRESULT its result carries
    std::set<ptp::wpd::Command> linkFail;           ///< commands whose SendCommand fails
    int shortBy = 0;                                ///< reads deliver this many bytes less than announced
    int openContexts() const { return static_cast<int>(contexts_.size()); }
    std::vector<ptp::wpd::Command> log;

private:
    struct Context {
        uint16_t op;
        std::vector<uint32_t> params;
        std::vector<uint8_t> data;
        std::size_t at = 0;
        bool write = false;
        ptp::Response response;
    };
    SonyCam& cam_;
    uint32_t optimal_;
    std::map<std::string, Context> contexts_;
    int next_ = 0;
};

/// The camera behind USB bulk pipes: PTP containers in packets.
class UsbPipe final : public ptp::UsbPipe {
public:
    explicit UsbPipe(SonyCam& cam, std::size_t packet = 512) : cam_(cam), packet_(packet) {}
    void write(std::span<const uint8_t> data) override;
    std::size_t read(std::span<uint8_t> buf, std::chrono::milliseconds timeout) override;
    std::size_t packetSize() const override { return packet_; }
    bool zlp = true;            ///< ends a container of whole packets with an empty one (false: runs on into the next)
    bool events = false;        ///< an event container before every response
    bool staleResponse = false; ///< one answer to an old transaction before the next response
    bool badContainer = false;  ///< the next answer is of an unknown container type
    bool interruptPipe = true;  ///< has an interrupt pipe, for the camera's events
    bool holdForEvents = false; ///< answers wait (up to half a second) while an event is unread, then never come
    bool hasEvents() const override { return interruptPipe; }
    std::size_t readEvent(std::span<uint8_t> buf, std::chrono::milliseconds timeout) override;
    void reset() override;
    int writes = 0, emptyWrites = 0, resets = 0;

private:
    void answer(uint16_t type, uint16_t code, uint32_t tid, std::span<const uint8_t> payload);
    SonyCam& cam_;
    std::size_t packet_;
    std::vector<uint8_t> pendingCommand_;   ///< a command whose data phase is still to come
    std::deque<std::vector<uint8_t>> out_;  ///< containers on their way to the host
    std::size_t outAt_ = 0;
    bool zlpDue_ = false;
};

/// Sony cameras for openCamera("sony"): these fakes, through one of the seams.
class SonySource final : public slopcap::SonyDeviceSource {
public:
    enum class Via { Direct, Wpd, Usb };
    std::vector<SonyCam*> cams;
    Via via = Via::Direct;
    std::string problem;   ///< reported for every camera
    std::vector<Entry> list() override;
    std::unique_ptr<ptp::Transport> open(const std::string& id) override;
};

/// Draws a disc whose edge softens with `blur` (pixels), on dark sky, in luminance.
void drawSoftDisc(uint8_t* px, int w, int h, double blur);

}  // namespace fake

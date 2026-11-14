#include "camera/sony_camera.hpp"

#include "camera/jpeg_luma.hpp"
#include "camera/ptp_usb.hpp"
#ifdef _WIN32
#include "camera/ptp_wpd.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace slopcap {

namespace {

using Clock = std::chrono::steady_clock;

/// When the camera doesn't list what it offers: the usual third-stop series (the camera stops on the nearest).
const std::vector<int64_t>& standardShutter() {
    static const std::vector<int64_t> v = [] {
        std::vector<int64_t> out;
        for (int s : {300, 250, 200, 150, 130, 100, 80, 60, 50, 40, 32, 25, 20, 16, 13, 10, 8, 6, 5, 4}) out.push_back(sony::shutterValue(s, 10));
        for (int d : {3, 4, 5, 6, 8, 10, 13, 15, 20, 25, 30, 40, 50, 60, 80, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250,
                      1600, 2000, 2500, 3200, 4000})
            out.push_back(sony::shutterValue(1, d));
        return out;
    }();
    return v;
}

const std::vector<int64_t>& standardIso() {
    static const std::vector<int64_t> v = {100,  125,  160,  200,  250,  320,  400,   500,   640,   800,   1000,  1250, 1600,
                                           2000, 2500, 3200, 4000, 5000, 6400, 8000, 10000, 12800, 16000, 20000, 25600};
    return v;
}

/// Exposure time in log seconds; bulb past the longest.
double shutterOrder(int64_t v) { return std::log(sony::shutterSeconds(v).value_or(1000.0)); }
/// ISO in log; auto below the lowest.
double isoOrder(int64_t v) { return std::log(static_cast<double>(sony::isoNumber(v).value_or(50))); }

int64_t nearest(const std::vector<int64_t>& values, double goal, double (*order)(int64_t)) {
    return *std::ranges::min_element(values, {}, [&](int64_t v) { return std::abs(order(v) - goal); });
}

SonyDeviceSource* g_source = nullptr;

class SystemSource final : public SonyDeviceSource {
public:
    std::vector<Entry> list() override {
        std::vector<Entry> out;
#ifdef _WIN32   // on Windows' own portable-device driver; or on WinUSB (below)
        for (const auto& d : ptp::wpd::listDevices())
            if (ptp::wpd::usbIdsOf(d.id).starts_with(std::format("vid_{:04x}", sony::kUsbVendor)))
                out.push_back({d.id, !d.name.empty() ? d.name : d.description, {}});
#endif
        for (const auto& u : ptp::listUsbCameras(sony::kUsbVendor)) out.push_back({u.id, u.model.empty() ? "Sony camera" : u.model, u.problem});
        return out;
    }
    std::unique_ptr<ptp::Transport> open(const std::string& id) override {
#ifdef _WIN32
        if (!id.starts_with("winusb:")) return std::make_unique<ptp::wpd::WpdTransport>(ptp::wpd::openDevice(id));
#endif
        return std::make_unique<ptp::UsbTransport>(ptp::openUsbCamera(id));
    }
};

SonyDeviceSource& source() { return g_source ? *g_source : systemSonyDevices(); }

}  // namespace

SonyDeviceSource& systemSonyDevices() {
    static SystemSource s;
    return s;
}

void setSonyDeviceSource(SonyDeviceSource* s) { g_source = s; }

std::vector<SonyDeviceSource::Entry> SonyCamera::list() { return source().list(); }

SonyCamera::SonyCamera(std::string_view which) {
    const auto all = list();
    const SonyDeviceSource::Entry* e = nullptr;
    int index = -1;
    if (!which.empty() && std::ranges::all_of(which, [](char c) { return c >= '0' && c <= '9'; })) index = std::stoi(std::string(which));
    for (std::size_t i = 0; i < all.size() && !e; ++i)
        if (which.empty() || static_cast<int>(i) == index ||
            (index < 0 && (all[i].model.find(which) != std::string::npos || all[i].id.find(which) != std::string::npos)))
            e = &all[i];
    if (!e)
        throw CameraError(which.empty() ? "no Sony camera found (connected by USB, with USB Connection set to PC Remote?)"
                                        : std::format("no Sony camera {}", which));
    t_ = source().open(e->id);
    s_ = std::make_unique<sony::Session>(*t_);

    const auto dev = ptp::parseDeviceInfo(ptp::receive(*t_, ptp::kGetDeviceInfo, {}, "camera information"));
    info_.vendor = "Sony";
    info_.model = dev.model.empty() ? e->model : dev.model;
    info_.serial = dev.serial.empty() ? info_.model : dev.serial;
    if (!dev.supports(sony::kSdioConnect))
        throw CameraError(std::format("{} doesn't offer remote control over USB: set the camera's USB Connection to PC Remote", info_.model));
    s_->connect();
    s_->settle(std::chrono::seconds(3));   // (the values read 0 until the camera has caught up)
    // RAW alone gives nothing to score: JPEG Fine while we have the camera (put back when we let go of it)
    if (const auto* q = s_->prop(sony::kCompression); q && q->current == sony::kQualityRaw && q->settable()) {
        s_->setValue(sony::kCompression, sony::kQualityFine, [](int64_t v) { return static_cast<double>(v); });
        restoreQuality_ = sony::kQualityRaw;
    }

    if (const auto* p = s_->prop(sony::kShutterSpeed); p && p->form == sony::Prop::Form::Enum)
        for (auto v : p->values)
            if (sony::shutterSeconds(v)) shutterList_.push_back(v);
    if (const auto* p = s_->prop(sony::kIso); p && p->form == sony::Prop::Form::Enum)
        for (auto v : p->values)
            if (sony::isoNumber(v) && (v >> 24) == 0) isoList_.push_back(v);
    if (shutterList_.empty()) shutterList_ = standardShutter();
    if (isoList_.empty()) isoList_ = standardIso();
    readCache();

    // one picture for the size
    const auto shot = s_->shoot(shotTimeout());
    const auto* jpeg = shot ? shot->jpeg() : nullptr;
    if (!jpeg)
        throw CameraError(std::format("{} sent no JPEG: set its Quality to Fine, Extra fine or RAW+JPEG (not RAW alone)", info_.model));
    const auto size = jpeg::size(jpeg->data);
    readCache();
    info_.sensorWidth = size.width;
    info_.sensorHeight = size.height;
    if (const double mm = sony::sensorWidthMm(info_.model); mm > 0) info_.pixelUm = mm * 1000 / size.width;
    info_.bayer = Bayer::None;   // the luminance of the JPEG
    info_.bins = {1};
    info_.bitDepths = {8};
    info_.stills = true;
    roi_ = {0, 0, size.width, size.height, 1};
}

SonyCamera::~SonyCamera() {
    try {
        stopStream();
        if (restoreQuality_) {
            std::lock_guard io(io_);
            s_->setValue(sony::kCompression, *restoreQuality_, [](int64_t v) { return static_cast<double>(v); });
        }
    } catch (const std::exception&) {   // (unplugged: nothing to put back)
    }
}

void SonyCamera::readCache() const {
    Cache c;
    if (const auto* p = s_->prop(sony::kShutterSpeed)) c.shutter = p->current, c.shutterSettable = p->settable();
    if (const auto* p = s_->prop(sony::kIso)) c.iso = p->current, c.isoSettable = p->settable();
    if (const auto* p = s_->prop(sony::kExposureProgram)) c.program = p->current;
    c.at = Clock::now();
    std::lock_guard l(state_);
    cache_ = c;
}

void SonyCamera::refreshCache() const {
    {
        std::lock_guard l(state_);
        if (streaming_ || Clock::now() - cache_.at < std::chrono::milliseconds(500)) return;   // the shots keep it fresh
    }
    std::lock_guard io(io_);
    try {
        s_->refresh();
        readCache();
    } catch (const std::exception&) {   // (the last values stand; the next shot says what is wrong)
    }
}

std::chrono::milliseconds SonyCamera::shotTimeout() const {
    std::lock_guard l(state_);
    const double s = sony::shutterSeconds(cache_.shutter).value_or(30);
    return std::chrono::milliseconds(static_cast<int64_t>((2 * s + 15) * 1000));   // (long-exposure noise reduction doubles it)
}

std::vector<ControlInfo> SonyCamera::controls() const {
    refreshCache();
    std::lock_guard l(state_);
    std::vector<ControlInfo> out;
    const auto [shortest, longest] = std::ranges::minmax(shutterList_, {}, shutterOrder);
    ControlInfo exp{"Exposure", *sony::shutterSeconds(shortest) * 1e6, *sony::shutterSeconds(longest) * 1e6, 0, std::nullopt,
                    cache_.shutterSettable, false};
    const auto [low, high] = std::ranges::minmax(isoList_, {}, isoOrder);
    ControlInfo iso{"ISO", static_cast<double>(*sony::isoNumber(low)), static_cast<double>(*sony::isoNumber(high)), 0, std::nullopt,
                    cache_.isoSettable, false};
    out.push_back(exp);
    out.push_back(iso);
    out.push_back({"Interval", 0, 600, 1, 0.0, true, false});
    return out;
}

std::optional<double> SonyCamera::control(std::string_view name) const {
    if (name == "Interval") {
        std::lock_guard l(state_);
        return interval_;
    }
    if (name != "Exposure" && name != "ISO") return std::nullopt;
    refreshCache();
    std::lock_guard l(state_);
    if (name == "Exposure") {
        const auto s = sony::shutterSeconds(cache_.shutter);
        return s ? std::optional(*s * 1e6) : std::nullopt;
    }
    const auto iso = sony::isoNumber(cache_.iso);
    return iso ? std::optional(static_cast<double>(*iso)) : std::nullopt;
}

void SonyCamera::setControl(std::string_view name, double value) {
    if (name == "Interval") {
        if (value < 0 || value > 600) throw CameraError("Interval out of range 0..600 s");
        std::lock_guard l(state_);
        interval_ = value;
        cv_.notify_all();   // a pause under way follows the new value
        return;
    }
    uint16_t code = 0;
    int64_t target = 0;
    double (*order)(int64_t) = nullptr;
    if (name == "Exposure") {
        if (!(value > 0)) throw CameraError("Exposure must be more than 0");
        code = sony::kShutterSpeed, order = shutterOrder;
        target = nearest(shutterList_, std::log(value / 1e6), order);
    } else if (name == "ISO") {
        if (!(value > 0)) throw CameraError("ISO must be more than 0");
        code = sony::kIso, order = isoOrder;
        target = nearest(isoList_, std::log(value), order);
    } else {
        throw CameraError(std::format("{} has no control {}", info_.model, name));
    }
    ++waitingForIo_;   // the next shot waits for this
    std::lock_guard io(io_);
    --waitingForIo_;
    s_->setValue(code, target, order);
    readCache();
}

Roi SonyCamera::roi() const {
    std::lock_guard l(state_);
    return roi_;
}

void SonyCamera::setRoi(const Roi& r) {
    if (streaming_) throw CameraError("stop the stream before changing the ROI");
    if (r.bin != 1) throw CameraError(std::format("bin {} not supported", r.bin));
    if (r.width <= 0 || r.height <= 0 || r.x < 0 || r.y < 0 || r.x + r.width > info_.sensorWidth || r.y + r.height > info_.sensorHeight)
        throw CameraError(std::format("ROI {}x{}+{}+{} outside the {}x{} picture", r.width, r.height, r.x, r.y, info_.sensorWidth,
                                      info_.sensorHeight));
    std::lock_guard l(state_);
    roi_ = r;
}

void SonyCamera::setBitDepth(int bits) {
    if (bits != 8) throw CameraError(std::format("{}-bit output not supported (the JPEG's luminance is 8-bit)", bits));
}

void SonyCamera::startStream() {
    if (streaming_) return;
    {
        std::lock_guard l(state_);
        fresh_ = false;
        error_.clear();
        sequence_ = dropped_ = 0;
        streaming_ = true;
    }
    stop_ = false;
    thread_ = std::thread([this] { worker(); });
}

void SonyCamera::stopStream() {
    if (!streaming_) return;
    {
        std::lock_guard l(state_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard l(state_);
    streaming_ = false;
}

void SonyCamera::fail(const std::string& why) {
    {
        std::lock_guard l(state_);
        error_ = why;
    }
    cv_.notify_all();
}

void SonyCamera::worker() {
    int failures = 0;
    bool first = true;
    while (!stop_) {
        {   // the pause between shots (a new interval set meanwhile counts from the same start)
            std::unique_lock l(state_);
            const auto from = Clock::now();
            while (!first && !stop_ && interval_ > 0) {
                const auto until = from + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(interval_));
                if (Clock::now() >= until) break;
                cv_.wait_until(l, until);
            }
        }
        first = false;
        while (waitingForIo_ > 0 && !stop_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (stop_) break;
        std::optional<sony::Shot> shot;
        const auto timeout = shotTimeout();
        try {
            std::lock_guard io(io_);
            shot = s_->shoot(timeout, [this] { return stop_.load(); });
            if (shot) readCache();
        } catch (const ptp::LinkError& e) {   // the link itself: the camera is gone
            fail(e.what());
            return;
        } catch (const std::exception& e) {
            if (++failures >= 3) return fail(e.what());
            continue;
        }
        if (!shot) break;
        const auto* j = shot->jpeg();
        if (!j) return fail(std::format("{} sent no JPEG: set its Quality to Fine, Extra fine or RAW+JPEG (not RAW alone)", info_.model));
        jpeg::Size size;
        try {
            size = jpeg::decodeLuma(j->data, luma_);
        } catch (const std::exception& e) {
            if (++failures >= 3) return fail(e.what());
            continue;
        }
        failures = 0;
        deliver(*shot, size.width, size.height);
    }
}

void SonyCamera::deliver(const sony::Shot& shot, int width, int height) {
    std::unique_lock l(state_);
    const Roi r = roi_;
    // the ROI out of the picture (a picture of another size, the camera's settings changed: what overlaps)
    frame_.assign(static_cast<std::size_t>(r.width) * static_cast<std::size_t>(r.height), 0);
    for (int y = 0; y < r.height && r.y + y < height; ++y) {
        const int n = std::min(r.width, width - r.x);
        if (n > 0)
            std::memcpy(frame_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(r.width),
                        luma_.data() + static_cast<std::size_t>(r.y + y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(r.x),
                        static_cast<std::size_t>(n));
    }
    if (fresh_) ++dropped_;   // the last one was never read
    const double exposure = sony::shutterSeconds(cache_.shutter).value_or(0);
    const auto sinceTrigger = Clock::now() - shot.triggered;
    frameInfo_ = {};
    frameInfo_.sequence = sequence_++;
    frameInfo_.timestamp = std::chrono::system_clock::now() - std::chrono::duration_cast<std::chrono::system_clock::duration>(sinceTrigger) +
                           std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<double>(exposure));
    frameInfo_.exposureStart = shot.triggered;
    frameInfo_.roi = r;
    frameInfo_.bitDepth = 8;
    frameInfo_.bytes = frame_.size();
    fresh_ = true;
    l.unlock();
    cv_.notify_all();
}

ReadResult SonyCamera::readFrame(std::span<std::byte> dst, FrameInfo& out, std::chrono::milliseconds timeout) {
    if (!streaming_) throw CameraError("stream not started");
    std::unique_lock l(state_);
    if (!cv_.wait_for(l, timeout, [this] { return fresh_ || !error_.empty(); })) return ReadResult::Timeout;
    if (!fresh_) throw CameraError(error_);
    if (dst.size() < frame_.size()) throw CameraError("destination buffer smaller than one frame");
    std::memcpy(dst.data(), frame_.data(), frame_.size());
    out = frameInfo_;
    fresh_ = false;
    return ReadResult::Ok;
}

uint64_t SonyCamera::droppedFrames() const {
    std::lock_guard l(state_);
    return dropped_;
}

}  // namespace slopcap

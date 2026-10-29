#include "pipeline/capture_session.hpp"

#include "io/ser.hpp"
#include "util/cpu_time.hpp"

#include <algorithm>
#include <format>
#include <future>

namespace slopcap {

namespace {

ser::ColorId serColor(Bayer b) {
    switch (b) {
        case Bayer::RGGB: return ser::ColorId::BayerRGGB;
        case Bayer::GRBG: return ser::ColorId::BayerGRBG;
        case Bayer::GBRG: return ser::ColorId::BayerGBRG;
        case Bayer::BGGR: return ser::ColorId::BayerBGGR;
        case Bayer::None: break;
    }
    return ser::ColorId::Mono;
}

}  // namespace

std::size_t recordingRingBytes(std::optional<uint64_t> totalRam, std::optional<uint64_t> availableRam) {
    constexpr uint64_t kGiB = uint64_t{1} << 30;
    uint64_t bytes = kGiB;
    if (totalRam) bytes = availableRam ? std::min(*totalRam / 8, *availableRam / 4) : *totalRam / 8;
    return static_cast<std::size_t>(std::clamp(bytes, kGiB, 4 * kGiB));
}

CaptureSession::CaptureSession(ICamera& camera, CaptureOptions options) : cam_(camera), opt_(std::move(options)) {}

CaptureSession::~CaptureSession() {
    if (captureThread_.joinable() || writerThread_.joinable()) stop();
}

void CaptureSession::fail(const std::exception& e) {
    std::lock_guard lock(errorMutex_);
    if (error_.empty()) error_ = e.what();
    stopRequested_.store(true);
}

void CaptureSession::start() {
    if (captureThread_.joinable()) throw std::logic_error("capture already started");
    const std::size_t fb = cam_.frameBytes();
    if (fb == 0) throw CameraError("camera reports a zero-size frame");
    const std::size_t slotBytes = (fb + 4095) / 4096 * 4096;   // FrameRing rounds slots up to whole pages
    // Live view reads into scratch and never touches the ring, so don't spend memory on one.
    const std::size_t slots = opt_.output.empty() ? 1 : std::clamp<std::size_t>(opt_.ringBytes / slotBytes, 8, 1u << 20);
    ring_ = std::make_unique<FrameRing>(slots, fb);
    scratch_.resize(fb);
    stopRequested_ = false;
    captureDone_ = false;
    writerDone_ = false;

    // The writer opens the file first, so a bad output path fails before the camera starts.
    std::promise<void> writerReady;
    auto ready = writerReady.get_future();
    writerThread_ = std::thread([this, p = std::move(writerReady)]() mutable {
        std::unique_ptr<ser::FrameSink> w;
        try {
            const auto& info = cam_.info();
            const Roi r = cam_.roi();
            ser::Header h;
            h.colorId = serColor(info.bayer);
            h.width = r.width;
            h.height = r.height;
            // Every backend delivers 16-bit samples MSB-aligned (ToupTek's are shifted up to match), so the header says
            // 16: readers take the depth as the samples' range, and a 12 over MSB-aligned data reads as 16x too bright
            // or clipped. The ADC's own depth goes in the sidecar.
            h.bitDepth = cam_.bitDepth() > 8 ? 16 : 8;
            h.observer = opt_.observer;
            h.instrument = info.model;
            h.telescope = opt_.telescope;
            if (opt_.output.empty()) {
                // live view only: frames are released unwritten
            } else if (opt_.directIO) {
                auto d = std::make_unique<ser::DirectWriter>(opt_.output, h, opt_.direct);
                direct_.store(d.get());
                w = std::move(d);
            } else {
                w = std::make_unique<ser::Writer>(opt_.output, h);
            }
            p.set_value();
        } catch (...) {
            p.set_exception(std::current_exception());
            return;
        }
        try {
            for (;;) {
                auto s = ring_->next(std::chrono::milliseconds(100));
                if (!s) {
                    if (captureDone_.load(std::memory_order_acquire) && ring_->queued() == 0) break;
                    continue;
                }
                auto& slot = ring_->slot(*s);
                if (!w) {
                    ring_->release(*s);
                    continue;
                }
                if (opt_.beforeWrite) opt_.beforeWrite();
                const auto t0 = std::chrono::steady_clock::now();
                w->writeFrame(slot.pixels.first(slot.info.bytes), ser::toTicks(slot.info.timestamp));
                writeLatency_.record(std::chrono::steady_clock::now() - t0);
                bytesWritten_.fetch_add(slot.info.bytes, std::memory_order_relaxed);
                written_.fetch_add(1, std::memory_order_relaxed);
                ring_->release(*s);
            }
            if (w) w->finish();
            if (auto* d = direct_.load()) {   // keep the direct writer's numbers after it's gone
                std::lock_guard lock(directMutex_);
                finalChunkP99_ = d->chunkWriteLatency().percentileUs(0.99);
                finalChunkMax_ = d->chunkWriteLatency().maxUs();
                finalStallMax_ = d->stallLatency().maxUs();
                direct_.store(nullptr);
            }
        } catch (const std::exception& e) {
            fail(e);
            {
                std::lock_guard lock(directMutex_);   // the writer is about to be destroyed
                direct_.store(nullptr);
            }
            // keep draining so capture isn't left without free slots
            while (auto s = ring_->next(std::chrono::milliseconds(50))) ring_->release(*s);
        }
        finishedNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_).count());
        writerDone_.store(true, std::memory_order_release);
    });
    try {
        ready.get();
    } catch (...) {
        writerThread_.join();
        throw;
    }

    captureEndNs_ = 0;
    cpuCaptureNs_ = 0;
    cpuStart_ = processCpuTime();
    started_ = std::chrono::steady_clock::now();
    try {
        cam_.startStream();
    } catch (...) {
        captureDone_.store(true, std::memory_order_release);   // no capture coming: let the writer finish
        ring_->close();
        writerThread_.join();
        throw;
    }
    captureThread_ = std::thread([this] { captureLoop(); });
}

void CaptureSession::captureLoop() {
    std::optional<uint32_t> held;   // a slot we own but haven't filled (kept across timeouts: only the writer frees)
    const bool recording = !opt_.output.empty();   // live view reads straight into scratch: no ring, no drops
    try {
        while (!stopRequested_.load(std::memory_order_relaxed)) {
            if (hasPosted_.load(std::memory_order_acquire)) runPosted();
            if (opt_.frameLimit && delivered_.load(std::memory_order_relaxed) >= *opt_.frameLimit) break;
            if (opt_.duration && std::chrono::steady_clock::now() - started_ >= *opt_.duration) break;

            if (recording && !held) held = ring_->acquire();
            FrameInfo fi;
            ReadResult r;
            if (held) {
                auto& slot = ring_->slot(*held);
                r = cam_.readFrame(slot.pixels, slot.info, std::chrono::milliseconds(250));
                if (r == ReadResult::Ok) {
                    if (opt_.preview && opt_.preview->wanted()) opt_.preview->publish(slot.pixels.first(slot.info.bytes), slot.info);
                    if (opt_.remote && opt_.remote->wanted()) opt_.remote->publish(slot.pixels.first(slot.info.bytes), slot.info);
                    ring_->publish(*held);
                    held.reset();
                    delivered_.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                // Ring full (or live view): keep the camera flowing, count the loss.
                r = cam_.readFrame(scratch_, fi, std::chrono::milliseconds(250));
                if (r == ReadResult::Ok) {
                    if (opt_.preview && opt_.preview->wanted()) opt_.preview->publish(std::span(scratch_).first(fi.bytes), fi);
                    if (opt_.remote && opt_.remote->wanted()) opt_.remote->publish(std::span(scratch_).first(fi.bytes), fi);
                    delivered_.fetch_add(1, std::memory_order_relaxed);
                    if (recording) ringDropped_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            cameraDropped_.store(cam_.droppedFrames(), std::memory_order_relaxed);
            if (r == ReadResult::EndOfStream) break;
        }
    } catch (const std::exception& e) {
        fail(e);
    }
    try {
        cam_.stopStream();
    } catch (const std::exception& e) {
        fail(e);
    }
    cameraDropped_.store(cam_.droppedFrames(), std::memory_order_relaxed);
    // The capture's own length and CPU: what the writer still has to write (the drain) is reported apart.
    cpuCaptureNs_.store((processCpuTime() - cpuStart_).count(), std::memory_order_relaxed);
    captureEndNs_.store(std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_).count()),
                        std::memory_order_release);
    // An unfilled held slot is simply never published; the ring is discarded with the session.
    captureDone_.store(true, std::memory_order_release);
    ring_->close();
}

void CaptureSession::post(std::function<void(ICamera&)> action) {
    std::lock_guard lock(postMutex_);
    posted_.push_back(std::move(action));
    hasPosted_.store(true, std::memory_order_release);
}

void CaptureSession::runPosted() {
    std::vector<std::function<void(ICamera&)>> todo;
    {
        std::lock_guard lock(postMutex_);
        todo.swap(posted_);
        hasPosted_.store(false, std::memory_order_relaxed);
    }
    for (auto& a : todo) {
        try {
            a(cam_);
        } catch (const std::exception& e) {
            std::lock_guard lock(errorMutex_);
            notice_ = e.what();
        }
    }
}

void CaptureSession::stop() {
    stopRequested_.store(true);
    if (captureThread_.joinable()) captureThread_.join();
    if (writerThread_.joinable()) writerThread_.join();
}

bool CaptureSession::wait(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!writerDone_.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stop();
    return true;
}

CaptureStats CaptureSession::stats() const {
    CaptureStats s;
    s.delivered = delivered_.load(std::memory_order_relaxed);
    s.written = written_.load(std::memory_order_relaxed);
    s.cameraDropped = cameraDropped_.load(std::memory_order_relaxed);
    s.ringDropped = ringDropped_.load(std::memory_order_relaxed);
    if (ring_) {
        s.ringCapacity = ring_->capacity();
        s.ringHighWater = ring_->highWater();
        s.ringQueued = ring_->queued();
    }
    using secs = std::chrono::duration<double>;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_);
    const int64_t fin = finishedNs_.load(std::memory_order_relaxed);
    const int64_t capEnd = captureEndNs_.load(std::memory_order_acquire);
    const auto captureSpan = capEnd ? std::chrono::nanoseconds(capEnd) : now;
    const auto writerSpan = fin ? std::chrono::nanoseconds(fin) : now;
    s.seconds = secs(captureSpan).count();
    s.drainSeconds = capEnd ? std::max(0.0, secs(writerSpan - captureSpan).count()) : 0;
    if (s.seconds > 0) {
        s.captureFps = static_cast<double>(s.delivered) / s.seconds;
        const auto cpu = capEnd ? std::chrono::nanoseconds(cpuCaptureNs_.load(std::memory_order_relaxed)) : processCpuTime() - cpuStart_;
        s.cpuCores = secs(cpu).count() / s.seconds;
    }
    if (writerSpan.count() > 0) s.writeMBps = static_cast<double>(bytesWritten_.load(std::memory_order_relaxed)) / 1e6 / secs(writerSpan).count();
    s.capturing = captureThread_.joinable() && !capEnd;
    s.running = captureThread_.joinable() && !writerDone_.load(std::memory_order_acquire);
    s.writeP50Us = writeLatency_.percentileUs(0.5);
    s.writeP99Us = writeLatency_.percentileUs(0.99);
    s.writeMaxUs = writeLatency_.maxUs();
    {
        std::lock_guard lock(directMutex_);
        if (auto* d = direct_.load()) {
            s.chunkP99Us = d->chunkWriteLatency().percentileUs(0.99);
            s.chunkMaxUs = d->chunkWriteLatency().maxUs();
            s.stallMaxUs = d->stallLatency().maxUs();
        } else {
            s.chunkP99Us = finalChunkP99_;
            s.chunkMaxUs = finalChunkMax_;
            s.stallMaxUs = finalStallMax_;
        }
    }
    std::lock_guard lock(errorMutex_);
    s.error = error_;
    s.notice = notice_;
    return s;
}

}  // namespace slopcap

#include "pipeline/frame_ring.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>
#include <stdexcept>

namespace slopcap {

namespace {
constexpr std::size_t kPage = 4096;
std::size_t roundUp(std::size_t n, std::size_t a) { return (n + a - 1) / a * a; }
}  // namespace

// ---------------------------------------------------------------- SPSC index queue

FrameRing::Spsc::Spsc(std::size_t minCapacity) : buf_(std::bit_ceil(std::max<std::size_t>(minCapacity, 2))), mask_(buf_.size() - 1) {}

bool FrameRing::Spsc::push(uint32_t v) {
    const uint64_t t = tail_.load(std::memory_order_relaxed);
    if (t - head_.load(std::memory_order_acquire) >= buf_.size()) return false;
    buf_[t & mask_] = v;
    tail_.store(t + 1, std::memory_order_release);
    return true;
}

std::optional<uint32_t> FrameRing::Spsc::pop() {
    const uint64_t h = head_.load(std::memory_order_relaxed);
    if (h == tail_.load(std::memory_order_acquire)) return std::nullopt;
    const uint32_t v = buf_[h & mask_];
    head_.store(h + 1, std::memory_order_release);
    return v;
}

std::size_t FrameRing::Spsc::size() const {
    return static_cast<std::size_t>(tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire));
}

// ---------------------------------------------------------------- FrameRing

FrameRing::FrameRing(std::size_t slots, std::size_t slotBytes)
    : slotBytes_(roundUp(std::max<std::size_t>(slotBytes, 1), kPage)), free_(slots), filled_(slots) {
    if (slots == 0 || slots > (1u << 30)) throw std::invalid_argument("FrameRing needs 1..2^30 slots");
    memoryBytes_ = slotBytes_ * slots;
    memory_ = static_cast<std::byte*>(::operator new(memoryBytes_, std::align_val_t{kPage}));
    // Touch every page now so the kernel commits the memory before capture starts, not during it.
    std::memset(memory_, 0, memoryBytes_);
    slots_.resize(slots);
    for (std::size_t i = 0; i < slots; ++i) {
        slots_[i].pixels = {memory_ + i * slotBytes_, slotBytes_};
        free_.push(static_cast<uint32_t>(i));
    }
}

FrameRing::~FrameRing() { ::operator delete(memory_, std::align_val_t{kPage}); }

std::optional<uint32_t> FrameRing::acquire() { return free_.pop(); }

void FrameRing::publish(uint32_t slot) {
    filled_.push(slot);   // cannot fail: at most capacity() slots exist
    const std::size_t q = filled_.size();
    std::size_t hw = highWater_.load(std::memory_order_relaxed);
    while (q > hw && !highWater_.compare_exchange_weak(hw, q, std::memory_order_relaxed)) {
    }
    wake(false);
}

// Sleep, don't poll: a polling nap of 200 us didn't sleep at all on Windows (mingw's sleep_for returns at once
// under 1 ms), so the writer spun a whole core whenever it waited.
std::optional<uint32_t> FrameRing::next(std::chrono::milliseconds timeout) {
    if (auto s = filled_.pop()) return s;
    {
        std::unique_lock lock(waitMutex_);
        // publish() and close() change the state before taking the mutex to notify, so checking under it can't
        // miss a wake-up
        waitCv_.wait_for(lock, timeout, [this] { return filled_.size() > 0 || closed_.load(std::memory_order_acquire); });
    }
    return filled_.pop();   // nullopt on timeout, or once closed and drained
}

void FrameRing::release(uint32_t slot) { free_.push(slot); }

void FrameRing::close() {
    closed_.store(true, std::memory_order_release);
    wake(true);
}

void FrameRing::wake(bool all) {
    {
        std::lock_guard lock(waitMutex_);
    }
    if (all)
        waitCv_.notify_all();
    else
        waitCv_.notify_one();
}

std::size_t FrameRing::queued() const { return filled_.size(); }

}  // namespace slopcap

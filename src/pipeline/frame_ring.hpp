// Fixed pool of pre-allocated, page-aligned frame slots handed between exactly one producer (the capture thread)
// and one consumer (the writer) without locks or copies:
//
//   capture: slot = acquire()  ->  camera writes pixels into slot  ->  publish(slot)
//   writer:  slot = next()     ->  write slot to disk               ->  release(slot)
//
// If every slot is in use the producer gets nullopt and must drop the frame (counted by the caller); capture never
// waits for the disk. Slots are page-aligned so they can later go straight to O_DIRECT / io_uring writes.
#pragma once

#include "camera/camera.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace slopcap {

class FrameRing {
public:
    struct Slot {
        std::span<std::byte> pixels;   ///< capacity: slotBytes()
        FrameInfo info;
    };

    /// `slots` buffers of `slotBytes` each (rounded up to the page size), allocated and touched up front.
    FrameRing(std::size_t slots, std::size_t slotBytes);
    ~FrameRing();
    FrameRing(const FrameRing&) = delete;
    FrameRing& operator=(const FrameRing&) = delete;

    std::size_t capacity() const { return slots_.size(); }
    std::size_t slotBytes() const { return slotBytes_; }

    // ---- producer side (one thread)
    /// A free slot to fill, or nullopt if all slots are queued or being written.
    std::optional<uint32_t> acquire();
    /// Hand a filled slot to the consumer.
    void publish(uint32_t slot);

    // ---- consumer side (one thread)
    /// The oldest published slot; waits up to `timeout` (returns nullopt on timeout or after close()).
    std::optional<uint32_t> next(std::chrono::milliseconds timeout);
    /// Return a consumed slot to the free list.
    void release(uint32_t slot);

    /// Producer is done: wakes the consumer; next() drains what's queued, then returns nullopt.
    void close();
    bool closed() const { return closed_.load(std::memory_order_acquire); }

    Slot& slot(uint32_t i) { return slots_[i]; }
    /// Slots currently published but not yet consumed.
    std::size_t queued() const;
    /// Most slots ever queued at once (a direct measure of how far the writer fell behind).
    std::size_t highWater() const { return highWater_.load(std::memory_order_relaxed); }

private:
    // Single-producer single-consumer queue of slot indices. Capacity is a power of two >= slots.
    class Spsc {
    public:
        explicit Spsc(std::size_t minCapacity);
        bool push(uint32_t v);                  ///< producer of this queue only
        std::optional<uint32_t> pop();          ///< consumer of this queue only
        std::size_t size() const;
    private:
        std::vector<uint32_t> buf_;
        uint64_t mask_;
        alignas(64) std::atomic<uint64_t> head_{0};   // next to pop
        alignas(64) std::atomic<uint64_t> tail_{0};   // next to push
    };

    std::size_t slotBytes_;
    std::byte* memory_ = nullptr;
    std::size_t memoryBytes_ = 0;
    std::vector<Slot> slots_;
    Spsc free_;     // writer -> capture
    Spsc filled_;   // capture -> writer
    std::atomic<bool> closed_{false};
    std::atomic<std::size_t> highWater_{0};
};

}  // namespace slopcap

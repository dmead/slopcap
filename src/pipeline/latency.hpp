// Lock-free latency histogram: log2 buckets in microseconds, safe to record from several threads.
// Good enough to answer "how bad are the stalls" (p50 / p99 / max) without storing every sample.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>

namespace slopcap {

class LatencyHistogram {
public:
    void record(std::chrono::nanoseconds d) {
        const auto us = static_cast<uint64_t>(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(d).count()));
        const std::size_t b = std::min<std::size_t>(us == 0 ? 0 : static_cast<std::size_t>(std::bit_width(us)), kBuckets - 1);
        buckets_[b].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
        uint64_t m = maxUs_.load(std::memory_order_relaxed);
        while (us > m && !maxUs_.compare_exchange_weak(m, us, std::memory_order_relaxed)) {
        }
    }
    uint64_t count() const { return count_.load(std::memory_order_relaxed); }
    uint64_t maxUs() const { return maxUs_.load(std::memory_order_relaxed); }
    /// Upper bound (µs) of the bucket holding the p-th percentile (0 < p <= 1); within a factor of 2.
    uint64_t percentileUs(double p) const {
        const uint64_t n = count();
        if (n == 0) return 0;
        const auto target = static_cast<uint64_t>(p * static_cast<double>(n) + 0.5);
        uint64_t seen = 0;
        for (std::size_t b = 0; b < kBuckets; ++b) {
            seen += buckets_[b].load(std::memory_order_relaxed);
            if (seen >= target) return std::min<uint64_t>(b == 0 ? 1 : (uint64_t{1} << b), maxUs());
        }
        return maxUs();
    }

private:
    static constexpr std::size_t kBuckets = 40;
    std::array<std::atomic<uint64_t>, kBuckets> buckets_{};
    std::atomic<uint64_t> count_{0};
    std::atomic<uint64_t> maxUs_{0};
};

}  // namespace slopcap

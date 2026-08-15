#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace jamn::core {

// Fixed-capacity, allocation-free histogram over a 5-second sliding window
// of int64 microsecond values - the shape JitterBuffer needs to compute
// playout = clamp(P99(transit) + 3ms, 0, 200ms) (ARCHITECTURE_PLAN.md's
// jitter-buffer section) without ever allocating on the audio thread.
//
// Samples are binned into 64 log-spaced buckets rather than stored
// individually, subdivided across a small number of time slots so samples
// older than the window age out of the P99 query for free, without ever
// tracking a per-sample timestamp.
class Histogram64 {
public:
    static constexpr std::size_t kNumBuckets = 64;
    static constexpr std::size_t kNumSlots = 5;
    static constexpr std::int64_t kWindowUs = 5'000'000;
    static constexpr std::int64_t kSlotDurationUs = kWindowUs / static_cast<std::int64_t>(kNumSlots);
    // Bucket range: 1us to 2s - comfortably above the 200ms playout clamp
    // this feeds, with headroom for a pathological outlier.
    static constexpr double kMinValueUs = 1.0;
    static constexpr double kMaxValueUs = 2'000'000.0;

    Histogram64() { slotStartUs_.fill(kNoSlot); }

    // Records one sample at time nowUs (microseconds, any monotonic
    // timebase the caller uses consistently). Values below kMinValueUs land
    // in bucket 0; values at or above kMaxValueUs land in the last bucket.
    void Record(std::int64_t valueUs, std::int64_t nowUs) {
        const std::size_t slot = SlotIndex(nowUs);
        const std::int64_t slotStart = SlotStart(nowUs);
        if (slotStartUs_[slot] != slotStart) {
            // This slot now represents a different period than last time it
            // was written - either it's fresh, or its old period has aged
            // out and wrapped back around. Either way, its old counts no
            // longer belong to the current window.
            counts_[slot].fill(0);
            slotStartUs_[slot] = slotStart;
        }
        ++counts_[slot][BucketFor(valueUs)];
    }

    // Returns the upper boundary of the bucket containing the 99th
    // percentile of samples recorded within the last 5 seconds of nowUs.
    // Returns 0 if there are no samples in the window.
    std::int64_t P99(std::int64_t nowUs) const { return Percentile(nowUs, 0.99); }

    std::int64_t Percentile(std::int64_t nowUs, double fraction) const {
        std::array<std::uint32_t, kNumBuckets> total{};
        std::uint64_t sum = 0;
        for (std::size_t slot = 0; slot < kNumSlots; ++slot) {
            if (!SlotInWindow(slot, nowUs)) continue;
            for (std::size_t b = 0; b < kNumBuckets; ++b) {
                total[b] += counts_[slot][b];
                sum += counts_[slot][b];
            }
        }
        if (sum == 0) return 0;
        const auto target = static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(sum)));
        std::uint64_t cumulative = 0;
        for (std::size_t b = 0; b < kNumBuckets; ++b) {
            cumulative += total[b];
            if (cumulative >= target) return BucketUpperBoundUs(b);
        }
        return BucketUpperBoundUs(kNumBuckets - 1);
    }

private:
    static constexpr std::int64_t kNoSlot = -1;

    static std::int64_t FloorDiv(std::int64_t a, std::int64_t b) {
        const std::int64_t q = a / b;
        const std::int64_t r = a % b;
        return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
    }

    static std::size_t SlotIndex(std::int64_t nowUs) {
        const std::int64_t period = FloorDiv(nowUs, kSlotDurationUs);
        const auto n = static_cast<std::int64_t>(kNumSlots);
        return static_cast<std::size_t>(((period % n) + n) % n);
    }

    static std::int64_t SlotStart(std::int64_t nowUs) { return FloorDiv(nowUs, kSlotDurationUs) * kSlotDurationUs; }

    bool SlotInWindow(std::size_t slot, std::int64_t nowUs) const {
        const std::int64_t start = slotStartUs_[slot];
        if (start == kNoSlot) return false;
        // A slot's samples stay valid until its period is more than one
        // window old. A slot whose period starts in the future relative to
        // this query (an out-of-order call) is treated as not-yet-valid.
        return start <= nowUs && (nowUs - start) < kWindowUs;
    }

    static double LogMin() { return std::log(kMinValueUs); }
    static double LogMax() { return std::log(kMaxValueUs); }

    static std::size_t BucketFor(std::int64_t valueUs) {
        const double v = std::max<double>(static_cast<double>(valueUs), kMinValueUs);
        if (v >= kMaxValueUs) return kNumBuckets - 1;
        const double t = (std::log(v) - LogMin()) / (LogMax() - LogMin());
        const auto idx = static_cast<std::size_t>(t * static_cast<double>(kNumBuckets));
        return std::min(idx, kNumBuckets - 1);
    }

    static std::int64_t BucketUpperBoundUs(std::size_t bucket) {
        const double t = static_cast<double>(bucket + 1) / static_cast<double>(kNumBuckets);
        const double v = std::exp(LogMin() + t * (LogMax() - LogMin()));
        return static_cast<std::int64_t>(v);
    }

    std::array<std::array<std::uint32_t, kNumBuckets>, kNumSlots> counts_{};
    std::array<std::int64_t, kNumSlots> slotStartUs_{};
};

}  // namespace jamn::core

#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace jamn::engine {

// One raw NTP-style ping/pong exchange. t1/t4 are local clock readings;
// t2/t3 are the remote peer's own clock readings, reported back over the
// wire (docs/CLOCK.md's "Clock 1: peer offset").
struct ClockSyncSample {
    std::int64_t t1 = 0;  // Local: ping sent.
    std::int64_t t2 = 0;  // Remote: ping received.
    std::int64_t t3 = 0;  // Remote: pong sent.
    std::int64_t t4 = 0;  // Local: pong received.

    std::int64_t Rtt() const { return (t4 - t1) - (t3 - t2); }
    std::int64_t Offset() const { return ((t2 - t1) + (t3 - t4)) / 2; }
};

// Min-RTT windowed NTP-style offset estimation for one peer, locked then
// slewed (never stepped once locked), with a feed-forward skew term and a
// re-lock escape hatch for a large discontinuity. This class only ever
// touches time - it has no notion of "notes"; SetReLockCallback is how the
// caller (ultimately EventScheduler) hooks in whatever "flush everything
// held" means.
class ClockSync {
public:
    static constexpr std::size_t kWindowSize = 64;
    static constexpr std::size_t kLockThreshold = 8;
    static constexpr std::int64_t kMaxSlewPpm = 500;
    static constexpr std::int64_t kReLockThresholdUs = 250'000;
    static constexpr std::int64_t kManualOffsetLimitUs = 50'000;

    // Ping cadence: 4Hz for the first 5 seconds of a session, then 1Hz.
    static constexpr std::int64_t kFastPeriodUs = 250'000;
    static constexpr std::int64_t kSlowPeriodUs = 1'000'000;
    static constexpr std::int64_t kFastPhaseDurationUs = 5'000'000;

    // Invoked synchronously from AddSample(), the instant a re-lock
    // triggers - before the sample that triggered it is folded into the
    // (now reset) window. The caller owns held-note state and performs the
    // actual flush; see docs/CLOCK.md: "a stuck note is worse than a
    // dropped one."
    using ReLockCallback = std::function<void()>;
    void SetReLockCallback(ReLockCallback callback) { reLockCallback_ = std::move(callback); }

    // Whether to send another ping right now, given the 4Hz/5s-then-1Hz
    // cadence. ClockSync never touches ITransport itself - the caller
    // drives the actual send and later calls AddSample() with the result.
    static bool ShouldPing(std::int64_t nowUs, std::int64_t sessionStartUs, std::int64_t lastPingUs);

    // Folds one completed round-trip into the window. nowUs is the local
    // time at which this sample is being processed (normally == sample.t4).
    void AddSample(const ClockSyncSample& sample, std::int64_t nowUs);

    bool IsLocked() const { return sampleCount_ >= kLockThreshold; }
    std::size_t SampleCount() const { return sampleCount_; }

    // The current best offset estimate in microseconds (remote clock minus
    // local clock, algorithmic estimate only - before the manual
    // adjustment below). Meaningless before IsLocked().
    std::int64_t AlgorithmicOffsetUs() const { return slewedOffsetUs_; }

    // AlgorithmicOffsetUs() plus the manual per-peer adjustment - what a
    // caller actually converts a remote SessionTime through.
    std::int64_t EstimatedOffsetUs() const { return slewedOffsetUs_ + manualOffsetUs_; }

    // Compensates the known-unfixable half-path-asymmetry bias
    // (docs/CLOCK.md) that no algorithm can measure from the endpoints -
    // clamped to +-kManualOffsetLimitUs.
    void SetManualOffsetUs(std::int64_t offsetUs);
    std::int64_t ManualOffsetUs() const { return manualOffsetUs_; }

    std::int64_t SkewPpm() const { return skewPpm_; }

private:
    struct WindowEntry {
        ClockSyncSample sample;
        std::int64_t localTimeUs = 0;
        std::uint64_t seq = 0;  // Insertion order, for recency tie-breaks.
    };

    // Fits offset(localTime) as a line over the current window via
    // ordinary least squares; returns the slope in ppm (0 if fewer than 2
    // samples or no time spread to fit against).
    std::int64_t FitSkewPpm() const;

    // The offset of whichever sample in the window has the lowest RTT -
    // the "min-RTT window, not averaging" estimate docs/CLOCK.md specifies.
    // Ties (e.g. a link with genuinely constant RTT) favor the most
    // recently inserted sample, not the oldest - otherwise a single early
    // low-RTT sample would pin the estimate forever and the window would
    // never notice a real offset change on a stable-latency link.
    std::int64_t MinRttOffsetUs() const;

    void ResetWindow();

    std::array<WindowEntry, kWindowSize> window_{};
    std::size_t windowNext_ = 0;
    std::size_t sampleCount_ = 0;
    std::uint64_t nextSeq_ = 0;

    std::int64_t slewedOffsetUs_ = 0;
    std::int64_t skewPpm_ = 0;
    std::int64_t manualOffsetUs_ = 0;
    std::int64_t lastUpdateUs_ = 0;
    bool haveLastUpdate_ = false;

    ReLockCallback reLockCallback_;
};

}  // namespace jamn::engine

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <set>

#include "jamn_engine/clock_sync.h"

using jamn::engine::ClockSync;
using jamn::engine::ClockSyncSample;

namespace {
// Builds a synthetic NTP-style sample for a ping sent at local real-time r1,
// given one-way forward/backward network delays and the remote clock's
// offset from true/real time at the moment of the exchange. Zero
// processing delay on the remote side (t3 == t2) - this is the model the
// classic offset = ((t2-t1)+(t3-t4))/2 formula, and its
// offset = trueOffset + (dForward-dBackward)/2 asymmetry-bias consequence,
// were verified against by hand before writing these tests.
ClockSyncSample MakeSample(std::int64_t r1, std::int64_t dForward, std::int64_t dBackward,
                            std::int64_t trueOffset) {
    ClockSyncSample s;
    s.t1 = r1;
    s.t2 = r1 + dForward + trueOffset;
    s.t3 = s.t2;
    s.t4 = r1 + dForward + dBackward;
    return s;
}
}  // namespace

TEST_CASE("ClockSync is not locked before kLockThreshold samples", "[engine][clock_sync][fast]") {
    ClockSync sync;
    std::int64_t t = 0;
    for (std::size_t i = 0; i < ClockSync::kLockThreshold - 1; ++i) {
        sync.AddSample(MakeSample(t, 5000, 5000, 1000), t);
        t += 250'000;
        REQUIRE_FALSE(sync.IsLocked());
    }
    sync.AddSample(MakeSample(t, 5000, 5000, 1000), t);
    REQUIRE(sync.IsLocked());
    REQUIRE(sync.SampleCount() == ClockSync::kLockThreshold);
}

TEST_CASE("ClockSync: synthetic offset - a symmetric-path peer converges to the true offset",
          "[engine][clock_sync][fast]") {
    ClockSync sync;
    const std::int64_t trueOffset = 75'000;
    std::int64_t t = 0;
    for (int i = 0; i < 12; ++i) {
        sync.AddSample(MakeSample(t, 5000, 5000, trueOffset), t);
        t += 250'000;
    }
    REQUIRE(sync.IsLocked());
    REQUIRE(sync.EstimatedOffsetUs() == trueOffset);
}

TEST_CASE("ClockSync: min-RTT window picks the single lowest-RTT sample's offset, not an average",
          "[engine][clock_sync][fast]") {
    ClockSync sync;
    std::int64_t t = 0;
    // 6 noisy, high-RTT, asymmetric samples - every one biased the same
    // way. Still below kLockThreshold, so each AddSample tracks its
    // estimate directly (no slew yet), keeping this test's arithmetic
    // exact rather than dependent on slew-rate timing.
    const std::int64_t noisyOffset = 75'000 - 10'000;  // trueOffset + (dF-dB)/2 with dF=5000, dB=25000.
    for (int i = 0; i < 6; ++i) {
        sync.AddSample(MakeSample(t, 5000, 25000, 75'000), t);
        t += 250'000;
        REQUIRE(sync.AlgorithmicOffsetUs() == noisyOffset);
    }
    REQUIRE_FALSE(sync.IsLocked());

    // One clean, low-RTT, symmetric sample - its RTT (2000us) is far below
    // the noisy samples' (30000us), so it alone should become the target,
    // not something averaged between 75000 and the noisy 65000.
    sync.AddSample(MakeSample(t, 1000, 1000, 75'000), t);
    REQUIRE_FALSE(sync.IsLocked());  // Still only 7 samples.
    REQUIRE(sync.AlgorithmicOffsetUs() == 75'000);
}

TEST_CASE("ClockSync: jitter - stays close to the true offset despite noisy high-RTT samples",
          "[engine][clock_sync][fast]") {
    ClockSync sync;
    const std::int64_t trueOffset = 20'000;
    std::int64_t t = 0;
    // Alternate a noisy, asymmetric, high-RTT sample with a clean,
    // symmetric, low-RTT one. A naive average across all of them would sit
    // roughly halfway between trueOffset and the noisy bias; min-RTT
    // selection should stay pinned near trueOffset throughout.
    for (int i = 0; i < 16; ++i) {
        if (i % 2 == 0) {
            sync.AddSample(MakeSample(t, 8000, 40000, trueOffset), t);  // rtt=48000, biased low.
        } else {
            sync.AddSample(MakeSample(t, 1000, 1000, trueOffset), t);  // rtt=2000, unbiased.
        }
        t += 250'000;
    }
    REQUIRE(sync.IsLocked());
    const std::int64_t error = sync.EstimatedOffsetUs() - trueOffset;
    REQUIRE(std::abs(error) < 2000);
}

TEST_CASE("ClockSync: skew - tracks a linearly drifting remote clock via feed-forward",
          "[engine][clock_sync][fast]") {
    ClockSync sync;
    const std::int64_t trueOffsetAtZero = 10'000;
    const std::int64_t driftPpm = 200;  // Remote clock gains 200us per second of local elapsed time.
    std::int64_t t = 0;
    for (int i = 0; i < 40; ++i) {
        const std::int64_t driftedOffset = trueOffsetAtZero + (driftPpm * t) / 1'000'000;
        sync.AddSample(MakeSample(t, 5000, 5000, driftedOffset), t);
        t += 250'000;
    }
    REQUIRE(sync.IsLocked());

    const std::int64_t expectedFinalOffset = trueOffsetAtZero + (driftPpm * t) / 1'000'000;
    const std::int64_t error = sync.EstimatedOffsetUs() - expectedFinalOffset;
    // A windowed least-squares fit weights every historical sample
    // equally, so it lags the true instantaneous rate somewhat - this
    // tolerance reflects that inherent lag, not slack for a bug.
    REQUIRE(std::abs(error) < 3000);
    // Proves it actually tracked the drift upward, not stayed pinned near
    // the initial value.
    REQUIRE(sync.EstimatedOffsetUs() > trueOffsetAtZero + 1000);
    REQUIRE(std::abs(sync.SkewPpm() - driftPpm) < 100);
}

TEST_CASE("ClockSync: asymmetry - the measured offset carries the known, unfixable half-path bias, "
          "and the manual slider compensates it",
          "[engine][clock_sync][fast]") {
    ClockSync sync;
    const std::int64_t trueOffset = 50'000;
    const std::int64_t dForward = 5000;
    const std::int64_t dBackward = 25000;
    const std::int64_t expectedBias = (dForward - dBackward) / 2;  // -10000
    std::int64_t t = 0;
    for (int i = 0; i < 12; ++i) {
        sync.AddSample(MakeSample(t, dForward, dBackward, trueOffset), t);
        t += 250'000;
    }
    REQUIRE(sync.IsLocked());
    REQUIRE(sync.AlgorithmicOffsetUs() == trueOffset + expectedBias);

    // No algorithm can recover from endpoint measurements alone - only a
    // manual correction can. Set it to exactly cancel the known bias.
    sync.SetManualOffsetUs(-expectedBias);
    REQUIRE(sync.EstimatedOffsetUs() == trueOffset);
}

TEST_CASE("ClockSync re-locks on an offset error above 250ms and flushes held notes at that moment",
          "[engine][clock_sync][fast]") {
    ClockSync sync;
    std::set<int> heldNotes{1, 2, 3};
    sync.SetReLockCallback([&]() { heldNotes.clear(); });

    std::int64_t t = 0;
    for (int i = 0; i < 10; ++i) {
        sync.AddSample(MakeSample(t, 5000, 5000, 1000), t);
        t += 250'000;
    }
    REQUIRE(sync.IsLocked());
    REQUIRE_FALSE(heldNotes.empty());

    // A discontinuity far larger than the 250ms re-lock threshold - a
    // laptop waking from sleep, a host restarting.
    sync.AddSample(MakeSample(t, 5000, 5000, 2'000'000), t);

    REQUIRE(heldNotes.empty());
    REQUIRE_FALSE(sync.IsLocked());  // Window was discarded and restarted from this one sample.
    REQUIRE(sync.SampleCount() == 1);
}

TEST_CASE("ClockSync does not re-lock on an offset error within the 250ms threshold", "[engine][clock_sync][fast]") {
    ClockSync sync;
    bool reLocked = false;
    sync.SetReLockCallback([&]() { reLocked = true; });

    std::int64_t t = 0;
    for (int i = 0; i < 10; ++i) {
        sync.AddSample(MakeSample(t, 5000, 5000, 1000), t);
        t += 250'000;
    }
    REQUIRE(sync.IsLocked());

    // 200ms jump - under the 250ms threshold.
    sync.AddSample(MakeSample(t, 5000, 5000, 200'000), t);
    REQUIRE_FALSE(reLocked);
    REQUIRE(sync.IsLocked());
}

TEST_CASE("ClockSync::SetManualOffsetUs clamps to +-50ms", "[engine][clock_sync][fast]") {
    ClockSync sync;
    sync.SetManualOffsetUs(999'999);
    REQUIRE(sync.ManualOffsetUs() == ClockSync::kManualOffsetLimitUs);
    sync.SetManualOffsetUs(-999'999);
    REQUIRE(sync.ManualOffsetUs() == -ClockSync::kManualOffsetLimitUs);
    sync.SetManualOffsetUs(1234);
    REQUIRE(sync.ManualOffsetUs() == 1234);
}

TEST_CASE("ClockSync::ShouldPing follows the 4Hz-for-5s-then-1Hz cadence", "[engine][clock_sync][fast]") {
    const std::int64_t sessionStart = 0;

    // Within the fast phase: due again after 250ms, not before.
    REQUIRE_FALSE(ClockSync::ShouldPing(1'000'000 + 100'000, sessionStart, 1'000'000));
    REQUIRE(ClockSync::ShouldPing(1'000'000 + 250'000, sessionStart, 1'000'000));

    // Past the 5s fast phase: due again after 1s, not at the old 250ms cadence.
    REQUIRE_FALSE(ClockSync::ShouldPing(5'000'000 + 500'000, sessionStart, 5'000'000));
    REQUIRE(ClockSync::ShouldPing(5'000'000 + 1'000'000, sessionStart, 5'000'000));
}

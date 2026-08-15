// T5.2: clock accuracy under virtual time (PHASE_0_5_PLAN.md's criterion
// #1, 0.5a reading). Acceptance criterion #1 itself ("clock offset error
// p99 < 500us over 30 minutes, host and client on one box") needs real
// sockets to run as a wall-clock two-process test, which is out of 0.5a's
// "no ENet, no audio device, no GUI" fence - this runs the same 30 minutes
// in *virtual* time instead, under SimClock, with true offset fixed at
// zero. Zero is the only configuration where ground truth is actually
// knowable: with an asymmetric path there is a real, unmeasurable bias
// (T4.1's asymmetry test), so a nonzero true offset could never be
// verified against here. Path delay/jitter is still modelled (forward and
// backward delays drawn independently, zero-mean around a common base),
// exercising the full min-RTT-window noise-filtering pipeline under
// realistic per-sample asymmetry that averages out only over many
// samples - it does not model actual clock frequency drift, which would
// make "true offset fixed at zero" untrue over 30 minutes.
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

#include "jamn_engine/clock_sync.h"

using jamn::engine::ClockSync;
using jamn::engine::ClockSyncSample;

namespace {

std::int64_t Percentile(std::vector<std::int64_t> values, double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto index =
        std::min(values.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(values.size())));
    return values[index];
}

}  // namespace

TEST_CASE("clock offset p99 stays under 500us over 30 virtual minutes at zero true offset",
          "[engine][clock_sync][fast]") {
    ClockSync sync;
    std::mt19937_64 rng(0x5EED'C10C'C10C'5EEDULL);
    // The real criterion #1 this substitutes for runs host and client on
    // one box (loopback), not over a real network - so this models a
    // loopback-scale round trip (sub-millisecond base, small jitter), not
    // LAN- or WAN-scale delay. Criterion #3 (two real machines on a LAN,
    // p99 < 30ms) is the far more lenient, network-scale reading; this
    // isn't it.
    std::uniform_int_distribution<std::int64_t> jitterDist(-100, 100);  // +-100us around a 500us base, per direction.
    constexpr std::int64_t kBaseDelayUs = 500;

    constexpr std::int64_t kSessionStartUs = 0;
    constexpr std::int64_t kDurationUs = 30 * 60 * 1'000'000LL;  // 30 virtual minutes.

    std::vector<std::int64_t> absErrorsUs;
    std::int64_t nowUs = 0;
    std::int64_t lastPingUs = -std::numeric_limits<std::int64_t>::max() / 2;

    while (nowUs < kDurationUs) {
        if (ClockSync::ShouldPing(nowUs, kSessionStartUs, lastPingUs)) {
            const std::int64_t dForward = kBaseDelayUs + jitterDist(rng);
            const std::int64_t dBackward = kBaseDelayUs + jitterDist(rng);

            ClockSyncSample sample;
            sample.t1 = nowUs;
            sample.t2 = nowUs + dForward;  // trueOffset = 0, so remote clock reads == real time.
            sample.t3 = sample.t2;
            sample.t4 = nowUs + dForward + dBackward;

            sync.AddSample(sample, sample.t4);
            lastPingUs = nowUs;

            if (sync.IsLocked()) {
                absErrorsUs.push_back(std::abs(sync.EstimatedOffsetUs()));
            }
        }
        nowUs += 50'000;  // 50ms simulation step - finer than the 1Hz slow-phase cadence.
    }

    REQUIRE(sync.IsLocked());
    REQUIRE(absErrorsUs.size() > 100);

    const std::int64_t p99 = Percentile(absErrorsUs, 0.99);
    // Printed unconditionally, not just on failure - T5.2's accept
    // criterion is that the measured value is visible, not just pass/fail.
    std::printf("Clock offset error p99 over 30 virtual minutes: %lld us (%zu samples)\n",
                static_cast<long long>(p99), absErrorsUs.size());

    REQUIRE(p99 < 500);
}

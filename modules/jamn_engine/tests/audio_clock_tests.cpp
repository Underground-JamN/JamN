// Every TEST_CASE name here contains "audio_clock", not only the tag.
// `ctest -R` matches test names and never tags, and exits 0 on an empty
// selection - the trap that made T3.3's accept command silently select
// nothing. The accept command is:
//
//     ctest --preset core-only -R 'audio_clock'   -> 11 tests
//
// What cannot be tested here is the real device feed: /dev/snd does not
// exist inside a sandboxed session. The DLL is pure arithmetic, so
// everything except "the numbers arriving are the ones the hardware
// produced" is covered off-device. That last part is T5.1's maintainer
// acceptance.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "jamn_core/realtime_scope.h"
#include "jamn_engine/audio_clock.h"

using jamn::core::SampleTime;
using jamn::engine::AudioClock;

namespace {

constexpr double kNominalRate = 48'000.0;
constexpr int kBlockFrames = 128;

// A synthetic callback stream. trueRate is what the device's crystal
// actually runs at, which is deliberately not the rate the driver reports
// - the whole reason Clock 2 exists. jitterNs is added to each arrival
// time and removed again from the next, so it perturbs when a callback is
// seen without changing the underlying rate, which is what real scheduling
// jitter does.
struct Block {
    SampleTime samples;
    std::int64_t steadyNs;
};

std::vector<Block> MakeStream(double trueRate, int blocks, std::int64_t jitterNs = 0, std::int64_t startNs = 1'000'000'000) {
    std::vector<Block> stream;
    stream.reserve(static_cast<std::size_t>(blocks));
    // A cheap deterministic pseudo-jitter - a real device's is not random
    // in any useful sense either, and a fixed sequence keeps the test from
    // being flaky.
    std::int64_t noise = 0;
    for (int index = 0; index < blocks; ++index) {
        const std::int64_t frames = static_cast<std::int64_t>(index) * kBlockFrames;
        const double seconds = static_cast<double>(frames) / trueRate;
        std::int64_t ns = startNs + static_cast<std::int64_t>(std::llround(seconds * 1e9));
        if (jitterNs > 0) {
            noise = (noise * 1103515245 + 12345) % 2147483647;
            ns += (noise % (2 * jitterNs)) - jitterNs;
        }
        stream.push_back({SampleTime(frames), ns});
    }
    return stream;
}

// Blocks needed to cover a given number of seconds at the nominal rate.
int BlocksFor(double seconds) {
    return static_cast<int>(seconds * kNominalRate / kBlockFrames);
}

}  // namespace

TEST_CASE("audio_clock reports the nominal rate before it has seen anything", "[audio_clock]") {
    AudioClock clock;
    clock.Prepare(kNominalRate, kBlockFrames);

    REQUIRE_FALSE(clock.IsLocked());
    REQUIRE(clock.UpdateCount() == 0);
    REQUIRE(clock.EstimatedSampleRate() == kNominalRate);
    // No mapping exists yet, and saying so with a defined value beats
    // asserting on the audio thread.
    REQUIRE(clock.SamplePositionAt(123'456).samples() == 0);
    REQUIRE(clock.LocalUsAt(SampleTime(4096)) == 0);
}

TEST_CASE("audio_clock converges on a rate the driver reported wrong", "[audio_clock]") {
    // 1000ppm high - far more than a real crystal, chosen so a filter that
    // did nothing at all could not pass by accident.
    const double trueRate = kNominalRate * 1.001;

    AudioClock clock;
    clock.Prepare(kNominalRate, kBlockFrames);
    for (const Block& block : MakeStream(trueRate, BlocksFor(60.0))) {
        clock.Update(block.samples, block.steadyNs);
    }

    REQUIRE(clock.IsLocked());
    const double errorPpm = (clock.EstimatedSampleRate() / trueRate - 1.0) * 1e6;
    REQUIRE(std::fabs(errorPpm) < 20.0);
    // And it says so in the unit the drift is actually discussed in.
    REQUIRE(clock.DriftPpm() > 900.0);
    REQUIRE(clock.DriftPpm() < 1100.0);
}

TEST_CASE("audio_clock converges through callback jitter", "[audio_clock]") {
    // The case the DLL exists for. Without jitter any estimator passes;
    // 200us is a large but not absurd perturbation on a 2667us block.
    const double trueRate = kNominalRate * 0.9997;

    AudioClock clock;
    clock.Prepare(kNominalRate, kBlockFrames);
    for (const Block& block : MakeStream(trueRate, BlocksFor(60.0), /*jitterNs=*/200'000)) {
        clock.Update(block.samples, block.steadyNs);
    }

    REQUIRE(clock.IsLocked());
    const double errorPpm = (clock.EstimatedSampleRate() / trueRate - 1.0) * 1e6;
    REQUIRE(std::fabs(errorPpm) < 50.0);
}

TEST_CASE("audio_clock never steps its sample timeline", "[audio_clock]") {
    // "Never steps" operationally: the position the mapping reports for
    // each block's own arrival advances monotonically and by close to one
    // block, every single update. A discontinuity here is what a stuck or
    // double-triggered note comes from, so the bound is tight and applies
    // from the very first update, not after some settling period.
    AudioClock clock;
    clock.Prepare(kNominalRate, kBlockFrames);

    std::int64_t previous = 0;
    bool havePrevious = false;
    for (const Block& block : MakeStream(kNominalRate * 1.0005, BlocksFor(30.0), /*jitterNs=*/150'000)) {
        clock.Update(block.samples, block.steadyNs);
        const std::int64_t position = clock.SamplePositionAt(block.steadyNs / 1000).samples();
        if (havePrevious) {
            const std::int64_t advance = position - previous;
            REQUIRE(advance > 0);
            // One block, plus what the jitter and the microsecond
            // resolution of the query can account for. Nowhere near the
            // multi-block jump a step would produce.
            REQUIRE(advance < 2 * kBlockFrames);
        }
        previous = position;
        havePrevious = true;
    }
}

TEST_CASE("audio_clock absorbs a repeated or backwards callback timestamp", "[audio_clock]") {
    // A driver can deliver two callbacks close enough together to carry the
    // same timestamp, and a coarse platform clock can even hand back one
    // that went backwards. Neither is allowed to break the headline claim:
    // the loop takes it as an ordinary negative error and the sample
    // timeline still only ever moves forward.
    AudioClock clock;
    clock.Prepare(kNominalRate, kBlockFrames);

    auto stream = MakeStream(kNominalRate, BlocksFor(20.0));
    for (std::size_t index = 20; index + 1 < stream.size(); index += 37) {
        // Repeat the previous stamp, then push one 100us backwards.
        stream[index].steadyNs = stream[index - 1].steadyNs;
        stream[index + 1].steadyNs = stream[index].steadyNs - 100'000;
    }

    std::int64_t previous = 0;
    bool havePrevious = false;
    for (const Block& block : stream) {
        clock.Update(block.samples, block.steadyNs);
        const std::int64_t position = clock.SamplePositionAt(block.steadyNs / 1000).samples();
        if (havePrevious) REQUIRE(position >= previous - kBlockFrames);
        previous = position;
        havePrevious = true;
    }

    REQUIRE(clock.IsLocked());
    // The rate estimate survives: these are perturbations of when a
    // callback was *seen*, not of how fast the crystal runs.
    REQUIRE(std::fabs(clock.DriftPpm()) < 500.0);
}

TEST_CASE("audio_clock tracks a resampling server's bimodal cadence", "[audio_clock]") {
    // The cadence a real device actually produced, which none of the
    // streams above resemble. PipeWire runs its graph at 48kHz and hands a
    // 44.1kHz stream nothing on some cycles, so intervals are not jittered
    // around a mean - they are bimodal, one graph quantum or exactly two,
    // with the doubles making up 1/0.91875 - 1 = 8.84% of intervals.
    //
    // **A known bias lives here, deliberately pinned rather than fixed.**
    // The loop settles about 164ppm below the true mean rate on this
    // input, where it is within a few ppm on a jittered one. 164ppm is
    // 50us of placement error over a 30ms playout horizon - inaudible, and
    // well inside what the scheduler quantises away at block granularity -
    // so this asserts the behaviour rather than chasing it. What it must
    // not do is drift, and the bound below is tight enough to catch that.
    constexpr double kGraphRate = 48'000.0;
    constexpr double kStreamRate = 44'100.0;
    const double quantumNs = kBlockFrames * 4.0 / kGraphRate * 1e9;  // 512 frames.

    AudioClock clock;
    clock.Prepare(kStreamRate, kBlockFrames * 4);

    std::int64_t ns = 1'000'000'000;
    std::int64_t frames = 0;
    double credit = 0.0;
    const std::int64_t firstNs = ns;
    std::int64_t lastNs = ns;
    constexpr int kUpdates = 4000;

    for (int index = 0; index < kUpdates; ++index) {
        clock.Update(SampleTime(frames), ns);
        lastNs = ns;
        frames += kBlockFrames * 4;
        ns += static_cast<std::int64_t>(quantumNs);
        credit += (1.0 / (kStreamRate / kGraphRate)) - 1.0;
        if (credit >= 1.0) {
            credit -= 1.0;
            ns += static_cast<std::int64_t>(quantumNs);
        }
    }

    REQUIRE(clock.IsLocked());
    const double meanIntervalSeconds = static_cast<double>(lastNs - firstNs) / 1e9 / (kUpdates - 1);
    const double trueRate = (kBlockFrames * 4) / meanIntervalSeconds;
    const double errorPpm = (clock.EstimatedSampleRate() / trueRate - 1.0) * 1e6;

    // Biased low, and by how much - not merely "close". A regression that
    // widened this would otherwise pass a one-sided tolerance.
    REQUIRE(errorPpm < 0.0);
    REQUIRE(errorPpm > -400.0);
}

TEST_CASE("audio_clock does not lock before its estimate is worth using", "[audio_clock]") {
    AudioClock clock;
    clock.Prepare(kNominalRate, kBlockFrames);

    const auto stream = MakeStream(kNominalRate * 1.001, BlocksFor(60.0));
    std::size_t index = 0;
    for (; index < stream.size(); ++index) {
        clock.Update(stream[index].samples, stream[index].steadyNs);
        if (clock.IsLocked()) break;
    }

    REQUIRE(clock.IsLocked());
    // Four time constants at 0.1Hz is ~6.4s. Assert the shape rather than
    // the exact figure: it must not claim a lock in the first second, and
    // must not take a quarter of a minute to get there.
    const double lockedAfterSeconds = static_cast<double>(index) * kBlockFrames / kNominalRate;
    REQUIRE(lockedAfterSeconds > 1.0);
    REQUIRE(lockedAfterSeconds < 15.0);

    // And by the time it says so, the estimate has actually arrived.
    const double errorPpm = (clock.EstimatedSampleRate() / (kNominalRate * 1.001) - 1.0) * 1e6;
    REQUIRE(std::fabs(errorPpm) < 100.0);
}

TEST_CASE("audio_clock maps time to samples and back again", "[audio_clock]") {
    const double trueRate = kNominalRate * 1.0002;

    AudioClock clock;
    clock.Prepare(kNominalRate, kBlockFrames);
    const auto stream = MakeStream(trueRate, BlocksFor(30.0));
    for (const Block& block : stream) {
        clock.Update(block.samples, block.steadyNs);
    }

    // The round trip goes through an integer sample position, and a sample
    // is 20.8us at this rate - so a sample period, not a microsecond, is
    // the tightest this can be. Asserting 1us here would be asserting a
    // resolution the sample axis does not have.
    const std::int64_t samplePeriodUs = static_cast<std::int64_t>(1e6 / trueRate) + 1;
    const std::int64_t lastUs = stream.back().steadyNs / 1000;
    for (const std::int64_t aheadUs : {-50'000, -1'000, 0, 1'000, 50'000}) {
        const std::int64_t queryUs = lastUs + aheadUs;
        const SampleTime position = clock.SamplePositionAt(queryUs);
        REQUIRE(std::llabs(clock.LocalUsAt(position) - queryUs) <= samplePeriodUs);
    }

    // Extrapolation runs both ways past the last block, which is what the
    // scheduler needs: it asks about events a jitter-buffer depth ahead,
    // always past the most recent callback.
    REQUIRE(clock.SamplePositionAt(lastUs + 100'000).samples() > clock.SamplePositionAt(lastUs).samples());
    REQUIRE(clock.SamplePositionAt(lastUs - 100'000).samples() < clock.SamplePositionAt(lastUs).samples());
}

TEST_CASE("audio_clock starts over when the device restarts", "[audio_clock]") {
    AudioClock clock;
    clock.Prepare(kNominalRate, kBlockFrames);
    for (const Block& block : MakeStream(kNominalRate * 1.001, BlocksFor(30.0))) {
        clock.Update(block.samples, block.steadyNs);
    }
    REQUIRE(clock.IsLocked());

    // A restart at a different rate and a different block size, with the
    // sample counter back at zero and a wall-clock gap - exactly what a
    // device stop/start looks like. Carrying the old loop across that gap
    // is the one step this class must never take.
    clock.Prepare(44'100.0, 256);
    REQUIRE_FALSE(clock.IsLocked());
    REQUIRE(clock.UpdateCount() == 0);
    REQUIRE(clock.EstimatedSampleRate() == 44'100.0);
    REQUIRE(clock.DriftPpm() == 0.0);
}

TEST_CASE("audio_clock stays inert when the device reports nothing usable", "[audio_clock]") {
    AudioClock clock;
    clock.Prepare(0.0, 0);

    clock.Update(SampleTime(0), 1'000'000'000);
    clock.Update(SampleTime(128), 1'002'666'667);

    REQUIRE_FALSE(clock.IsLocked());
    REQUIRE(clock.UpdateCount() == 0);
    REQUIRE(clock.DriftPpm() == 0.0);
    REQUIRE(clock.SamplePositionAt(1'000'000).samples() == 0);
}

TEST_CASE("audio_clock is safe to drive from a realtime scope", "[audio_clock]") {
    AudioClock clock;
    clock.Prepare(kNominalRate, kBlockFrames);
    const auto stream = MakeStream(kNominalRate * 1.001, BlocksFor(1.0));

    // Prepare is the audioDeviceAboutToStart moment and is not covered
    // here; Update and both queries are the per-block path and are.
    jamn::core::RealtimeScope scope;
    for (const Block& block : stream) {
        clock.Update(block.samples, block.steadyNs);
        (void)clock.SamplePositionAt(block.steadyNs / 1000);
        (void)clock.LocalUsAt(block.samples);
    }
    REQUIRE(clock.UpdateCount() == stream.size());
}

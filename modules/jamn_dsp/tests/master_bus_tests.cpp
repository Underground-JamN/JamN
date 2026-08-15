#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "jamn_core/file_audio_device.h"
#include "jamn_core/realtime_scope.h"
#include "jamn_dsp/master_bus.h"

using jamn::dsp::MasterBus;

TEST_CASE("MasterBus passes audio through unchanged at its default unity gain", "[dsp][master_bus][fast]") {
    MasterBus bus;
    bus.Prepare(48000.0);

    const int numFrames = 128;
    std::vector<float> channel(numFrames, 1.0f);
    float* channels[] = {channel.data()};

    bus.Process(channels, 1, numFrames);

    for (int frame = 0; frame < numFrames; ++frame) {
        REQUIRE(channel[frame] == 1.0f);
    }
}

TEST_CASE("MasterBus ramps toward a new gain target instead of stepping to it", "[dsp][master_bus][fast]") {
    MasterBus bus;
    bus.Prepare(48000.0);
    bus.SetGain(0.0f);

    const int numFrames = 128;
    std::vector<float> channel(numFrames, 1.0f);
    float* channels[] = {channel.data()};

    bus.Process(channels, 1, numFrames);

    REQUIRE(channel[0] > 0.99f);
    for (int frame = 1; frame < numFrames; ++frame) {
        REQUIRE(std::fabs(channel[frame] - channel[frame - 1]) < 0.01f);
    }
}

TEST_CASE("MasterBus arrives at its gain target after enough blocks", "[dsp][master_bus][fast]") {
    MasterBus bus;
    bus.Prepare(48000.0);
    bus.SetGain(0.5f);

    const int numFrames = 128;
    std::vector<float> channel(numFrames);
    float* channels[] = {channel.data()};

    for (int block = 0; block < 100; ++block) {
        for (int frame = 0; frame < numFrames; ++frame) {
            channel[frame] = 1.0f;
        }
        bus.Process(channels, 1, numFrames);
    }

    REQUIRE(std::fabs(bus.currentGain() - 0.5f) < 1.0e-3f);
}

TEST_CASE("MasterBus applies the same gain to every channel in a block", "[dsp][master_bus][fast]") {
    MasterBus bus;
    bus.Prepare(48000.0);
    bus.SetGain(0.5f);

    const int numFrames = 64;
    std::vector<float> left(numFrames, 1.0f);
    std::vector<float> right(numFrames, 1.0f);
    float* channels[] = {left.data(), right.data()};

    bus.Process(channels, 2, numFrames);

    for (int frame = 0; frame < numFrames; ++frame) {
        REQUIRE(left[frame] == right[frame]);
    }
}

TEST_CASE("MasterBus gain is publishable without a lock", "[dsp][master_bus][fast]") {
    REQUIRE(std::atomic<float>::is_always_lock_free);
}

// --- Limiter ---------------------------------------------------------------
//
// The limiter has no lookahead, because docs/CLOCK.md forbids delaying a
// player's own monitoring. So the smoothed gain reduction is always at
// least one sample late and the hard clamp behind it is what actually
// guarantees the ceiling - the cases below are split along exactly that
// line: what the clamp guarantees, and what the envelope then does so the
// clamp is not working continuously.

namespace {

constexpr double kSampleRate = 48000.0;

struct Signal {
    Signal(int numFrames, int numChannels, float value)
        : channels(static_cast<std::size_t>(numChannels),
                   std::vector<float>(numFrames, value)) {
        for (auto& channel : channels) pointers.push_back(channel.data());
    }

    void Refill(float value) {
        for (auto& channel : channels) std::fill(channel.begin(), channel.end(), value);
    }

    float* const* data() { return pointers.data(); }

    float Peak() const {
        float peak = 0.0f;
        for (const auto& channel : channels) {
            for (float sample : channel) peak = std::max(peak, std::fabs(sample));
        }
        return peak;
    }

    std::vector<std::vector<float>> channels;
    std::vector<float*> pointers;
};

}  // namespace

TEST_CASE("The limiter holds a full mix of peers at or below 0dBFS", "[dsp][master_bus][limiter][fast]") {
    MasterBus bus;
    bus.Prepare(kSampleRate);

    // What PeerMixer actually hands over with every strip driven at full
    // scale: kMaxPeers summed, well past the ceiling.
    const float fullMix = 8.0f;
    const int numFrames = 128;
    Signal signal(numFrames, 2, fullMix);

    for (int block = 0; block < 64; ++block) {
        signal.Refill(fullMix);
        bus.Process(signal.data(), 2, numFrames);
        REQUIRE(signal.Peak() <= MasterBus::kCeiling);
    }
}

TEST_CASE("The limiter's ceiling holds on the very first sample of an over",
          "[dsp][master_bus][limiter][fast]") {
    MasterBus bus;
    bus.Prepare(kSampleRate);

    // No warm-up: the envelope is fully released, so this is the worst
    // case for a limiter with no lookahead and the one only the clamp can
    // catch.
    const int numFrames = 16;
    Signal signal(numFrames, 1, 4.0f);
    bus.Process(signal.data(), 1, numFrames);

    REQUIRE(signal.channels[0].front() <= MasterBus::kCeiling);
    REQUIRE(signal.Peak() <= MasterBus::kCeiling);
}

TEST_CASE("The limiter pulls gain reduction in rather than clamping continuously",
          "[dsp][master_bus][limiter][fast]") {
    MasterBus bus;
    bus.Prepare(kSampleRate);
    REQUIRE(bus.currentReduction() == 1.0f);

    const int numFrames = 128;
    Signal signal(numFrames, 1, 4.0f);
    for (int block = 0; block < 16; ++block) {
        signal.Refill(4.0f);
        bus.Process(signal.data(), 1, numFrames);
    }

    // Settled on roughly 1/4, so the signal arrives at the clamp already
    // at the ceiling instead of being sheared by it every sample. Without
    // the envelope this would still read 1.0 and the output would be a
    // square wave.
    REQUIRE(bus.currentReduction() < 0.3f);
    REQUIRE(bus.currentReduction() > 0.2f);
}

TEST_CASE("The limiter releases back to unity once the mix is quiet again",
          "[dsp][master_bus][limiter][fast]") {
    MasterBus bus;
    bus.Prepare(kSampleRate);

    const int numFrames = 128;
    Signal signal(numFrames, 1, 4.0f);
    for (int block = 0; block < 16; ++block) {
        signal.Refill(4.0f);
        bus.Process(signal.data(), 1, numFrames);
    }
    REQUIRE(bus.currentReduction() < 1.0f);

    // kReleaseSeconds is a 100ms time constant, so releasing from ~0.25 to
    // within a float ULP of unity takes ln(0.75 / 2.1e-5) ~= 10.5 of them,
    // just over a second - about 394 blocks of 128 at 48kHz, after which
    // the shared no-progress snap lands it on exactly unity (gain_ramp.h).
    // 600 clears that with margin. This is why the assertion is exact
    // rather than "close to 1.0": the snap is the thing under test.
    for (int block = 0; block < 600; ++block) {
        signal.Refill(0.1f);
        bus.Process(signal.data(), 1, numFrames);
    }
    REQUIRE(bus.currentReduction() == 1.0f);
}

TEST_CASE("The limiter reduces every channel by the same amount", "[dsp][master_bus][limiter][fast]") {
    MasterBus bus;
    bus.Prepare(kSampleRate);

    const int numFrames = 64;
    Signal signal(numFrames, 2, 0.0f);
    // Only the left channel is over. Detecting per channel would duck it
    // alone and shift the stereo image; one reduction for the frame must
    // pull both down together.
    std::fill(signal.channels[0].begin(), signal.channels[0].end(), 4.0f);
    std::fill(signal.channels[1].begin(), signal.channels[1].end(), 0.5f);

    bus.Process(signal.data(), 2, numFrames);

    for (int frame = 0; frame < numFrames; ++frame) {
        const float ratio = signal.channels[1][frame] / 0.5f;
        const float leftUnclamped = 4.0f * ratio;
        // The right channel never reaches the ceiling, so its scaling is
        // the reduction itself - and the left must have had that same
        // reduction applied before the clamp took the rest.
        REQUIRE(signal.channels[0][frame] <= MasterBus::kCeiling);
        REQUIRE(leftUnclamped >= signal.channels[0][frame] - 1.0e-6f);
    }
}

TEST_CASE("Prepare releases the limiter rather than inheriting the last device's reduction",
          "[dsp][master_bus][limiter][fast]") {
    MasterBus bus;
    bus.Prepare(kSampleRate);

    const int numFrames = 128;
    Signal signal(numFrames, 1, 4.0f);
    for (int block = 0; block < 16; ++block) {
        signal.Refill(4.0f);
        bus.Process(signal.data(), 1, numFrames);
    }
    REQUIRE(bus.currentReduction() < 1.0f);

    // A device restart must not come up still ducked from whatever the
    // previous device's last block happened to contain - the same
    // stale-state bug Strip::Prepare's anySolo parameter exists to stop.
    bus.Prepare(kSampleRate);
    REQUIRE(bus.currentReduction() == 1.0f);
}

TEST_CASE("A signal sitting at exactly full scale passes the limiter untouched",
          "[dsp][master_bus][limiter][fast]") {
    MasterBus bus;
    bus.Prepare(kSampleRate);

    const int numFrames = 128;
    Signal signal(numFrames, 1, MasterBus::kCeiling);
    bus.Process(signal.data(), 1, numFrames);

    // The ceiling comparison is strictly greater-than for this reason. An
    // epsilon-shaved ceiling here would make full-scale audio quietly
    // lossy.
    for (int frame = 0; frame < numFrames; ++frame) {
        REQUIRE(signal.channels[0][frame] == MasterBus::kCeiling);
    }
    REQUIRE(bus.currentReduction() == 1.0f);
}

TEST_CASE("The limiter runs under the allocation guard while actively reducing",
          "[dsp][master_bus][limiter][fast]") {
    jamn::core::SetRealtimeViolationHandler([](const char*) {
        throw std::runtime_error("rt violation");
    });

    MasterBus bus;
    bus.Prepare(kSampleRate);

    // Driven past the ceiling for the whole run, so the guard covers the
    // reducing path - the branchy one - not just the pass-through case the
    // existing JamAudio run already exercises.
    jamn::core::FileAudioDevice device(2, 128);
    bool reported = false;
    try {
        device.Process(64, [&](float* const* output, int numChannels, int numFrames) {
            for (int channel = 0; channel < numChannels; ++channel) {
                for (int frame = 0; frame < numFrames; ++frame) {
                    output[channel][frame] = 8.0f;
                }
            }
            bus.Process(output, numChannels, numFrames);
        });
    } catch (const std::runtime_error&) {
        reported = true;
    }

    jamn::core::SetRealtimeViolationHandler(nullptr);
    REQUIRE_FALSE(reported);
    REQUIRE(bus.currentReduction() < 1.0f);
}

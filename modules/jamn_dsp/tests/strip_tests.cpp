#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "jamn_dsp/strip.h"

using jamn::dsp::IInstrument;
using jamn::dsp::Strip;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockFrames = 128;

// kRampSeconds is a one-pole time constant, not a completion time: after
// one time constant the gain has only moved ~63% of the way. Both
// directions do land on the target exactly, but for two different reasons
// (see gain_ramp.h): downward the residual shrinks under kSnapEpsilon,
// upward the ramp stalls on a float ULP and the no-progress condition
// catches it, at sample 7656. 100 blocks of 128 clears both with margin,
// which is what lets every assertion below compare exactly rather than
// against an epsilon.
constexpr int kSettleBlocks = 100;

// Renders a constant 1.0 into every channel. A tone would make every gain
// assertion depend on where in the waveform the block happened to end;
// DC makes the rendered value the gain itself.
class ConstantInstrument final : public IInstrument {
public:
    void Prepare(double) noexcept override {}
    void NoteOn(std::uint8_t, std::uint8_t) noexcept override {}
    void NoteOff(std::uint8_t) noexcept override {}
    void AllNotesOff() noexcept override {}
    int ActiveVoiceCount() const noexcept override { return 1; }

    void Render(float* const* outputChannels, int numChannels, int numFrames) noexcept override {
        for (int channel = 0; channel < numChannels; ++channel) {
            for (int frame = 0; frame < numFrames; ++frame) {
                outputChannels[channel][frame] += 1.0f;
            }
        }
    }
};

struct Buffers {
    Buffers()
        : out(kBlockFrames, 0.0f),
          scratch(kBlockFrames, 0.0f),
          outPointers{out.data()},
          scratchPointers{scratch.data()} {}

    void ClearOut() { std::fill(out.begin(), out.end(), 0.0f); }

    float Peak() const {
        float peak = 0.0f;
        for (float sample : out) peak = std::max(peak, std::fabs(sample));
        return peak;
    }

    std::vector<float> out;
    std::vector<float> scratch;
    float* outPointers[1];
    float* scratchPointers[1];
};

// Renders enough blocks for the ramp to settle, and returns the peak of
// the final block only - the earlier ones are still mid-fade by design.
float PeakAfterSettling(Strip& strip, Buffers& buffers, bool anySolo) {
    for (int block = 0; block < kSettleBlocks; ++block) {
        buffers.ClearOut();
        strip.Render(buffers.outPointers, buffers.scratchPointers, 1, kBlockFrames, anySolo);
    }
    return buffers.Peak();
}

}  // namespace

TEST_CASE("A strip with no instrument renders silence", "[dsp][strip][fast]") {
    Strip strip;
    strip.Prepare(kSampleRate, false);

    Buffers buffers;
    strip.Render(buffers.outPointers, buffers.scratchPointers, 1, kBlockFrames, false);

    REQUIRE(buffers.Peak() == 0.0f);
}

TEST_CASE("A strip adds its instrument's output rather than replacing it", "[dsp][strip][fast]") {
    ConstantInstrument instrument;
    Strip strip;
    strip.SetInstrument(&instrument);
    strip.Prepare(kSampleRate, false);

    Buffers buffers;
    std::fill(buffers.out.begin(), buffers.out.end(), 0.5f);
    strip.Render(buffers.outPointers, buffers.scratchPointers, 1, kBlockFrames, false);

    // Prepare snapped the ramp to unity, so every sample is 0.5 + 1.0.
    REQUIRE(buffers.out.front() == 1.5f);
    REQUIRE(buffers.out.back() == 1.5f);
}

TEST_CASE("A strip's volume scales its instrument", "[dsp][strip][fast]") {
    ConstantInstrument instrument;
    Strip strip;
    strip.SetInstrument(&instrument);
    strip.SetVolume(0.25f);
    strip.Prepare(kSampleRate, false);

    Buffers buffers;
    const float peak = PeakAfterSettling(strip, buffers, false);

    REQUIRE(peak == 0.25f);
    REQUIRE(strip.currentGain() == 0.25f);
}

TEST_CASE("Muting a strip fades it to silence and unmuting restores it", "[dsp][strip][fast]") {
    ConstantInstrument instrument;
    Strip strip;
    strip.SetInstrument(&instrument);
    strip.Prepare(kSampleRate, false);

    Buffers buffers;
    strip.SetMute(true);
    REQUIRE(PeakAfterSettling(strip, buffers, false) == 0.0f);

    strip.SetMute(false);
    REQUIRE(PeakAfterSettling(strip, buffers, false) == 1.0f);
}

TEST_CASE("A mute is not instant - the strip is still audible one block in", "[dsp][strip][fast]") {
    ConstantInstrument instrument;
    Strip strip;
    strip.SetInstrument(&instrument);
    strip.Prepare(kSampleRate, false);
    strip.SetMute(true);

    Buffers buffers;
    strip.Render(buffers.outPointers, buffers.scratchPointers, 1, kBlockFrames, false);

    // The point of the ramp: an instant mute is a jump from full scale to
    // zero, which clicks. If this ever reads 0.0f the ramp has been lost.
    REQUIRE(buffers.Peak() > 0.0f);
}

TEST_CASE("A solo elsewhere silences a non-soloed strip", "[dsp][strip][fast]") {
    ConstantInstrument instrument;
    Strip strip;
    strip.SetInstrument(&instrument);
    strip.Prepare(kSampleRate, false);

    Buffers buffers;
    REQUIRE(PeakAfterSettling(strip, buffers, true) == 0.0f);
    REQUIRE(PeakAfterSettling(strip, buffers, false) == 1.0f);
}

TEST_CASE("Mute and solo compose predictably on the same strip", "[dsp][strip][fast]") {
    ConstantInstrument instrument;
    Strip strip;
    strip.SetInstrument(&instrument);
    strip.SetSolo(true);
    strip.SetMute(true);
    strip.Prepare(kSampleRate, true);

    // Soloed and muted: silent. Solo does not override an explicit mute,
    // or soloing a muted strip would unmute it behind the user's back.
    REQUIRE(strip.TargetGain(true) == 0.0f);

    Buffers buffers;
    REQUIRE(PeakAfterSettling(strip, buffers, true) == 0.0f);

    strip.SetMute(false);
    REQUIRE(PeakAfterSettling(strip, buffers, true) == 1.0f);
}

TEST_CASE("A strip prepared under someone else's solo comes up silent, not fading",
          "[dsp][strip][fast]") {
    ConstantInstrument instrument;
    Strip strip;
    strip.SetInstrument(&instrument);
    strip.Prepare(kSampleRate, true);

    REQUIRE(strip.currentGain() == 0.0f);

    Buffers buffers;
    strip.Render(buffers.outPointers, buffers.scratchPointers, 1, kBlockFrames, true);
    REQUIRE(buffers.Peak() == 0.0f);
}

TEST_CASE("A strip with no instrument still advances its ramp", "[dsp][strip][fast]") {
    Strip strip;
    strip.Prepare(kSampleRate, false);
    strip.SetMute(true);

    Buffers buffers;
    for (int block = 0; block < kSettleBlocks; ++block) {
        strip.Render(buffers.outPointers, buffers.scratchPointers, 1, kBlockFrames, false);
    }
    REQUIRE(strip.currentGain() == 0.0f);

    // Attaching an instrument now must not un-mute it by resuming from a
    // gain frozen back when the strip was still at unity.
    ConstantInstrument instrument;
    strip.SetInstrument(&instrument);
    buffers.ClearOut();
    strip.Render(buffers.outPointers, buffers.scratchPointers, 1, kBlockFrames, false);
    REQUIRE(buffers.Peak() == 0.0f);
}

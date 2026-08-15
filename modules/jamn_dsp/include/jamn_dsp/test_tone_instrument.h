#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "jamn_dsp/instrument.h"

namespace jamn::dsp {

// A polyphonic sine instrument: enough to prove a remote peer's scheduled
// NoteEvent becomes audible, and nothing more. BlipVoice, its Phase 0
// predecessor, is monophonic with no pitch and no note-off, so it cannot
// represent a remote peer playing at all.
//
// Fixed voice pool, no allocation anywhere. When every voice is busy the
// oldest sounding one is stolen - the choice matters and is not arbitrary:
// stealing the newest would make a fast passage silence itself, and
// refusing the note outright would drop it with no note-off ever following,
// which is the stuck-note shape in reverse.
class TestToneInstrument : public IInstrument {
public:
    static constexpr std::size_t kMaxVoices = 8;

    void Prepare(double sampleRate) noexcept override;
    void NoteOn(std::uint8_t pitch, std::uint8_t velocity) noexcept override;
    void NoteOff(std::uint8_t pitch) noexcept override;
    void AllNotesOff() noexcept override;
    void Render(float* const* outputChannels, int numChannels, int numFrames) noexcept override;
    int ActiveVoiceCount() const noexcept override;

private:
    static constexpr double kAttackSeconds = 0.005;
    static constexpr double kReleaseSeconds = 0.020;
    // Headroom per voice: kMaxVoices sounding at once must not on its own
    // demand the master limiter, so a full chord is not quieter than the
    // same chord played twice as loud through gain reduction.
    static constexpr float kVoiceAmplitude = 0.12f;

    struct Voice {
        bool active = false;
        bool releasing = false;
        std::uint8_t pitch = 0;
        float phase = 0.0f;
        float phaseIncrement = 0.0f;
        float envelope = 0.0f;
        float amplitude = 0.0f;
        // Monotonic counter, not a timestamp: voice stealing only needs an
        // order, and a counter cannot disagree with the audio clock.
        std::uint64_t startedAt = 0;
    };

    Voice* ClaimVoice() noexcept;

    std::array<Voice, kMaxVoices> voices_{};
    double sampleRate_ = 48000.0;
    float attackStep_ = 0.0f;
    // Linear, not exponential. An exponential release never actually
    // reaches zero, so "this pitch is silent" would be an asymptote rather
    // than a time - and how long a released voice keeps occupying the pool
    // would depend on where the cutoff threshold was set. A linear ramp
    // finishes in kReleaseSeconds exactly, which is what a voice pool with
    // stealing needs to reason about.
    float releaseStep_ = 0.0f;
    std::uint64_t nextStartStamp_ = 1;
};

}  // namespace jamn::dsp

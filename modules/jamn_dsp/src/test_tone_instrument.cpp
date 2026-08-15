#include "jamn_dsp/test_tone_instrument.h"

#include <cmath>
#include <numbers>

namespace jamn::dsp {
namespace {

// Equal temperament, A440 at MIDI note 69. std::pow allocates nothing and
// takes no lock, so calling it from NoteOn on the audio thread is fine -
// the rule is no allocation and no locks, not no arithmetic.
float FrequencyForPitch(std::uint8_t pitch) noexcept {
    return 440.0f * std::pow(2.0f, (static_cast<float>(pitch) - 69.0f) / 12.0f);
}

}  // namespace

void TestToneInstrument::Prepare(double sampleRate) noexcept {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    int attackSamples = static_cast<int>(kAttackSeconds * sampleRate_);
    if (attackSamples < 1) attackSamples = 1;
    attackStep_ = 1.0f / static_cast<float>(attackSamples);

    int releaseSamples = static_cast<int>(kReleaseSeconds * sampleRate_);
    if (releaseSamples < 1) releaseSamples = 1;
    releaseStep_ = 1.0f / static_cast<float>(releaseSamples);

    AllNotesOff();
    for (Voice& voice : voices_) voice = Voice{};
}

TestToneInstrument::Voice* TestToneInstrument::ClaimVoice() noexcept {
    for (Voice& voice : voices_) {
        if (!voice.active) return &voice;
    }

    // All busy - steal the oldest still-sounding voice. A releasing voice
    // is preferred over a held one, since it was already on its way out.
    Voice* oldestReleasing = nullptr;
    Voice* oldestHeld = nullptr;
    for (Voice& voice : voices_) {
        Voice*& candidate = voice.releasing ? oldestReleasing : oldestHeld;
        if (candidate == nullptr || voice.startedAt < candidate->startedAt) candidate = &voice;
    }
    return oldestReleasing != nullptr ? oldestReleasing : oldestHeld;
}

void TestToneInstrument::NoteOn(std::uint8_t pitch, std::uint8_t velocity) noexcept {
    // MIDI's own convention: velocity 0 is a note-off. Honouring it here
    // means a sender that only ever emits note-ons cannot strand a voice.
    if (velocity == 0) {
        NoteOff(pitch);
        return;
    }

    Voice* voice = ClaimVoice();
    if (voice == nullptr) return;  // Unreachable with kMaxVoices > 0; not worth an assert on this path.

    voice->active = true;
    voice->releasing = false;
    voice->pitch = pitch;
    voice->phase = 0.0f;
    voice->phaseIncrement = FrequencyForPitch(pitch) / static_cast<float>(sampleRate_);
    voice->envelope = 0.0f;
    voice->amplitude = kVoiceAmplitude * (static_cast<float>(velocity) / 127.0f);
    voice->startedAt = nextStartStamp_++;
}

void TestToneInstrument::NoteOff(std::uint8_t pitch) noexcept {
    // Every matching voice, not just the newest - see IInstrument::NoteOff.
    for (Voice& voice : voices_) {
        if (voice.active && !voice.releasing && voice.pitch == pitch) voice.releasing = true;
    }
}

void TestToneInstrument::AllNotesOff() noexcept {
    for (Voice& voice : voices_) {
        if (voice.active) voice.releasing = true;
    }
}

int TestToneInstrument::ActiveVoiceCount() const noexcept {
    int count = 0;
    for (const Voice& voice : voices_) {
        if (voice.active) ++count;
    }
    return count;
}

void TestToneInstrument::Render(float* const* outputChannels, int numChannels, int numFrames) noexcept {
    for (Voice& voice : voices_) {
        if (!voice.active) continue;

        for (int frame = 0; frame < numFrames; ++frame) {
            if (voice.releasing) {
                voice.envelope -= releaseStep_;
                if (voice.envelope <= 0.0f) {
                    voice.active = false;
                    voice.releasing = false;
                    voice.envelope = 0.0f;
                    break;
                }
            } else if (voice.envelope < 1.0f) {
                voice.envelope += attackStep_;
                if (voice.envelope > 1.0f) voice.envelope = 1.0f;
            }

            const float sample =
                std::sin(voice.phase * 2.0f * std::numbers::pi_v<float>) * voice.envelope * voice.amplitude;
            voice.phase += voice.phaseIncrement;
            if (voice.phase >= 1.0f) voice.phase -= 1.0f;

            for (int channel = 0; channel < numChannels; ++channel) {
                outputChannels[channel][frame] += sample;
            }
        }
    }
}

}  // namespace jamn::dsp

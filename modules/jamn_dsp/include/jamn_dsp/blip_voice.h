#pragma once

namespace jamn::dsp {

// A single synthesised sound: a short sine blip with a linear attack and an
// exponential decay, retriggerable from the audio thread. One voice, no
// polyphony - Phase 0 is one button, one sound. Add voice stealing when a
// second button needs it.
//
// Retrigger semantics: Trigger() restarts from the beginning. Two triggers
// in different blocks produce two audible attacks (a 128-frame block is
// 2.7ms at 48kHz - no human double-presses inside that); two triggers
// drained within the same block collapse to one attack. The alternative -
// one trigger honored per block - would drop presses outright, which is
// worse.
class BlipVoice {
public:
    // Message thread, before any Trigger()/Render() call.
    void Prepare(double sampleRate) noexcept;

    // Audio thread. Restarts the voice from the beginning of its envelope.
    void Trigger() noexcept;

    bool IsActive() const noexcept;

    // Audio thread. Adds into outputChannels - never clears it. The caller
    // owns clearing the buffer first.
    void Render(float* const* outputChannels, int numChannels, int numFrames) noexcept;

private:
    static constexpr float kFrequencyHz = 880.0f;
    static constexpr double kAttackSeconds = 0.004;
    static constexpr double kDecaySeconds = 0.090;
    static constexpr double kLifeSeconds = 0.400;
    static constexpr float kAmplitude = 0.35f;

    float phase_ = 0.0f;
    float phaseIncrement_ = 0.0f;
    float envelope_ = 0.0f;
    float attackStep_ = 0.0f;
    float decayCoeff_ = 0.0f;
    int attackSamples_ = 0;
    int attackSamplesRemaining_ = 0;
    int lifeSamples_ = 0;
    int samplesRemaining_ = 0;
};

}  // namespace jamn::dsp

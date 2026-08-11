#pragma once

#include <atomic>

namespace jamn::dsp {

// A single gain stage applied to every output channel. Publishes its target
// gain from the message thread via a plain atomic store and ramps toward it
// on the audio thread - a step change would click audibly, and
// juce::SmoothedValue isn't available here (jamn_dsp is JUCE-free).
class MasterBus {
public:
    static_assert(std::atomic<float>::is_always_lock_free,
                   "MasterBus publishes gain by plain atomic store; a locking "
                   "std::atomic<float> would put a lock on the audio callback "
                   "(docs/RT_RULES.md)");

    // Message thread. Computes the one-pole ramp coefficient for this sample
    // rate and snaps the current gain to the target, so a device restart
    // doesn't audibly re-ramp from stale state.
    void Prepare(double sampleRate) noexcept;

    // Message thread.
    void SetGain(float linearGain) noexcept;
    float gain() const noexcept;

    // The ramp's current position, not the target. Audio thread or tests.
    float currentGain() const noexcept;

    // Audio thread. Multiplies every sample in every channel in place.
    void Process(float* const* outputChannels, int numChannels, int numFrames) noexcept;

private:
    static constexpr double kRampSeconds = 0.015;
    static constexpr float kSnapEpsilon = 1.0e-6f;

    std::atomic<float> targetGain_{1.0f};
    float currentGain_ = 1.0f;
    float rampCoeff_ = 1.0f;
};

}  // namespace jamn::dsp

#pragma once

#include <atomic>

namespace jamn::dsp {

// The final gain stage and the limiter that stops the summed peers leaving
// the machine above full scale. Publishes its target gain from the message
// thread via a plain atomic store and ramps toward it on the audio thread -
// a step change would click audibly, and juce::SmoothedValue isn't
// available here (jamn_dsp is JUCE-free).
//
// The limiter is peak-detecting with no lookahead, and that is a
// constraint rather than a simplification: docs/CLOCK.md makes "a player's
// own local input is always monitored live, never delayed" non-negotiable,
// and a lookahead delay line on the master is exactly that delay. So the
// smoothed gain reduction cannot catch the first transient of an over on
// its own, and the hard clamp behind it - not the envelope - is what
// actually guarantees the ceiling. The envelope is what stops a sustained
// over (six peers at unity) being clamped continuously into distortion.
class MasterBus {
public:
    static_assert(std::atomic<float>::is_always_lock_free,
                   "MasterBus publishes gain by plain atomic store; a locking "
                   "std::atomic<float> would put a lock on the audio callback "
                   "(docs/RT_RULES.md)");

    // Full scale. Deliberately exactly 1.0 and compared with a strict
    // greater-than, so a signal sitting at exactly full scale passes
    // through untouched rather than being shaved by the limiter.
    static constexpr float kCeiling = 1.0f;

    // Message thread. Computes the ramp coefficients for this sample rate,
    // snaps the current gain to the target and releases any gain
    // reduction, so a device restart doesn't audibly re-ramp from stale
    // state.
    void Prepare(double sampleRate) noexcept;

    // Message thread.
    void SetGain(float linearGain) noexcept;
    float gain() const noexcept;

    // The ramp's current position, not the target. Audio thread or tests.
    float currentGain() const noexcept;

    // The limiter's current gain reduction: 1.0 when it is doing nothing,
    // lower while it is holding a loud mix down. Diagnostics and tests.
    float currentReduction() const noexcept;

    // Audio thread. Applies gain, then limiting, to every channel in
    // place. The peak is detected across all channels per frame and one
    // reduction is applied to all of them - detecting per channel would
    // pull one side of a stereo image down on its own.
    void Process(float* const* outputChannels, int numChannels, int numFrames) noexcept;

private:
    // Fast enough that the clamp behind it only ever sees the leading edge
    // of an over, slow enough not to modulate the signal audibly. The
    // release is much longer than the attack because a release short
    // enough to hear is what "pumping" is.
    static constexpr double kAttackSeconds = 0.001;
    static constexpr double kReleaseSeconds = 0.100;

    std::atomic<float> targetGain_{1.0f};
    float currentGain_ = 1.0f;
    float rampCoeff_ = 1.0f;

    float reduction_ = 1.0f;
    float attackCoeff_ = 1.0f;
    float releaseCoeff_ = 1.0f;
};

}  // namespace jamn::dsp

#include "jamn_dsp/master_bus.h"

#include <cmath>

#include "jamn_dsp/gain_ramp.h"

namespace jamn::dsp {

namespace {

// The limiter's envelope uses its own attack/release rather than the
// shared mixer ramp: a fader move and a peak catch are different events,
// and 15ms of attack would let a whole transient through to the clamp.
float EnvelopeCoeffFor(double seconds, double sampleRate) noexcept {
    return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * sampleRate)));
}

}  // namespace

void MasterBus::Prepare(double sampleRate) noexcept {
    rampCoeff_ = ramp::CoeffFor(sampleRate);
    currentGain_ = targetGain_.load(std::memory_order_relaxed);

    attackCoeff_ = EnvelopeCoeffFor(kAttackSeconds, sampleRate);
    releaseCoeff_ = EnvelopeCoeffFor(kReleaseSeconds, sampleRate);
    // Fully released, not whatever reduction the last device's final block
    // happened to leave behind.
    reduction_ = 1.0f;
}

void MasterBus::SetGain(float linearGain) noexcept {
    targetGain_.store(linearGain, std::memory_order_relaxed);
}

float MasterBus::gain() const noexcept {
    return targetGain_.load(std::memory_order_relaxed);
}

float MasterBus::currentGain() const noexcept {
    return currentGain_;
}

float MasterBus::currentReduction() const noexcept {
    return reduction_;
}

void MasterBus::Process(float* const* outputChannels, int numChannels, int numFrames) noexcept {
    const float target = targetGain_.load(std::memory_order_relaxed);
    const float blockStartGain = currentGain_;
    const float blockStartReduction = reduction_;

    float gain = currentGain_;
    bool anyOver = false;

    for (int frame = 0; frame < numFrames; ++frame) {
        gain += (target - gain) * rampCoeff_;

        // Peak across every channel of this frame, post-gain: what the
        // limiter has to hold down is the loudest thing about to leave.
        float peak = 0.0f;
        for (int channel = 0; channel < numChannels; ++channel) {
            peak = std::fmax(peak, std::fabs(outputChannels[channel][frame] * gain));
        }

        // Strictly greater, so exactly full scale is not an over and needs
        // no reduction at all.
        float reductionTarget = 1.0f;
        if (peak > kCeiling) {
            reductionTarget = kCeiling / peak;
            anyOver = true;
        }

        // Down fast, up slow. Getting this backwards is audible as the
        // limiter chasing the signal instead of holding it.
        const float coeff = (reductionTarget < reduction_) ? attackCoeff_ : releaseCoeff_;
        reduction_ += (reductionTarget - reduction_) * coeff;

        const float applied = gain * reduction_;
        for (int channel = 0; channel < numChannels; ++channel) {
            const float sample = outputChannels[channel][frame] * applied;
            // The backstop, and the only thing here that actually
            // guarantees the ceiling - without lookahead the envelope is
            // always at least one sample late. See master_bus.h.
            outputChannels[channel][frame] = std::fmin(std::fmax(sample, -kCeiling), kCeiling);
        }
    }

    currentGain_ = ramp::SnapAfterBlock(target, gain, blockStartGain, numFrames);

    // Only meaningful to snap the envelope when it was heading home all
    // block: mid-block the target moves every frame, so "no progress" says
    // nothing about where it was going.
    if (!anyOver) {
        reduction_ = ramp::SnapAfterBlock(1.0f, reduction_, blockStartReduction, numFrames);
    }
}

}  // namespace jamn::dsp

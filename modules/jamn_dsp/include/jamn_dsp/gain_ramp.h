#pragma once

#include <cmath>

namespace jamn::dsp::ramp {

// One click-free time constant for every gain stage in the mixer. Until
// now MasterBus and Strip each carried their own copy of this constant and
// their own copy of the snap rule below, kept in agreement by a comment -
// which is exactly the drift that turns into a master fading at a
// different rate from a strip's mute, read by a listener as the mix
// wobbling rather than as one fade.
inline constexpr double kRampSeconds = 0.015;

// How close counts as arrived. Not the only arrival condition - see
// SnapAfterBlock.
inline constexpr float kSnapEpsilon = 1.0e-6f;

// The per-sample coefficient of the one-pole. Message thread only: this
// calls exp().
inline float CoeffFor(double sampleRate) noexcept {
    return static_cast<float>(1.0 - std::exp(-1.0 / (kRampSeconds * sampleRate)));
}

// Where the ramp lands at the end of a block, given where it started.
//
// A one-pole never exactly arrives, so snapping once per block keeps the
// residual from grinding down into denormals on an idle control. But it
// also stops arriving entirely: once the per-sample increment
// (residual * coeff) falls below half a float ULP at the current
// magnitude, `gain += increment` becomes a no-op and the ramp stalls
// there. Measured ramping to unity at 48kHz, it stalls at sample 7656 on
// 0.999978542 - a residual of 2.1e-5, far above kSnapEpsilon, so an
// absolute-epsilon test alone can never fire on the way up. Hence the
// second condition: a block that moved the gain not at all has stalled,
// whatever the residual, and the target is where it was heading.
inline float SnapAfterBlock(float target, float gain, float blockStartGain,
                            int numFrames) noexcept {
    if (std::fabs(target - gain) < kSnapEpsilon) {
        return target;
    }
    // numFrames guard: a zero-length block makes no progress by
    // definition, and must not be read as a stall and teleported.
    if (numFrames > 0 && gain == blockStartGain) {
        return target;
    }
    return gain;
}

}  // namespace jamn::dsp::ramp

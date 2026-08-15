#include "jamn_dsp/strip.h"

#include "jamn_dsp/gain_ramp.h"

namespace jamn::dsp {

void Strip::Prepare(double sampleRate, bool anySolo) noexcept {
    rampCoeff_ = ramp::CoeffFor(sampleRate);
    // Snapping to the target rather than to volume_: a strip prepared while
    // muted must come up silent, not come up at full volume and fade.
    currentGain_ = TargetGain(anySolo);
}

void Strip::SetInstrument(IInstrument* instrument) noexcept {
    instrument_.store(instrument, std::memory_order_relaxed);
}

IInstrument* Strip::instrument() const noexcept {
    return instrument_.load(std::memory_order_relaxed);
}

void Strip::SetVolume(float linearGain) noexcept {
    volume_.store(linearGain, std::memory_order_relaxed);
}

float Strip::volume() const noexcept {
    return volume_.load(std::memory_order_relaxed);
}

void Strip::SetMute(bool muted) noexcept {
    muted_.store(muted, std::memory_order_relaxed);
}

bool Strip::muted() const noexcept {
    return muted_.load(std::memory_order_relaxed);
}

void Strip::SetSolo(bool soloed) noexcept {
    soloed_.store(soloed, std::memory_order_relaxed);
}

bool Strip::soloed() const noexcept {
    return soloed_.load(std::memory_order_relaxed);
}

float Strip::TargetGain(bool anySolo) const noexcept {
    if (muted_.load(std::memory_order_relaxed)) {
        return 0.0f;
    }
    // Mute wins over solo, checked first: a strip that is both soloed and
    // muted is silent. The other order would make soloing a muted strip
    // unmute it, which is not what either control says it does.
    if (anySolo && !soloed_.load(std::memory_order_relaxed)) {
        return 0.0f;
    }
    return volume_.load(std::memory_order_relaxed);
}

float Strip::currentGain() const noexcept {
    return currentGain_;
}

void Strip::AdvanceGain(float target, int numFrames) noexcept {
    const float blockStartGain = currentGain_;
    float gain = currentGain_;
    for (int frame = 0; frame < numFrames; ++frame) {
        gain += (target - gain) * rampCoeff_;
    }
    currentGain_ = ramp::SnapAfterBlock(target, gain, blockStartGain, numFrames);
}

void Strip::Render(float* const* out, float* const* scratch, int numChannels, int numFrames,
                   bool anySolo) noexcept {
    const float target = TargetGain(anySolo);

    IInstrument* const instrument = instrument_.load(std::memory_order_relaxed);
    if (instrument == nullptr) {
        AdvanceGain(target, numFrames);
        return;
    }

    // IInstrument::Render adds into its buffer and never clears it, so the
    // clear is ours - scratch still holds the previous strip's output.
    for (int channel = 0; channel < numChannels; ++channel) {
        for (int frame = 0; frame < numFrames; ++frame) {
            scratch[channel][frame] = 0.0f;
        }
    }
    instrument->Render(scratch, numChannels, numFrames);

    const float blockStartGain = currentGain_;
    float gain = currentGain_;
    for (int frame = 0; frame < numFrames; ++frame) {
        gain += (target - gain) * rampCoeff_;
        for (int channel = 0; channel < numChannels; ++channel) {
            out[channel][frame] += scratch[channel][frame] * gain;
        }
    }
    // Same once-per-block snap as MasterBus::Process - literally the same
    // function now, see gain_ramp.h.
    currentGain_ = ramp::SnapAfterBlock(target, gain, blockStartGain, numFrames);
}

}  // namespace jamn::dsp

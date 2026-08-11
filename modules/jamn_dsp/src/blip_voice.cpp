#include "jamn_dsp/blip_voice.h"

#include <cmath>
#include <numbers>

namespace jamn::dsp {

void BlipVoice::Prepare(double sampleRate) noexcept {
    phaseIncrement_ = static_cast<float>(kFrequencyHz / sampleRate);

    attackSamples_ = static_cast<int>(kAttackSeconds * sampleRate);
    if (attackSamples_ < 1) {
        attackSamples_ = 1;
    }
    attackStep_ = kAmplitude / static_cast<float>(attackSamples_);

    lifeSamples_ = static_cast<int>(kLifeSeconds * sampleRate);
    decayCoeff_ = static_cast<float>(std::exp(-1.0 / (kDecaySeconds * sampleRate)));

    attackSamplesRemaining_ = 0;
    samplesRemaining_ = 0;
    envelope_ = 0.0f;
    phase_ = 0.0f;
}

void BlipVoice::Trigger() noexcept {
    phase_ = 0.0f;
    envelope_ = 0.0f;
    attackSamplesRemaining_ = attackSamples_;
    samplesRemaining_ = lifeSamples_;
}

bool BlipVoice::IsActive() const noexcept {
    return samplesRemaining_ > 0;
}

void BlipVoice::Render(float* const* outputChannels, int numChannels, int numFrames) noexcept {
    for (int frame = 0; frame < numFrames; ++frame) {
        if (samplesRemaining_ <= 0) {
            break;
        }

        if (attackSamplesRemaining_ > 0) {
            envelope_ += attackStep_;
            --attackSamplesRemaining_;
        } else {
            envelope_ *= decayCoeff_;
        }

        const float sample = std::sin(phase_ * 2.0f * std::numbers::pi_v<float>) * envelope_;
        phase_ += phaseIncrement_;
        if (phase_ >= 1.0f) {
            phase_ -= 1.0f;
        }

        for (int channel = 0; channel < numChannels; ++channel) {
            outputChannels[channel][frame] += sample;
        }

        --samplesRemaining_;
    }
}

}  // namespace jamn::dsp

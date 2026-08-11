#include "jamn_dsp/master_bus.h"

#include <cmath>

namespace jamn::dsp {

void MasterBus::Prepare(double sampleRate) noexcept {
    const double coeff = 1.0 - std::exp(-1.0 / (kRampSeconds * sampleRate));
    rampCoeff_ = static_cast<float>(coeff);
    currentGain_ = targetGain_.load(std::memory_order_relaxed);
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

void MasterBus::Process(float* const* outputChannels, int numChannels, int numFrames) noexcept {
    const float target = targetGain_.load(std::memory_order_relaxed);
    float gain = currentGain_;
    for (int frame = 0; frame < numFrames; ++frame) {
        gain += (target - gain) * rampCoeff_;
        for (int channel = 0; channel < numChannels; ++channel) {
            outputChannels[channel][frame] *= gain;
        }
    }
    // A one-pole never exactly arrives; snapping once per block stops the
    // residual from grinding down into denormals on an idle slider.
    currentGain_ = (std::fabs(target - gain) < kSnapEpsilon) ? target : gain;
}

}  // namespace jamn::dsp

#include "jamn_dsp/jam_audio.h"

namespace jamn::dsp {

void JamAudio::Prepare(double sampleRate) noexcept {
    voice_.Prepare(sampleRate);
    peerMixer_.Prepare(sampleRate);
    masterBus_.Prepare(sampleRate);
}

bool JamAudio::Trigger() noexcept {
    return triggers_.Push(TriggerEvent::kBlip);
}

void JamAudio::SetGain(float linearGain) noexcept {
    masterBus_.SetGain(linearGain);
}

float JamAudio::gain() const noexcept {
    return masterBus_.gain();
}

PeerMixer& JamAudio::peers() noexcept {
    return peerMixer_;
}

const PeerMixer& JamAudio::peers() const noexcept {
    return peerMixer_;
}

void JamAudio::Process(float* const* outputChannels, int numChannels, int numFrames) noexcept {
    // Bounded at compile time by TriggerRing's capacity - see jam_audio.h.
    TriggerEvent event;
    while (triggers_.Pop(event)) {
        voice_.Trigger();
    }

    for (int channel = 0; channel < numChannels; ++channel) {
        for (int frame = 0; frame < numFrames; ++frame) {
            outputChannels[channel][frame] = 0.0f;
        }
    }

    voice_.Render(outputChannels, numChannels, numFrames);
    peerMixer_.Render(outputChannels, numChannels, numFrames);
    masterBus_.Process(outputChannels, numChannels, numFrames);
}

}  // namespace jamn::dsp

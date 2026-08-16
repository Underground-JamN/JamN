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

void JamAudio::SetLocalInstrument(IInstrument* instrument) noexcept {
    localInstrument_.store(instrument, std::memory_order_release);
}

IInstrument* JamAudio::localInstrument() const noexcept {
    return localInstrument_.load(std::memory_order_acquire);
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
    // Adds into the buffer the same way BlipVoice does, and ahead of the
    // mixer only because both are additive and the order cannot matter.
    // One acquire load per block, not per frame.
    if (IInstrument* local = localInstrument_.load(std::memory_order_acquire); local != nullptr) {
        local->Render(outputChannels, numChannels, numFrames);
    }
    peerMixer_.Render(outputChannels, numChannels, numFrames);
    masterBus_.Process(outputChannels, numChannels, numFrames);
}

}  // namespace jamn::dsp

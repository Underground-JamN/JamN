#pragma once

#include <cstdint>

#include "jamn_core/spsc_ring.h"
#include "jamn_dsp/blip_voice.h"
#include "jamn_dsp/master_bus.h"

namespace jamn::dsp {

enum class TriggerEvent : std::uint8_t { kBlip };

// Producer: the message thread (a button click). Consumer: the audio thread
// (JamAudio::Process). That is the SpscRing contract - exactly one of each,
// never two callers of Trigger() from different threads.
using TriggerRing = jamn::core::SpscRing<TriggerEvent, 16>;

// The whole Phase 0 signal path, fixed at compile time: one voice into one
// master gain. This is deliberately NOT a graph and must not grow into one
// (AGENTS.md: "a PR that adds a graph node type is a warning sign"). Tests
// and jamn_app both drive this same object, through jamn::core::AudioCallback
// - so a passing test proves something about what actually ships.
class JamAudio {
public:
    // Message thread.
    void Prepare(double sampleRate) noexcept;

    // Message thread. Returns false if the trigger ring is full.
    bool Trigger() noexcept;

    // Message thread.
    void SetGain(float linearGain) noexcept;
    float gain() const noexcept;

    // Audio thread. Matches jamn::core::AudioCallback's shape exactly.
    void Process(float* const* outputChannels, int numChannels, int numFrames) noexcept;

private:
    TriggerRing triggers_;
    BlipVoice voice_;
    MasterBus masterBus_;
};

}  // namespace jamn::dsp

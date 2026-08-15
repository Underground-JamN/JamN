#pragma once

#include <array>
#include <cstddef>

#include "jamn_core/session_limits.h"
#include "jamn_dsp/strip.h"

namespace jamn::dsp {

// Every peer's strip, summed. A fixed array indexed by peer slot, not a
// graph and not a container that grows - jam_audio.h says the signal path
// is "deliberately NOT a graph and must not grow into one" and
// AGENTS.md puts arbitrary routing permanently out of scope. There is no
// Add/Remove here on purpose: a peer joining or leaving repoints a slot's
// instrument, it does not resize anything.
//
// This class exists rather than JamAudio holding the array directly so the
// solo/mute interaction - the part with a real off-by-one risk, where one
// solo mutes every other strip - is unit-testable without going through
// the whole audio path.
class PeerMixer {
public:
    static constexpr std::size_t kNumStrips = jamn::core::kMaxPeers;

    // Scratch is one strip's render buffer, reused across strips, and it is
    // a fixed member rather than sized at Prepare from the device's block
    // size. Block size is not a promise: the device can restart with a
    // larger one, and resizing on the audio thread is exactly the
    // allocation docs/RT_RULES.md forbids. Render chunks anything longer
    // instead, so an oversized block costs an extra pass, never a resize
    // and never an overrun.
    static constexpr int kMaxBlockFrames = 1024;

    // The peer mix is stereo. jamn_platform opens 2 output channels
    // (jamn_app/src/main.cpp), and a wider device gets the peer mix on its
    // first two channels - stated here rather than left to whatever the
    // scratch array happened to be sized at. This is deliberately not
    // jamn_platform's kMaxOutputChannels: jamn_dsp does not depend on
    // jamn_platform and must not start.
    static constexpr int kMaxChannels = 2;

    PeerMixer() noexcept;

    // Message thread.
    void Prepare(double sampleRate) noexcept;

    // Message thread for the controls, audio thread only via Render below.
    Strip& strip(std::size_t index) noexcept;
    const Strip& strip(std::size_t index) const noexcept;

    // True if any strip is soloed, which is what makes every non-soloed
    // strip silent. Read once per block by Render so a solo toggled
    // mid-block cannot split one buffer across two different answers.
    bool AnySolo() const noexcept;

    // Audio thread. Adds the summed strips into out; never clears it, the
    // same contract IInstrument::Render and Strip::Render already have.
    void Render(float* const* out, int numChannels, int numFrames) noexcept;

private:
    std::array<Strip, kNumStrips> strips_{};
    std::array<float, static_cast<std::size_t>(kMaxChannels) * kMaxBlockFrames> scratch_{};
    std::array<float*, kMaxChannels> scratchChannels_{};
    // Rebased once per chunk to point into the caller's buffer at the
    // chunk's frame offset. A member, not a local, only so the type is
    // obviously fixed-size - no allocation either way.
    std::array<float*, kMaxChannels> outChannels_{};
};

}  // namespace jamn::dsp

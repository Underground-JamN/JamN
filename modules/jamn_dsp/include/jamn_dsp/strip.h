#pragma once

#include <atomic>

#include "jamn_dsp/instrument.h"

namespace jamn::dsp {

// One peer's channel: an instrument, a volume, a mute and a solo. Nothing
// else - no sends, no inserts, no routing. This is a leaf, and PeerMixer
// holds a fixed array of them; neither is a node in a graph, and
// jam_audio.h plus AGENTS.md both put a general audio graph permanently
// out of scope.
//
// Controls publish from the message thread by plain atomic store and are
// read on the audio thread, the same idiom MasterBus already uses for
// gain. The resulting gain is ramped rather than stepped: an unmute is a
// jump from 0 to full scale, which clicks audibly if applied instantly.
// The ramp itself comes from gain_ramp.h, shared with MasterBus, so the
// whole mixer fades on one time constant.
class Strip {
public:
    static_assert(std::atomic<float>::is_always_lock_free,
                   "Strip publishes volume by plain atomic store; a locking "
                   "std::atomic<float> would put a lock on the audio callback "
                   "(docs/RT_RULES.md)");

    // Message thread. Snaps the ramp to the current target, so a device
    // restart doesn't audibly re-ramp from stale state. anySolo is a
    // parameter for the same reason it is one on TargetGain below - a
    // strip cannot answer it alone, and restarting the device while
    // another strip is soloed must not bring this one up at full volume
    // for the 15ms it takes to fade back down.
    void Prepare(double sampleRate, bool anySolo) noexcept;

    // Message thread, and the pointer must outlive every Render call. Not
    // owned: instruments are owned by whoever assembled the mix, so a strip
    // can be repointed without anything on the audio thread freeing memory.
    // Null is the ordinary state, not an error - a strip for a peer slot
    // nobody has joined yet renders silence.
    void SetInstrument(IInstrument* instrument) noexcept;
    IInstrument* instrument() const noexcept;

    // Message thread.
    void SetVolume(float linearGain) noexcept;
    float volume() const noexcept;
    void SetMute(bool muted) noexcept;
    bool muted() const noexcept;
    void SetSolo(bool soloed) noexcept;
    bool soloed() const noexcept;

    // The gain this strip resolves to right now, before ramping: zero if
    // muted, zero if anything is soloed and this is not it, otherwise
    // volume. Solo is the subtle one - a solo anywhere mutes every
    // non-soloed strip, which is why anySolo is a parameter rather than
    // something a strip could answer alone.
    float TargetGain(bool anySolo) const noexcept;

    // The ramp's current position, not the target. Audio thread or tests.
    float currentGain() const noexcept;

    // Audio thread. Clears scratch, renders this strip's instrument into
    // it, then adds scratch * (ramped gain) into out. out is added to,
    // never cleared - the caller owns that. Both buffers must have at least
    // numChannels channels of numFrames frames. With no instrument set,
    // adds nothing but still advances the ramp, so a strip that gains one
    // mid-fade resumes from where the fade actually got to rather than from
    // a gain frozen whenever the instrument was last removed.
    void Render(float* const* out, float* const* scratch, int numChannels, int numFrames,
                bool anySolo) noexcept;

private:
    // Runs the ramp forward numFrames without producing output, and returns
    // where it ended up. Render's own loop cannot be reused for this: it
    // ramps per frame interleaved with the summing, which is the whole
    // point of it.
    void AdvanceGain(float target, int numFrames) noexcept;

    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> soloed_{false};
    std::atomic<IInstrument*> instrument_{nullptr};

    float currentGain_ = 1.0f;
    float rampCoeff_ = 1.0f;
};

}  // namespace jamn::dsp

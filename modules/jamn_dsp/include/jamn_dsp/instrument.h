#pragma once

#include <cstdint>

namespace jamn::dsp {

// What turns a scheduled NoteEvent into sound. A runtime virtual, not a
// template parameter, for the same reason IDeadlineResolver is one: an
// instrument is swappable per peer at runtime (InstrumentAssign is already
// a protocol message), and a compile-time seam could not express that.
//
// Every method below NoteOn runs on the audio thread and must be
// allocation-free and lock-free per docs/RT_RULES.md. A virtual call is
// fine - the resolver seam set that precedent - but a virtual call that
// allocates is not, and that includes the obvious traps: no voice pool
// that grows, no std::function stored per note, no logging on a stolen
// voice.
class IInstrument {
public:
    virtual ~IInstrument() = default;

    // Message thread, before any note or Render call.
    virtual void Prepare(double sampleRate) noexcept = 0;

    // Audio thread. pitch is a MIDI note number (60 = middle C); velocity
    // is 1-127, with 0 treated as a note-off the way MIDI does, so a
    // sender that only ever emits note-ons cannot strand a voice.
    virtual void NoteOn(std::uint8_t pitch, std::uint8_t velocity) noexcept = 0;

    // Audio thread. Silences every voice sounding that pitch - all of
    // them, not just the newest: two note-ons for one pitch followed by
    // one note-off must not leave a voice sounding forever. A stuck note
    // is worse than a dropped one (docs/CLOCK.md).
    virtual void NoteOff(std::uint8_t pitch) noexcept = 0;

    // Audio thread. The panic path, and what a clock re-lock triggers.
    virtual void AllNotesOff() noexcept = 0;

    // Audio thread. Adds into outputChannels - never clears it. The caller
    // owns clearing the buffer first, the same contract BlipVoice::Render
    // already has.
    virtual void Render(float* const* outputChannels, int numChannels, int numFrames) noexcept = 0;

    // Audio thread or tests. Diagnostics only.
    virtual int ActiveVoiceCount() const noexcept = 0;
};

}  // namespace jamn::dsp

#include "jamn_platform/key_note_input.h"

#include <algorithm>

namespace jamn::platform {

std::size_t BuildBindings(const std::uint32_t* rawCodes, std::size_t count, KeyBinding* out, std::size_t capacity) {
    if (rawCodes == nullptr || out == nullptr) return 0;

    const std::size_t written = std::min({count, capacity, kKeyPositions});
    for (std::size_t index = 0; index < written; ++index) {
        out[index].rawCode = rawCodes[index];
        out[index].pitch = kLayoutPitches[index];
    }
    return written;
}

void KeyNoteInput::SetBindings(const KeyBinding* bindings, std::size_t count) {
    bindingCount_ = 0;
    if (bindings == nullptr) return;

    const std::size_t accepted = std::min(count, kMaxBindings);
    for (std::size_t index = 0; index < accepted; ++index) bindings_[index] = bindings[index];
    bindingCount_ = accepted;
}

bool KeyNoteInput::AnotherKeyHolds(std::uint8_t pitch, std::size_t except) const {
    for (std::size_t index = 0; index < heldCount_; ++index) {
        if (index != except && held_[index].pitch == pitch) return true;
    }
    return false;
}

bool KeyNoteInput::OnKey(std::uint32_t rawCode, bool pressed, Event& out) {
    std::size_t heldIndex = kMaxHeldKeys;
    for (std::size_t index = 0; index < heldCount_; ++index) {
        if (held_[index].rawCode == rawCode) {
            heldIndex = index;
            break;
        }
    }

    if (pressed) {
        // Already down. On X11 this is what auto-repeat looks like -
        // there is no repeat bit to test, unlike Windows' lParam bit 30
        // and macOS's isARepeat - so the held set is what filters it, on
        // every platform, rather than three different tests.
        if (heldIndex < kMaxHeldKeys) return false;

        std::uint8_t pitch = 0;
        bool bound = false;
        for (std::size_t index = 0; index < bindingCount_; ++index) {
            if (bindings_[index].rawCode == rawCode) {
                pitch = bindings_[index].pitch;
                bound = true;
                break;
            }
        }
        if (!bound) return false;

        if (heldCount_ >= kMaxHeldKeys) {
            // The seventh key. Counted rather than queued: a key that is
            // physically down is not a key whose note can be played
            // later, and holding it for a slot would sound a note the
            // player has by then let go of.
            ++ignoredOverRollover_;
            return false;
        }

        held_[heldCount_] = HeldKey{rawCode, pitch};
        ++heldCount_;

        // Emitted even when the pitch is already sounding from the other
        // key bound to it, because a retrigger is what a player pressing
        // a key expects to hear. The instrument's own contract covers the
        // rest: two note-ons for one pitch followed by one note-off must
        // not leave a voice sounding (jamn_dsp/instrument.h).
        out.kind = Event::Kind::kNoteOn;
        out.pitch = pitch;
        return true;
    }

    // A release of something never held: unmapped, or refused at the
    // rollover cap. Nothing sounded, so nothing is silenced.
    if (heldIndex >= kMaxHeldKeys) return false;

    const std::uint8_t pitch = held_[heldIndex].pitch;
    const bool stillHeldElsewhere = AnotherKeyHolds(pitch, heldIndex);

    held_[heldIndex] = held_[heldCount_ - 1];
    --heldCount_;

    // The overlapping fifth, handled: `,` and `Q` are both C4, and
    // NoteOff silences every voice at that pitch, so releasing one while
    // the other is still down would cut a note the player is holding.
    if (stillHeldElsewhere) return false;

    out.kind = Event::Kind::kNoteOff;
    out.pitch = pitch;
    return true;
}

std::size_t KeyNoteInput::ReleaseAll(Event* out, std::size_t capacity) {
    if (out == nullptr) return 0;

    std::size_t written = 0;
    for (std::size_t index = 0; index < heldCount_; ++index) {
        // One note-off per pitch, not per key: two keys on the same pitch
        // would otherwise emit a note-off for a pitch already silent.
        bool alreadyWritten = false;
        for (std::size_t seen = 0; seen < written; ++seen) {
            if (out[seen].pitch == held_[index].pitch) {
                alreadyWritten = true;
                break;
            }
        }
        if (alreadyWritten) continue;
        if (written >= capacity) break;

        out[written].kind = Event::Kind::kNoteOff;
        out[written].pitch = held_[index].pitch;
        ++written;
    }

    // Cleared whether or not every note-off fitted. A caller given too
    // small an array has already lost those releases, and keeping the
    // keys held would mean the *next* release emits a note-off for a key
    // this call was supposed to have ended.
    heldCount_ = 0;
    return written;
}

}  // namespace jamn::platform

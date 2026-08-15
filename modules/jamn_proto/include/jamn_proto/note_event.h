#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"

namespace jamn::proto {

enum class NoteEventKind : std::uint8_t { kNoteOn = 0, kNoteOff = 1, kAllNotesOff = 2 };

// Bit 0 of NoteEvent::flags: "musical time follows" - when set, an 8-byte
// t_absolute_ppq tail is present after the 16 fixed bytes
// (docs/PROTOCOL.md's "Notes are normalized, not tunnelled" section names
// this as one of the fields reserved now, defined later; this is that
// definition).
inline constexpr std::uint16_t kMusicalTimeFlag = 0x1;

// 16 bytes fixed, plus an 8-byte optional tail when kMusicalTimeFlag is
// set in flags - one event inside a NoteBurst (note_burst.h).
struct NoteEvent {
    std::int32_t dtUs = 0;          // Delta from the burst's base_t_session_us.
    std::uint16_t eventSeq = 0;
    std::uint8_t slot = 0;
    NoteEventKind kind = NoteEventKind::kNoteOn;
    std::uint8_t a = 0;
    std::uint8_t b = 0;
    std::uint16_t c = 0;
    std::uint16_t stateRev = 0;
    std::uint16_t flags = 0;
    std::int64_t tAbsolutePpq = 0;  // Only meaningful when flags & kMusicalTimeFlag.

    static constexpr std::size_t kFixedEncodedSize = 16;
    static constexpr std::size_t kTailEncodedSize = 8;

    friend bool operator==(const NoteEvent& a, const NoteEvent& b) {
        const bool fixedEqual = a.dtUs == b.dtUs && a.eventSeq == b.eventSeq && a.slot == b.slot &&
                                 a.kind == b.kind && a.a == b.a && a.b == b.b && a.c == b.c &&
                                 a.stateRev == b.stateRev && a.flags == b.flags;
        if (!fixedEqual) return false;
        if ((a.flags & kMusicalTimeFlag) != 0) return a.tAbsolutePpq == b.tAbsolutePpq;
        return true;
    }
    friend bool operator!=(const NoteEvent& a, const NoteEvent& b) { return !(a == b); }
};

bool EncodeNoteEvent(const NoteEvent& event, jamn::core::ByteWriter& out);
bool DecodeNoteEvent(jamn::core::ByteReader& in, NoteEvent& out);

}  // namespace jamn::proto

#include "jamn_proto/note_burst.h"

namespace jamn::proto {

bool EncodeNoteBurst(const NoteBurst& burst, jamn::core::ByteWriter& out) {
    if (burst.eventCount > kMaxEventsPerBurst) return false;
    if (!out.WriteI64(burst.baseTSessionUs)) return false;
    if (!out.WriteU16(burst.burstSeq)) return false;
    if (!out.WriteU8(burst.eventCount)) return false;
    if (!out.WriteU8(burst.reserved)) return false;
    for (std::size_t i = 0; i < burst.eventCount; ++i) {
        if (!EncodeNoteEvent(burst.events[i], out)) return false;
    }
    return true;
}

bool DecodeNoteBurst(jamn::core::ByteReader& in, NoteBurst& out) {
    NoteBurst burst;
    if (!in.ReadI64(burst.baseTSessionUs)) return false;
    if (!in.ReadU16(burst.burstSeq)) return false;
    if (!in.ReadU8(burst.eventCount)) return false;
    if (!in.ReadU8(burst.reserved)) return false;
    // Bounds-checked before touching the fixed-size array - a decoded
    // count above kMaxEventsPerBurst is a malformed/hostile packet, not a
    // license to write out of bounds.
    if (burst.eventCount > kMaxEventsPerBurst) return false;
    for (std::size_t i = 0; i < burst.eventCount; ++i) {
        if (!DecodeNoteEvent(in, burst.events[i])) return false;
    }
    out = burst;
    return true;
}

}  // namespace jamn::proto

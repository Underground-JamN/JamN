#include "jamn_proto/note_event.h"

namespace jamn::proto {

bool EncodeNoteEvent(const NoteEvent& event, jamn::core::ByteWriter& out) {
    if (!out.WriteI32(event.dtUs)) return false;
    if (!out.WriteU16(event.eventSeq)) return false;
    if (!out.WriteU8(event.slot)) return false;
    if (!out.WriteU8(static_cast<std::uint8_t>(event.kind))) return false;
    if (!out.WriteU8(event.a)) return false;
    if (!out.WriteU8(event.b)) return false;
    if (!out.WriteU16(event.c)) return false;
    if (!out.WriteU16(event.stateRev)) return false;
    if (!out.WriteU16(event.flags)) return false;
    if ((event.flags & kMusicalTimeFlag) != 0) {
        if (!out.WriteI64(event.tAbsolutePpq)) return false;
    }
    return true;
}

bool DecodeNoteEvent(jamn::core::ByteReader& in, NoteEvent& out) {
    NoteEvent event;
    if (!in.ReadI32(event.dtUs)) return false;
    if (!in.ReadU16(event.eventSeq)) return false;
    if (!in.ReadU8(event.slot)) return false;
    std::uint8_t kind = 0;
    if (!in.ReadU8(kind)) return false;
    event.kind = static_cast<NoteEventKind>(kind);
    if (!in.ReadU8(event.a)) return false;
    if (!in.ReadU8(event.b)) return false;
    if (!in.ReadU16(event.c)) return false;
    if (!in.ReadU16(event.stateRev)) return false;
    if (!in.ReadU16(event.flags)) return false;
    if ((event.flags & kMusicalTimeFlag) != 0) {
        if (!in.ReadI64(event.tAbsolutePpq)) return false;
    }
    out = event;
    return true;
}

}  // namespace jamn::proto

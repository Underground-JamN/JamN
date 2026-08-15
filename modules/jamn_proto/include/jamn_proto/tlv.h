#pragma once

#include <cstdint>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"

namespace jamn::proto {

// One TLV entry's header: a 2-byte type (the MessageType wire value) and a
// 2-byte length, per docs/PROTOCOL.md's packet shape.
struct TlvHeader {
    std::uint16_t type = 0;
    std::uint16_t len = 0;
};

inline bool WriteTlvHeader(jamn::core::ByteWriter& out, std::uint16_t type, std::uint16_t len) {
    return out.WriteU16(type) && out.WriteU16(len);
}

inline bool ReadTlvHeader(jamn::core::ByteReader& in, TlvHeader& out) {
    return in.ReadU16(out.type) && in.ReadU16(out.len);
}

// Iterates every TLV remaining in `reader`, calling
// visit(type, valueReader) for each one. valueReader is scoped to exactly
// that TLV's `len` bytes via ByteReader::ReadSlice - the visitor may read
// fewer bytes than len (rule 2: a body that grew) or not recognise the
// type at all (rule 1: an unknown type is simply skipped, never an error);
// either way this function itself advances past the full `len`, never
// trusting the visitor to have consumed exactly that many bytes.
//
// Returns false, stopping iteration, only on a malformed stream - a
// truncated TLV header, or a `len` that runs past what remains. An
// unrecognised type is never a failure; the visitor decides what "skip"
// means for a type it doesn't know by simply not consuming valueReader.
template <typename Visitor>
bool ForEachTlv(jamn::core::ByteReader& reader, Visitor&& visit) {
    while (reader.Remaining() > 0) {
        TlvHeader header;
        if (!ReadTlvHeader(reader, header)) return false;
        jamn::core::ByteReader value(nullptr, 0);
        if (!reader.ReadSlice(value, header.len)) return false;
        visit(header.type, value);
    }
    return true;
}

}  // namespace jamn::proto

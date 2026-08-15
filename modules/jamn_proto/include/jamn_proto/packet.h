#pragma once

#include <utility>

#include "jamn_core/byte_reader.h"
#include "jamn_proto/packet_header.h"
#include "jamn_proto/tlv.h"

namespace jamn::proto {

// Decodes a full wire packet (PacketHeader followed by TLVs, per
// docs/PROTOCOL.md's "Packet shape") and visits each successfully-framed
// TLV via visit(type, valueReader), exactly as ForEachTlv does. Returns
// false, without calling visit at all, on framing-level malformity a
// header decode alone can't catch: a truncated header (DecodePacketHeader
// already handles that), or a body_len claiming more bytes than actually
// follow it in the buffer.
//
// This is the entry point tests/fuzz_corpus's replay test drives every
// corpus blob through - by construction (every read underneath is
// ByteReader's bounds-checked primitives) it never reads out of bounds and
// always terminates, regardless of what header/TLV fields claim.
template <typename Visitor>
bool DecodePacket(jamn::core::ByteReader& reader, PacketHeader& outHeader, Visitor&& visit) {
    if (!DecodePacketHeader(reader, outHeader)) return false;
    jamn::core::ByteReader body(nullptr, 0);
    if (!reader.ReadSlice(body, outHeader.bodyLen)) return false;
    return ForEachTlv(body, std::forward<Visitor>(visit));
}

// As DecodePacket, but a datagram from a peer_id isPeerKnown rejects is
// dropped immediately after the header decodes - before the body is even
// sliced off, let alone before ForEachTlv looks at a single TLV. This is
// the structural form of "a datagram from an unrecognised peer_id is
// dropped before any TLV parsing begins": the visitor is provably
// unreachable for an unknown peer, not merely untested for one, the same
// pattern EventScheduler's resolver seam (Wave 4) will later use for
// "unimplemented cannot mean runs anyway".
template <typename PeerKnownPredicate, typename Visitor>
bool DecodePacketAuthenticated(jamn::core::ByteReader& reader, PacketHeader& outHeader,
                                PeerKnownPredicate&& isPeerKnown, Visitor&& visit) {
    if (!DecodePacketHeader(reader, outHeader)) return false;
    if (!isPeerKnown(outHeader.peerId)) return false;
    jamn::core::ByteReader body(nullptr, 0);
    if (!reader.ReadSlice(body, outHeader.bodyLen)) return false;
    return ForEachTlv(body, std::forward<Visitor>(visit));
}

}  // namespace jamn::proto

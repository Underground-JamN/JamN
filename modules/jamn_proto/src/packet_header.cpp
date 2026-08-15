#include "jamn_proto/packet_header.h"

namespace jamn::proto {

bool EncodePacketHeader(const PacketHeader& header, jamn::core::ByteWriter& out) {
    return out.WriteU16(header.magic) && out.WriteU8(header.protoMajor) && out.WriteU8(header.protoMinor) &&
           out.WriteU16(header.peerId) && out.WriteU16(header.bodyLen);
}

bool DecodePacketHeader(jamn::core::ByteReader& in, PacketHeader& out) {
    PacketHeader header;
    if (!in.ReadU16(header.magic)) return false;
    if (!in.ReadU8(header.protoMajor)) return false;
    if (!in.ReadU8(header.protoMinor)) return false;
    if (!in.ReadU16(header.peerId)) return false;
    if (!in.ReadU16(header.bodyLen)) return false;
    out = header;
    return true;
}

}  // namespace jamn::proto

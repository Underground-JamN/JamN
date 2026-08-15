#pragma once

#include <cstddef>
#include <cstdint>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"

namespace jamn::proto {

// Bytes 'J','M' on the wire, little-endian (docs/PROTOCOL.md's packet
// shape: `u16 magic 'JM'`).
inline constexpr std::uint16_t kMagic = 0x4D4A;

// This is the value docs/adr/0001-protocol-v0-1.md locks in - a change to
// either constant needs its own new ADR, per that file's own precedent.
inline constexpr std::uint8_t kCurrentProtoMajor = 1;
inline constexpr std::uint8_t kCurrentProtoMinor = 0;

// The 8-byte little-endian header docs/PROTOCOL.md's "Packet shape" section
// specifies, preceding a run of TLVs (see tlv.h).
struct PacketHeader {
    std::uint16_t magic = kMagic;
    std::uint8_t protoMajor = kCurrentProtoMajor;
    std::uint8_t protoMinor = kCurrentProtoMinor;
    std::uint16_t peerId = 0;
    std::uint16_t bodyLen = 0;

    static constexpr std::size_t kEncodedSize = 8;
};

bool EncodePacketHeader(const PacketHeader& header, jamn::core::ByteWriter& out);
bool DecodePacketHeader(jamn::core::ByteReader& in, PacketHeader& out);

}  // namespace jamn::proto

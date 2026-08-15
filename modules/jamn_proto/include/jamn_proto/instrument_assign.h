#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"

namespace jamn::proto {

// InstrumentAssign (MessageType::kInstrumentAssign) carries a reserved,
// inert soundfont identity triple - shipped now so a later phase can give
// it real meaning without a proto_major bump. bankName is a fixed-width,
// zero-padded ASCII field (no length-prefixed string type exists in this
// protocol - docs/PROTOCOL.md rule 4 rules out anything but fixed-width
// fields going through the bounds-checked reader/writer).
struct InstrumentAssign {
    static constexpr std::size_t kBankNameBytes = 64;
    static constexpr std::size_t kSha256Bytes = 32;

    std::array<std::uint8_t, kBankNameBytes> bankName{};
    std::array<std::uint8_t, kSha256Bytes> sha256{};
    std::uint32_t preset = 0;

    static constexpr std::size_t kEncodedSize = kBankNameBytes + kSha256Bytes + 4;
};

bool EncodeInstrumentAssign(const InstrumentAssign& assign, jamn::core::ByteWriter& out);
bool DecodeInstrumentAssign(jamn::core::ByteReader& in, InstrumentAssign& out);

}  // namespace jamn::proto

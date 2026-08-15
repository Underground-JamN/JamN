#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"

namespace jamn::proto {

// The join message (MessageType::kJoin). session_token is compared against
// the host's configured passphrase (T2.6's join authentication); build_hash
// identifies the peer's build for diagnostics, not for compatibility -
// proto_major/proto_minor (packet_header.h) are what gate compatibility.
struct Hello {
    static constexpr std::size_t kSessionTokenBytes = 32;
    static constexpr std::size_t kBuildHashBytes = 32;

    std::array<std::uint8_t, kSessionTokenBytes> sessionToken{};
    std::array<std::uint8_t, kBuildHashBytes> buildHash{};
    std::uint32_t instrumentBankVersion = 0;
    std::uint32_t capabilities = 0;

    static constexpr std::size_t kEncodedSize = kSessionTokenBytes + kBuildHashBytes + 4 + 4;
};

bool EncodeHello(const Hello& hello, jamn::core::ByteWriter& out);
bool DecodeHello(jamn::core::ByteReader& in, Hello& out);

}  // namespace jamn::proto

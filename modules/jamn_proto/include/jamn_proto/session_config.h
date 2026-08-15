#pragma once

#include <cstddef>
#include <cstdint>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"

namespace jamn::proto {

// SessionConfig (MessageType::kSessionConfig) carries a reserved, inert
// SyncMode selector - shipped now so a later phase can give it real
// meaning (live vs. bar-synced session default, docs/CLOCK.md's "one
// scheduler, two resolvers") without a proto_major bump. Always 0 today;
// nothing reads a nonzero value as meaningful yet.
struct SessionConfig {
    std::uint8_t syncMode = 0;

    static constexpr std::size_t kEncodedSize = 1;
};

bool EncodeSessionConfig(const SessionConfig& config, jamn::core::ByteWriter& out);
bool DecodeSessionConfig(jamn::core::ByteReader& in, SessionConfig& out);

}  // namespace jamn::proto

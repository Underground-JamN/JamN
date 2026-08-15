#pragma once

#include <cstdint>
#include <string>

namespace jamn::proto {

// docs/PROTOCOL.md rule 3: a proto_major mismatch refuses the connection
// with a human-readable reason; a proto_minor mismatch operates at
// min(minor) rather than being refused.
struct NegotiationResult {
    bool accepted = false;
    // Only meaningful when accepted is true.
    std::uint8_t negotiatedMinor = 0;
    // Non-empty only when accepted is false.
    std::string reason;
};

// Runs once per peer at join time, off the audio thread - the std::string
// in NegotiationResult is fine for that reason (docs/RT_RULES.md's
// no-allocation rule binds the audio callback, not the network thread).
NegotiationResult NegotiateVersion(std::uint8_t peerMajor, std::uint8_t peerMinor);

}  // namespace jamn::proto

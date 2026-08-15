#pragma once

#include <cstddef>
#include <cstdint>

namespace jamn::proto {

// Constant-time byte comparison: never branches on where a and b first
// differ, so its running time doesn't leak how many leading bytes matched.
// Used to compare a peer's Hello::sessionToken against the host's
// configured passphrase - a naive compare (memcmp, std::equal, ==) exits
// early on the first mismatched byte, which lets a remote attacker recover
// the passphrase one byte at a time via timing.
bool ConstantTimeEquals(const std::uint8_t* a, const std::uint8_t* b, std::size_t len);

}  // namespace jamn::proto

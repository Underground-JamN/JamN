#pragma once

#include <cstddef>

namespace jamn::core {

// How many peers can be live at once - the bound every fixed-capacity
// per-peer array in this project is sized against: jamn_engine's scheduler
// and dedupe rings, jamn_session's roster, jamn_dsp's per-peer strips.
//
// It lives in jamn_core because that is the only module all of those
// already depend on. Putting it anywhere else would mean either a new
// module edge (jamn_dsp -> jamn_net, say, for nothing but a number) or a
// second copy of the same 8, which is exactly the drift this constant
// exists to prevent - each of those types static_asserts its own constant
// against this one, so a change here is a compile error there rather than
// a silently mismatched capacity.
//
// 8, not 6: AGENTS.md's "2-6 friends" is the design target, and the two
// spare slots absorb a peer that is still handshaking or still leaving
// while six are already playing.
inline constexpr std::size_t kMaxPeers = 8;

}  // namespace jamn::core

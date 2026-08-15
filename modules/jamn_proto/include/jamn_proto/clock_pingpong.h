#pragma once

#include <cstddef>
#include <cstdint>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"

namespace jamn::proto {

// The NTP-style round trip behind jamn_engine's ClockSync
// (MessageType::kClockPing / kClockPong, channel 1 Realtime). ClockSync
// never touches ITransport itself - it only folds in samples the caller
// already decoded - so these are the two messages a caller needs in order
// to produce one.
//
// The field names are the same t1..t4 docs/CLOCK.md and ClockSyncSample
// use, and they line up one for one: a caller receiving a ClockPong builds
// its sample from (pong.t1, pong.t2, pong.t3, localNow) with no extra
// state kept between the ping and the pong. That is why t1 is echoed back
// rather than looked up locally by pingSeq - an unsequenced channel can
// deliver a pong for a ping the sender has long since forgotten.
struct ClockPing {
    std::uint16_t pingSeq = 0;
    std::int64_t t1 = 0;  // Sender's clock when this ping was sent.

    static constexpr std::size_t kEncodedSize = 10;
};

struct ClockPong {
    std::uint16_t pingSeq = 0;
    std::int64_t t1 = 0;  // Echoed from the ping, untouched.
    std::int64_t t2 = 0;  // Responder's clock when the ping arrived.
    std::int64_t t3 = 0;  // Responder's clock when this pong was sent.

    static constexpr std::size_t kEncodedSize = 26;
};

bool EncodeClockPing(const ClockPing& ping, jamn::core::ByteWriter& out);
bool DecodeClockPing(jamn::core::ByteReader& in, ClockPing& out);

bool EncodeClockPong(const ClockPong& pong, jamn::core::ByteWriter& out);
bool DecodeClockPong(jamn::core::ByteReader& in, ClockPong& out);

}  // namespace jamn::proto

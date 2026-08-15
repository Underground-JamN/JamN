#pragma once

#include <cstdint>

namespace jamn::engine {

// Serial-number arithmetic (RFC 1982's technique) for ordering a u16
// sequence number that wraps - event_seq wraps roughly every 11 minutes at
// 100 events/s. A naive `a > b` comparison goes deaf for a moment on every
// wrap (a freshly-wrapped small value would compare as "older" than a
// large pre-wrap value it actually follows); casting the difference to
// int16_t and checking its sign gets this right on both sides of the wrap,
// as long as the true distance between the two values being compared never
// exceeds half the number space (32768) - which holds here, since nothing
// in this protocol lets two live event_seq values drift that far apart.
inline bool IsNewerSerial(std::uint16_t a, std::uint16_t b) {
    return static_cast<std::int16_t>(a - b) > 0;
}

}  // namespace jamn::engine

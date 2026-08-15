#include "jamn_proto/clock_pingpong.h"

namespace jamn::proto {

bool EncodeClockPing(const ClockPing& ping, jamn::core::ByteWriter& out) {
    return out.WriteU16(ping.pingSeq) && out.WriteI64(ping.t1);
}

bool DecodeClockPing(jamn::core::ByteReader& in, ClockPing& out) {
    ClockPing ping;
    if (!in.ReadU16(ping.pingSeq)) return false;
    if (!in.ReadI64(ping.t1)) return false;
    out = ping;
    return true;
}

bool EncodeClockPong(const ClockPong& pong, jamn::core::ByteWriter& out) {
    return out.WriteU16(pong.pingSeq) && out.WriteI64(pong.t1) && out.WriteI64(pong.t2) && out.WriteI64(pong.t3);
}

bool DecodeClockPong(jamn::core::ByteReader& in, ClockPong& out) {
    ClockPong pong;
    if (!in.ReadU16(pong.pingSeq)) return false;
    if (!in.ReadI64(pong.t1)) return false;
    if (!in.ReadI64(pong.t2)) return false;
    if (!in.ReadI64(pong.t3)) return false;
    out = pong;
    return true;
}

}  // namespace jamn::proto

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "jamn_core/byte_reader.h"

namespace jamn::net {

// The channel model docs/PROTOCOL.md's "Transport" table describes,
// expressed as a parameter rather than as separate methods - so an
// implementation (SimTransport now, EnetTransport in 0.5b) decides how
// each channel's delivery semantics map onto whatever it sits on top of,
// without ITransport itself knowing anything about ENet.
enum class Channel : std::uint8_t {
    kControl = 0,   // Reliable, ordered.
    kRealtime = 1,  // Unsequenced.
    kBulk = 2,      // Reliable, fragmented.
};

using PeerId = std::uint16_t;

// A link to one peer coming up or going down. Deliberately not "joined" or
// "left" - those are jamn_session's words for a protocol-level state
// machine, and a transport knows nothing about them. A kDisconnected here
// means the link is gone (timeout, remote close, local Disconnect), and it
// is the last thing ever reported for that PeerId until it connects again.
enum class PeerEvent : std::uint8_t {
    kConnected = 0,
    kDisconnected = 1,
};

// Everything above this seam - jamn_proto's encode/decode, jamn_engine's
// scheduler - talks only to this interface, never to a concrete transport
// directly (docs/PROTOCOL.md's "Transport" section). SimTransport is the
// deterministic in-process implementation; EnetTransport (enet_transport.h)
// is the real socket, built after SimTransport and the timing core had
// already been exercised against each other, so the clock and scheduler
// could be developed and reviewed before a real socket's nondeterminism
// complicated the diagnosis.
//
// Connection establishment is deliberately absent: Listen/Connect take an
// address, which only a real socket has, so they live on EnetTransport's
// own concrete surface rather than on a seam SimTransport would have to
// fake. What this interface does carry is the part both implementations
// can honestly provide - servicing, delivery, and link lifetime.
class ITransport {
public:
    virtual ~ITransport() = default;

    // Sends len bytes to peer over channel. What "sent" means - when it's
    // delivered, whether it can be lost, duplicated or reordered - is
    // entirely up to the implementation and the chosen channel; the caller
    // only picks the channel, never the guarantee.
    virtual bool Send(PeerId peer, Channel channel, const std::uint8_t* data, std::size_t len) = 0;

    // Services the transport: every ReceiveCallback and PeerEventCallback
    // invocation this transport will ever make happens synchronously
    // inside a Poll call, on the thread that called it, and nowhere else.
    // That is the whole threading contract, and both implementations hold
    // it - SimTransport because it has no threads at all, EnetTransport
    // because enet_host_service is a synchronous pump with no thread of
    // its own behind it. One caller, one thread: a runtime that polls from
    // exactly one thread therefore has exactly one delivery thread by
    // construction, which is what lets the net-to-audio crossing above be
    // single-producer without any further argument.
    //
    // nowUs is the caller's monotonic microsecond clock, passed in rather
    // than read here so a virtual-time harness and a real one drive the
    // same code path.
    virtual void Poll(std::int64_t nowUs) = 0;

    // Invoked once per received packet, inside Poll, on the polling thread
    // (see Poll's contract above).
    using ReceiveCallback = std::function<void(PeerId from, Channel channel, jamn::core::ByteReader& body)>;
    virtual void SetReceiveCallback(ReceiveCallback callback) = 0;

    // Invoked once per link coming up or going down, inside Poll, on the
    // polling thread (see Poll's contract above) - never from a
    // Send/Disconnect call itself, so a caller can safely mutate its own
    // roster from this callback without reentering the transport.
    using PeerEventCallback = std::function<void(PeerId peer, PeerEvent event)>;
    virtual void SetPeerEventCallback(PeerEventCallback callback) = 0;

    // Tears the link to peer down. The matching kDisconnected PeerEvent is
    // reported from a later Poll, not from inside this call. Disconnecting
    // a peer that is already gone is a no-op, not an error.
    virtual void Disconnect(PeerId peer) = 0;
};

}  // namespace jamn::net

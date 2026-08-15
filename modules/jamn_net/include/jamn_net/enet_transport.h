#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "jamn_net/transport.h"

namespace jamn::net {

// The real socket behind ITransport: one ENet host over one UDP socket and
// one port, with docs/PROTOCOL.md's channel table mapped onto ENet's packet
// flags (kControl and kBulk reliable, kRealtime explicitly UNSEQUENCED -
// ENet's plain "unreliable" is unreliable-*sequenced* and silently discards
// stale packets, which would quietly break protocol rules 1 and 2).
//
// Compiled only when JAMN_CORE_ONLY is off - the core-only preset never
// fetches ENet, so this header exists but nothing in that build includes or
// links it. No ENet type appears in this header: the whole library sits
// behind Impl, so including this from a translation unit that has no ENet
// include path still compiles.
//
// Threading contract, per ITransport::Poll: every ReceiveCallback and
// PeerEventCallback fires synchronously inside Poll, on the calling thread.
// ENet has no thread of its own - enet_host_service is a synchronous pump -
// so a runtime polling from one thread has exactly one delivery thread by
// construction. Nothing else on this class is thread-safe; Send, Poll,
// Connect, Listen and Disconnect all belong to that same thread.
//
// PeerId here is a *link* identity - the ENet peer slot index on this host,
// assigned by ENet on connect - not the protocol-level peer_id that rides
// in a PacketHeader. The two are deliberately separate: a transport must be
// able to route to a link before anything has negotiated a protocol
// identity over it. Mapping one to the other is jamn_session's job.
class EnetTransport : public ITransport {
public:
    EnetTransport();
    ~EnetTransport() override;

    EnetTransport(const EnetTransport&) = delete;
    EnetTransport& operator=(const EnetTransport&) = delete;

    // Connection establishment: on this concrete class rather than on
    // ITransport, because both take an address, which is a thing only a
    // real socket has - a seam SimTransport would have to fake.

    // Binds a host socket on 127.0.0.1:port (or every interface when
    // bindLoopbackOnly is false) and accepts up to maxPeers links. False if
    // the socket could not be created or bound.
    bool Listen(std::uint16_t port, std::size_t maxPeers, bool bindLoopbackOnly = true);

    // Creates an unbound client host and begins connecting to
    // hostName:port. Returns false only on immediate local failure - a
    // successful return means the handshake has *started*, and the link is
    // usable only once Poll reports kConnected for it.
    bool Connect(const char* hostName, std::uint16_t port);

    bool Send(PeerId peer, Channel channel, const std::uint8_t* data, std::size_t len) override;
    void Poll(std::int64_t nowUs) override;
    void SetReceiveCallback(ReceiveCallback callback) override;
    void SetPeerEventCallback(PeerEventCallback callback) override;
    void Disconnect(PeerId peer) override;

    // The largest kRealtime payload Send will accept for this peer right
    // now, in bytes: the fragment threshold read from the live ENet peer,
    // since ENet negotiates the MTU down on connect rather than always
    // using ENET_HOST_DEFAULT_MTU. Zero if the peer is unknown.
    //
    // This is a hard limit rather than advice. A packet flagged
    // UNSEQUENCED alone that exceeds the threshold does *not* become
    // unreliable-fragmented: ENet's fragmenting branch takes the unreliable
    // path only when (flags & (RELIABLE|UNRELIABLE_FRAGMENT)) ==
    // UNRELIABLE_FRAGMENT, so UNSEQUENCED alone falls through to a
    // reliable, acknowledged, head-of-line-blocking fragmented send, with
    // no error and no log. Send rejects instead.
    std::size_t MaxRealtimePayloadFor(PeerId peer) const;

    // Whether a host socket currently exists (Listen or Connect succeeded).
    bool IsOpen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace jamn::net

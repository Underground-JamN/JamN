#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "jamn_core/byte_reader.h"
#include "jamn_net/transport.h"
#include "jamn_proto/hello.h"
#include "jamn_proto/leave.h"
#include "jamn_proto/packet_header.h"
#include "jamn_session/roster.h"

namespace jamn::session {

// The result of the host's accept/refuse decision for one Join.
struct JoinOutcome {
    bool accepted = false;
    std::uint8_t negotiatedMinor = 0;
    std::uint16_t assignedPeerId = 0;
    // Non-empty only when accepted is false. Human-readable, per
    // docs/PROTOCOL.md rule 3 - this is the string Wave 6's dialog shows.
    std::string reason;
};

// Host-side authority: who is allowed in, what protocol peer_id they get,
// and when they are removed. JUCE-free and transport-agnostic - it acts on
// an ITransport but never constructs one, so the same object is driven by
// SimTransport in tests and EnetTransport in a real session.
//
// This does not install itself as the transport's callback. It exposes
// HandlePeerEvent/HandleControlPacket for whoever owns the poll loop to
// call, so the peer runtime (Wave 4) stays the single owner of the
// transport's callbacks rather than fighting this class for them.
//
// Threading: same thread as the poll loop, per ITransport::Poll's contract.
// The std::string in JoinOutcome is fine for that reason - the
// no-allocation rule binds the audio callback, not the network thread.
class SessionHost {
public:
    explicit SessionHost(jamn::net::ITransport& transport) : transport_(transport) {}

    // The passphrase a joining peer's Hello::sessionToken is compared
    // against, in constant time. Left all-zero, any token matches all-zero
    // and nothing else - an unconfigured host is not an open one.
    void SetSessionToken(const std::array<std::uint8_t, jamn::proto::Hello::kSessionTokenBytes>& token) {
        sessionToken_ = token;
    }

    // Invoked with the human-readable reason each time a join is refused.
    // Off the audio thread; a std::string here is deliberate.
    using RefusalCallback = std::function<void(jamn::net::PeerId link, const std::string& reason)>;
    void SetRefusalCallback(RefusalCallback callback) { refusalCallback_ = std::move(callback); }

    // Invoked once per accepted join, after the roster entry exists.
    using JoinCallback = std::function<void(jamn::net::PeerId link, std::uint16_t peerId)>;
    void SetJoinCallback(JoinCallback callback) { joinCallback_ = std::move(callback); }

    // Feed these from the transport's callbacks.
    void HandlePeerEvent(jamn::net::PeerId link, jamn::net::PeerEvent event);

    // Decodes one control-channel packet and acts on the messages in it.
    // Returns false only for a packet this host would not act on at all: a
    // framing-level malformity, a wrong magic, or a header whose peer_id
    // does not match what this link was assigned. **A false return is never
    // grounds for disconnecting** - protocol rule 1 forbids that, and an
    // unknown message type does not even produce one, since an unrecognised
    // TLV is skipped and the packet still reports true.
    bool HandleControlPacket(jamn::net::PeerId link, jamn::core::ByteReader& packet);

    // The decision itself, without applying it - exposed so the accept and
    // refuse paths can be tested apart from the packet plumbing.
    JoinOutcome EvaluateJoin(const jamn::proto::PacketHeader& header, const jamn::proto::Hello& hello) const;

    const Roster& roster() const { return roster_; }

private:
    // Refuses a link: reports the reason upward, best-effort tells the peer
    // it was refused, and drops the link.
    void Refuse(jamn::net::PeerId link, const std::string& reason);

    void SendLeave(jamn::net::PeerId link, std::uint16_t peerId, jamn::proto::LeaveReason reason);

    // Host-assigned protocol peer_id. Starts at 1: 0 is PacketHeader's
    // default, so a peer that never had one assigned must not collide with
    // a peer that did.
    std::uint16_t NextPeerId();

    jamn::net::ITransport& transport_;
    Roster roster_;
    std::array<std::uint8_t, jamn::proto::Hello::kSessionTokenBytes> sessionToken_{};
    std::uint16_t nextPeerId_ = 1;
    RefusalCallback refusalCallback_;
    JoinCallback joinCallback_;
};

}  // namespace jamn::session

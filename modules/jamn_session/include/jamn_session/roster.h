#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "jamn_core/session_limits.h"
#include "jamn_net/transport.h"
#include "jamn_proto/leave.h"

namespace jamn::session {

// Where a peer is in its life on this node. The states are ordered by
// progress, and every transition is explicit - there is no "probably
// joined" reading of a half-filled slot, which is the failure this state
// machine exists to prevent: a link that drops mid-handshake must leave no
// trace, not a slot that looks occupied to the roster and unknown to
// everything above it.
enum class PeerState : std::uint8_t {
    kDisconnected = 0,  // Slot is free. Not a peer at all.
    kHandshaking = 1,   // Transport link is up; no accepted Join yet.
    kJoined = 2,        // Accepted. The only state anything above may treat as a real peer.
    kLeaving = 3,       // Leave seen or sent; link not yet down.
};

struct RosterEntry {
    // The transport's link identity (ITransport's PeerId) - assigned by the
    // transport the moment the link comes up, before anything protocol-level
    // has been negotiated over it.
    jamn::net::PeerId link = 0;
    // The protocol-level peer_id that rides in a PacketHeader. Only
    // meaningful in kJoined/kLeaving: it does not exist until a Join is
    // accepted, which is exactly why it is a separate field from link
    // rather than the same number reused.
    std::uint16_t peerId = 0;
    std::uint8_t negotiatedMinor = 0;
    PeerState state = PeerState::kDisconnected;
};

// Fixed-capacity peer table with an explicit per-peer state machine.
// Allocation-free by construction - a plain array, no std::map or
// std::vector anywhere - the same discipline the rest of the JUCE-free
// modules hold, so this stays usable from any thread's budget without a
// separate argument about when it might allocate.
//
// Not thread-safe, and deliberately so: it is owned by whichever thread
// polls the transport, and every mutation here originates in a peer event
// or a control message, both of which arrive on that one thread
// (ITransport::Poll's contract).
class Roster {
public:
    static constexpr std::size_t kMaxPeers = jamn::core::kMaxPeers;

    // The transport reported kConnected. Claims a free slot in
    // kHandshaking. False if the roster is full - the caller refuses the
    // link rather than overflowing. A link already known is left as it is
    // and reported true, so a duplicate connect event is not an error.
    bool OnLinkUp(jamn::net::PeerId link);

    // The transport reported kDisconnected, or the link is being dropped
    // for any other reason. Frees the slot outright whatever state it was
    // in - including mid-handshake, where the point is that no half-joined
    // slot survives.
    void OnLinkDown(jamn::net::PeerId link);

    // A Join was accepted for this link: kHandshaking -> kJoined. False if
    // the link is unknown or is not currently handshaking, so a Join
    // replayed on an already-joined peer cannot rewrite its identity.
    bool MarkJoined(jamn::net::PeerId link, std::uint16_t peerId, std::uint8_t negotiatedMinor);

    // A Leave was seen for this link: kJoined -> kLeaving. The slot is not
    // freed here - it frees when the link actually goes down, or when the
    // caller calls OnLinkDown itself. False if the link is not joined.
    bool MarkLeaving(jamn::net::PeerId link);

    const RosterEntry* Find(jamn::net::PeerId link) const;

    // Lookup by protocol peer_id rather than link identity - what a decoded
    // PacketHeader gives you. Only ever matches a kJoined/kLeaving entry,
    // since nothing else has a meaningful peerId.
    const RosterEntry* FindByPeerId(std::uint16_t peerId) const;

    bool IsJoined(jamn::net::PeerId link) const;

    std::size_t JoinedCount() const;
    std::size_t OccupiedCount() const;
    bool IsFull() const { return OccupiedCount() >= kMaxPeers; }

    const std::array<RosterEntry, kMaxPeers>& entries() const { return entries_; }

private:
    RosterEntry* FindMutable(jamn::net::PeerId link);

    std::array<RosterEntry, kMaxPeers> entries_{};
};

}  // namespace jamn::session

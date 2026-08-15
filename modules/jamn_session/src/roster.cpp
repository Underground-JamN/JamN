#include "jamn_session/roster.h"

namespace jamn::session {

RosterEntry* Roster::FindMutable(jamn::net::PeerId link) {
    for (RosterEntry& entry : entries_) {
        if (entry.state != PeerState::kDisconnected && entry.link == link) return &entry;
    }
    return nullptr;
}

const RosterEntry* Roster::Find(jamn::net::PeerId link) const {
    for (const RosterEntry& entry : entries_) {
        if (entry.state != PeerState::kDisconnected && entry.link == link) return &entry;
    }
    return nullptr;
}

const RosterEntry* Roster::FindByPeerId(std::uint16_t peerId) const {
    for (const RosterEntry& entry : entries_) {
        const bool hasIdentity = entry.state == PeerState::kJoined || entry.state == PeerState::kLeaving;
        if (hasIdentity && entry.peerId == peerId) return &entry;
    }
    return nullptr;
}

bool Roster::OnLinkUp(jamn::net::PeerId link) {
    if (Find(link) != nullptr) return true;  // Duplicate connect event, not an error.

    for (RosterEntry& entry : entries_) {
        if (entry.state != PeerState::kDisconnected) continue;
        entry = RosterEntry{};
        entry.link = link;
        entry.state = PeerState::kHandshaking;
        return true;
    }
    return false;  // Full - the caller refuses the link rather than overflowing.
}

void Roster::OnLinkDown(jamn::net::PeerId link) {
    RosterEntry* entry = FindMutable(link);
    if (entry == nullptr) return;
    // Reset the whole entry, not just the state: a stale peerId left behind
    // in a free slot would be visible to FindByPeerId the moment that slot
    // was reused mid-handshake by a different link.
    *entry = RosterEntry{};
}

bool Roster::MarkJoined(jamn::net::PeerId link, std::uint16_t peerId, std::uint8_t negotiatedMinor) {
    RosterEntry* entry = FindMutable(link);
    if (entry == nullptr || entry->state != PeerState::kHandshaking) return false;
    entry->peerId = peerId;
    entry->negotiatedMinor = negotiatedMinor;
    entry->state = PeerState::kJoined;
    return true;
}

bool Roster::MarkLeaving(jamn::net::PeerId link) {
    RosterEntry* entry = FindMutable(link);
    if (entry == nullptr || entry->state != PeerState::kJoined) return false;
    entry->state = PeerState::kLeaving;
    return true;
}

bool Roster::IsJoined(jamn::net::PeerId link) const {
    const RosterEntry* entry = Find(link);
    return entry != nullptr && entry->state == PeerState::kJoined;
}

std::size_t Roster::JoinedCount() const {
    std::size_t count = 0;
    for (const RosterEntry& entry : entries_) {
        if (entry.state == PeerState::kJoined) ++count;
    }
    return count;
}

std::size_t Roster::OccupiedCount() const {
    std::size_t count = 0;
    for (const RosterEntry& entry : entries_) {
        if (entry.state != PeerState::kDisconnected) ++count;
    }
    return count;
}

}  // namespace jamn::session

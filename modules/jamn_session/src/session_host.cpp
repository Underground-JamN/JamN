#include "jamn_session/session_host.h"

#include <array>
#include <cstddef>

#include "jamn_core/byte_writer.h"
#include "jamn_proto/join_auth.h"
#include "jamn_proto/message_type.h"
#include "jamn_proto/negotiation.h"
#include "jamn_proto/packet.h"
#include "jamn_proto/tlv.h"

namespace jamn::session {
namespace {

using namespace jamn::proto;

// Control-channel messages are small and fixed - the largest this host ever
// writes is a Leave. A stack buffer keeps the send path allocation-free even
// though nothing here runs on the audio thread; it costs nothing and means
// this class never needs revisiting if that ever changes.
constexpr std::size_t kControlPacketBufferBytes = 128;

bool WriteOneTlvPacket(jamn::core::ByteWriter& out, std::uint16_t peerId, MessageType type,
                       const std::uint8_t* value, std::uint16_t valueLen) {
    PacketHeader header;
    header.peerId = peerId;
    header.bodyLen = static_cast<std::uint16_t>(4 + valueLen);  // TLV header + value.
    if (!EncodePacketHeader(header, out)) return false;
    if (!WriteTlvHeader(out, static_cast<std::uint16_t>(type), valueLen)) return false;
    return out.WriteBytes(value, valueLen);
}

}  // namespace

std::uint16_t SessionHost::NextPeerId() {
    const std::uint16_t assigned = nextPeerId_;
    // Wrap back to 1 rather than to 0, for the reason the header gives.
    nextPeerId_ = (nextPeerId_ == 0xFFFF) ? 1 : static_cast<std::uint16_t>(nextPeerId_ + 1);
    return assigned;
}

void SessionHost::HandlePeerEvent(jamn::net::PeerId link, jamn::net::PeerEvent event) {
    switch (event) {
        case jamn::net::PeerEvent::kConnected:
            if (!roster_.OnLinkUp(link)) {
                // Full. Refused before any Join is even read - the roster
                // never overflows, and the refusal is a capacity decision
                // rather than anything the joining peer did wrong.
                Refuse(link, "Session is full");
            }
            break;

        case jamn::net::PeerEvent::kDisconnected:
            // Frees the slot whatever state it held, including
            // mid-handshake: no half-joined entry survives a dropped link.
            roster_.OnLinkDown(link);
            break;
    }
}

JoinOutcome SessionHost::EvaluateJoin(const PacketHeader& header, const Hello& hello) const {
    JoinOutcome outcome;

    const NegotiationResult negotiation = NegotiateVersion(header.protoMajor, header.protoMinor);
    if (!negotiation.accepted) {
        outcome.reason = negotiation.reason;
        return outcome;
    }

    // Constant time, so how many leading bytes matched never leaks through
    // how long this took.
    if (!ConstantTimeEquals(hello.sessionToken.data(), sessionToken_.data(), sessionToken_.size())) {
        outcome.reason = "Wrong session passphrase";
        return outcome;
    }

    outcome.accepted = true;
    outcome.negotiatedMinor = negotiation.negotiatedMinor;
    return outcome;
}

void SessionHost::SendLeave(jamn::net::PeerId link, std::uint16_t peerId, LeaveReason reason) {
    Leave leave;
    leave.peerId = peerId;
    leave.reason = reason;

    std::array<std::uint8_t, Leave::kEncodedSize> value{};
    jamn::core::ByteWriter valueWriter(value.data(), value.size());
    if (!EncodeLeave(leave, valueWriter)) return;

    std::array<std::uint8_t, kControlPacketBufferBytes> packet{};
    jamn::core::ByteWriter packetWriter(packet.data(), packet.size());
    if (!WriteOneTlvPacket(packetWriter, peerId, MessageType::kLeave, value.data(),
                            static_cast<std::uint16_t>(valueWriter.Position()))) {
        return;
    }
    transport_.Send(link, jamn::net::Channel::kControl, packet.data(), packetWriter.Position());
}

void SessionHost::Refuse(jamn::net::PeerId link, const std::string& reason) {
    if (refusalCallback_) refusalCallback_(link, reason);

    // Best-effort: tells the peer it was refused rather than leaving it to
    // infer a refusal from a link that simply went quiet. The reason *text*
    // does not travel - no message type carries a string today, and
    // inventing one is a protocol decision, not this task's. A peer whose
    // proto_major we just rejected may not parse this at all; the header
    // framing is fixed by rule 4, so it costs nothing to try.
    const RosterEntry* entry = roster_.Find(link);
    SendLeave(link, entry != nullptr ? entry->peerId : 0, LeaveReason::kKicked);

    roster_.OnLinkDown(link);
    transport_.Disconnect(link);
}

bool SessionHost::HandleControlPacket(jamn::net::PeerId link, jamn::core::ByteReader& packet) {
    PacketHeader header;
    jamn::core::ByteReader body(nullptr, 0);
    if (!DecodePacketHeader(packet, header)) return false;
    if (header.magic != kMagic) return false;
    if (!packet.ReadSlice(body, header.bodyLen)) return false;

    const RosterEntry* entry = roster_.Find(link);
    if (entry == nullptr) return false;  // No link state at all - nothing to act on.

    const bool joined = entry->state == PeerState::kJoined || entry->state == PeerState::kLeaving;
    if (joined && header.peerId != entry->peerId) {
        // A joined link may only speak as the peer_id it was assigned.
        // Dropped, never disconnected - rule 1 is about unknown *types*,
        // but the same reasoning applies: a stale packet in flight across a
        // rejoin is not misbehaviour worth tearing a link down for.
        return false;
    }

    // Refusals are decided inside the visitor but acted on afterward:
    // Refuse() frees the roster slot and disconnects, and doing that while
    // ForEachTlv is still walking the body would leave the loop reading
    // against state it had already invalidated.
    bool refuse = false;
    std::string refusalReason;
    bool joinAccepted = false;
    std::uint16_t assignedPeerId = 0;
    std::uint8_t negotiatedMinor = 0;

    const bool wellFormed = ForEachTlv(body, [&](std::uint16_t type, jamn::core::ByteReader& value) {
        switch (static_cast<MessageType>(type)) {
            case MessageType::kJoin: {
                if (entry->state != PeerState::kHandshaking) return;  // Replayed Join - ignore.
                Hello hello;
                if (!DecodeHello(value, hello)) {
                    refuse = true;
                    refusalReason = "Malformed join message";
                    return;
                }
                const JoinOutcome outcome = EvaluateJoin(header, hello);
                if (!outcome.accepted) {
                    refuse = true;
                    refusalReason = outcome.reason;
                    return;
                }
                joinAccepted = true;
                assignedPeerId = NextPeerId();
                negotiatedMinor = outcome.negotiatedMinor;
                break;
            }

            case MessageType::kLeave: {
                if (!joined) return;
                Leave leave;
                if (!DecodeLeave(value, leave)) return;
                roster_.MarkLeaving(link);
                break;
            }

            default:
                // Rule 1: an unknown message type is skipped and the peer
                // stays connected. ForEachTlv has already advanced past the
                // value, so doing nothing here *is* the skip. This is the
                // one branch that must never grow a disconnect.
                break;
        }
    });

    if (refuse) {
        Refuse(link, refusalReason);
        return true;  // Acted on, even though the peer is now gone.
    }

    if (joinAccepted) {
        roster_.MarkJoined(link, assignedPeerId, negotiatedMinor);
        if (joinCallback_) joinCallback_(link, assignedPeerId);
    }

    return wellFormed;
}

}  // namespace jamn::session

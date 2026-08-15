// Real sockets, not a simulation: every case here binds a UDP port on
// 127.0.0.1 and runs ENet's actual handshake. That is why these carry the
// `net` label and not `fast` - the fast list must be identical under
// core-only (where ENet is never fetched) and the full build, and both gate
// scripts prove that by running -L fast twice.
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "jamn_net/enet_transport.h"

using namespace jamn::net;

namespace {

struct Received {
    PeerId from;
    Channel channel;
    std::vector<std::uint8_t> body;
};

// Ports are per-case so two cases in one binary can never collide, and so a
// re-run does not depend on the previous run's socket having gone away.
constexpr std::uint16_t kPortExchange = 47901;
constexpr std::uint16_t kPortOversized = 47902;
constexpr std::uint16_t kPortDisconnect = 47903;

// Polls both ends until predicate holds or the deadline passes. Wall-clock
// bounded rather than iteration-bounded: this is a real network, so "how
// many polls" is not a meaningful unit, and a hung handshake must fail the
// test rather than spin forever.
template <typename Predicate>
bool PumpUntil(EnetTransport& a, EnetTransport& b, Predicate predicate, int timeoutMs = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        a.Poll(0);
        b.Poll(0);
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// A connected host/client pair on 127.0.0.1, with each side's PeerId for
// the other resolved from the kConnected peer event.
struct ConnectedPair {
    EnetTransport host;
    EnetTransport client;
    PeerId hostsIdForClient = 0;
    PeerId clientsIdForHost = 0;
    bool hostSawConnect = false;
    bool clientSawConnect = false;
};

bool EstablishPair(ConnectedPair& pair, std::uint16_t port) {
    if (!pair.host.Listen(port, /*maxPeers=*/4)) return false;

    pair.host.SetPeerEventCallback([&pair](PeerId peer, PeerEvent event) {
        if (event == PeerEvent::kConnected) {
            pair.hostsIdForClient = peer;
            pair.hostSawConnect = true;
        }
    });
    pair.client.SetPeerEventCallback([&pair](PeerId peer, PeerEvent event) {
        if (event == PeerEvent::kConnected) {
            pair.clientsIdForHost = peer;
            pair.clientSawConnect = true;
        }
    });

    if (!pair.client.Connect("127.0.0.1", port)) return false;
    return PumpUntil(pair.host, pair.client, [&pair] { return pair.hostSawConnect && pair.clientSawConnect; });
}

}  // namespace

TEST_CASE("Two EnetTransports on 127.0.0.1 exchange a packet on every channel", "[net][enet_transport]") {
    ConnectedPair pair;
    REQUIRE(EstablishPair(pair, kPortExchange));

    std::vector<Received> atHost;
    pair.host.SetReceiveCallback([&atHost](PeerId from, Channel channel, jamn::core::ByteReader& body) {
        std::vector<std::uint8_t> bytes(body.Remaining());
        body.ReadBytes(bytes.data(), bytes.size());
        atHost.push_back({from, channel, std::move(bytes)});
    });

    const std::uint8_t control[] = {0xC0, 0x01};
    const std::uint8_t realtime[] = {0x27, 0x02};

    const std::uint8_t bulk[] = {0xB0, 0x03};
    REQUIRE(pair.client.Send(pair.clientsIdForHost, Channel::kControl, control, sizeof(control)));
    REQUIRE(pair.client.Send(pair.clientsIdForHost, Channel::kRealtime, realtime, sizeof(realtime)));
    REQUIRE(pair.client.Send(pair.clientsIdForHost, Channel::kBulk, bulk, sizeof(bulk)));

    REQUIRE(PumpUntil(pair.host, pair.client, [&atHost] { return atHost.size() >= 3; }));

    bool sawControl = false;
    bool sawRealtime = false;
    bool sawBulk = false;
    for (const Received& packet : atHost) {
        REQUIRE(packet.from == pair.hostsIdForClient);
        if (packet.channel == Channel::kControl) {
            sawControl = true;
            REQUIRE(packet.body == std::vector<std::uint8_t>{control[0], control[1]});
        } else if (packet.channel == Channel::kRealtime) {
            sawRealtime = true;
        } else if (packet.channel == Channel::kBulk) {
            sawBulk = true;
            REQUIRE(packet.body == std::vector<std::uint8_t>{bulk[0], bulk[1]});
        }
    }
    REQUIRE(sawControl);
    REQUIRE(sawRealtime);
    REQUIRE(sawBulk);
}

TEST_CASE("An oversized kRealtime Send is refused rather than handed to ENet", "[net][enet_transport]") {
    ConnectedPair pair;
    REQUIRE(EstablishPair(pair, kPortOversized));

    const std::size_t limit = pair.client.MaxRealtimePayloadFor(pair.clientsIdForHost);
    INFO("negotiated realtime payload limit: " << limit);
    REQUIRE(limit > 0);

    // Exactly at the limit is fine; one byte over is refused. If it were
    // handed to ENet instead, the UNSEQUENCED flag alone would not keep it
    // unreliable - it would be silently promoted to a reliable, ordered,
    // retransmitted fragmented send, with no error anywhere.
    std::vector<std::uint8_t> atLimit(limit, 0xAA);
    std::vector<std::uint8_t> overLimit(limit + 1, 0xAA);
    REQUIRE(pair.client.Send(pair.clientsIdForHost, Channel::kRealtime, atLimit.data(), atLimit.size()));
    REQUIRE_FALSE(pair.client.Send(pair.clientsIdForHost, Channel::kRealtime, overLimit.data(), overLimit.size()));

    // The same oversized payload on a reliable channel is accepted -
    // fragmenting is what kBulk is for, so the refusal above is specific to
    // the realtime channel's contract, not a blanket size cap.
    REQUIRE(pair.client.Send(pair.clientsIdForHost, Channel::kBulk, overLimit.data(), overLimit.size()));
}

TEST_CASE("Send and MaxRealtimePayloadFor refuse an unknown peer instead of crashing",
          "[net][enet_transport]") {
    EnetTransport transport;
    const std::uint8_t payload[] = {1};
    REQUIRE_FALSE(transport.Send(0, Channel::kControl, payload, sizeof(payload)));
    REQUIRE(transport.MaxRealtimePayloadFor(0) == 0);
    REQUIRE_FALSE(transport.IsOpen());
    transport.Disconnect(0);  // No-op, not a crash.
    transport.Poll(0);        // Likewise with no host.
}

TEST_CASE("Disconnect is reported to both ends as a kDisconnected peer event", "[net][enet_transport]") {
    ConnectedPair pair;
    REQUIRE(EstablishPair(pair, kPortDisconnect));

    bool hostSawDisconnect = false;
    bool clientSawDisconnect = false;
    pair.host.SetPeerEventCallback([&](PeerId peer, PeerEvent event) {
        if (event == PeerEvent::kDisconnected && peer == pair.hostsIdForClient) hostSawDisconnect = true;
    });
    pair.client.SetPeerEventCallback([&](PeerId peer, PeerEvent event) {
        if (event == PeerEvent::kDisconnected && peer == pair.clientsIdForHost) clientSawDisconnect = true;
    });

    pair.client.Disconnect(pair.clientsIdForHost);
    REQUIRE(PumpUntil(pair.host, pair.client, [&] { return hostSawDisconnect && clientSawDisconnect; }));

    const std::uint8_t payload[] = {1};
    REQUIRE_FALSE(pair.client.Send(pair.clientsIdForHost, Channel::kControl, payload, sizeof(payload)));
}

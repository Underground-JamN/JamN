#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <utility>
#include <vector>

#include "jamn_net/sim_transport.h"

using namespace jamn::net;

namespace {

struct ReceivedPacket {
    PeerId from;
    Channel channel;
    std::vector<std::uint8_t> body;
};

// Every callback fires inside Poll (ITransport::Poll's threading
// contract), so a test that advances virtual time must also poll the nodes
// it expects to hear from - Advance() alone only moves packets out of
// flight and into each destination's ready queue.
void PollAll(std::initializer_list<SimTransport*> nodes) {
    for (SimTransport* node : nodes) node->Poll(0);
}

}  // namespace

TEST_CASE("SimTransport delivers a sent packet to the destination node", "[net][sim_transport][fast]") {
    SimNetwork net(/*seed=*/1);
    SimTransport& a = net.CreateNode(1);
    SimTransport& b = net.CreateNode(2);

    std::vector<ReceivedPacket> received;
    b.SetReceiveCallback([&](PeerId from, Channel channel, jamn::core::ByteReader& body) {
        std::vector<std::uint8_t> bytes(body.Remaining());
        body.ReadBytes(bytes.data(), bytes.size());
        received.push_back({from, channel, std::move(bytes)});
    });

    const std::uint8_t payload[] = {1, 2, 3};
    REQUIRE(a.Send(2, Channel::kControl, payload, sizeof(payload)));
    b.Poll(0);
    REQUIRE(received.empty());  // Not delivered until time advances.

    net.Advance(1);
    b.Poll(0);
    REQUIRE(received.size() == 1);
    REQUIRE(received[0].from == 1);
    REQUIRE(received[0].channel == Channel::kControl);
    REQUIRE(received[0].body == std::vector<std::uint8_t>{1, 2, 3});
}

TEST_CASE("SimTransport::Send fails for an unknown peer", "[net][sim_transport][fast]") {
    SimNetwork net(/*seed=*/1);
    SimTransport& a = net.CreateNode(1);
    const std::uint8_t payload[] = {1};
    REQUIRE_FALSE(a.Send(999, Channel::kControl, payload, sizeof(payload)));
}

TEST_CASE("A configured one-way delay is applied independently per direction", "[net][sim_transport][fast]") {
    SimNetwork net(/*seed=*/1);
    SimTransport& a = net.CreateNode(1);
    SimTransport& b = net.CreateNode(2);
    net.SetLinkConfig(1, 2, LinkConfig{.delayUs = 1000});
    net.SetLinkConfig(2, 1, LinkConfig{.delayUs = 5000});

    int aToBDeliveredAt = -1;
    int bToADeliveredAt = -1;
    b.SetReceiveCallback([&](PeerId, Channel, jamn::core::ByteReader&) {
        aToBDeliveredAt = static_cast<int>(net.Now().us());
    });
    a.SetReceiveCallback(
        [&](PeerId, Channel, jamn::core::ByteReader&) { bToADeliveredAt = static_cast<int>(net.Now().us()); });

    const std::uint8_t payload[] = {0};
    REQUIRE(a.Send(2, Channel::kRealtime, payload, sizeof(payload)));
    REQUIRE(b.Send(1, Channel::kRealtime, payload, sizeof(payload)));

    net.Advance(1000);
    PollAll({&a, &b});
    REQUIRE(aToBDeliveredAt == 1000);
    REQUIRE(bToADeliveredAt == -1);  // The 2->1 direction's longer delay hasn't elapsed yet.

    net.Advance(4000);
    PollAll({&a, &b});
    REQUIRE(bToADeliveredAt == 5000);
}

TEST_CASE("The same seed produces an identical delivery order across two runs in one process",
          "[net][sim_transport][fast]") {
    auto runOnce = [](std::uint64_t seed) {
        SimNetwork net(seed);
        SimTransport& a = net.CreateNode(1);
        SimTransport& b = net.CreateNode(2);
        SimTransport& c = net.CreateNode(3);
        net.SetLinkConfig(1, 3, LinkConfig{.delayUs = 10000, .jitterUs = 5000});
        net.SetLinkConfig(2, 3, LinkConfig{.delayUs = 10000, .jitterUs = 5000});

        std::vector<PeerId> arrivalOrder;
        c.SetReceiveCallback(
            [&](PeerId from, Channel, jamn::core::ByteReader&) { arrivalOrder.push_back(from); });

        for (int i = 0; i < 50; ++i) {
            const std::uint8_t payload[] = {static_cast<std::uint8_t>(i)};
            REQUIRE(a.Send(3, Channel::kRealtime, payload, sizeof(payload)));
            REQUIRE(b.Send(3, Channel::kRealtime, payload, sizeof(payload)));
            net.Advance(200);
            c.Poll(0);
        }
        net.Advance(20000);  // Drain anything still in flight.
        c.Poll(0);
        return arrivalOrder;
    };

    const auto first = runOnce(0xABCD1234);
    const auto second = runOnce(0xABCD1234);
    REQUIRE(first == second);
    REQUIRE_FALSE(first.empty());
}

TEST_CASE("A lossProbability of 1.0 means the packet is never delivered", "[net][sim_transport][fast]") {
    SimNetwork net(/*seed=*/7);
    SimTransport& a = net.CreateNode(1);
    SimTransport& b = net.CreateNode(2);
    net.SetLinkConfig(1, 2, LinkConfig{.lossProbability = 1.0});

    int deliveries = 0;
    b.SetReceiveCallback([&](PeerId, Channel, jamn::core::ByteReader&) { ++deliveries; });

    const std::uint8_t payload[] = {1};
    REQUIRE(a.Send(2, Channel::kRealtime, payload, sizeof(payload)));  // Still "sent" locally.
    net.Advance(100000);
    b.Poll(0);
    REQUIRE(deliveries == 0);
}

TEST_CASE("A duplicateProbability of 1.0 delivers the packet twice", "[net][sim_transport][fast]") {
    SimNetwork net(/*seed=*/7);
    SimTransport& a = net.CreateNode(1);
    SimTransport& b = net.CreateNode(2);
    net.SetLinkConfig(1, 2, LinkConfig{.duplicateProbability = 1.0});

    int deliveries = 0;
    b.SetReceiveCallback([&](PeerId, Channel, jamn::core::ByteReader&) { ++deliveries; });

    const std::uint8_t payload[] = {1};
    REQUIRE(a.Send(2, Channel::kRealtime, payload, sizeof(payload)));
    net.Advance(1);
    b.Poll(0);
    REQUIRE(deliveries == 2);
}

// T1.1: peer events and Disconnect.

TEST_CASE("Connect reports kConnected to both ends, from Poll and not before",
          "[net][sim_transport][peer_event][fast]") {
    SimNetwork net(/*seed=*/1);
    SimTransport& a = net.CreateNode(1);
    SimTransport& b = net.CreateNode(2);

    std::vector<std::pair<PeerId, PeerEvent>> aEvents;
    std::vector<std::pair<PeerId, PeerEvent>> bEvents;
    a.SetPeerEventCallback([&](PeerId peer, PeerEvent event) { aEvents.emplace_back(peer, event); });
    b.SetPeerEventCallback([&](PeerId peer, PeerEvent event) { bEvents.emplace_back(peer, event); });

    net.Connect(1, 2);
    REQUIRE(aEvents.empty());  // Nothing fires outside Poll.
    REQUIRE(bEvents.empty());

    PollAll({&a, &b});
    REQUIRE(aEvents == std::vector<std::pair<PeerId, PeerEvent>>{{2, PeerEvent::kConnected}});
    REQUIRE(bEvents == std::vector<std::pair<PeerId, PeerEvent>>{{1, PeerEvent::kConnected}});
}

TEST_CASE("Disconnect reports kDisconnected to both ends and stops routing between them",
          "[net][sim_transport][peer_event][fast]") {
    SimNetwork net(/*seed=*/1);
    SimTransport& a = net.CreateNode(1);
    SimTransport& b = net.CreateNode(2);

    std::vector<std::pair<PeerId, PeerEvent>> aEvents;
    std::vector<std::pair<PeerId, PeerEvent>> bEvents;
    a.SetPeerEventCallback([&](PeerId peer, PeerEvent event) { aEvents.emplace_back(peer, event); });
    b.SetPeerEventCallback([&](PeerId peer, PeerEvent event) { bEvents.emplace_back(peer, event); });

    int deliveries = 0;
    b.SetReceiveCallback([&](PeerId, Channel, jamn::core::ByteReader&) { ++deliveries; });

    a.Disconnect(2);
    REQUIRE(aEvents.empty());  // Reported from a later Poll, not from the Disconnect call.
    PollAll({&a, &b});
    REQUIRE(aEvents == std::vector<std::pair<PeerId, PeerEvent>>{{2, PeerEvent::kDisconnected}});
    REQUIRE(bEvents == std::vector<std::pair<PeerId, PeerEvent>>{{1, PeerEvent::kDisconnected}});

    const std::uint8_t payload[] = {1};
    REQUIRE_FALSE(a.Send(2, Channel::kControl, payload, sizeof(payload)));
    net.Advance(1000);
    PollAll({&a, &b});
    REQUIRE(deliveries == 0);
}

TEST_CASE("Disconnecting an already-disconnected peer is a no-op, not a repeated event",
          "[net][sim_transport][peer_event][fast]") {
    SimNetwork net(/*seed=*/1);
    SimTransport& a = net.CreateNode(1);
    SimTransport& b = net.CreateNode(2);

    int aEventCount = 0;
    a.SetPeerEventCallback([&](PeerId, PeerEvent) { ++aEventCount; });

    a.Disconnect(2);
    a.Disconnect(2);
    b.Disconnect(1);
    PollAll({&a, &b});
    REQUIRE(aEventCount == 1);
}

TEST_CASE("A packet still in flight when the link is severed is dropped, not delivered late",
          "[net][sim_transport][peer_event][fast]") {
    SimNetwork net(/*seed=*/1);
    SimTransport& a = net.CreateNode(1);
    SimTransport& b = net.CreateNode(2);
    net.SetLinkConfig(1, 2, LinkConfig{.delayUs = 10000});

    int deliveries = 0;
    b.SetReceiveCallback([&](PeerId, Channel, jamn::core::ByteReader&) { ++deliveries; });

    const std::uint8_t payload[] = {1};
    REQUIRE(a.Send(2, Channel::kControl, payload, sizeof(payload)));
    a.Disconnect(2);
    net.Advance(50000);
    PollAll({&a, &b});
    REQUIRE(deliveries == 0);
}

// T1.2: channel reliability.

TEST_CASE("A lossProbability of 1.0 still delivers every kControl packet while dropping every kRealtime one",
          "[net][sim_transport][channel_reliability][fast]") {
    SimNetwork net(/*seed=*/11);
    SimTransport& a = net.CreateNode(1);
    SimTransport& b = net.CreateNode(2);
    net.SetLinkConfig(1, 2, LinkConfig{.lossProbability = 1.0});

    int controlDeliveries = 0;
    int bulkDeliveries = 0;
    int realtimeDeliveries = 0;
    b.SetReceiveCallback([&](PeerId, Channel channel, jamn::core::ByteReader&) {
        if (channel == Channel::kControl) ++controlDeliveries;
        if (channel == Channel::kBulk) ++bulkDeliveries;
        if (channel == Channel::kRealtime) ++realtimeDeliveries;
    });

    const std::uint8_t payload[] = {1};
    for (int i = 0; i < 20; ++i) {
        REQUIRE(a.Send(2, Channel::kControl, payload, sizeof(payload)));
        REQUIRE(a.Send(2, Channel::kBulk, payload, sizeof(payload)));
        REQUIRE(a.Send(2, Channel::kRealtime, payload, sizeof(payload)));
    }
    net.Advance(1);
    b.Poll(0);

    REQUIRE(controlDeliveries == 20);
    REQUIRE(bulkDeliveries == 20);
    REQUIRE(realtimeDeliveries == 0);
}

TEST_CASE("kControl arrives in send order under jitter that reorders kRealtime",
          "[net][sim_transport][channel_reliability][fast]") {
    SimNetwork net(/*seed=*/0xBEEF);
    SimTransport& a = net.CreateNode(1);
    SimTransport& b = net.CreateNode(2);
    net.SetLinkConfig(1, 2, LinkConfig{.delayUs = 20000, .jitterUs = 15000});

    std::vector<std::uint8_t> controlOrder;
    std::vector<std::uint8_t> realtimeOrder;
    b.SetReceiveCallback([&](PeerId, Channel channel, jamn::core::ByteReader& body) {
        std::uint8_t value = 0;
        REQUIRE(body.ReadU8(value));
        (channel == Channel::kControl ? controlOrder : realtimeOrder).push_back(value);
    });

    constexpr int kCount = 40;
    for (int i = 0; i < kCount; ++i) {
        const std::uint8_t payload[] = {static_cast<std::uint8_t>(i)};
        REQUIRE(a.Send(2, Channel::kControl, payload, sizeof(payload)));
        REQUIRE(a.Send(2, Channel::kRealtime, payload, sizeof(payload)));
        net.Advance(1000);
        b.Poll(0);
    }
    net.Advance(100000);
    b.Poll(0);

    std::vector<std::uint8_t> expected;
    for (int i = 0; i < kCount; ++i) expected.push_back(static_cast<std::uint8_t>(i));
    REQUIRE(controlOrder == expected);

    // The point of the case: the same jitter that left kControl in order
    // did genuinely reorder kRealtime, so the kControl assertion above is
    // evidence of the channel split rather than of a jitter setting too
    // mild to reorder anything.
    REQUIRE(realtimeOrder != expected);
}

TEST_CASE("A duplicateProbability of 1.0 never duplicates a reliable channel",
          "[net][sim_transport][channel_reliability][fast]") {
    SimNetwork net(/*seed=*/7);
    SimTransport& a = net.CreateNode(1);
    SimTransport& b = net.CreateNode(2);
    net.SetLinkConfig(1, 2, LinkConfig{.duplicateProbability = 1.0});

    int deliveries = 0;
    b.SetReceiveCallback([&](PeerId, Channel, jamn::core::ByteReader&) { ++deliveries; });

    const std::uint8_t payload[] = {1};
    REQUIRE(a.Send(2, Channel::kBulk, payload, sizeof(payload)));
    net.Advance(1);
    b.Poll(0);
    REQUIRE(deliveries == 1);
}

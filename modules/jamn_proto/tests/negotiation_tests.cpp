#include <catch2/catch_test_macros.hpp>
#include <algorithm>

#include "jamn_proto/negotiation.h"
#include "jamn_proto/packet_header.h"

using jamn::proto::kCurrentProtoMajor;
using jamn::proto::kCurrentProtoMinor;
using jamn::proto::NegotiateVersion;

TEST_CASE("NegotiateVersion accepts a matching major and minor", "[proto][negotiation][fast]") {
    const auto result = NegotiateVersion(kCurrentProtoMajor, kCurrentProtoMinor);
    REQUIRE(result.accepted);
    REQUIRE(result.negotiatedMinor == kCurrentProtoMinor);
    REQUIRE(result.reason.empty());
}

TEST_CASE("NegotiateVersion refuses a proto_major mismatch with a non-empty reason", "[proto][negotiation][fast]") {
    const auto result = NegotiateVersion(static_cast<std::uint8_t>(kCurrentProtoMajor + 1), kCurrentProtoMinor);
    REQUIRE_FALSE(result.accepted);
    REQUIRE_FALSE(result.reason.empty());
}

TEST_CASE("NegotiateVersion operates at min(minor) on a minor-only mismatch", "[proto][negotiation][fast]") {
    // A peer on a newer minor than us negotiates down to ours - kCurrentProtoMinor
    // is 0 today, so this is the only direction actually exercisable until a
    // later minor bump lands.
    const auto result = NegotiateVersion(kCurrentProtoMajor, static_cast<std::uint8_t>(kCurrentProtoMinor + 5));
    REQUIRE(result.accepted);
    REQUIRE(result.negotiatedMinor == kCurrentProtoMinor);
    REQUIRE(result.negotiatedMinor == std::min(kCurrentProtoMinor, static_cast<std::uint8_t>(kCurrentProtoMinor + 5)));
}

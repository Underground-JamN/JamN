#include <catch2/catch_test_macros.hpp>
#include <array>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"
#include "jamn_proto/session_config.h"

using jamn::core::ByteReader;
using jamn::core::ByteWriter;
using jamn::proto::DecodeSessionConfig;
using jamn::proto::EncodeSessionConfig;
using jamn::proto::SessionConfig;

TEST_CASE("SessionConfig round-trips its reserved syncMode field", "[proto][session_config][fast]") {
    SessionConfig in;
    in.syncMode = 5;  // Reserved value - no meaning is assigned to it yet.

    std::array<std::uint8_t, SessionConfig::kEncodedSize> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodeSessionConfig(in, w));
    REQUIRE(w.Position() == SessionConfig::kEncodedSize);

    ByteReader r(buf.data(), w.Position());
    SessionConfig out;
    REQUIRE(DecodeSessionConfig(r, out));
    REQUIRE(out.syncMode == 5);
}

TEST_CASE("SessionConfig defaults syncMode to zero", "[proto][session_config][fast]") {
    REQUIRE(SessionConfig().syncMode == 0);
}

TEST_CASE("SessionConfig decode fails cleanly on an empty buffer", "[proto][session_config][fast]") {
    ByteReader r(nullptr, 0);
    SessionConfig out;
    REQUIRE_FALSE(DecodeSessionConfig(r, out));
}

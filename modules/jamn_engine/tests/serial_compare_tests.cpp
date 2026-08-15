#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "jamn_engine/serial_compare.h"

using jamn::engine::IsNewerSerial;

TEST_CASE("IsNewerSerial orders normally, well away from the wrap", "[engine][serial_compare][fast]") {
    REQUIRE(IsNewerSerial(101, 100));
    REQUIRE_FALSE(IsNewerSerial(100, 101));
    REQUIRE_FALSE(IsNewerSerial(100, 100));
}

TEST_CASE("IsNewerSerial correctly orders event_seq across the u16 wrap", "[engine][serial_compare][fast]") {
    // A naive `a > b` would call 5 "older" than 65530, since as plain
    // unsigned integers 5 < 65530 - but 5 comes right after 65530 in
    // sequence-number order (65530, 65531, ..., 65535, 0, 1, ..., 5).
    REQUIRE(IsNewerSerial(5, 65530));
    REQUIRE_FALSE(IsNewerSerial(65530, 5));

    REQUIRE(IsNewerSerial(0, 65535));
    REQUIRE_FALSE(IsNewerSerial(65535, 0));
}

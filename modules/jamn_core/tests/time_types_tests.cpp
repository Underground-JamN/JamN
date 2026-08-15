#include <catch2/catch_test_macros.hpp>
#include <type_traits>

#include "jamn_core/time_types.h"

using jamn::core::kTicksPerQuarter;
using jamn::core::MusicalTime;
using jamn::core::SampleTime;
using jamn::core::SessionTime;

// The enforcement the plan's acceptance criterion asks for, checkable by a
// reviewer as more than a comment: none of the three time types converts
// implicitly to another, nor to plain int64_t, in either direction.
static_assert(!std::is_convertible_v<SessionTime, SampleTime>);
static_assert(!std::is_convertible_v<SessionTime, MusicalTime>);
static_assert(!std::is_convertible_v<SampleTime, SessionTime>);
static_assert(!std::is_convertible_v<SampleTime, MusicalTime>);
static_assert(!std::is_convertible_v<MusicalTime, SessionTime>);
static_assert(!std::is_convertible_v<MusicalTime, SampleTime>);
static_assert(!std::is_convertible_v<SessionTime, std::int64_t>);
static_assert(!std::is_convertible_v<SampleTime, std::int64_t>);
static_assert(!std::is_convertible_v<MusicalTime, std::int64_t>);
static_assert(!std::is_convertible_v<std::int64_t, SessionTime>);
static_assert(!std::is_convertible_v<std::int64_t, SampleTime>);
static_assert(!std::is_convertible_v<std::int64_t, MusicalTime>);

TEST_CASE("Time types are not implicitly convertible to each other or to int64_t", "[core][time_types][fast]") {
    // Compile-time checks above are the real assertion; this case exists so
    // the enforcement shows up as a named, runnable test rather than only a
    // build-time fact nobody sees pass.
    SUCCEED("static_assert block above compiled");
}

TEST_CASE("SessionTime round-trips through us() and supports arithmetic", "[core][time_types][fast]") {
    const SessionTime a(1000);
    REQUIRE(a.us() == 1000);

    const SessionTime b = a + 500;
    REQUIRE(b.us() == 1500);
    REQUIRE((b - a) == 500);

    REQUIRE(a < b);
    REQUIRE(a != b);
    REQUIRE(SessionTime(1000) == a);
}

TEST_CASE("SampleTime round-trips through samples() and supports arithmetic", "[core][time_types][fast]") {
    const SampleTime a(48000);
    REQUIRE(a.samples() == 48000);

    const SampleTime b = a + 128;
    REQUIRE(b.samples() == 48128);
    REQUIRE((b - a) == 128);
    REQUIRE(a < b);
}

TEST_CASE("MusicalTime round-trips through ticks() and kTicksPerQuarter is 960", "[core][time_types][fast]") {
    REQUIRE(kTicksPerQuarter == 960);

    const MusicalTime a(kTicksPerQuarter);
    REQUIRE(a.ticks() == 960);

    const MusicalTime b = a + kTicksPerQuarter;
    REQUIRE(b.ticks() == 1920);
    REQUIRE((b - a) == kTicksPerQuarter);
    REQUIRE(a < b);
}

TEST_CASE("Default-constructed time types are zero", "[core][time_types][fast]") {
    REQUIRE(SessionTime().us() == 0);
    REQUIRE(SampleTime().samples() == 0);
    REQUIRE(MusicalTime().ticks() == 0);
}

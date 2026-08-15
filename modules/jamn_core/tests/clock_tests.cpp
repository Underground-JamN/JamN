#include <catch2/catch_test_macros.hpp>
#include <thread>

#include "jamn_core/sim_clock.h"
#include "jamn_core/steady_clock.h"

using jamn::core::SessionTime;
using jamn::core::SimClock;
using jamn::core::SteadyClock;

TEST_CASE("SimClock starts at its constructed value", "[core][clock][fast]") {
    SimClock clock(SessionTime(1'000'000));
    REQUIRE(clock.nowUs() == SessionTime(1'000'000));

    SimClock defaulted;
    REQUIRE(defaulted.nowUs() == SessionTime(0));
}

TEST_CASE("SimClock's time does not advance except when explicitly advanced", "[core][clock][fast]") {
    SimClock clock;
    const SessionTime before = clock.nowUs();
    // No call to Advance() in between - reading nowUs() twice must not move
    // the clock on its own, unlike a real wall clock.
    const SessionTime still = clock.nowUs();
    REQUIRE(before == still);

    clock.Advance(500);
    REQUIRE(clock.nowUs() == SessionTime(500));
}

TEST_CASE("SimClock never runs backwards", "[core][clock][fast]") {
    SimClock clock(SessionTime(1000));
    clock.Advance(-500);
    // A negative delta must not move the clock backwards - it clamps to no
    // movement rather than decreasing.
    REQUIRE(clock.nowUs() == SessionTime(1000));

    clock.Advance(200);
    REQUIRE(clock.nowUs() == SessionTime(1200));
    clock.Advance(-1'000'000);
    REQUIRE(clock.nowUs() == SessionTime(1200));
}

TEST_CASE("SimClock advances by exactly the requested delta, cumulatively", "[core][clock][fast]") {
    SimClock clock;
    clock.Advance(100);
    clock.Advance(250);
    clock.Advance(1);
    REQUIRE(clock.nowUs() == SessionTime(351));
}

TEST_CASE("SteadyClock reports a non-negative, monotonically non-decreasing time", "[core][clock][fast]") {
    SteadyClock clock;
    const SessionTime a = clock.nowUs();
    REQUIRE(a.us() >= 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const SessionTime b = clock.nowUs();
    REQUIRE(b >= a);
}

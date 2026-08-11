#include <catch2/catch_test_macros.hpp>

#include "jamn_core/spsc_ring.h"

using jamn::core::SpscRing;

TEST_CASE("SpscRing preserves FIFO order", "[core][spsc_ring][fast]") {
    SpscRing<int, 4> ring;
    REQUIRE(ring.Push(1));
    REQUIRE(ring.Push(2));
    REQUIRE(ring.Push(3));

    int value = 0;
    REQUIRE(ring.Pop(value));
    REQUIRE(value == 1);
    REQUIRE(ring.Pop(value));
    REQUIRE(value == 2);
    REQUIRE(ring.Pop(value));
    REQUIRE(value == 3);
    REQUIRE_FALSE(ring.Pop(value));
}

TEST_CASE("SpscRing rejects Push once full", "[core][spsc_ring][fast]") {
    SpscRing<int, 2> ring;
    REQUIRE(ring.Push(1));
    REQUIRE(ring.Push(2));
    REQUIRE_FALSE(ring.Push(3));

    int value = 0;
    REQUIRE(ring.Pop(value));
    REQUIRE(value == 1);
    REQUIRE(ring.Push(3));
    REQUIRE(ring.Pop(value));
    REQUIRE(value == 2);
    REQUIRE(ring.Pop(value));
    REQUIRE(value == 3);
}

TEST_CASE("SpscRing Pop fails on an empty ring", "[core][spsc_ring][fast]") {
    SpscRing<int, 4> ring;
    int value = 0;
    REQUIRE_FALSE(ring.Pop(value));
}

TEST_CASE("SpscRing SizeApprox tracks pushes and pops", "[core][spsc_ring][fast]") {
    SpscRing<int, 4> ring;
    REQUIRE(ring.SizeApprox() == 0);
    ring.Push(1);
    ring.Push(2);
    REQUIRE(ring.SizeApprox() == 2);
    int value = 0;
    ring.Pop(value);
    REQUIRE(ring.SizeApprox() == 1);
}

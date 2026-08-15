#include <catch2/catch_test_macros.hpp>

#include "jamn_core/histogram64.h"

using jamn::core::Histogram64;

TEST_CASE("Histogram64 with no samples reports P99 of zero", "[core][histogram64][fast]") {
    Histogram64 h;
    REQUIRE(h.P99(0) == 0);
}

TEST_CASE("Histogram64 P99 matches a known synthetic distribution", "[core][histogram64][fast]") {
    Histogram64 h;
    // 99 samples clustered near 1ms, one outlier near 100ms, all recorded
    // at the same instant. The 99th percentile of 100 samples should land
    // in the cluster, not be dragged out to the outlier.
    for (int i = 0; i < 99; ++i) {
        h.Record(1000, 0);
    }
    h.Record(100'000, 0);

    const std::int64_t p99 = h.P99(0);
    REQUIRE(p99 > 500);
    REQUIRE(p99 < 5000);
}

TEST_CASE("Histogram64 P99 tracks a shifted distribution", "[core][histogram64][fast]") {
    Histogram64 low;
    for (int i = 0; i < 100; ++i) low.Record(1000, 0);

    Histogram64 high;
    for (int i = 0; i < 100; ++i) high.Record(50000, 0);

    REQUIRE(high.P99(0) > low.P99(0));
}

TEST_CASE("Histogram64 samples outside the 5-second window are not counted", "[core][histogram64][fast]") {
    Histogram64 h;
    for (int i = 0; i < 50; ++i) {
        h.Record(1000, 0);
    }
    REQUIRE(h.P99(0) > 0);

    // 10 seconds later - well past the 5-second window - the old samples
    // must no longer contribute.
    const std::int64_t muchLater = 10'000'000;
    REQUIRE(h.P99(muchLater) == 0);

    // A fresh sample recorded now should be the only thing the query sees.
    h.Record(2000, muchLater);
    const std::int64_t p99 = h.P99(muchLater);
    REQUIRE(p99 > 0);
    REQUIRE(p99 < 5000);
}

TEST_CASE("Histogram64 samples just inside the window still count", "[core][histogram64][fast]") {
    Histogram64 h;
    h.Record(1000, 0);
    // 4.9 seconds later - inside the 5-second window.
    REQUIRE(h.P99(4'900'000) > 0);
}

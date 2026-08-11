#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "jamn_bench/bench_result.h"

using Catch::Matchers::ContainsSubstring;
using jamn::bench::BenchResult;
using jamn::bench::ToJson;

TEST_CASE("ToJson of no results produces an empty results array", "[bench][bench_result][fast]") {
    const std::string json = ToJson({});
    REQUIRE_THAT(json, ContainsSubstring("\"results\": ["));
    REQUIRE_THAT(json, ContainsSubstring("]"));
}

TEST_CASE("ToJson includes backend and device as open strings, not an enum", "[bench][bench_result][fast]") {
    BenchResult result;
    result.backend = "wasapi-exclusive";
    result.device = "Focusrite Scarlett 2i2";
    result.sample_rate_hz = 48000;
    result.block_size = 128;
    result.num_blocks = 100;
    result.xruns = 0;

    const std::string json = ToJson({result});
    REQUIRE_THAT(json, ContainsSubstring("\"backend\": \"wasapi-exclusive\""));
    REQUIRE_THAT(json, ContainsSubstring("\"device\": \"Focusrite Scarlett 2i2\""));
    REQUIRE_THAT(json, ContainsSubstring("\"sample_rate_hz\": 48000"));
    REQUIRE_THAT(json, ContainsSubstring("\"xruns\": 0"));
}

TEST_CASE("ToJson escapes quotes and backslashes in device names", "[bench][bench_result][fast]") {
    BenchResult result;
    result.backend = "alsa";
    result.device = R"(hw:0,0 "USB Device" C:\path)";

    const std::string json = ToJson({result});
    REQUIRE_THAT(json, ContainsSubstring(R"(\"USB Device\")"));
    REQUIRE_THAT(json, ContainsSubstring(R"(C:\\path)"));
}

TEST_CASE("ToJson marks unmeasured xruns as -1, not 0", "[bench][bench_result][fast]") {
    BenchResult result;
    result.backend = "file-audio-device";
    result.device = "in-process, no real hardware";
    result.xruns = -1;

    const std::string json = ToJson({result});
    REQUIRE_THAT(json, ContainsSubstring("\"xruns\": -1"));
}

TEST_CASE("ToJson renders multiple results as separate rows", "[bench][bench_result][fast]") {
    BenchResult alsaDefault;
    alsaDefault.backend = "alsa";
    alsaDefault.device = "default";

    BenchResult alsaHw;
    alsaHw.backend = "alsa";
    alsaHw.device = "hw:0,0";

    const std::string json = ToJson({alsaDefault, alsaHw});
    REQUIRE_THAT(json, ContainsSubstring("\"device\": \"default\""));
    REQUIRE_THAT(json, ContainsSubstring("\"device\": \"hw:0,0\""));
}

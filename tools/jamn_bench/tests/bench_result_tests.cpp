#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstring>

#include "jamn_bench/bench_result.h"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using jamn::bench::BenchResult;
using jamn::bench::FromAudioBlockTiming;
using jamn::bench::ToJson;
using jamn::core::AudioBlockTiming;

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

TEST_CASE("ToJson marks not-applicable events_per_second as -1, not 0", "[bench][bench_result][fast]") {
    BenchResult result;
    result.backend = "file-audio-device";

    const std::string json = ToJson({result});
    REQUIRE_THAT(json, ContainsSubstring("\"events_per_second\": -1"));
}

TEST_CASE("ToJson reports a measured events_per_second for a throughput backend", "[bench][bench_result][fast]") {
    BenchResult result;
    result.backend = "event-scheduler";
    result.events_per_second = 1234567.0;

    const std::string json = ToJson({result});
    REQUIRE_THAT(json, ContainsSubstring("\"events_per_second\": 1.23457e+06"));
}

TEST_CASE("ToJson marks not-applicable clock_offset_p99_us as -1, not 0", "[bench][bench_result][fast]") {
    // Same not-measured convention as xruns and events_per_second, and it
    // matters more here than for either: 0 would read as a perfect clock.
    BenchResult result;
    result.backend = "file-audio-device";

    const std::string json = ToJson({result});
    REQUIRE_THAT(json, ContainsSubstring("\"clock_offset_p99_us\": -1"));
}

TEST_CASE("ToJson reports a measured clock_offset_p99_us for a clock backend", "[bench][bench_result][fast]") {
    BenchResult result;
    result.backend = "loopback-clock";
    result.clock_offset_p99_us = 273.0;

    const std::string json = ToJson({result});
    REQUIRE_THAT(json, ContainsSubstring("\"clock_offset_p99_us\": 273"));
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

// FromAudioBlockTiming is the whole reason AudioBlockTiming lives in
// jamn_core rather than inside the JUCE-linking class that fills it: these
// cases run with no device, no JUCE and no hardware, under `ctest -L fast`
// in the core-only preset, which is the only place the arithmetic below can
// be checked at all. The dev box has no /dev/snd.
//
// Named without the substring "ToJson" on purpose, so the accept command
// recorded for that function still selects exactly its own nine cases.

namespace {

void SetFixed(char* buffer, std::size_t size, const char* text) {
    std::memset(buffer, 0, size);
    std::memcpy(buffer, text, std::strlen(text));
}

// A plausible three-block run, with figures chosen to divide exactly so a
// delivered-rate assertion is testing the formula rather than float noise.
AudioBlockTiming PlausibleTiming() {
    AudioBlockTiming timing;
    timing.blocks = 3;
    timing.deviceStarts = 1;
    timing.sampleRate = 44100.0;
    timing.blockSize = 100;
    SetFixed(timing.deviceName, sizeof(timing.deviceName), "USB Audio, USB Audio; Direct hardware device");
    SetFixed(timing.typeName, sizeof(timing.typeName), "ALSA");
    timing.outputLatencySamples = 384;
    timing.frames = 300;
    timing.spanNs = 2'000'000'000;
    timing.intervalCount = 2;
    timing.minIntervalUs = 900;
    timing.maxIntervalUs = 1100;
    timing.meanIntervalUs = 1000;
    timing.p50IntervalUs = 1000;
    timing.p99IntervalUs = 1100;
    return timing;
}

}  // namespace

TEST_CASE("FromAudioBlockTiming names the device and its type separately", "[bench][bench_result][fast]") {
    // Both, not one: on Linux the sound server's default and a raw hw: card
    // are both type "ALSA", so the type alone cannot tell two rows apart -
    // and the device name alone loses the WASAPI shared/exclusive
    // distinction that matters on Windows.
    const BenchResult result = FromAudioBlockTiming(PlausibleTiming());
    REQUIRE(result.backend == "ALSA");
    REQUIRE(result.device == "USB Audio, USB Audio; Direct hardware device");
}

TEST_CASE("FromAudioBlockTiming reports the granted rate and block size", "[bench][bench_result][fast]") {
    // A device is free to refuse a requested block size and keep its own.
    // What is recorded has to be what it granted, or the row describes a
    // run that never happened.
    const BenchResult result = FromAudioBlockTiming(PlausibleTiming());
    REQUIRE(result.sample_rate_hz == 44100);
    REQUIRE(result.block_size == 100);
    REQUIRE(result.num_blocks == 3);
    REQUIRE(result.frames == 300);
    REQUIRE(result.span_us == 2'000'000);
    REQUIRE(result.configured_buffer_depth_samples == 384);
}

TEST_CASE("FromAudioBlockTiming carries the interval statistics through", "[bench][bench_result][fast]") {
    const BenchResult result = FromAudioBlockTiming(PlausibleTiming());
    REQUIRE(result.callback_interval.min_us == 900);
    REQUIRE(result.callback_interval.max_us == 1100);
    REQUIRE(result.callback_interval.mean_us == 1000);
    REQUIRE(result.callback_interval.p50_us == 1000);
    REQUIRE(result.callback_interval.p99_us == 1100);
}

TEST_CASE("FromAudioBlockTiming discounts one block from the delivered rate", "[bench][bench_result][fast]") {
    // The span runs from the *first* callback's entry, so the frames that
    // callback delivered fall outside it. (300 - 100) frames over 2s is
    // 100Hz; using all 300 would report 150Hz and overstate the device by
    // half.
    const BenchResult result = FromAudioBlockTiming(PlausibleTiming());
    REQUIRE(result.delivered_rate_hz == Approx(100.0));
}

TEST_CASE("FromAudioBlockTiming does not measure callback duration or xruns", "[bench][bench_result][fast]") {
    // A device backend reports when callbacks arrived, not what they cost,
    // and nothing here counts xruns. Both stay -1 rather than 0: a
    // zero-cost callback and an xrun-free run are both claims this path
    // cannot make, and 0 would read as exactly those claims.
    const BenchResult result = FromAudioBlockTiming(PlausibleTiming());
    REQUIRE(result.callback_duration.min_ns == -1.0);
    REQUIRE(result.callback_duration.max_ns == -1.0);
    REQUIRE(result.callback_duration.mean_ns == -1.0);
    REQUIRE(result.xruns == -1);
}

TEST_CASE("FromAudioBlockTiming leaves an empty reading at -1, not 0", "[bench][bench_result][fast]") {
    // A device that opened and entered no callbacks. num_blocks stays 0,
    // because zero callbacks is a real and quite interesting reading -
    // everything derived from callbacks that did not happen is -1.
    AudioBlockTiming timing;
    timing.deviceStarts = 1;
    SetFixed(timing.typeName, sizeof(timing.typeName), "ALSA");

    const BenchResult result = FromAudioBlockTiming(timing);
    REQUIRE(result.num_blocks == 0);
    REQUIRE(result.frames == -1);
    REQUIRE(result.span_us == -1);
    REQUIRE(result.sample_rate_hz == -1);
    REQUIRE(result.block_size == -1);
    REQUIRE(result.delivered_rate_hz == -1.0);
    REQUIRE(result.callback_interval.mean_us == -1);
    REQUIRE(result.callback_interval.p99_us == -1);
}

TEST_CASE("FromAudioBlockTiming survives a zero span without dividing by it", "[bench][bench_result][fast]") {
    // Two callbacks entered inside the same clock tick. Rare, but a
    // division by zero here would take the process down at exactly the
    // moment it was trying to report a measurement.
    AudioBlockTiming timing = PlausibleTiming();
    timing.spanNs = 0;

    const BenchResult result = FromAudioBlockTiming(timing);
    REQUIRE(result.delivered_rate_hz == -1.0);
    REQUIRE(result.span_us == -1);
}

TEST_CASE("FromAudioBlockTiming reports a broken sample timeline rather than folding it away",
          "[bench][bench_result][fast]") {
    // With more than one device start, frames is still a true total but is
    // not one continuous count - so the row has to say so. Dropping the
    // field would leave a reading that looks interpretable and is not.
    AudioBlockTiming timing = PlausibleTiming();
    timing.deviceStarts = 3;

    const BenchResult result = FromAudioBlockTiming(timing);
    REQUIRE(result.device_starts == 3);
}

TEST_CASE("FromAudioBlockTiming's row renders its device fields as JSON", "[bench][bench_result][fast]") {
    const std::string json = ToJson({FromAudioBlockTiming(PlausibleTiming())});
    REQUIRE_THAT(json, ContainsSubstring("\"backend\": \"ALSA\""));
    REQUIRE_THAT(json, ContainsSubstring("\"device_starts\": 1"));
    REQUIRE_THAT(json, ContainsSubstring("\"frames\": 300"));
    REQUIRE_THAT(json, ContainsSubstring("\"span_us\": 2000000"));
    REQUIRE_THAT(json, ContainsSubstring("\"delivered_rate_hz\": 100"));
    REQUIRE_THAT(json, ContainsSubstring("\"configured_buffer_depth_samples\": 384"));
    REQUIRE_THAT(json, ContainsSubstring("\"callback_interval_us\""));
    REQUIRE_THAT(json, ContainsSubstring("\"p99\": 1100"));
    // The field this must never be called. A JSON key is read by scripts
    // with no comment attached, and JUCE's figure is a configured buffer
    // depth, not a measured latency.
    REQUIRE_THAT(json, !ContainsSubstring("latency"));
}

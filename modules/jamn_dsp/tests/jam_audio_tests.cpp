#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "jamn_core/file_audio_device.h"
#include "jamn_core/realtime_scope.h"
#include "jamn_dsp/jam_audio.h"

using jamn::core::FileAudioDevice;
using jamn::core::SetRealtimeViolationHandler;
using jamn::dsp::JamAudio;

namespace {

float PeakAbs(const std::vector<float>& buffer) {
    float peak = 0.0f;
    for (float sample : buffer) {
        peak = std::max(peak, std::fabs(sample));
    }
    return peak;
}

}  // namespace

TEST_CASE("JamAudio renders silence with no trigger queued", "[dsp][jam_audio][fast]") {
    JamAudio audio;
    audio.Prepare(48000.0);

    const int numFrames = 128;
    std::vector<float> channel(numFrames, 1.0f);
    float* channels[] = {channel.data()};

    audio.Process(channels, 1, numFrames);

    REQUIRE(PeakAbs(channel) == 0.0f);
}

TEST_CASE("JamAudio plays a blip for a trigger pushed from the message thread", "[dsp][jam_audio][fast]") {
    JamAudio audio;
    audio.Prepare(48000.0);
    REQUIRE(audio.Trigger());

    const int numFrames = 128;
    std::vector<float> channel(numFrames, 1.0f);
    float* channels[] = {channel.data()};

    audio.Process(channels, 1, numFrames);

    REQUIRE(PeakAbs(channel) > 0.0f);
}

TEST_CASE("JamAudio drains every queued trigger within one block", "[dsp][jam_audio][fast]") {
    JamAudio audio;
    audio.Prepare(48000.0);
    REQUIRE(audio.Trigger());
    REQUIRE(audio.Trigger());
    REQUIRE(audio.Trigger());

    const int numFrames = 128;
    std::vector<float> channel(numFrames, 1.0f);
    float* channels[] = {channel.data()};

    audio.Process(channels, 1, numFrames);

    // A fourth push after the drain must succeed - proves the ring was
    // actually emptied, not just that Trigger() kept returning true.
    REQUIRE(audio.Trigger());
}

TEST_CASE("JamAudio's master gain scales what it renders", "[dsp][jam_audio][fast]") {
    const int numFrames = 128;

    JamAudio quiet;
    quiet.SetGain(0.25f);
    quiet.Prepare(48000.0);
    quiet.Trigger();
    std::vector<float> quietChannel(numFrames, 0.0f);
    float* quietChannels[] = {quietChannel.data()};
    quiet.Process(quietChannels, 1, numFrames);

    JamAudio loud;
    loud.SetGain(1.0f);
    loud.Prepare(48000.0);
    loud.Trigger();
    std::vector<float> loudChannel(numFrames, 0.0f);
    float* loudChannels[] = {loudChannel.data()};
    loud.Process(loudChannels, 1, numFrames);

    const float quietPeak = PeakAbs(quietChannel);
    const float loudPeak = PeakAbs(loudChannel);
    REQUIRE(quietPeak > 0.0f);
    REQUIRE(loudPeak > 0.0f);
    REQUIRE(quietPeak < loudPeak);
}

TEST_CASE("JamAudio's real-time guard is actually armed in this binary", "[dsp][jam_audio][fast]") {
    SetRealtimeViolationHandler([](const char*) { throw std::runtime_error("rt violation"); });

    bool reported = false;
    volatile int sink = 0;
    try {
        jamn::core::RealtimeScope scope;
        int* value = new int(5);
        sink = *value;
        delete value;
    } catch (const std::runtime_error&) {
        reported = true;
    }
    (void)sink;

    SetRealtimeViolationHandler(nullptr);
    REQUIRE(reported);
}

TEST_CASE("JamAudio renders a full run of blocks through FileAudioDevice without allocating",
          "[dsp][jam_audio][fast]") {
    SetRealtimeViolationHandler([](const char*) { throw std::runtime_error("rt violation"); });

    JamAudio audio;
    audio.Prepare(48000.0);
    audio.SetGain(0.5f);
    audio.Trigger();

    FileAudioDevice device(2, 128);
    bool reported = false;
    try {
        device.Process(64, [&](float* const* output, int numChannels, int numFrames) {
            audio.Process(output, numChannels, numFrames);
        });
    } catch (const std::runtime_error&) {
        reported = true;
    }

    SetRealtimeViolationHandler(nullptr);
    REQUIRE_FALSE(reported);
}

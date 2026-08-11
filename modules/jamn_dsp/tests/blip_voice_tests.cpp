#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "jamn_dsp/blip_voice.h"

using jamn::dsp::BlipVoice;

namespace {

float PeakAbs(const std::vector<float>& buffer) {
    float peak = 0.0f;
    for (float sample : buffer) {
        peak = std::max(peak, std::fabs(sample));
    }
    return peak;
}

}  // namespace

TEST_CASE("BlipVoice renders silence until it is triggered", "[dsp][blip_voice][fast]") {
    BlipVoice voice;
    voice.Prepare(48000.0);

    const int numFrames = 128;
    std::vector<float> channel(numFrames, 0.0f);
    float* channels[] = {channel.data()};

    voice.Render(channels, 1, numFrames);

    REQUIRE_FALSE(voice.IsActive());
    REQUIRE(PeakAbs(channel) == 0.0f);
}

TEST_CASE("BlipVoice sounds after Trigger and falls silent on its own", "[dsp][blip_voice][fast]") {
    BlipVoice voice;
    voice.Prepare(48000.0);
    voice.Trigger();

    const int numFrames = 128;
    std::vector<float> channel(numFrames, 0.0f);
    float* channels[] = {channel.data()};

    voice.Render(channels, 1, numFrames);
    REQUIRE(PeakAbs(channel) > 0.1f);

    // 400ms life + margin, at 48kHz.
    for (int block = 0; block < 200; ++block) {
        for (int frame = 0; frame < numFrames; ++frame) {
            channel[frame] = 0.0f;
        }
        voice.Render(channels, 1, numFrames);
    }

    REQUIRE_FALSE(voice.IsActive());
    for (float sample : channel) {
        REQUIRE(sample == 0.0f);
    }
}

TEST_CASE("BlipVoice adds into the buffer rather than overwriting it", "[dsp][blip_voice][fast]") {
    BlipVoice voice;
    voice.Prepare(48000.0);
    voice.Trigger();

    const int numFrames = 32;
    std::vector<float> withVoice(numFrames, 1.0f);
    std::vector<float> withoutVoice(numFrames, 1.0f);
    float* withVoiceChannels[] = {withVoice.data()};

    voice.Render(withVoiceChannels, 1, numFrames);

    bool anyDiffered = false;
    for (int frame = 0; frame < numFrames; ++frame) {
        const float contribution = withVoice[frame] - withoutVoice[frame];
        if (contribution != 0.0f) {
            anyDiffered = true;
        }
        // The voice's amplitude is well under 1.0 - a bug that overwrote
        // rather than added would produce a contribution near -1.0 instead.
        REQUIRE(std::fabs(contribution) < 0.5f);
    }
    REQUIRE(anyDiffered);
}

TEST_CASE("BlipVoice opens with an attack rather than a click", "[dsp][blip_voice][fast]") {
    BlipVoice voice;
    voice.Prepare(48000.0);
    voice.Trigger();

    const int numFrames = 8;
    std::vector<float> channel(numFrames, 0.0f);
    float* channels[] = {channel.data()};

    voice.Render(channels, 1, numFrames);

    REQUIRE(std::fabs(channel[0]) < 0.01f);
    for (int frame = 1; frame < numFrames; ++frame) {
        REQUIRE(std::fabs(channel[frame] - channel[frame - 1]) < 0.1f);
    }
}

TEST_CASE("BlipVoice restarts from the beginning when retriggered mid-sound", "[dsp][blip_voice][fast]") {
    BlipVoice voice;
    voice.Prepare(48000.0);
    voice.Trigger();

    const int numFrames = 128;
    std::vector<float> channel(numFrames, 0.0f);
    float* channels[] = {channel.data()};

    // Run partway through the decay.
    for (int block = 0; block < 20; ++block) {
        for (int frame = 0; frame < numFrames; ++frame) {
            channel[frame] = 0.0f;
        }
        voice.Render(channels, 1, numFrames);
    }
    const float midDecayPeak = PeakAbs(channel);

    voice.Trigger();
    for (int frame = 0; frame < numFrames; ++frame) {
        channel[frame] = 0.0f;
    }
    voice.Render(channels, 1, numFrames);

    // First sample after a fresh Trigger() is back near the start of the
    // attack ramp, not wherever the decay had gotten to.
    REQUIRE(std::fabs(channel[0]) < 0.01f);
    REQUIRE(voice.IsActive());
    (void)midDecayPeak;
}

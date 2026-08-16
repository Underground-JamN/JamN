#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <cstdint>

#include "jamn_core/file_audio_device.h"
#include "jamn_core/realtime_scope.h"
#include "jamn_dsp/jam_audio.h"

using jamn::core::FileAudioDevice;
using jamn::core::SetRealtimeViolationHandler;
using jamn::dsp::IInstrument;
using jamn::dsp::JamAudio;

namespace {

float PeakAbs(const std::vector<float>& buffer) {
    float peak = 0.0f;
    for (float sample : buffer) {
        peak = std::max(peak, std::fabs(sample));
    }
    return peak;
}

// See strip_tests.cpp: DC rather than a tone, so a rendered value is the
// gain that produced it.
class ConstantInstrument final : public IInstrument {
public:
    void Prepare(double) noexcept override {}
    void NoteOn(std::uint8_t, std::uint8_t) noexcept override {}
    void NoteOff(std::uint8_t) noexcept override {}
    void AllNotesOff() noexcept override {}
    int ActiveVoiceCount() const noexcept override { return 1; }

    void Render(float* const* outputChannels, int numChannels, int numFrames) noexcept override {
        for (int channel = 0; channel < numChannels; ++channel) {
            for (int frame = 0; frame < numFrames; ++frame) {
                outputChannels[channel][frame] += 1.0f;
            }
        }
    }
};

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

TEST_CASE("JamAudio's peer mixer reaches the output through the master gain",
          "[dsp][jam_audio][fast]") {
    ConstantInstrument instrument;
    JamAudio audio;
    audio.peers().strip(0).SetInstrument(&instrument);
    audio.SetGain(0.5f);
    audio.Prepare(48000.0);

    const int numFrames = 128;
    std::vector<float> channel(numFrames, 0.0f);
    float* channels[] = {channel.data()};
    audio.Process(channels, 1, numFrames);

    // Both ramps snapped at Prepare, so this is exactly one strip at unity
    // through a master gain of 0.5 - not an inequality that would still
    // pass if the mixer were summing at the wrong point in the chain.
    REQUIRE(channel.front() == 0.5f);
}

TEST_CASE("JamAudio's blip is not mutable by a peer's solo", "[dsp][jam_audio][fast]") {
    const int numFrames = 128;

    // Master gain well below full scale on purpose. A soloed peer at unity
    // already sits exactly at the ceiling, so at unity gain the limiter
    // would hold blip-plus-peer down to the same 1.0 the peer reaches
    // alone, and the comparison below could not tell the blip apart from
    // silence. This asks the question underneath the limiter, not through
    // it.
    auto peakOf = [numFrames](bool trigger) {
        ConstantInstrument instrument;
        JamAudio audio;
        audio.peers().strip(0).SetInstrument(&instrument);
        audio.peers().strip(0).SetSolo(true);
        audio.SetGain(0.25f);
        audio.Prepare(48000.0);
        if (trigger) {
            REQUIRE(audio.Trigger());
        }

        std::vector<float> channel(numFrames, 0.0f);
        float* channels[] = {channel.data()};
        audio.Process(channels, 1, numFrames);
        return PeakAbs(channel);
    };

    // The local blip lives outside the mixer, so a peer soloing themselves
    // must not silence it - see the comment on JamAudio::peers().
    REQUIRE(peakOf(true) > peakOf(false));
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

TEST_CASE("JamAudio renders the local monitor instrument", "[dsp][jam_audio][fast]") {
    ConstantInstrument instrument;
    JamAudio audio;
    audio.SetLocalInstrument(&instrument);
    audio.SetGain(0.5f);
    audio.Prepare(48000.0);

    const int numFrames = 128;
    std::vector<float> channel(numFrames, 0.0f);
    float* channels[] = {channel.data()};
    audio.Process(channels, 1, numFrames);

    // One instrument at unity through a master gain of 0.5, with no strip
    // in the path at all - the same exact figure the peer-mixer case
    // above asserts, reached by the route that skips the mixer.
    REQUIRE(channel.front() == 0.5f);
    REQUIRE(audio.localInstrument() == &instrument);
}

TEST_CASE("JamAudio's local monitor is not mutable by a peer's solo", "[dsp][jam_audio][fast]") {
    // The reason it is not a ninth strip. A player must always hear
    // themselves, including while somebody else is soloed - a strip would
    // put that under another peer's control.
    const int numFrames = 128;

    ConstantInstrument peer;
    ConstantInstrument local;
    JamAudio audio;
    audio.peers().strip(0).SetInstrument(&peer);
    audio.peers().strip(0).SetSolo(true);
    audio.SetLocalInstrument(&local);
    // Below full scale for the same reason the blip case above is: at
    // unity the limiter would flatten the sum back onto the soloed peer's
    // own ceiling and the comparison could not see the difference.
    audio.SetGain(0.25f);
    audio.Prepare(48000.0);

    std::vector<float> channel(numFrames, 0.0f);
    float* channels[] = {channel.data()};
    audio.Process(channels, 1, numFrames);

    // Both sources at unity, so the sum is twice what the soloed peer
    // reaches alone.
    REQUIRE(channel.front() == 0.5f);
}

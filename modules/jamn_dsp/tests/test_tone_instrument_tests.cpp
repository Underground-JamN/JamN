#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "jamn_core/file_audio_device.h"
#include "jamn_core/realtime_scope.h"
#include "jamn_dsp/test_tone_instrument.h"

using jamn::core::FileAudioDevice;
using jamn::core::SetRealtimeViolationHandler;
using jamn::dsp::TestToneInstrument;

namespace {

constexpr double kSampleRate = 48000.0;

struct Block {
    explicit Block(int numFrames, int numChannels = 1)
        : channels(static_cast<std::size_t>(numChannels), std::vector<float>(numFrames, 0.0f)) {
        for (auto& channel : channels) pointers.push_back(channel.data());
    }

    void Clear() {
        for (auto& channel : channels) std::fill(channel.begin(), channel.end(), 0.0f);
    }

    float* const* data() { return pointers.data(); }

    float Peak() const {
        float peak = 0.0f;
        for (const auto& channel : channels) {
            for (float sample : channel) peak = std::max(peak, std::fabs(sample));
        }
        return peak;
    }

    std::vector<std::vector<float>> channels;
    std::vector<float*> pointers;
};

// Renders enough blocks for a released voice to finish decaying, and
// returns the peak of the final block - the release is exponential, so
// "silent" is a settled state a few tens of milliseconds later, not the
// instant after NoteOff.
float PeakAfterSettling(TestToneInstrument& instrument, int numFrames, int blocks) {
    Block block(numFrames);
    for (int i = 0; i < blocks; ++i) {
        block.Clear();
        instrument.Render(block.data(), 1, numFrames);
    }
    return block.Peak();
}

}  // namespace

TEST_CASE("TestToneInstrument is silent until a note is played", "[dsp][instrument][test_tone][fast]") {
    TestToneInstrument instrument;
    instrument.Prepare(kSampleRate);

    Block block(128);
    instrument.Render(block.data(), 1, 128);
    REQUIRE(block.Peak() == 0.0f);
    REQUIRE(instrument.ActiveVoiceCount() == 0);
}

TEST_CASE("A note-on sounds and a note-off actually silences that pitch",
          "[dsp][instrument][test_tone][fast]") {
    TestToneInstrument instrument;
    instrument.Prepare(kSampleRate);

    instrument.NoteOn(60, 100);
    REQUIRE(instrument.ActiveVoiceCount() == 1);

    Block block(128);
    instrument.Render(block.data(), 1, 128);
    REQUIRE(block.Peak() > 0.0f);

    instrument.NoteOff(60);
    REQUIRE(PeakAfterSettling(instrument, 128, 40) == 0.0f);
    REQUIRE(instrument.ActiveVoiceCount() == 0);
}

TEST_CASE("A note-off for one pitch leaves another pitch sounding",
          "[dsp][instrument][test_tone][fast]") {
    TestToneInstrument instrument;
    instrument.Prepare(kSampleRate);

    instrument.NoteOn(60, 100);
    instrument.NoteOn(67, 100);
    REQUIRE(instrument.ActiveVoiceCount() == 2);

    instrument.NoteOff(60);
    REQUIRE(PeakAfterSettling(instrument, 128, 40) > 0.0f);
    REQUIRE(instrument.ActiveVoiceCount() == 1);
}

TEST_CASE("Two note-ons for one pitch are both silenced by a single note-off",
          "[dsp][instrument][test_tone][fast]") {
    // The stuck-note shape this guards: releasing only the newest voice
    // would leave the first one sounding with no note-off ever coming.
    TestToneInstrument instrument;
    instrument.Prepare(kSampleRate);

    instrument.NoteOn(60, 100);
    instrument.NoteOn(60, 100);
    REQUIRE(instrument.ActiveVoiceCount() == 2);

    instrument.NoteOff(60);
    REQUIRE(PeakAfterSettling(instrument, 128, 40) == 0.0f);
    REQUIRE(instrument.ActiveVoiceCount() == 0);
}

TEST_CASE("Velocity zero is treated as a note-off, the way MIDI does",
          "[dsp][instrument][test_tone][fast]") {
    TestToneInstrument instrument;
    instrument.Prepare(kSampleRate);

    instrument.NoteOn(60, 100);
    instrument.NoteOn(60, 0);
    REQUIRE(PeakAfterSettling(instrument, 128, 40) == 0.0f);
    REQUIRE(instrument.ActiveVoiceCount() == 0);
}

TEST_CASE("Velocity scales how loud a note is", "[dsp][instrument][test_tone][fast]") {
    auto peakForVelocity = [](std::uint8_t velocity) {
        TestToneInstrument instrument;
        instrument.Prepare(kSampleRate);
        instrument.NoteOn(60, velocity);
        Block block(1024);
        instrument.Render(block.data(), 1, 1024);
        return block.Peak();
    };

    const float quiet = peakForVelocity(30);
    const float loud = peakForVelocity(127);
    REQUIRE(quiet > 0.0f);
    REQUIRE(quiet < loud);
}

TEST_CASE("A higher pitch produces more zero crossings than a lower one",
          "[dsp][instrument][test_tone][fast]") {
    // Cheaper and more direct than an FFT: pitch is a frequency claim, and
    // zero crossings over a fixed window are a frequency measurement.
    auto crossingsForPitch = [](std::uint8_t pitch) {
        TestToneInstrument instrument;
        instrument.Prepare(kSampleRate);
        instrument.NoteOn(pitch, 127);

        const int numFrames = 4800;  // 100ms.
        Block block(numFrames);
        instrument.Render(block.data(), 1, numFrames);

        int crossings = 0;
        const std::vector<float>& samples = block.channels[0];
        for (int i = 1; i < numFrames; ++i) {
            if ((samples[i - 1] < 0.0f) != (samples[i] < 0.0f)) ++crossings;
        }
        return crossings;
    };

    const int lowCrossings = crossingsForPitch(60);
    const int highCrossings = crossingsForPitch(72);  // One octave up.
    REQUIRE(lowCrossings > 0);
    // An octave is a doubling; allow slack for the attack ramp and for
    // where the window happens to cut the waveform.
    REQUIRE(highCrossings > lowCrossings * 3 / 2);
}

TEST_CASE("At pool capacity a new note steals a voice rather than being dropped",
          "[dsp][instrument][test_tone][fast]") {
    TestToneInstrument instrument;
    instrument.Prepare(kSampleRate);

    for (std::size_t i = 0; i < TestToneInstrument::kMaxVoices; ++i) {
        instrument.NoteOn(static_cast<std::uint8_t>(60 + i), 100);
    }
    REQUIRE(instrument.ActiveVoiceCount() == static_cast<int>(TestToneInstrument::kMaxVoices));

    // One more than the pool holds. The count must not grow, and the new
    // note must genuinely sound - dropping it would leave a note-on with
    // no matching sound and, later, a note-off for a voice that never was.
    instrument.NoteOn(90, 127);
    REQUIRE(instrument.ActiveVoiceCount() == static_cast<int>(TestToneInstrument::kMaxVoices));

    // The oldest note (60) was the one stolen, so its note-off now matches
    // nothing while the newest note still sounds.
    instrument.NoteOff(60);
    REQUIRE(PeakAfterSettling(instrument, 128, 40) > 0.0f);
}

TEST_CASE("AllNotesOff silences everything", "[dsp][instrument][test_tone][fast]") {
    TestToneInstrument instrument;
    instrument.Prepare(kSampleRate);
    for (std::size_t i = 0; i < TestToneInstrument::kMaxVoices; ++i) {
        instrument.NoteOn(static_cast<std::uint8_t>(60 + i), 100);
    }

    instrument.AllNotesOff();
    REQUIRE(PeakAfterSettling(instrument, 128, 40) == 0.0f);
    REQUIRE(instrument.ActiveVoiceCount() == 0);
}

TEST_CASE("Render adds into the buffer rather than overwriting it",
          "[dsp][instrument][test_tone][fast]") {
    TestToneInstrument instrument;
    instrument.Prepare(kSampleRate);
    instrument.NoteOn(60, 100);

    const int numFrames = 64;
    std::vector<float> channel(numFrames, 1.0f);
    float* channels[] = {channel.data()};
    instrument.Render(channels, 1, numFrames);

    for (int frame = 0; frame < numFrames; ++frame) {
        REQUIRE(channel[frame] != 0.0f);  // The 1.0f it started with is still in there.
    }
    REQUIRE(channel[numFrames - 1] != 1.0f);  // ...and something was added to it.
}

TEST_CASE("A full run of notes through FileAudioDevice never allocates",
          "[dsp][instrument][test_tone][fast]") {
    SetRealtimeViolationHandler([](const char*) { throw std::runtime_error("rt violation"); });

    TestToneInstrument instrument;
    instrument.Prepare(kSampleRate);

    FileAudioDevice device(2, 128);
    bool reported = false;
    int block = 0;
    try {
        device.Process(200, [&](float* const* output, int numChannels, int numFrames) {
            // Notes on and off from inside the callback, which is where they
            // really arrive: EventScheduler::PopReady runs at block start on
            // the audio thread.
            if (block % 4 == 0) instrument.NoteOn(static_cast<std::uint8_t>(48 + (block % 40)), 100);
            if (block % 4 == 2) instrument.NoteOff(static_cast<std::uint8_t>(48 + ((block - 2) % 40)));
            if (block == 150) instrument.AllNotesOff();
            instrument.Render(output, numChannels, numFrames);
            ++block;
        });
    } catch (const std::runtime_error&) {
        reported = true;
    }

    SetRealtimeViolationHandler(nullptr);
    REQUIRE_FALSE(reported);
}

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "jamn_core/file_audio_device.h"
#include "jamn_core/realtime_scope.h"
#include "jamn_dsp/peer_mixer.h"
#include "jamn_dsp/test_tone_instrument.h"

using jamn::core::FileAudioDevice;
using jamn::core::SetRealtimeViolationHandler;
using jamn::dsp::IInstrument;
using jamn::dsp::PeerMixer;
using jamn::dsp::TestToneInstrument;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockFrames = 128;

// See strip_tests.cpp for why 100: kRampSeconds is a time constant, not a
// completion time, and the end-of-block snap to an exact target needs
// ~78 blocks of 128 from full scale.
constexpr int kSettleBlocks = 100;

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

    float PeakOf(std::size_t channel) const {
        float peak = 0.0f;
        for (float sample : channels[channel]) peak = std::max(peak, std::fabs(sample));
        return peak;
    }

    std::vector<std::vector<float>> channels;
    std::vector<float*> pointers;
};

float PeakAfterSettling(PeerMixer& mixer, Block& block, int numChannels, int numFrames) {
    for (int i = 0; i < kSettleBlocks; ++i) {
        block.Clear();
        mixer.Render(block.data(), numChannels, numFrames);
    }
    return block.Peak();
}

}  // namespace

TEST_CASE("An empty peer mixer renders silence", "[dsp][peer_mixer][mixer][fast]") {
    PeerMixer mixer;
    mixer.Prepare(kSampleRate);

    Block block(kBlockFrames);
    mixer.Render(block.data(), 1, kBlockFrames);

    REQUIRE(block.Peak() == 0.0f);
}

TEST_CASE("The peer mixer sums every occupied strip", "[dsp][peer_mixer][mixer][fast]") {
    ConstantInstrument first;
    ConstantInstrument second;
    ConstantInstrument third;

    PeerMixer mixer;
    mixer.strip(0).SetInstrument(&first);
    mixer.strip(1).SetInstrument(&second);
    mixer.strip(2).SetInstrument(&third);
    mixer.Prepare(kSampleRate);

    Block block(kBlockFrames);
    mixer.Render(block.data(), 1, kBlockFrames);

    // Three strips at unity sum past full scale, and the mixer does not
    // hold them back - MasterBus's limiter downstream is what stops this
    // reaching the device. Exactly 3.0 also proves the empty strips added
    // nothing.
    REQUIRE(block.channels[0].front() == 3.0f);
}

TEST_CASE("The peer mixer adds into its output and never clears it", "[dsp][peer_mixer][mixer][fast]") {
    PeerMixer mixer;
    mixer.Prepare(kSampleRate);

    Block block(kBlockFrames);
    std::fill(block.channels[0].begin(), block.channels[0].end(), 0.5f);
    mixer.Render(block.data(), 1, kBlockFrames);

    REQUIRE(block.channels[0].front() == 0.5f);
}

TEST_CASE("Soloing one of three peers silences the other two strips and un-soloing restores them",
          "[dsp][peer_mixer][mixer][fast]") {
    ConstantInstrument first;
    ConstantInstrument second;
    ConstantInstrument third;

    PeerMixer mixer;
    mixer.strip(0).SetInstrument(&first);
    mixer.strip(1).SetInstrument(&second);
    mixer.strip(2).SetInstrument(&third);
    mixer.Prepare(kSampleRate);

    Block block(kBlockFrames);
    REQUIRE(PeakAfterSettling(mixer, block, 1, kBlockFrames) == 3.0f);

    mixer.strip(1).SetSolo(true);
    REQUIRE(mixer.AnySolo());
    // 1.0, not merely "less than 3.0": pinning the value distinguishes
    // solo working from solo silencing the soloed strip too, or from an
    // off-by-one leaving one neighbour audible.
    REQUIRE(PeakAfterSettling(mixer, block, 1, kBlockFrames) == 1.0f);
    REQUIRE(mixer.strip(0).currentGain() == 0.0f);
    REQUIRE(mixer.strip(1).currentGain() == 1.0f);
    REQUIRE(mixer.strip(2).currentGain() == 0.0f);

    mixer.strip(1).SetSolo(false);
    REQUIRE_FALSE(mixer.AnySolo());
    REQUIRE(PeakAfterSettling(mixer, block, 1, kBlockFrames) == 3.0f);
}

TEST_CASE("Two soloed peers are both audible and the rest are not", "[dsp][peer_mixer][mixer][fast]") {
    ConstantInstrument first;
    ConstantInstrument second;
    ConstantInstrument third;

    PeerMixer mixer;
    mixer.strip(0).SetInstrument(&first);
    mixer.strip(1).SetInstrument(&second);
    mixer.strip(2).SetInstrument(&third);
    mixer.strip(0).SetSolo(true);
    mixer.strip(2).SetSolo(true);
    mixer.Prepare(kSampleRate);

    Block block(kBlockFrames);
    REQUIRE(PeakAfterSettling(mixer, block, 1, kBlockFrames) == 2.0f);
}

TEST_CASE("Mute and solo compose predictably across the mixer's strips",
          "[dsp][peer_mixer][mixer][fast]") {
    ConstantInstrument first;
    ConstantInstrument second;

    PeerMixer mixer;
    mixer.strip(0).SetInstrument(&first);
    mixer.strip(1).SetInstrument(&second);
    mixer.Prepare(kSampleRate);

    // Soloed and muted at once: the mute wins, so the mix is silent even
    // though the only soloed strip is the muted one.
    mixer.strip(0).SetSolo(true);
    mixer.strip(0).SetMute(true);

    Block block(kBlockFrames);
    REQUIRE(PeakAfterSettling(mixer, block, 1, kBlockFrames) == 0.0f);

    mixer.strip(0).SetMute(false);
    REQUIRE(PeakAfterSettling(mixer, block, 1, kBlockFrames) == 1.0f);
}

TEST_CASE("The peer mixer renders a block longer than its scratch capacity",
          "[dsp][peer_mixer][mixer][fast]") {
    ConstantInstrument instrument;
    PeerMixer mixer;
    mixer.strip(0).SetInstrument(&instrument);
    mixer.Prepare(kSampleRate);

    // Deliberately not a multiple of kMaxBlockFrames, so the final chunk is
    // a partial one - that is where a chunking off-by-one shows up.
    const int numFrames = PeerMixer::kMaxBlockFrames + 137;
    Block block(numFrames);
    mixer.Render(block.data(), 1, numFrames);

    REQUIRE(block.channels[0].front() == 1.0f);
    REQUIRE(block.channels[0].back() == 1.0f);
    for (float sample : block.channels[0]) {
        REQUIRE(sample == 1.0f);
    }
}

TEST_CASE("The peer mixer fills every channel it covers", "[dsp][peer_mixer][mixer][fast]") {
    ConstantInstrument instrument;
    PeerMixer mixer;
    mixer.strip(0).SetInstrument(&instrument);
    mixer.Prepare(kSampleRate);

    Block block(kBlockFrames, PeerMixer::kMaxChannels);
    mixer.Render(block.data(), PeerMixer::kMaxChannels, kBlockFrames);

    for (int channel = 0; channel < PeerMixer::kMaxChannels; ++channel) {
        REQUIRE(block.PeakOf(static_cast<std::size_t>(channel)) == 1.0f);
    }
}

TEST_CASE("The peer mixer leaves channels past its stereo capacity untouched",
          "[dsp][peer_mixer][mixer][fast]") {
    ConstantInstrument instrument;
    PeerMixer mixer;
    mixer.strip(0).SetInstrument(&instrument);
    mixer.Prepare(kSampleRate);

    const int numChannels = PeerMixer::kMaxChannels + 2;
    Block block(kBlockFrames, numChannels);
    mixer.Render(block.data(), numChannels, kBlockFrames);

    // Documented clamp, asserted rather than left to whatever the scratch
    // array happened to be sized at - the alternative failure is a silent
    // write past the end of it.
    REQUIRE(block.PeakOf(0) == 1.0f);
    REQUIRE(block.PeakOf(static_cast<std::size_t>(PeerMixer::kMaxChannels)) == 0.0f);
    REQUIRE(block.PeakOf(static_cast<std::size_t>(numChannels - 1)) == 0.0f);
}

TEST_CASE("A full mixer of peers renders through FileAudioDevice without allocating",
          "[dsp][peer_mixer][mixer][fast]") {
    SetRealtimeViolationHandler([](const char*) { throw std::runtime_error("rt violation"); });

    std::vector<TestToneInstrument> instruments(PeerMixer::kNumStrips);
    PeerMixer mixer;
    for (std::size_t i = 0; i < PeerMixer::kNumStrips; ++i) {
        instruments[i].Prepare(kSampleRate);
        instruments[i].NoteOn(static_cast<std::uint8_t>(60 + i), 100);
        mixer.strip(i).SetInstrument(&instruments[i]);
    }
    mixer.Prepare(kSampleRate);
    // Solo and mute changes are the message-thread side of the same state
    // the audio thread reads every block; set them before the run so the
    // guard covers the soloed path too, not just the plain sum.
    mixer.strip(0).SetSolo(true);
    mixer.strip(1).SetMute(true);

    FileAudioDevice device(2, kBlockFrames);
    bool reported = false;
    try {
        device.Process(64, [&](float* const* output, int numChannels, int numFrames) {
            for (int channel = 0; channel < numChannels; ++channel) {
                for (int frame = 0; frame < numFrames; ++frame) {
                    output[channel][frame] = 0.0f;
                }
            }
            mixer.Render(output, numChannels, numFrames);
        });
    } catch (const std::runtime_error&) {
        reported = true;
    }

    SetRealtimeViolationHandler(nullptr);
    REQUIRE_FALSE(reported);
}

#include "jamn_dsp/peer_mixer.h"

#include <algorithm>

namespace jamn::dsp {

PeerMixer::PeerMixer() noexcept {
    for (int channel = 0; channel < kMaxChannels; ++channel) {
        scratchChannels_[static_cast<std::size_t>(channel)] =
            scratch_.data() + static_cast<std::size_t>(channel) * kMaxBlockFrames;
    }
}

void PeerMixer::Prepare(double sampleRate) noexcept {
    const bool anySolo = AnySolo();
    for (auto& strip : strips_) {
        strip.Prepare(sampleRate, anySolo);
    }
}

Strip& PeerMixer::strip(std::size_t index) noexcept {
    return strips_[index];
}

const Strip& PeerMixer::strip(std::size_t index) const noexcept {
    return strips_[index];
}

bool PeerMixer::AnySolo() const noexcept {
    for (const auto& strip : strips_) {
        if (strip.soloed()) {
            return true;
        }
    }
    return false;
}

void PeerMixer::Render(float* const* out, int numChannels, int numFrames) noexcept {
    const int channels = std::min(numChannels, kMaxChannels);
    if (channels <= 0 || numFrames <= 0) {
        return;
    }

    // Read once for the whole block, not per chunk and not per strip: a
    // solo toggled from the message thread mid-render must take effect on
    // the next block whole, never on half of this one.
    const bool anySolo = AnySolo();

    for (int offset = 0; offset < numFrames; offset += kMaxBlockFrames) {
        const int frames = std::min(kMaxBlockFrames, numFrames - offset);
        for (int channel = 0; channel < channels; ++channel) {
            outChannels_[static_cast<std::size_t>(channel)] = out[channel] + offset;
        }
        for (auto& strip : strips_) {
            strip.Render(outChannels_.data(), scratchChannels_.data(), channels, frames, anySolo);
        }
    }
}

}  // namespace jamn::dsp

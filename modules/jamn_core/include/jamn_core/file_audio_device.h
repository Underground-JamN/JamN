#pragma once

#include <functional>
#include <string>

namespace jamn::core {

// The shape a real audio device callback takes: deinterleaved float
// buffers, numChannels of them, numFrames samples each. Output-only - a
// simplified subset of JUCE's AudioIODeviceCallback shape, so
// jamn_platform's real backend can bridge into the same abstraction later.
using AudioCallback = std::function<void(float* const* outputChannels, int numChannels, int numFrames)>;

// Drives an AudioCallback for a fixed number of blocks, as a real audio
// device would, with no real hardware and no JUCE - this is what lets
// jamn_engine/jamn_dsp code get exercised over a realistic repeated-callback
// loop in CI. Each call is wrapped in a RealtimeScope, so a test using this
// class also exercises the allocation trap (see realtime_scope.h), not just
// a single synthetic call.
//
// Input is not modeled - Phase 0 is output-only (a button that plays a
// sound, a volume slider). Add an input path when something needs it.
class FileAudioDevice {
public:
    FileAudioDevice(int numChannels, int blockSize);

    // If set, Process() appends every block's output to this file as
    // headerless interleaved 32-bit float PCM, so tests can read it back.
    // Not a WAV writer - jamn_platform owns that, for real recordings.
    void SetOutputPath(std::string path);

    void Process(int numBlocks, const AudioCallback& callback);

    int numChannels() const { return numChannels_; }
    int blockSize() const { return blockSize_; }

private:
    int numChannels_;
    int blockSize_;
    std::string outputPath_;
};

}  // namespace jamn::core

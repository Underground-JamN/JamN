#include "jamn_core/file_audio_device.h"

#include <cstdio>
#include <stdexcept>
#include <vector>

#include "jamn_core/realtime_scope.h"

namespace jamn::core {

FileAudioDevice::FileAudioDevice(int numChannels, int blockSize)
    : numChannels_(numChannels), blockSize_(blockSize) {}

void FileAudioDevice::SetOutputPath(std::string path) {
    outputPath_ = std::move(path);
}

void FileAudioDevice::Process(int numBlocks, const AudioCallback& callback) {
    // All scratch space is allocated up front, outside the RealtimeScope
    // below - this loop simulates a real device, and a real device's
    // callback never gets to allocate either.
    std::vector<float> channelStorage(static_cast<std::size_t>(numChannels_) * blockSize_);
    std::vector<float*> channelPointers(numChannels_);
    for (int channel = 0; channel < numChannels_; ++channel) {
        channelPointers[channel] = channelStorage.data() + static_cast<std::size_t>(channel) * blockSize_;
    }
    std::vector<float> interleaved(channelStorage.size());

    std::FILE* outputFile = nullptr;
    if (!outputPath_.empty()) {
        outputFile = std::fopen(outputPath_.c_str(), "wb");
        if (!outputFile) {
            throw std::runtime_error("FileAudioDevice: could not open output path " + outputPath_);
        }
    }

    for (int block = 0; block < numBlocks; ++block) {
        {
            RealtimeScope scope;
            callback(channelPointers.data(), numChannels_, blockSize_);
        }

        // Interleaving and writing to disk happen off the simulated
        // real-time path, same as a real system would hand this off to a
        // separate disk-writer thread rather than do it on the audio
        // callback itself.
        if (outputFile) {
            for (int frame = 0; frame < blockSize_; ++frame) {
                for (int channel = 0; channel < numChannels_; ++channel) {
                    interleaved[static_cast<std::size_t>(frame) * numChannels_ + channel] =
                        channelPointers[channel][frame];
                }
            }
            std::fwrite(interleaved.data(), sizeof(float), interleaved.size(), outputFile);
        }
    }

    if (outputFile) {
        std::fclose(outputFile);
    }
}

}  // namespace jamn::core

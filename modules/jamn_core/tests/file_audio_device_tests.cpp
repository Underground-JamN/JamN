#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <stdexcept>
#include <vector>

#include "jamn_core/file_audio_device.h"
#include "jamn_core/realtime_scope.h"

using jamn::core::FileAudioDevice;
using jamn::core::RealtimeScope;
using jamn::core::SetRealtimeViolationHandler;

TEST_CASE("FileAudioDevice calls back numBlocks times with the configured shape", "[core][file_audio_device][fast]") {
    FileAudioDevice device(2, 4);
    int callCount = 0;

    device.Process(3, [&](float* const* output, int numChannels, int numFrames) {
        ++callCount;
        REQUIRE(numChannels == 2);
        REQUIRE(numFrames == 4);
        for (int channel = 0; channel < numChannels; ++channel) {
            for (int frame = 0; frame < numFrames; ++frame) {
                output[channel][frame] = 0.0f;
            }
        }
    });

    REQUIRE(callCount == 3);
}

TEST_CASE("FileAudioDevice's callback runs inside an active RealtimeScope", "[core][file_audio_device][fast]") {
    REQUIRE_FALSE(RealtimeScope::IsActive());
    FileAudioDevice device(1, 2);

    bool wasActiveInsideCallback = false;
    device.Process(1, [&](float* const* output, int, int numFrames) {
        wasActiveInsideCallback = RealtimeScope::IsActive();
        for (int frame = 0; frame < numFrames; ++frame) {
            output[0][frame] = 0.0f;
        }
    });

    REQUIRE(wasActiveInsideCallback);
    REQUIRE_FALSE(RealtimeScope::IsActive());
}

TEST_CASE("FileAudioDevice writes correctly interleaved output to its output path", "[core][file_audio_device][fast]") {
    const std::string path = "file_audio_device_test_output.raw";
    const int numChannels = 2;
    const int blockSize = 3;
    const int numBlocks = 2;

    FileAudioDevice device(numChannels, blockSize);
    device.SetOutputPath(path);

    device.Process(numBlocks, [](float* const* output, int channels, int frames) {
        static int block = 0;
        for (int channel = 0; channel < channels; ++channel) {
            for (int frame = 0; frame < frames; ++frame) {
                output[channel][frame] = static_cast<float>(block * 100 + frame * 10 + channel);
            }
        }
        ++block;
    });

    std::FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);

    std::vector<float> samples(static_cast<std::size_t>(numChannels) * blockSize * numBlocks);
    const std::size_t read = std::fread(samples.data(), sizeof(float), samples.size(), file);
    std::fclose(file);
    std::remove(path.c_str());

    REQUIRE(read == samples.size());

    std::size_t index = 0;
    for (int block = 0; block < numBlocks; ++block) {
        for (int frame = 0; frame < blockSize; ++frame) {
            for (int channel = 0; channel < numChannels; ++channel) {
                REQUIRE(samples[index++] == static_cast<float>(block * 100 + frame * 10 + channel));
            }
        }
    }
}

TEST_CASE("An allocation inside FileAudioDevice's callback is reported as a RealtimeScope violation",
          "[core][file_audio_device][fast]") {
    SetRealtimeViolationHandler([](const char*) { throw std::runtime_error("rt violation"); });

    FileAudioDevice device(1, 4);
    bool reported = false;
    try {
        device.Process(1, [](float* const* output, int, int numFrames) {
            std::vector<float> scratch(static_cast<std::size_t>(numFrames));
            for (int frame = 0; frame < numFrames; ++frame) {
                output[0][frame] = scratch[static_cast<std::size_t>(frame)];
            }
        });
    } catch (const std::runtime_error&) {
        reported = true;
    }

    SetRealtimeViolationHandler(nullptr);
    REQUIRE(reported);
}

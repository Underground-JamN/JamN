#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <vector>

#include "jamn_dsp/master_bus.h"

using jamn::dsp::MasterBus;

TEST_CASE("MasterBus passes audio through unchanged at its default unity gain", "[dsp][master_bus][fast]") {
    MasterBus bus;
    bus.Prepare(48000.0);

    const int numFrames = 128;
    std::vector<float> channel(numFrames, 1.0f);
    float* channels[] = {channel.data()};

    bus.Process(channels, 1, numFrames);

    for (int frame = 0; frame < numFrames; ++frame) {
        REQUIRE(channel[frame] == 1.0f);
    }
}

TEST_CASE("MasterBus ramps toward a new gain target instead of stepping to it", "[dsp][master_bus][fast]") {
    MasterBus bus;
    bus.Prepare(48000.0);
    bus.SetGain(0.0f);

    const int numFrames = 128;
    std::vector<float> channel(numFrames, 1.0f);
    float* channels[] = {channel.data()};

    bus.Process(channels, 1, numFrames);

    REQUIRE(channel[0] > 0.99f);
    for (int frame = 1; frame < numFrames; ++frame) {
        REQUIRE(std::fabs(channel[frame] - channel[frame - 1]) < 0.01f);
    }
}

TEST_CASE("MasterBus arrives at its gain target after enough blocks", "[dsp][master_bus][fast]") {
    MasterBus bus;
    bus.Prepare(48000.0);
    bus.SetGain(0.5f);

    const int numFrames = 128;
    std::vector<float> channel(numFrames);
    float* channels[] = {channel.data()};

    for (int block = 0; block < 100; ++block) {
        for (int frame = 0; frame < numFrames; ++frame) {
            channel[frame] = 1.0f;
        }
        bus.Process(channels, 1, numFrames);
    }

    REQUIRE(std::fabs(bus.currentGain() - 0.5f) < 1.0e-3f);
}

TEST_CASE("MasterBus applies the same gain to every channel in a block", "[dsp][master_bus][fast]") {
    MasterBus bus;
    bus.Prepare(48000.0);
    bus.SetGain(0.5f);

    const int numFrames = 64;
    std::vector<float> left(numFrames, 1.0f);
    std::vector<float> right(numFrames, 1.0f);
    float* channels[] = {left.data(), right.data()};

    bus.Process(channels, 2, numFrames);

    for (int frame = 0; frame < numFrames; ++frame) {
        REQUIRE(left[frame] == right[frame]);
    }
}

TEST_CASE("MasterBus gain is publishable without a lock", "[dsp][master_bus][fast]") {
    REQUIRE(std::atomic<float>::is_always_lock_free);
}

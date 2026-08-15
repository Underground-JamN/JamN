#pragma once

#include <chrono>

#include "jamn_core/clock.h"

namespace jamn::core {

// Wraps std::chrono::steady_clock. nowUs() reports microseconds elapsed
// since this object was constructed, not since any real epoch - session
// time is always relative to when a session started, never to wall-clock
// midnight.
class SteadyClock : public IClock {
public:
    SteadyClock() : epoch_(std::chrono::steady_clock::now()) {}

    SessionTime nowUs() const override {
        const auto elapsed = std::chrono::steady_clock::now() - epoch_;
        return SessionTime(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    }

private:
    std::chrono::steady_clock::time_point epoch_;
};

}  // namespace jamn::core

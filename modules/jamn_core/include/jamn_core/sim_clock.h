#pragma once

#include <algorithm>
#include <cstdint>

#include "jamn_core/clock.h"

namespace jamn::core {

// A settable virtual clock. Monotonic by construction - Advance() clamps a
// negative delta to zero rather than letting the clock run backwards - and
// it never advances on its own: wall time never touches it. Only whatever
// drives it (the sim harness, Wave 3's SimTransport driver) calls Advance().
class SimClock : public IClock {
public:
    explicit SimClock(SessionTime start = SessionTime(0)) : now_(start) {}

    SessionTime nowUs() const override { return now_; }

    void Advance(std::int64_t deltaUs) { now_ = now_ + std::max<std::int64_t>(deltaUs, 0); }

private:
    SessionTime now_;
};

}  // namespace jamn::core

#pragma once

#include "jamn_core/time_types.h"

namespace jamn::core {

// Common surface for anything that can report "now" as a SessionTime.
// SteadyClock wraps the real wall clock; SimClock is a settable virtual
// clock the sim harness drives explicitly - both live in modules/jamn_core
// so the code under test never has to know which one it was given.
class IClock {
public:
    virtual ~IClock() = default;
    virtual SessionTime nowUs() const = 0;
};

}  // namespace jamn::core

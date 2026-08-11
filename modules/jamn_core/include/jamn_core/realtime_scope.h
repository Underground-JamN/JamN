#pragma once

#include <cstdio>
#include <cstdlib>

namespace jamn::core {

// Called when a violation (currently: an allocation) is detected while a
// RealtimeScope is active. Defaults to aborting the process; tests replace
// it with a handler that throws instead, so a violation can be asserted on
// without killing the whole test binary.
using RealtimeViolationHandler = void (*)(const char* what);

inline void DefaultRealtimeViolationHandler(const char* what) {
    std::fprintf(stderr, "RealtimeScope violation: %s\n", what);
    std::abort();
}

inline RealtimeViolationHandler& RealtimeViolationHandlerSlot() {
    static RealtimeViolationHandler handler = &DefaultRealtimeViolationHandler;
    return handler;
}

inline void SetRealtimeViolationHandler(RealtimeViolationHandler handler) {
    RealtimeViolationHandlerSlot() = handler ? handler : &DefaultRealtimeViolationHandler;
}

inline void ReportRealtimeViolation(const char* what) {
    RealtimeViolationHandlerSlot()(what);
}

// RAII marker for a real-time-safe section (the audio callback, ultimately).
// Construction/destruction only ever touch a thread-local int, so the scope
// itself never allocates, locks or logs - see AGENTS.md's real-time rules.
//
// On its own this only tracks whether a real-time section is active,
// checkable via IsActive(). The actual trap - global operator new/delete
// calling ReportRealtimeViolation() when IsActive() is true - lives in
// tests/realtime_allocation_guard.cpp, compiled only into jamn_core_tests,
// not into jamn_core itself: overriding global operator new/delete is a
// whole-binary decision that production code should opt into deliberately,
// not inherit for free from linking this header.
class RealtimeScope {
public:
    RealtimeScope() { ++Depth(); }
    ~RealtimeScope() { --Depth(); }
    RealtimeScope(const RealtimeScope&) = delete;
    RealtimeScope& operator=(const RealtimeScope&) = delete;

    static bool IsActive() { return Depth() > 0; }

private:
    static int& Depth() {
        thread_local int depth = 0;
        return depth;
    }
};

}  // namespace jamn::core

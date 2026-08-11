// Global operator new/delete overrides that report a RealtimeScope
// violation for any allocation made while a RealtimeScope is active.
// Linked only into jamn_core_tests (see that target's CMakeLists.txt) -
// this is what docs/RT_RULES.md means by "test builds trap allocations",
// not a property of jamn_core itself.
//
// Verified under the linux-asan preset: our own definitions win over
// AddressSanitizer's own operator new/delete replacements at link time, so
// this must define every overload actually reachable from user code
// (sized and non-sized delete, both included above C++14) with matching
// malloc/free semantics throughout. Defining only the non-sized delete let
// the sized overload fall through to ASan's own replacement instead, which
// expects ASan-tracked memory - reported as an alloc-dealloc-mismatch on
// every allocation, since ours came from a plain malloc.

#include "jamn_core/realtime_scope.h"

#include <cstdlib>
#include <new>

namespace {

// A violation handler that throws (as tests do, to assert on a violation
// without aborting the process) allocates while constructing its exception
// object - e.g. std::runtime_error's message storage. Without this guard
// that re-enters operator new while RealtimeScope is still active,
// reporting a second violation, throwing again while constructing *that*
// exception, and so on until the stack overflows. The RAII guard makes the
// nested allocation fall through to malloc unchecked instead, and resets
// itself via the destructor even when ReportRealtimeViolation throws.
thread_local bool g_reportingRealtimeViolation = false;

struct ReentrancyGuard {
    ReentrancyGuard() { g_reportingRealtimeViolation = true; }
    ~ReentrancyGuard() { g_reportingRealtimeViolation = false; }
};

}  // namespace

void* operator new(std::size_t size) {
    if (jamn::core::RealtimeScope::IsActive() && !g_reportingRealtimeViolation) {
        ReentrancyGuard guard;
        jamn::core::ReportRealtimeViolation("operator new");
    }
    if (void* ptr = std::malloc(size)) {
        return ptr;
    }
    throw std::bad_alloc();
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

void* operator new[](std::size_t size) {
    return operator new(size);
}

void operator delete[](void* ptr) noexcept {
    operator delete(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    operator delete(ptr);
}

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include "jamn_core/realtime_scope.h"

using jamn::core::RealtimeScope;
using jamn::core::SetRealtimeViolationHandler;

TEST_CASE("RealtimeScope is inactive with no scope on the stack", "[core][realtime_scope][fast]") {
    REQUIRE_FALSE(RealtimeScope::IsActive());
}

TEST_CASE("RealtimeScope is active only for its lifetime", "[core][realtime_scope][fast]") {
    REQUIRE_FALSE(RealtimeScope::IsActive());
    {
        RealtimeScope scope;
        REQUIRE(RealtimeScope::IsActive());
    }
    REQUIRE_FALSE(RealtimeScope::IsActive());
}

TEST_CASE("Nested RealtimeScopes stay active until the outermost ends", "[core][realtime_scope][fast]") {
    RealtimeScope outer;
    REQUIRE(RealtimeScope::IsActive());
    {
        RealtimeScope inner;
        REQUIRE(RealtimeScope::IsActive());
    }
    REQUIRE(RealtimeScope::IsActive());
}

TEST_CASE("Allocating outside any RealtimeScope succeeds normally", "[core][realtime_scope][fast]") {
    REQUIRE_FALSE(RealtimeScope::IsActive());
    int* value = new int(7);
    REQUIRE(*value == 7);
    delete value;
}

TEST_CASE("Allocating inside a RealtimeScope reports a violation", "[core][realtime_scope][fast]") {
    SetRealtimeViolationHandler([](const char*) { throw std::runtime_error("rt violation"); });

    bool reported = false;
    // volatile forces the allocation to be observable: a new/delete pair
    // with no use of the pointee is a standard-sanctioned target for
    // allocator elision ([expr.new]), which GCC applies at -O1 under
    // -fsanitize=thread (though not, usefully, under -fsanitize=address,
    // which needs every allocation to actually happen) - found by this
    // exact test silently passing-by-doing-nothing under linux-tsan.
    volatile int sink = 0;
    try {
        RealtimeScope scope;
        int* value = new int(5);
        sink = *value;
        delete value;
    } catch (const std::runtime_error&) {
        reported = true;
    }
    (void)sink;

    SetRealtimeViolationHandler(nullptr);
    REQUIRE(reported);
}

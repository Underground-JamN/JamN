#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace jamn::core {

// Single-producer, single-consumer, fixed-capacity ring buffer. Push/Pop
// never allocate, lock or block, so both are safe to call from the audio
// callback as long as only one thread ever calls Push and only one (possibly
// different) thread ever calls Pop.
template <typename T, std::size_t Capacity>
class SpscRing {
public:
    static_assert(Capacity > 0, "SpscRing capacity must be positive");

    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    bool Push(const T& value) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = Advance(head);
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        buffer_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool Pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        out = buffer_[tail];
        tail_.store(Advance(tail), std::memory_order_release);
        return true;
    }

    // Racy against a concurrent Push/Pop by design; useful only for
    // diagnostics, never for correctness decisions.
    std::size_t SizeApprox() const {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (head + kSlots - tail) % kSlots;
    }

    static constexpr std::size_t capacity() { return Capacity; }

private:
    static constexpr std::size_t kSlots = Capacity + 1;

    static constexpr std::size_t Advance(std::size_t index) {
        return (index + 1) % kSlots;
    }

    std::array<T, kSlots> buffer_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};

}  // namespace jamn::core

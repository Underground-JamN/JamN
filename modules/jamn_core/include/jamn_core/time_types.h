#pragma once

#include <cstdint>

namespace jamn::core {

// Three distinct wrapper types over int64_t - never interchangeable
// (docs/CLOCK.md's "Three time types" table). Each constructor is
// `explicit` and none defines a conversion operator, so there is no
// implicit conversion between any pair of these types, nor to plain
// int64_t in either direction; every conversion is a named accessor called
// explicitly at the point of use (`.us()`, `.samples()`, `.ticks()`).

// Microseconds since the session epoch. The only "when" that appears on
// the wire - shared across every peer.
class SessionTime {
public:
    constexpr SessionTime() = default;
    explicit constexpr SessionTime(std::int64_t us) : us_(us) {}

    constexpr std::int64_t us() const { return us_; }

    friend constexpr bool operator==(SessionTime a, SessionTime b) { return a.us_ == b.us_; }
    friend constexpr bool operator!=(SessionTime a, SessionTime b) { return a.us_ != b.us_; }
    friend constexpr bool operator<(SessionTime a, SessionTime b) { return a.us_ < b.us_; }
    friend constexpr bool operator<=(SessionTime a, SessionTime b) { return a.us_ <= b.us_; }
    friend constexpr bool operator>(SessionTime a, SessionTime b) { return a.us_ > b.us_; }
    friend constexpr bool operator>=(SessionTime a, SessionTime b) { return a.us_ >= b.us_; }

    friend constexpr SessionTime operator+(SessionTime a, std::int64_t deltaUs) {
        return SessionTime(a.us_ + deltaUs);
    }
    friend constexpr std::int64_t operator-(SessionTime a, SessionTime b) { return a.us_ - b.us_; }

private:
    std::int64_t us_ = 0;
};

// Samples at the local device's rate. Local only - peers run different
// device rates, so this never appears on the wire.
class SampleTime {
public:
    constexpr SampleTime() = default;
    explicit constexpr SampleTime(std::int64_t samples) : samples_(samples) {}

    constexpr std::int64_t samples() const { return samples_; }

    friend constexpr bool operator==(SampleTime a, SampleTime b) { return a.samples_ == b.samples_; }
    friend constexpr bool operator!=(SampleTime a, SampleTime b) { return a.samples_ != b.samples_; }
    friend constexpr bool operator<(SampleTime a, SampleTime b) { return a.samples_ < b.samples_; }
    friend constexpr bool operator<=(SampleTime a, SampleTime b) { return a.samples_ <= b.samples_; }
    friend constexpr bool operator>(SampleTime a, SampleTime b) { return a.samples_ > b.samples_; }
    friend constexpr bool operator>=(SampleTime a, SampleTime b) { return a.samples_ >= b.samples_; }

    friend constexpr SampleTime operator+(SampleTime a, std::int64_t deltaSamples) {
        return SampleTime(a.samples_ + deltaSamples);
    }
    friend constexpr std::int64_t operator-(SampleTime a, SampleTime b) { return a.samples_ - b.samples_; }

private:
    std::int64_t samples_ = 0;
};

// PPQ ticks, absolute and ever-increasing. Shared across peers, but only
// meaningful once derived through a tempo map (Phase 2b) - nothing in
// Phase 0.5 produces one.
inline constexpr std::int64_t kTicksPerQuarter = 960;

class MusicalTime {
public:
    constexpr MusicalTime() = default;
    explicit constexpr MusicalTime(std::int64_t ticks) : ticks_(ticks) {}

    constexpr std::int64_t ticks() const { return ticks_; }

    friend constexpr bool operator==(MusicalTime a, MusicalTime b) { return a.ticks_ == b.ticks_; }
    friend constexpr bool operator!=(MusicalTime a, MusicalTime b) { return a.ticks_ != b.ticks_; }
    friend constexpr bool operator<(MusicalTime a, MusicalTime b) { return a.ticks_ < b.ticks_; }
    friend constexpr bool operator<=(MusicalTime a, MusicalTime b) { return a.ticks_ <= b.ticks_; }
    friend constexpr bool operator>(MusicalTime a, MusicalTime b) { return a.ticks_ > b.ticks_; }
    friend constexpr bool operator>=(MusicalTime a, MusicalTime b) { return a.ticks_ >= b.ticks_; }

    friend constexpr MusicalTime operator+(MusicalTime a, std::int64_t deltaTicks) {
        return MusicalTime(a.ticks_ + deltaTicks);
    }
    friend constexpr std::int64_t operator-(MusicalTime a, MusicalTime b) { return a.ticks_ - b.ticks_; }

private:
    std::int64_t ticks_ = 0;
};

}  // namespace jamn::core

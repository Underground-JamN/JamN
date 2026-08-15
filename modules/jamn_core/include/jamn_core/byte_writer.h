#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace jamn::core {

// Bounds-checked writer into a caller-owned byte buffer. Every write either
// succeeds and advances the cursor, or fails (returns false) and leaves the
// cursor unchanged - it never writes past the end of the buffer and never
// throws. No allocation: the buffer is supplied by the caller, sized ahead
// of encoding, matching ByteReader's symmetric bounds-checked contract.
class ByteWriter {
public:
    ByteWriter(std::uint8_t* data, std::size_t capacity) : data_(data), capacity_(capacity) {}

    bool WriteU8(std::uint8_t v) { return WriteLE(v); }
    bool WriteU16(std::uint16_t v) { return WriteLE(v); }
    bool WriteU32(std::uint32_t v) { return WriteLE(v); }
    bool WriteU64(std::uint64_t v) { return WriteLE(v); }

    bool WriteI8(std::int8_t v) { return WriteLE(static_cast<std::uint8_t>(v)); }
    bool WriteI16(std::int16_t v) { return WriteLE(static_cast<std::uint16_t>(v)); }
    bool WriteI32(std::int32_t v) { return WriteLE(static_cast<std::uint32_t>(v)); }
    bool WriteI64(std::int64_t v) { return WriteLE(static_cast<std::uint64_t>(v)); }

    // Copies exactly len bytes from src. False, cursor unchanged, if fewer
    // than len bytes of capacity remain.
    bool WriteBytes(const std::uint8_t* src, std::size_t len) {
        if (Remaining() < len) return false;
        for (std::size_t i = 0; i < len; ++i) {
            data_[pos_ + i] = src[i];
        }
        pos_ += len;
        return true;
    }

    std::size_t Remaining() const { return capacity_ - pos_; }
    std::size_t Position() const { return pos_; }
    std::size_t Capacity() const { return capacity_; }

private:
    template <typename T>
    bool WriteLE(T v) {
        static_assert(std::is_unsigned_v<T>, "WriteLE is for unsigned fixed-width integers only");
        constexpr std::size_t n = sizeof(T);
        if (Remaining() < n) return false;
        for (std::size_t i = 0; i < n; ++i) {
            data_[pos_ + i] = static_cast<std::uint8_t>(v >> (8 * i));
        }
        pos_ += n;
        return true;
    }

    std::uint8_t* data_;
    std::size_t capacity_;
    std::size_t pos_ = 0;
};

}  // namespace jamn::core

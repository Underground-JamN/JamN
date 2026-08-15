#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace jamn::core {

// Bounds-checked reader over a caller-owned byte buffer. Every read either
// succeeds and advances the cursor, or fails (returns false) and leaves the
// cursor unchanged - it never reads past the end of the buffer and never
// throws. This is the "fuzz-tested invariant" docs/PROTOCOL.md rule 4 rests
// on: no struct memcpy, no bitfields, no #pragma pack, everything decoded
// byte-by-byte as fixed-width little-endian.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool ReadU8(std::uint8_t& out) { return ReadLE(out); }
    bool ReadU16(std::uint16_t& out) { return ReadLE(out); }
    bool ReadU32(std::uint32_t& out) { return ReadLE(out); }
    bool ReadU64(std::uint64_t& out) { return ReadLE(out); }

    bool ReadI8(std::int8_t& out) {
        std::uint8_t u = 0;
        if (!ReadLE(u)) return false;
        out = static_cast<std::int8_t>(u);
        return true;
    }
    bool ReadI16(std::int16_t& out) {
        std::uint16_t u = 0;
        if (!ReadLE(u)) return false;
        out = static_cast<std::int16_t>(u);
        return true;
    }
    bool ReadI32(std::int32_t& out) {
        std::uint32_t u = 0;
        if (!ReadLE(u)) return false;
        out = static_cast<std::int32_t>(u);
        return true;
    }
    bool ReadI64(std::int64_t& out) {
        std::uint64_t u = 0;
        if (!ReadLE(u)) return false;
        out = static_cast<std::int64_t>(u);
        return true;
    }

    // Copies exactly len bytes into out. False, cursor unchanged, if fewer
    // than len bytes remain - out is left untouched in that case.
    bool ReadBytes(std::uint8_t* out, std::size_t len) {
        if (Remaining() < len) return false;
        for (std::size_t i = 0; i < len; ++i) {
            out[i] = data_[pos_ + i];
        }
        pos_ += len;
        return true;
    }

    // Advances past len bytes without copying them. False, cursor
    // unchanged, if fewer than len bytes remain - this is how an unknown
    // TLV type is skipped per docs/PROTOCOL.md rule 1.
    bool Skip(std::size_t len) {
        if (Remaining() < len) return false;
        pos_ += len;
        return true;
    }

    // Zero-copy: points out at exactly the next len bytes and advances past
    // them, without copying. False, cursor unchanged, if fewer than len
    // bytes remain. This is how TLV framing (jamn_proto/tlv.h) hands a
    // decoder a reader scoped to just its own value, regardless of how many
    // bytes that decoder actually reads from it.
    bool ReadSlice(ByteReader& out, std::size_t len) {
        if (Remaining() < len) return false;
        out = ByteReader(data_ + pos_, len);
        pos_ += len;
        return true;
    }

    std::size_t Remaining() const { return size_ - pos_; }
    std::size_t Position() const { return pos_; }
    std::size_t Size() const { return size_; }

private:
    template <typename T>
    bool ReadLE(T& out) {
        static_assert(std::is_unsigned_v<T>, "ReadLE is for unsigned fixed-width integers only");
        constexpr std::size_t n = sizeof(T);
        if (Remaining() < n) return false;
        T value = 0;
        for (std::size_t i = 0; i < n; ++i) {
            value |= static_cast<T>(static_cast<T>(data_[pos_ + i]) << (8 * i));
        }
        pos_ += n;
        out = value;
        return true;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

}  // namespace jamn::core

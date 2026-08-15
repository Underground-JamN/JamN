#include "jamn_proto/join_auth.h"

namespace jamn::proto {

bool ConstantTimeEquals(const std::uint8_t* a, const std::uint8_t* b, std::size_t len) {
    // volatile forces every byte to actually be read and accumulated, not
    // just optimized down to something branchy (or to a memcmp call) that
    // would reopen the timing side-channel this function exists to close.
    volatile std::uint8_t diff = 0;
    for (std::size_t i = 0; i < len; ++i) {
        diff = static_cast<std::uint8_t>(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

}  // namespace jamn::proto

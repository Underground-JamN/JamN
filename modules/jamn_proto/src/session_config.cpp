#include "jamn_proto/session_config.h"

namespace jamn::proto {

bool EncodeSessionConfig(const SessionConfig& config, jamn::core::ByteWriter& out) {
    return out.WriteU8(config.syncMode);
}

bool DecodeSessionConfig(jamn::core::ByteReader& in, SessionConfig& out) {
    SessionConfig config;
    if (!in.ReadU8(config.syncMode)) return false;
    out = config;
    return true;
}

}  // namespace jamn::proto

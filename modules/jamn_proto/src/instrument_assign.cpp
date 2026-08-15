#include "jamn_proto/instrument_assign.h"

namespace jamn::proto {

bool EncodeInstrumentAssign(const InstrumentAssign& assign, jamn::core::ByteWriter& out) {
    if (!out.WriteBytes(assign.bankName.data(), assign.bankName.size())) return false;
    if (!out.WriteBytes(assign.sha256.data(), assign.sha256.size())) return false;
    if (!out.WriteU32(assign.preset)) return false;
    return true;
}

bool DecodeInstrumentAssign(jamn::core::ByteReader& in, InstrumentAssign& out) {
    InstrumentAssign assign;
    if (!in.ReadBytes(assign.bankName.data(), assign.bankName.size())) return false;
    if (!in.ReadBytes(assign.sha256.data(), assign.sha256.size())) return false;
    if (!in.ReadU32(assign.preset)) return false;
    out = assign;
    return true;
}

}  // namespace jamn::proto

#include "jamn_proto/hello.h"

namespace jamn::proto {

bool EncodeHello(const Hello& hello, jamn::core::ByteWriter& out) {
    if (!out.WriteBytes(hello.sessionToken.data(), hello.sessionToken.size())) return false;
    if (!out.WriteBytes(hello.buildHash.data(), hello.buildHash.size())) return false;
    if (!out.WriteU32(hello.instrumentBankVersion)) return false;
    if (!out.WriteU32(hello.capabilities)) return false;
    return true;
}

bool DecodeHello(jamn::core::ByteReader& in, Hello& out) {
    Hello hello;
    if (!in.ReadBytes(hello.sessionToken.data(), hello.sessionToken.size())) return false;
    if (!in.ReadBytes(hello.buildHash.data(), hello.buildHash.size())) return false;
    if (!in.ReadU32(hello.instrumentBankVersion)) return false;
    if (!in.ReadU32(hello.capabilities)) return false;
    out = hello;
    return true;
}

}  // namespace jamn::proto

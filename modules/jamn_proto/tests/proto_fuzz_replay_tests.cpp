// Replays every blob in tests/fuzz_corpus/ (golden vectors wrapped as real
// packets, plus hand-written hostile framing) through the full decode
// path. There is no CI and no nightly fuzzer job in this project - this
// corpus-replay test, run on every `ctest -L fast`, is what satisfies
// docs/PROTOCOL.md rule 4's "fuzz-tested invariant" instead (T2.5's
// decision, see fuzz_corpus/README.md). The assertion is structural: this
// test passes by *completing* - a crash, an ASan/UBSan violation, or a
// hang would fail the whole binary, not just one REQUIRE.
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "jamn_core/byte_reader.h"
#include "jamn_proto/hello.h"
#include "jamn_proto/instrument_assign.h"
#include "jamn_proto/message_type.h"
#include "jamn_proto/note_burst.h"
#include "jamn_proto/packet.h"
#include "jamn_proto/session_config.h"

using namespace jamn::core;
using namespace jamn::proto;

namespace {

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Best-effort dispatch to each known message type's decoder. Failure to
// decode a given TLV's value is not an error here - a hostile blob is
// *supposed* to fail cleanly. Only a crash or a hang would be a problem,
// and neither can happen: every read underneath is ByteReader's
// bounds-checked primitives.
void TryDecodeKnownType(std::uint16_t type, ByteReader& value) {
    switch (static_cast<MessageType>(type)) {
        case MessageType::kJoin: {
            Hello h;
            DecodeHello(value, h);
            break;
        }
        case MessageType::kSessionConfig: {
            SessionConfig c;
            DecodeSessionConfig(value, c);
            break;
        }
        case MessageType::kInstrumentAssign: {
            InstrumentAssign ia;
            DecodeInstrumentAssign(value, ia);
            break;
        }
        case MessageType::kNoteBurst: {
            NoteBurst nb;
            DecodeNoteBurst(value, nb);
            break;
        }
        default:
            break;  // Reserved or unrecognised - rule 1: skip, not an error.
    }
}

void ReplayPacket(const std::vector<std::uint8_t>& data) {
    ByteReader reader(data.data(), data.size());
    PacketHeader header;
    // Return value deliberately unchecked - a hostile blob returning false
    // (malformed framing) is exactly what several corpus entries expect.
    DecodePacket(reader, header, [](std::uint16_t type, ByteReader& value) { TryDecodeKnownType(type, value); });
}

}  // namespace

TEST_CASE("fuzz corpus replay: every committed blob decodes without crashing", "[proto][fuzz_corpus][fast]") {
    const std::filesystem::path dir(JAMN_PROTO_FUZZ_CORPUS_DIR);
    int filesReplayed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".bin") continue;
        INFO("replaying: " << entry.path().filename());
        const std::vector<std::uint8_t> data = ReadFile(entry.path());
        ReplayPacket(data);
        ++filesReplayed;
    }
    // Proves the corpus was actually found and iterated, not silently
    // skipped by a wrong directory or extension filter.
    REQUIRE(filesReplayed >= 7);
}

// T4.3's checkpoint peer: one process, one real UDP socket, one PeerRuntime.
// Two of these - one --role host, one --role client - are driven against
// each other on 127.0.0.1 by run_two_process_loopback.py, which is what
// registers the actual ctest case under the `net` label.
//
// This is a separate process rather than a second thread on purpose. The
// point of the checkpoint is that the timing core works across a real
// socket with a real peer that shares none of its memory - a same-process
// pair (which modules/jamn_net/tests/enet_transport_tests.cpp already
// covers for the transport itself) cannot prove that.
//
// **The join handshake is deliberately not exercised here.** `jamn_session`
// has host-side authority only; how a refusal reason reaches a client is an
// open protocol decision belonging to a later wave, and inventing a
// client-side state machine now would pre-empt it. What this proves is the
// note path: connect, exchange, dedupe, cross to the audio side, and finish
// with nothing stuck.
//
// Output is `key=value` lines on stdout for the driver to parse. The exit
// code is the verdict; the lines are the diagnosis.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <utility>

#include "jamn_engine/note_crossing.h"
#include "jamn_engine/peer_runtime.h"
#include "jamn_net/enet_transport.h"

namespace {

using jamn::engine::NoteCrossing;
using jamn::engine::PeerRuntime;
using jamn::net::Channel;
using jamn::net::PeerEvent;
using jamn::net::PeerId;
using jamn::proto::NoteEvent;
using jamn::proto::NoteEventKind;

std::int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char* OptionValue(int argc, char** argv, const char* name, const char* fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return fallback;
}

struct Outcome {
    std::uint64_t submitted = 0;
    std::uint64_t delivered = 0;
    std::uint64_t noteOns = 0;
    std::uint64_t noteOffs = 0;
    std::uint64_t peerEvents = 0;
    std::uint64_t controlPackets = 0;
    std::size_t stuck = 0;
    bool everConnected = false;
};

// One note-on every kNoteIntervalUs, its note-off kNoteHoldUs later, for
// kPlayMs - then kDrainMs of silence so trailing K=3 copies and the last
// note-off have somewhere to land before anything is counted.
constexpr std::int64_t kNoteIntervalUs = 25'000;
constexpr std::int64_t kNoteHoldUs = 12'000;
constexpr std::int64_t kServiceSleepUs = 1000;

int RunPeer(bool isHost, std::uint16_t port, std::int64_t playMs, std::int64_t drainMs) {
    jamn::net::EnetTransport transport;
    if (isHost) {
        if (!transport.Listen(port, /*maxPeers=*/4)) {
            std::fprintf(stderr, "loopback_peer: could not bind 127.0.0.1:%u\n", port);
            return 2;
        }
    } else if (!transport.Connect("127.0.0.1", port)) {
        std::fprintf(stderr, "loopback_peer: could not start connect to 127.0.0.1:%u\n", port);
        return 2;
    }

    const std::int64_t startUs = NowUs();
    PeerRuntime runtime(transport, startUs);
    Outcome outcome;

    runtime.SetPeerEventCallback([&outcome](PeerId, PeerEvent event) {
        ++outcome.peerEvents;
        if (event == PeerEvent::kConnected) outcome.everConnected = true;
    });
    // Wired to a counter rather than to SessionHost: this proves the control
    // seam is reachable across a real socket without asserting anything
    // about a handshake this checkpoint does not run.
    runtime.SetControlPacketCallback([&outcome](PeerId, jamn::core::ByteReader&) { ++outcome.controlPackets; });

    std::set<std::pair<PeerId, std::uint8_t>> held;
    std::int64_t nextNoteOnUs = startUs;
    bool haveHeldNote = false;
    std::int64_t heldUntilUs = 0;
    std::uint8_t heldNote = 0;
    std::uint8_t noteCounter = 0;
    bool sentControlProbe = false;

    const std::int64_t playUntilUs = startUs + playMs * 1000;
    const std::int64_t stopUs = playUntilUs + drainMs * 1000;

    while (true) {
        const std::int64_t nowUs = NowUs();
        if (nowUs >= stopUs) break;

        if (outcome.everConnected) {
            if (nowUs < playUntilUs && nowUs >= nextNoteOnUs && !haveHeldNote) {
                NoteEvent on;
                on.kind = NoteEventKind::kNoteOn;
                on.a = noteCounter++ % 128;
                if (runtime.SubmitLocalEvent(on, nowUs)) ++outcome.submitted;
                haveHeldNote = true;
                heldNote = on.a;
                heldUntilUs = nowUs + kNoteHoldUs;
                nextNoteOnUs = nowUs + kNoteIntervalUs;
            }
            // Deliberately *not* gated on the play window. A note still held
            // when that window closes has to be released anyway, or the
            // sender simply never sends a note-off and the receiver's stuck
            // note measures this harness rather than the runtime.
            if (haveHeldNote && nowUs >= heldUntilUs) {
                NoteEvent off;
                off.kind = NoteEventKind::kNoteOff;
                off.a = heldNote;
                if (runtime.SubmitLocalEvent(off, nowUs)) ++outcome.submitted;
                haveHeldNote = false;
            }
            // One control-channel packet, once, from the client only - the
            // cheapest thing that proves the seam carries real traffic.
            if (!isHost && !sentControlProbe) {
                const std::uint8_t probe[] = {0xC0, 0xDE};
                for (std::size_t slot = 0; slot < PeerRuntime::kMaxPeers; ++slot) {
                    const PeerId peer = runtime.PeerAt(slot);
                    if (peer == PeerRuntime::kNoPeer) continue;
                    if (transport.Send(peer, Channel::kControl, probe, sizeof(probe))) sentControlProbe = true;
                }
            }
        }

        runtime.Service(nowUs);

        // The audio thread's side of the crossing. Driven from this loop
        // rather than from a real callback: there is no audio device in
        // this process, and the crossing's contract is what is under test.
        NoteCrossing::RemoteNote note;
        for (std::size_t slot = 0; slot < PeerRuntime::kMaxPeers; ++slot) {
            while (runtime.crossing().Consume(slot, note)) {
                ++outcome.delivered;
                const auto key = std::make_pair(note.peer, note.event.a);
                if (note.event.kind == NoteEventKind::kNoteOn) {
                    ++outcome.noteOns;
                    held.insert(key);
                } else if (note.event.kind == NoteEventKind::kNoteOff) {
                    ++outcome.noteOffs;
                    held.erase(key);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::microseconds(kServiceSleepUs));
    }

    outcome.stuck = held.size();
    const PeerRuntime::Stats& stats = runtime.stats();

    std::printf("role=%s\n", isHost ? "host" : "client");
    std::printf("connected=%d\n", outcome.everConnected ? 1 : 0);
    std::printf("peer_events=%llu\n", static_cast<unsigned long long>(outcome.peerEvents));
    std::printf("submitted=%llu\n", static_cast<unsigned long long>(outcome.submitted));
    std::printf("delivered=%llu\n", static_cast<unsigned long long>(outcome.delivered));
    std::printf("note_ons=%llu\n", static_cast<unsigned long long>(outcome.noteOns));
    std::printf("note_offs=%llu\n", static_cast<unsigned long long>(outcome.noteOffs));
    std::printf("stuck=%llu\n", static_cast<unsigned long long>(outcome.stuck));
    std::printf("control_packets=%llu\n", static_cast<unsigned long long>(outcome.controlPackets));
    std::printf("deduped=%llu\n", static_cast<unsigned long long>(stats.notesDeduped));
    std::printf("crossing_drops=%llu\n", static_cast<unsigned long long>(stats.notesDroppedAtCrossing));
    std::printf("packets_rejected=%llu\n", static_cast<unsigned long long>(stats.packetsRejected));
    std::printf("unknown_peer_packets=%llu\n", static_cast<unsigned long long>(stats.packetsFromUnknownPeer));
    std::printf("sends_failed=%llu\n", static_cast<unsigned long long>(stats.sendsFailed));
    std::printf("packets_too_large=%llu\n", static_cast<unsigned long long>(stats.packetsTooLarge));
    std::printf("clock_samples=%llu\n", static_cast<unsigned long long>(stats.clockSamplesFolded));
    std::fflush(stdout);

    // Every one of these is a way this run could look green while having
    // proven nothing. A peer that never connected exchanges no notes and
    // has no stuck ones either.
    if (!outcome.everConnected) {
        std::fprintf(stderr, "loopback_peer: never saw a kConnected peer event\n");
        return 3;
    }
    if (outcome.delivered == 0 || outcome.noteOns == 0 || outcome.noteOffs == 0) {
        std::fprintf(stderr, "loopback_peer: connected but exchanged no notes\n");
        return 4;
    }
    if (outcome.stuck != 0) {
        std::fprintf(stderr, "loopback_peer: %zu stuck note(s)\n", outcome.stuck);
        return 5;
    }
    if (stats.packetsRejected != 0 || stats.packetsFromUnknownPeer != 0 || stats.notesDroppedAtCrossing != 0 ||
        stats.sendsFailed != 0 || stats.packetsTooLarge != 0) {
        std::fprintf(stderr, "loopback_peer: runtime reported a fault\n");
        return 6;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* role = OptionValue(argc, argv, "--role", "");
    const bool isHost = std::strcmp(role, "host") == 0;
    if (!isHost && std::strcmp(role, "client") != 0) {
        std::fprintf(stderr, "usage: jamn_loopback_peer --role host|client --port N [--play-ms N] [--drain-ms N]\n");
        return 2;
    }

    const int port = std::atoi(OptionValue(argc, argv, "--port", "0"));
    if (port <= 0 || port > 65535) {
        std::fprintf(stderr, "jamn_loopback_peer: --port must be 1..65535\n");
        return 2;
    }

    const std::int64_t playMs = std::atoll(OptionValue(argc, argv, "--play-ms", "2000"));
    const std::int64_t drainMs = std::atoll(OptionValue(argc, argv, "--drain-ms", "1000"));
    return RunPeer(isHost, static_cast<std::uint16_t>(port), playMs, drainMs);
}

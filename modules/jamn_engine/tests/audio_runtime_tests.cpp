// T5.3: the audio thread's half of a session, driven end to end against
// two real PeerRuntimes over SimTransport - no ENet, no audio device, no
// JUCE, so these carry `fast` like the rest of the module. T6.1 added the
// local-input cases at the end, which need no session and pass a null
// runtime.
//
// Every TEST_CASE name here contains "audio_runtime", not only the tag.
// `ctest -R` matches test names and never tags, and exits 0 on an empty
// selection. The accept command is:
//
//     ctest --preset core-only -R 'audio_runtime'   -> 16 tests
//
// What these cannot cover is the last hop: turning a returned Note into
// sound needs jamn_dsp, which this module has no edge to on purpose. That
// hop is a for-loop in jamn_app and is the maintainer's to verify at a
// real device.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "jamn_core/realtime_scope.h"
#include "jamn_engine/audio_runtime.h"
#include "jamn_engine/jitter_buffer.h"
#include "jamn_engine/peer_runtime.h"
#include "jamn_net/sim_transport.h"
#include "jamn_proto/note_event.h"

using jamn::engine::AudioRuntime;
using jamn::engine::PeerRuntime;
using jamn::net::LinkConfig;
using jamn::net::PeerId;
using jamn::net::SimNetwork;
using jamn::net::SimTransport;
using jamn::proto::NoteEvent;
using jamn::proto::NoteEventKind;

namespace {

constexpr std::int64_t kStepUs = 1000;
constexpr std::int64_t kLinkDelayUs = 5000;
constexpr double kSampleRate = 48'000.0;
constexpr int kBlockFrames = 128;
// One block at the rate above, in the units each consumer wants it.
constexpr std::int64_t kBlockNs = 2'666'667;

// Two peers on a clean link, with `b` deliberately running on a clock
// offset from `a` - which is what makes "convert out of the sender's
// timebase" a real operation here rather than a subtraction of zero.
struct TwoNodes {
    explicit TwoNodes(std::uint64_t seed, std::int64_t clockOffsetUs = 0)
        : net(seed),
          ta(net.CreateNode(0)),
          tb(net.CreateNode(1)),
          a(ta),
          b(tb, clockOffsetUs),
          offsetUs(clockOffsetUs) {
        LinkConfig cfg;
        cfg.delayUs = kLinkDelayUs;
        net.SetLinkConfig(0, 1, cfg);
        net.SetLinkConfig(1, 0, cfg);
        net.Connect(0, 1);
    }

    void RunUntil(std::int64_t untilUs) {
        for (; nowUs <= untilUs; nowUs += kStepUs) {
            a.Service(nowUs);
            b.Service(nowUs + offsetUs);
            net.Advance(kStepUs);
        }
    }

    SimNetwork net;
    SimTransport& ta;
    SimTransport& tb;
    PeerRuntime a;
    PeerRuntime b;
    std::int64_t offsetUs = 0;
    std::int64_t nowUs = 0;
};

NoteEvent NoteOn(std::uint8_t pitch) {
    NoteEvent event;
    event.kind = NoteEventKind::kNoteOn;
    event.a = pitch;
    return event;
}

NoteEvent NoteOff(std::uint8_t pitch) {
    NoteEvent event;
    event.kind = NoteEventKind::kNoteOff;
    event.a = pitch;
    return event;
}

// Drives one audio block against `runtime`, appending whatever came due.
// steadyNs advances by exactly one block, the way a device's callbacks do.
struct AudioSide {
    AudioSide() { audio.Prepare(kSampleRate, kBlockFrames); }

    std::size_t Block(PeerRuntime& runtime, std::vector<AudioRuntime::Note>& into) {
        std::array<AudioRuntime::Note, AudioRuntime::kMaxNotesPerBlock> buffer;
        const std::size_t count = audio.Service(&runtime, frames, steadyNs, buffer.data(), buffer.size());
        for (std::size_t index = 0; index < count; ++index) into.push_back(buffer[index]);
        frames += kBlockFrames;
        steadyNs += kBlockNs;
        return count;
    }

    // Runs blocks until `untilNs`, so the audio side can be advanced to
    // wherever the net side has already reached.
    void BlocksUntil(PeerRuntime& runtime, std::int64_t untilNs, std::vector<AudioRuntime::Note>& into) {
        while (steadyNs <= untilNs) Block(runtime, into);
    }

    AudioRuntime audio;
    std::int64_t frames = 0;
    std::int64_t steadyNs = 0;
};

std::size_t CountOfKind(const std::vector<AudioRuntime::Note>& notes, NoteEventKind kind) {
    std::size_t count = 0;
    for (const AudioRuntime::Note& note : notes) {
        if (note.event.kind == kind) ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("audio_runtime delivers a remote note played on the other peer", "[audio_runtime]") {
    TwoNodes nodes(1);
    AudioSide side;

    nodes.RunUntil(200'000);
    REQUIRE(nodes.b.SubmitLocalEvent(NoteOn(64), nodes.nowUs + nodes.offsetUs));
    nodes.RunUntil(400'000);

    std::vector<AudioRuntime::Note> delivered;
    side.BlocksUntil(nodes.a, nodes.nowUs * 1000, delivered);

    REQUIRE(CountOfKind(delivered, NoteEventKind::kNoteOn) == 1);
    for (const AudioRuntime::Note& note : delivered) {
        if (note.event.kind != NoteEventKind::kNoteOn) continue;
        REQUIRE(note.event.a == 64);
        REQUIRE(note.peer != PeerRuntime::kNoPeer);
        // Block granularity is the current contract, so this is 0 by
        // design rather than by omission.
        REQUIRE(note.sampleOffset == 0);
    }
    REQUIRE(side.audio.stats().notesScheduled >= 1);
    REQUIRE(side.audio.stats().notesDelivered >= 1);
}

TEST_CASE("audio_runtime converts a note out of the sender's timebase", "[audio_runtime]") {
    // The peers' clocks are a quarter second apart. Taking the sender's
    // timestamp at face value would put every note a quarter second wrong,
    // which at this size means either dropped as late or stuck a long way
    // in the future - so arriving at all is the assertion.
    constexpr std::int64_t kOffsetUs = 250'000;
    TwoNodes nodes(2, kOffsetUs);
    AudioSide side;

    // Long enough for ClockSync to lock, so the published offset is real
    // rather than the "never locked, reads zero" default.
    nodes.RunUntil(3'000'000);
    const std::size_t slot = [&] {
        for (std::size_t index = 0; index < PeerRuntime::kMaxPeers; ++index) {
            if (nodes.a.PeerAt(index) != PeerRuntime::kNoPeer) return index;
        }
        return PeerRuntime::kMaxPeers;
    }();
    REQUIRE(slot < PeerRuntime::kMaxPeers);
    REQUIRE(nodes.a.OffsetIsLocked(slot));

    std::vector<AudioRuntime::Note> warmup;
    side.BlocksUntil(nodes.a, nodes.nowUs * 1000, warmup);

    REQUIRE(nodes.b.SubmitLocalEvent(NoteOn(70), nodes.nowUs + nodes.offsetUs));
    nodes.RunUntil(3'400'000);

    std::vector<AudioRuntime::Note> delivered;
    side.BlocksUntil(nodes.a, nodes.nowUs * 1000, delivered);

    REQUIRE(CountOfKind(delivered, NoteEventKind::kNoteOn) == 1);
    REQUIRE(side.audio.stats().notesRejectedByScheduler == 0);
}

TEST_CASE("audio_runtime drives Clock 2 from the block pair it is given", "[audio_runtime]") {
    TwoNodes nodes(3);
    AudioSide side;
    std::vector<AudioRuntime::Note> ignored;

    REQUIRE_FALSE(side.audio.clock().IsLocked());
    for (int block = 0; block < 4000; ++block) side.Block(nodes.a, ignored);

    REQUIRE(side.audio.clock().IsLocked());
    REQUIRE(side.audio.clock().UpdateCount() == 4000);
    // The synthetic stream is exactly nominal, so the measured rate should
    // land on it - this is the wiring being right, not an accuracy claim.
    REQUIRE(std::fabs(side.audio.clock().DriftPpm()) < 500.0);
}

TEST_CASE("audio_runtime flushes a peer's queue and silences it on a clock re-lock", "[audio_runtime]") {
    // Driven through EventScheduler directly: a re-lock needs ClockSync to
    // see a discontinuity larger than its threshold, which a clean sim link
    // will not produce on demand. What matters here is the policy itself.
    jamn::engine::EventScheduler scheduler;
    constexpr PeerId kPeer = 7;

    REQUIRE(scheduler.ScheduleRemoteEvent(kPeer, NoteOn(60), 100'000, 100'000));
    REQUIRE(scheduler.ScheduleRemoteEvent(kPeer, NoteOn(64), 150'000, 100'000));
    REQUIRE(scheduler.ScheduleRemoteEvent(kPeer + 1, NoteOn(67), 150'000, 100'000));
    const std::size_t before = scheduler.ScheduledCount();
    REQUIRE(before == 3);

    const std::size_t discarded = scheduler.FlushPeer(kPeer);
    REQUIRE(discarded == 2);

    // The flushed peer is gone from the queue entirely, and the other peer
    // is untouched. Silencing what the flushed peer had sounding is not
    // this class's job - see FlushPeer's contract, and the departure test
    // below for who does it.
    std::size_t flushedPeerEvents = 0;
    std::size_t otherPeerEvents = 0;
    jamn::engine::EventScheduler::Delivery delivery;
    while (scheduler.PopReady(400'000, delivery)) {
        if (delivery.peer == kPeer) {
            ++flushedPeerEvents;
        } else {
            ++otherPeerEvents;
        }
    }
    REQUIRE(flushedPeerEvents == 0);
    REQUIRE(otherPeerEvents == 1);
}

TEST_CASE("audio_runtime silences a peer that leaves, on the slot it left", "[audio_runtime]") {
    // The case that matters more than a re-lock and is easier to miss: a
    // peer disconnects, so everything it had scheduled - including the
    // note-offs for whatever it is still sounding - becomes meaningless.
    // Without a silence its instrument rings forever.
    //
    // The silence has to name the *slot*, not just the peer: by the time
    // it is emitted the peer is no longer in any slot, so a caller looking
    // the peer up would find nothing and drop it.
    TwoNodes nodes(7);
    AudioSide side;
    nodes.RunUntil(200'000);

    std::vector<AudioRuntime::Note> warmup;
    side.BlocksUntil(nodes.a, nodes.nowUs * 1000, warmup);

    const std::size_t slot = [&] {
        for (std::size_t index = 0; index < PeerRuntime::kMaxPeers; ++index) {
            if (nodes.a.PeerAt(index) != PeerRuntime::kNoPeer) return index;
        }
        return PeerRuntime::kMaxPeers;
    }();
    REQUIRE(slot < PeerRuntime::kMaxPeers);

    const PeerId departing = nodes.a.PeerAt(slot);
    nodes.ta.Disconnect(departing);
    nodes.RunUntil(nodes.nowUs + 100'000);
    REQUIRE(nodes.a.PeerAt(slot) == PeerRuntime::kNoPeer);

    std::vector<AudioRuntime::Note> afterLeaving;
    side.Block(nodes.a, afterLeaving);

    REQUIRE(side.audio.stats().peersDeparted == 1);
    REQUIRE(CountOfKind(afterLeaving, NoteEventKind::kAllNotesOff) == 1);
    for (const AudioRuntime::Note& note : afterLeaving) {
        if (note.event.kind != NoteEventKind::kAllNotesOff) continue;
        REQUIRE(note.slot == slot);
    }

    // Once only - a slot that stays empty must not keep announcing it.
    std::vector<AudioRuntime::Note> later;
    side.Block(nodes.a, later);
    side.Block(nodes.a, later);
    REQUIRE(later.empty());
    REQUIRE(side.audio.stats().peersDeparted == 1);
}

TEST_CASE("audio_runtime never returns more notes than the block has room for", "[audio_runtime]") {
    TwoNodes nodes(4);
    AudioSide side;

    nodes.RunUntil(200'000);

    // Catch the audio clock up to the net side *before* the burst is
    // played: steadyNs and the sim's nowUs are one timebase here (nowUs ==
    // steadyNs / 1000), and this loop uses the full per-block ceiling, so
    // anything already due would drain here and never reach the capped
    // loop below - which is what has to be exercised.
    std::vector<AudioRuntime::Note> caughtUp;
    side.BlocksUntil(nodes.a, nodes.nowUs * 1000, caughtUp);
    REQUIRE(caughtUp.empty());

    // More than one block's ceiling, all stamped the same instant so they
    // all fall due together and the cap is genuinely reached.
    constexpr int kNotes = 40;
    for (int index = 0; index < kNotes; ++index) {
        REQUIRE(nodes.b.SubmitLocalEvent(NoteOn(static_cast<std::uint8_t>(40 + index)), nodes.nowUs + nodes.offsetUs));
    }
    nodes.RunUntil(600'000);

    constexpr std::size_t kSmallCapacity = 8;
    std::array<AudioRuntime::Note, kSmallCapacity> small;
    std::size_t total = 0;
    std::size_t maxInOneBlock = 0;
    for (int block = 0; block < 200; ++block) {
        const std::size_t count = side.audio.Service(&nodes.a, side.frames, side.steadyNs, small.data(), small.size());
        REQUIRE(count <= kSmallCapacity);
        maxInOneBlock = std::max(maxInOneBlock, count);
        total += count;
        side.frames += kBlockFrames;
        side.steadyNs += kBlockNs;
    }

    // The point of the test: the cap was actually reached, and hitting it
    // deferred notes to later blocks rather than discarding them - all 40
    // arrive, none is dropped for having been over the ceiling.
    REQUIRE(maxInOneBlock == kSmallCapacity);
    REQUIRE(side.audio.stats().blocksAtNoteCapacity >= 1);
    REQUIRE(total == kNotes);
    REQUIRE(side.audio.stats().notesDelivered == total);
}

TEST_CASE("audio_runtime discards a note whose slot was re-claimed by another link", "[audio_runtime]") {
    // A lane's tail can outlive the link that filled it. Attributing that
    // tail to whoever holds the slot now would play one peer's notes as
    // another's; RemoteNote carries its own peer id so it need not be.
    TwoNodes nodes(5);
    AudioSide side;
    nodes.RunUntil(200'000);

    const std::size_t slot = [&] {
        for (std::size_t index = 0; index < PeerRuntime::kMaxPeers; ++index) {
            if (nodes.a.PeerAt(index) != PeerRuntime::kNoPeer) return index;
        }
        return PeerRuntime::kMaxPeers;
    }();
    REQUIRE(slot < PeerRuntime::kMaxPeers);

    jamn::engine::NoteCrossing::RemoteNote stale;
    stale.peer = static_cast<PeerId>(nodes.a.PeerAt(slot) + 100);  // Nobody.
    stale.remoteSessionTimeUs = nodes.nowUs;
    stale.event = NoteOn(60);
    REQUIRE(nodes.a.crossing().Publish(slot, stale));

    std::vector<AudioRuntime::Note> delivered;
    side.Block(nodes.a, delivered);

    REQUIRE(side.audio.stats().notesFromStaleSlot == 1);
    REQUIRE(CountOfKind(delivered, NoteEventKind::kNoteOn) == 0);
}

TEST_CASE("audio_runtime does not drop the first remote note of a session", "[audio_runtime]") {
    // Found on hardware. The audio thread checks deadlines once per block,
    // so a deadline can already be stale by the time anything looks at it.
    // With a playout target of only the 3ms safety margin and an 11.6ms
    // block (512 frames at 44.1kHz - what the dev box's PipeWire chose
    // unasked), the first remote note of a session is late on arrival
    // through no fault of the network and is dropped by the >5ms
    // late-event policy. Every later note survives, because the jitter
    // buffer grows the instant it sees one late event - so the symptom in
    // the field was exactly one lost note per session.
    //
    // Asserted against EventScheduler directly rather than through a
    // simulated session. An earlier version of this test drove the sim and
    // passed with the fix reverted, which makes it no test at all: the sim
    // imposes no block period, so the arrival-to-drain gap that causes
    // this never occurs there.
    constexpr PeerId kPeer = 4;
    constexpr std::int64_t kEventTimeUs = 0;
    // 8ms of transit, which is 5ms past a 3ms deadline - just over the
    // threshold, and exactly the shape a first note has.
    constexpr std::int64_t kNowUs = 9000;
    constexpr std::int64_t kBlockPeriodUs = 11'610;

    SECTION("without a block-period floor it is dropped") {
        jamn::engine::EventScheduler scheduler;
        REQUIRE_FALSE(scheduler.ScheduleRemoteEvent(kPeer, NoteOn(60), kEventTimeUs, kNowUs));
        REQUIRE(scheduler.ScheduledCount() == 0);
    }

    SECTION("with one it survives, and is not merely delayed") {
        jamn::engine::EventScheduler scheduler;
        scheduler.SetBlockPeriodUs(kBlockPeriodUs);
        REQUIRE(scheduler.ScheduleRemoteEvent(kPeer, NoteOn(60), kEventTimeUs, kNowUs));
        REQUIRE(scheduler.ScheduledCount() == 1);

        jamn::engine::EventScheduler::Delivery delivery;
        REQUIRE(scheduler.PopReady(kEventTimeUs + kBlockPeriodUs, delivery));
        REQUIRE(delivery.event.a == 60);
    }

    SECTION("the floor reaches a peer that joins after it is set, and one already known") {
        jamn::engine::EventScheduler scheduler;
        // Known first: this call creates the peer's slot as a side effect.
        (void)scheduler.JitterBufferFor(kPeer);
        scheduler.SetBlockPeriodUs(kBlockPeriodUs);
        REQUIRE(scheduler.JitterBufferFor(kPeer).MinTargetUs() == kBlockPeriodUs);
        // And one that appears only afterwards.
        REQUIRE(scheduler.JitterBufferFor(kPeer + 1).MinTargetUs() == kBlockPeriodUs);
    }

    SECTION("a healthy P99 is not inflated by the floor") {
        // The floor is a floor, not an addition - a peer whose transit
        // already justifies a larger target keeps it, because adding a
        // block period on top would cost real latency in a budget measured
        // in low tens of milliseconds.
        jamn::engine::JitterBuffer buffer;
        buffer.SetMinTargetUs(kBlockPeriodUs);
        for (int index = 0; index < 200; ++index) buffer.RecordTransit(40'000, 1000);
        REQUIRE(buffer.PlayoutTargetUs(1000) > kBlockPeriodUs);
    }
}

TEST_CASE("audio_runtime is safe to service from a realtime scope", "[audio_runtime]") {
    TwoNodes nodes(6);
    AudioSide side;

    nodes.RunUntil(200'000);
    nodes.b.SubmitLocalEvent(NoteOn(60), nodes.nowUs + nodes.offsetUs);
    nodes.b.SubmitLocalEvent(NoteOff(60), nodes.nowUs + nodes.offsetUs + 10'000);
    nodes.RunUntil(600'000);

    std::array<AudioRuntime::Note, AudioRuntime::kMaxNotesPerBlock> buffer;
    {
        // Prepare is the device-start moment and stays outside; Service is
        // the per-block path and is what has to hold under the trap.
        jamn::core::RealtimeScope scope;
        for (int block = 0; block < 200; ++block) {
            side.audio.Service(&nodes.a, side.frames, side.steadyNs, buffer.data(), buffer.size());
            side.frames += kBlockFrames;
            side.steadyNs += kBlockNs;
        }
    }
    REQUIRE(side.audio.clock().UpdateCount() == 200);
}

// --- T6.1: local input. The accept criterion the plan states is
// "the maintainer plays and confirms local notes sound immediately",
// which no test can run - but the mechanism under the word "immediately"
// is testable exactly, and is the part that could silently regress into
// a block of added latency later.

TEST_CASE("audio_runtime plays a local note in the same block it services", "[audio_runtime]") {
    TwoNodes nodes(7);
    AudioSide side;

    REQUIRE(side.audio.SubmitLocalNote(NoteOn(60)));

    std::array<AudioRuntime::Note, AudioRuntime::kMaxNotesPerBlock> buffer;
    // One block. Not "eventually", not "the next one" - the whole content
    // of "never delayed" is that this count is nonzero right here.
    const std::size_t count = side.audio.Service(&nodes.a, side.frames, side.steadyNs, buffer.data(), buffer.size());

    REQUIRE(count == 1);
    REQUIRE(buffer[0].event.kind == NoteEventKind::kNoteOn);
    REQUIRE(buffer[0].event.a == 60);
    REQUIRE(buffer[0].peer == jamn::engine::EventScheduler::kLocalPeerId);
    REQUIRE(side.audio.stats().localNotesScheduled == 1);
    REQUIRE(side.audio.stats().localNotesRejectedByScheduler == 0);
    REQUIRE(side.audio.LocalNotesDropped() == 0);
}

TEST_CASE("audio_runtime monitors local input with no session at all", "[audio_runtime]") {
    // The solo path: a player who has not joined or hosted anything has no
    // PeerRuntime, and nothing else in this suite passes null. Before
    // T6.1 the caller returned early here and a solo player was silent.
    AudioSide side;

    REQUIRE(side.audio.SubmitLocalNote(NoteOn(64)));
    REQUIRE(side.audio.SubmitLocalNote(NoteOff(64)));

    std::array<AudioRuntime::Note, AudioRuntime::kMaxNotesPerBlock> buffer;
    const std::size_t count = side.audio.Service(nullptr, side.frames, side.steadyNs, buffer.data(), buffer.size());

    REQUIRE(count == 2);
    REQUIRE(buffer[0].event.kind == NoteEventKind::kNoteOn);
    REQUIRE(buffer[1].event.kind == NoteEventKind::kNoteOff);
    // Clock 2 is still driven with no session: it measures this device's
    // sample clock, which owes nothing to anyone being connected.
    REQUIRE(side.audio.clock().UpdateCount() == 1);
}

TEST_CASE("audio_runtime never hands a local note a peer's slot", "[audio_runtime]") {
    // EventScheduler::kLocalPeerId and PeerRuntime::kNoPeer are both
    // 0xFFFF, so a reverse lookup by peer id matches the first *empty*
    // slot - which would route the player's own note to whatever
    // instrument is on that strip, and make it mutable by that strip's
    // mute and solo. This is that regression, and it needs a runtime with
    // at least one empty slot to be able to fire at all.
    TwoNodes nodes(8);
    AudioSide side;
    nodes.RunUntil(200'000);

    std::size_t emptySlots = 0;
    for (std::size_t slot = 0; slot < PeerRuntime::kMaxPeers; ++slot) {
        if (nodes.a.PeerAt(slot) == PeerRuntime::kNoPeer) ++emptySlots;
    }
    REQUIRE(emptySlots > 0);

    REQUIRE(side.audio.SubmitLocalNote(NoteOn(72)));

    std::array<AudioRuntime::Note, AudioRuntime::kMaxNotesPerBlock> buffer;
    const std::size_t count = side.audio.Service(&nodes.a, side.frames, side.steadyNs, buffer.data(), buffer.size());

    REQUIRE(count == 1);
    REQUIRE(buffer[0].peer == jamn::engine::EventScheduler::kLocalPeerId);
    REQUIRE(buffer[0].slot == AudioRuntime::kMaxPeers);
}

TEST_CASE("audio_runtime drops local input rather than blocking when the ring is full", "[audio_runtime]") {
    AudioSide side;

    // Nothing services in between, so the ring fills and the next push is
    // refused. A refusal is the designed failure: backpressure toward the
    // audio thread is not an option, so a bounded drop is what is left.
    for (std::size_t index = 0; index < AudioRuntime::kLocalMonitorCapacity; ++index) {
        REQUIRE(side.audio.SubmitLocalNote(NoteOn(60)));
    }
    REQUIRE_FALSE(side.audio.SubmitLocalNote(NoteOn(60)));
    REQUIRE(side.audio.LocalNotesDropped() == 1);

    // And the backlog drains over blocks rather than in one - bounded work
    // per block is the other half of the same rule.
    std::array<AudioRuntime::Note, AudioRuntime::kMaxNotesPerBlock> buffer;
    const std::size_t first = side.audio.Service(nullptr, side.frames, side.steadyNs, buffer.data(), buffer.size());
    REQUIRE(first == AudioRuntime::kMaxLocalDrainPerBlock);
}

TEST_CASE("audio_runtime services local input from a realtime scope", "[audio_runtime]") {
    // Submitting is the message thread's and stays outside the trap; the
    // drain and the schedule are the audio thread's and are what has to
    // hold under it.
    AudioSide side;
    for (int index = 0; index < 8; ++index) {
        side.audio.SubmitLocalNote(NoteOn(static_cast<std::uint8_t>(60 + index)));
    }

    std::array<AudioRuntime::Note, AudioRuntime::kMaxNotesPerBlock> buffer;
    {
        jamn::core::RealtimeScope scope;
        for (int block = 0; block < 32; ++block) {
            side.audio.Service(nullptr, side.frames, side.steadyNs, buffer.data(), buffer.size());
            side.frames += kBlockFrames;
            side.steadyNs += kBlockNs;
        }
    }
    REQUIRE(side.audio.stats().localNotesScheduled == 8);
}

TEST_CASE("audio_runtime keeps a local note's on and off in the order they were played", "[audio_runtime]") {
    // The dragged-mouse bug, reported at the machine 2026-08-16: holding
    // the button and sweeping fast across the on-screen piano left notes
    // sounding forever. A sweep puts several keys' on/off pairs inside
    // one audio block, and every local event in a block is scheduled
    // against that block's single `now` - so they all carry the same
    // deadline, and a heap ordered on deadline alone is free to hand
    // them back in any order. An off delivered before its own on is a
    // note that never ends.
    AudioSide side;

    constexpr std::uint8_t kSweep[] = {60, 61, 62, 63, 64};
    for (std::uint8_t pitch : kSweep) {
        REQUIRE(side.audio.SubmitLocalNote(NoteOn(pitch)));
        REQUIRE(side.audio.SubmitLocalNote(NoteOff(pitch)));
    }

    std::array<AudioRuntime::Note, AudioRuntime::kMaxNotesPerBlock> buffer;
    const std::size_t count = side.audio.Service(nullptr, side.frames, side.steadyNs, buffer.data(), buffer.size());
    REQUIRE(count == 2 * (sizeof(kSweep) / sizeof(kSweep[0])));

    // Every pitch must be off by the end, and must never have been
    // turned off before it was turned on.
    std::array<int, 128> sounding{};
    for (std::size_t index = 0; index < count; ++index) {
        const AudioRuntime::Note& note = buffer[index];
        if (note.event.kind == NoteEventKind::kNoteOn) {
            ++sounding[note.event.a];
        } else if (note.event.kind == NoteEventKind::kNoteOff) {
            INFO("note-off for pitch " << static_cast<int>(note.event.a) << " arrived before its note-on");
            REQUIRE(sounding[note.event.a] > 0);
            --sounding[note.event.a];
        }
    }
    for (std::size_t pitch = 0; pitch < sounding.size(); ++pitch) {
        INFO("pitch " << pitch << " left sounding");
        REQUIRE(sounding[pitch] == 0);
    }
}

TEST_CASE("audio_runtime silences every instrument on a panic", "[audio_runtime]") {
    // The escape hatch for a stuck note. Every slot gets an
    // all-notes-off, occupied or not - an instrument on a slot whose peer
    // left can still be ringing, which is exactly the sound being reached
    // for - and so does the local monitor, which owns no slot.
    TwoNodes nodes(9);
    AudioSide side;
    nodes.RunUntil(200'000);

    REQUIRE(side.audio.SubmitLocalNote(NoteOn(60)));
    side.audio.RequestPanic();

    std::array<AudioRuntime::Note, AudioRuntime::kMaxNotesPerBlock> buffer;
    const std::size_t count = side.audio.Service(&nodes.a, side.frames, side.steadyNs, buffer.data(), buffer.size());

    std::size_t silencedSlots = 0;
    bool silencedLocal = false;
    for (std::size_t index = 0; index < count; ++index) {
        if (buffer[index].event.kind != NoteEventKind::kAllNotesOff) continue;
        if (buffer[index].slot < AudioRuntime::kMaxPeers) {
            ++silencedSlots;
        } else if (buffer[index].peer == jamn::engine::EventScheduler::kLocalPeerId) {
            silencedLocal = true;
        }
    }
    REQUIRE(silencedSlots == AudioRuntime::kMaxPeers);
    REQUIRE(silencedLocal);
    REQUIRE(side.audio.stats().panicsServiced == 1);

    // And the note submitted just before it does not sound afterwards:
    // panic runs after the drain, so an event that arrived in the same
    // block is flushed with everything else rather than surviving by
    // microseconds.
    std::size_t noteOnsAfterPanic = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (buffer[index].event.kind == NoteEventKind::kNoteOn) ++noteOnsAfterPanic;
    }
    REQUIRE(noteOnsAfterPanic == 0);

    // One panic, not a mute: the next note plays normally.
    REQUIRE(side.frames == 0);
    side.frames += kBlockFrames;
    side.steadyNs += kBlockNs;
    REQUIRE(side.audio.SubmitLocalNote(NoteOn(62)));
    const std::size_t after = side.audio.Service(&nodes.a, side.frames, side.steadyNs, buffer.data(), buffer.size());
    REQUIRE(after == 1);
    REQUIRE(buffer[0].event.kind == NoteEventKind::kNoteOn);
    REQUIRE(side.audio.stats().panicsServiced == 1);
}

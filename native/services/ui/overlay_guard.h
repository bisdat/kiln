// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <sstream>
namespace kui::overlay_guard {
// drawing (since 0.2.0-dev.30.2): the overlay's first draw of the process began its own work (its Vulkan resources,
// ImGui, the frame, the submit: vk.cpp draw()). Before, a start that died there read "swapchain created" too, and a
// start that only waited after its swapchain (nothing of Kiln's drawn yet) could not be told from one that drew.
enum class Stage { begun, hooks, instance, device, swapchain, drawing, frame };
inline const char* stage_name(Stage s) {
    static const char* names[] = {"before hooks", "hooks installed", "instance seen", "device seen", "swapchain created",
                                  "the overlay's first draw", "first frame presented"};
    return names[unsigned(s)];
}
enum class Phase { complete, trying, guarded, retry, locked };
struct Record {
    Phase phase = Phase::complete;
    Stage stage = Stage::begun, failed_stage = Stage::begun;
    unsigned failures = 0;
    std::string modules;
    bool wrapper_retry = false;
    // The Kiln that wrote the record (KilnApi::version, as kiln_token), since 0.2.0-dev.29.19; "" in older records.
    std::string kiln = {};
};
struct Start { Record record; bool off; };
// No record/complete -> trying. Unfinished trying -> guarded (one death), or locked
// on a second death at the SAME stage; a death at "swapchain created" (after the game's swapchain, before the overlay's
// first draw: none of the overlay's own work had run) guards that one start and counts towards no lock (0.2.0-dev.30.2).
// Guarded or retry -> trying, preserving the first death: the guarded start ran
// with the overlay's drawing off (no Vulkan hook of Kiln's), so however it ended (a normal quit, a crash, the game
// killed) it says nothing about the overlay, and the start after it tries again. A guarded start writes no new
// attempt of its own. Until 0.2.0-dev.29.18 a guarded start earned that only by running a minute; one that ended
// sooner left the record guarded, and every later start kept the overlay off. Retry is a guarded start that exited
// normally (at_exit), or ran that minute in an older Kiln. A successful overlay frame resets trying (finish); locked
// stays locked. A wrapper change permits one recovery until a drawn frame succeeds. Further changes preserve the
// history, so a wrapper updated on every start cannot keep crashing forever. Deleting the record resets it all.
// Versioned identities give old presence-only records one fresh attempt after an update too.
inline Start begin(Record r, const std::string& modules) {
    if (r.modules != modules && !r.wrapper_retry) {
        const bool had_identity = !r.modules.empty();
        std::string kiln = std::move(r.kiln);
        r = {};
        r.kiln = std::move(kiln);
        r.wrapper_retry = had_identity;
    }
    r.modules = modules;
    if (r.phase == Phase::trying && r.stage == Stage::swapchain) {
        // The start died after its swapchain was made and before the overlay's first draw: Kiln's overlay had done none
        // of its own work. A game killed or crashed for another reason while its first frames stalled (ReShade's Vulkan
        // layer compiles its effects there) reads so too; until 0.2.0-dev.30.2 two such ends locked the overlay off. Only
        // this one start is guarded: the failure history stays as it was (failed stage and count), the next start tries.
        r.phase = Phase::guarded;
    } else if (r.phase == Phase::trying) {
        r.failures = r.failures && r.failed_stage == r.stage ? 2 : 1;
        r.failed_stage = r.stage;
        r.phase = r.failures == 2 ? Phase::locked : Phase::guarded;
    } else if (r.phase == Phase::complete || r.phase == Phase::retry || r.phase == Phase::guarded) {
        r.phase = Phase::trying;
        r.stage = Stage::begun;
    }
    return {r, r.phase == Phase::guarded || r.phase == Phase::locked};
}
// The Kiln version as the record keeps it: one token without blanks, at most 64 bytes; "unknown" when there is none
// ("-" stands for no version in the record, encode).
inline std::string kiln_token(std::string v) {
    if (v.size() > 64) v.resize(64);
    for (char& c : v)
        if (static_cast<unsigned char>(c) <= ' ' || c == 0x7f) c = '_';
    return v.empty() || v == "-" ? std::string("unknown") : v;
}
// Before begin(): a record another Kiln wrote (an update, a downgrade) or one without a version (every Kiln before
// 0.2.0-dev.29.19) starts fresh, history, lock and wrapper recovery cleared: the deaths it counts were another Kiln's,
// and a lock 0.2.0-dev.29.17 wrote for bugs a later Kiln fixed kept the overlay off after the update (inbox 97). A record
// of this very Kiln is returned as it is.
inline Record for_kiln(Record r, const std::string& kiln, const std::string& modules) {
    if (r.kiln == kiln) return r;
    Record fresh;
    fresh.modules = modules;
    fresh.kiln = kiln;
    return fresh;
}
inline Record finish(Record r) {
    if (r.phase == Phase::guarded) r.phase = Phase::retry;
    else if (r.phase == Phase::trying) { r.phase = Phase::complete; r.failures = 0; r.wrapper_retry = false; }
    return r;
}
// A start that has not drawn yet is fine too when the game kept presenting through Kiln's hooks for a while after
// its swapchain, or exited normally (code 0): the overlay draws only once something is open (Kiln's menu, a mod's
// window), and a session that never opened one is not a death at the overlay's start. Only a trying record changes,
// and its failure history stays (only a drawn frame clears it): a real death at the first draw after an earlier one at
// the same stage still locks.
inline Record idle(Record r) {
    if (r.phase == Phase::trying) r.phase = Phase::complete;
    return r;
}
// The first draw of a process whose start idle() completed (or the overlay's drawing failing after its start counted as
// fine): a death before its frames are presented is a death at the overlay again (stage: the overlay's first draw), so it
// arms the record once more until they are.
inline Record rearm(Record r) {
    if (r.phase == Phase::complete) { r.phase = Phase::trying; r.stage = Stage::drawing; }
    return r;
}
// The process exits normally (code 0, Kiln's exit steps ran: overlay_guard_exit). A start still trying did not die
// at the overlay (idle: its failure history stays). A guarded start ended well: retry, so the record no longer reads
// guarded (begin treats both alike; the record then tells a guarded start that quit from one that crashed). Locked
// stays locked: a normal exit without the overlay says nothing about it.
inline Record at_exit(Record r) {
    if (r.phase == Phase::trying) return idle(r);
    if (r.phase == Phase::guarded) r.phase = Phase::retry;
    return r;
}
inline std::string encode(const Record& r) {
    return "4 " + std::to_string(unsigned(r.phase)) + " " + std::to_string(unsigned(r.stage)) + " " +
        std::to_string(unsigned(r.failed_stage)) + " " + std::to_string(r.failures) + " " +
        std::to_string(unsigned(r.wrapper_retry)) + " " + (r.kiln.empty() ? std::string("-") : r.kiln) + " " + r.modules + "\n";
}
// First field is the format version: 4 since 0.2.0-dev.30.2 (stage 5 the overlay's first draw, 6 its frames
// presented), 3 since 0.2.0-dev.29.19 (the Kiln version before the wrapper identity), 2 and 1 before; in formats 1 to 3
// stage 5 is the frames presented (read as Stage::frame). Reject partial, oversized, future or malformed records. A Kiln
// before 0.2.0-dev.29.19 rejects a version 3 record, one before 0.2.0-dev.30.2 a version 4 record, as one it cannot read
// and starts without it: it tries the overlay, it does not lock.
inline bool parse(const std::string& text, Record& out) {
    out = {};
    if (text.size() > 512) return false;
    std::istringstream in(text);
    unsigned version, phase, stage, failed, count, wrapper_retry = 0;
    std::string modules, extra, kiln;
    if (!(in >> version >> phase >> stage >> failed >> count) || version < 1 || version > 4) return false;
    if (version >= 2 && (!(in >> wrapper_retry) || wrapper_retry > 1)) return false;
    if (version >= 3 && (!(in >> kiln) || kiln.size() > 64)) return false;
    if (kiln == "-") kiln.clear();
    const unsigned last = version >= 4 ? unsigned(Stage::frame) : 5;
    if (!(in >> modules) || (in >> extra) || phase > 4 || stage > last || failed > last || count > 2 || modules.size() > 128)
        return false;
    if (version < 4) {  // no first-draw stage before format 4: its 5 is the frames presented
        if (stage == 5) stage = unsigned(Stage::frame);
        if (failed == 5) failed = unsigned(Stage::frame);
    }
    out = {Phase(phase), Stage(stage), Stage(failed), count, modules, wrapper_retry != 0, kiln};
    return true;
}
} // namespace kui::overlay_guard

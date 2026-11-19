// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <sstream>
namespace kui::overlay_guard {
enum class Stage { begun, hooks, instance, device, swapchain, frame };
inline const char* stage_name(Stage s) {
    static const char* names[] = {"before hooks", "hooks installed", "instance seen", "device seen", "swapchain created", "first frame presented"};
    return names[unsigned(s)];
}
enum class Phase { complete, trying, guarded, retry, locked };
struct Record {
    Phase phase = Phase::complete;
    Stage stage = Stage::begun, failed_stage = Stage::begun;
    unsigned failures = 0;
    std::string modules;
    bool wrapper_retry = false;
};
struct Start { Record record; bool off; };
// No record/complete -> trying. Unfinished trying -> guarded (one death), or locked
// on a second death at the SAME stage. Guarded or retry -> trying, preserving the first death: the guarded start ran
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
        r = {};
        r.wrapper_retry = had_identity;
    }
    r.modules = modules;
    if (r.phase == Phase::trying) {
        r.failures = r.failures && r.failed_stage == r.stage ? 2 : 1;
        r.failed_stage = r.stage;
        r.phase = r.failures == 2 ? Phase::locked : Phase::guarded;
    } else if (r.phase == Phase::complete || r.phase == Phase::retry || r.phase == Phase::guarded) {
        r.phase = Phase::trying;
        r.stage = Stage::begun;
    }
    return {r, r.phase == Phase::guarded || r.phase == Phase::locked};
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
// The first draw of a process whose start idle() completed: a death before that frame is presented is a death at
// the overlay's start again (stage: swapchain created), so it arms the record once more until the frame is presented.
inline Record rearm(Record r) {
    if (r.phase == Phase::complete) { r.phase = Phase::trying; r.stage = Stage::swapchain; }
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
    return "2 " + std::to_string(unsigned(r.phase)) + " " + std::to_string(unsigned(r.stage)) + " " +
        std::to_string(unsigned(r.failed_stage)) + " " + std::to_string(r.failures) + " " +
        std::to_string(unsigned(r.wrapper_retry)) + " " + r.modules + "\n";
}
// First field is the format version. Reject partial, oversized, future or malformed records.
inline bool parse(const std::string& text, Record& out) {
    out = {};
    if (text.size() > 512) return false;
    std::istringstream in(text);
    unsigned version, phase, stage, failed, count, wrapper_retry = 0;
    std::string modules, extra;
    if (!(in >> version >> phase >> stage >> failed >> count) || (version != 1 && version != 2)) return false;
    if (version == 2 && (!(in >> wrapper_retry) || wrapper_retry > 1)) return false;
    if (!(in >> modules) || (in >> extra) || phase > 4 || stage > 5 || failed > 5 || count > 2 || modules.size() > 128)
        return false;
    out = {Phase(phase), Stage(stage), Stage(failed), count, modules, wrapper_retry != 0};
    return true;
}
} // namespace kui::overlay_guard

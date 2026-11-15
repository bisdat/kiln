// SPDX-License-Identifier: MIT
// Host test of kiln.window_clock's state machine (src/services/window_clock_core.h): tests/test-window-clock.sh.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/services/window_clock_core.h"

namespace wc = kiln::window_clock;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass;
    else {
        ++g_fail;
        std::printf("FAIL %s\n", what);
    }
}

wc::Core g_core;
// What each window call saw: "<name>@<phase>:<epoch>/<step>".
std::vector<std::string> g_log;
void note(const char* name, uint64_t epoch, uint64_t step) {
    const KilnWindowStateV1 s = g_core.get();
    g_log.push_back(std::string(name) + "@" + std::to_string(s.phase) + ":" + std::to_string(epoch) + "/" + std::to_string(step));
}
void KILN_CALL dec_begin(uint64_t e, uint64_t s) { note("dec.begin", e, s); }
void KILN_CALL dec_end(uint64_t e, uint64_t s) { note("dec.end", e, s); }
void KILN_CALL chg_begin(uint64_t e, uint64_t s) { note("chg.begin", e, s); }
void KILN_CALL chg_end(uint64_t e, uint64_t s) { note("chg.end", e, s); }

uint32_t g_want = 1;
int g_asked = 0;
uint32_t KILN_CALL dec_wanted() { return ++g_asked, g_want; }

std::vector<uint32_t> g_told;
void KILN_CALL dec_faulted(uint32_t code) { g_told.push_back(code); }

bool g_fault_dec_begin = false;
bool faulting_caller(wc::WindowFn fn, uint64_t e, uint64_t s) {
    if (fn == &dec_begin && g_fault_dec_begin) return false;
    fn(e, s);
    return true;
}

KilnDecisionsApiV1 decisions_table() {
    KilnDecisionsApiV1 t{};
    t.header = {sizeof t, KILN_DECISIONS_VERSION};
    t.window_begin = &dec_begin;
    t.window_end = &dec_end;
    return t;
}
uint32_t g_chg_want = 1, g_chg_asked = 0;
uint32_t KILN_CALL chg_wanted() {
    ++g_chg_asked;
    return g_chg_want;
}
std::vector<uint32_t> g_chg_told;
void KILN_CALL chg_faulted(uint32_t code) { g_chg_told.push_back(code); }
KilnComponentChangesApiV1 changes_table() {
    KilnComponentChangesApiV1 t{};
    t.header = {sizeof t, KILN_COMPONENT_CHANGES_VERSION};
    t.window_begin = &chg_begin;
    t.window_end = &chg_end;
    return t;
}
}  // namespace

int main() {
    const uint8_t session[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    g_core.set_session(session);
    const KilnDecisionsApiV1 dec = decisions_table();
    const KilnComponentChangesApiV1 chg = changes_table();
    const wc::Windows both = wc::Windows::from(&dec, &chg);

    // before any world
    KilnWindowStateV1 s = g_core.get();
    check(s.size == sizeof s && s.phase == KILN_WINDOW_NONE && s.world_epoch == 0 && s.world_thread == 0, "idle state");
    check(std::memcmp(s.process_session, session, 16) == 0, "session bytes");
    check(!kiln_window_world_safe(&s, 77), "no world: not world-safe");

    // one whole step: phases 1,2,3,4,5 and the call order
    g_core.world_started();
    check(g_core.get().world_epoch == 1 && g_core.get().phase == KILN_WINDOW_NONE, "epoch 1 at world start, no phase yet");
    g_core.pre(10, 77);
    s = g_core.get();
    check(s.phase == KILN_WINDOW_PRE && s.step == 10 && s.world_thread == 77, "pre");
    check(kiln_window_world_safe(&s, 77) && !kiln_window_world_safe(&s, 78), "pre is world-safe on the world thread only");
    check(g_core.begin(true, both) == 0, "begin: no fault");
    s = g_core.get();
    check(s.phase == KILN_WINDOW_SIM, "sim after begin");
    check(!kiln_window_world_safe(&s, 77), "sim is not world-safe, also on the world thread");
    check(g_core.end(both) == 0, "end: no fault");
    s = g_core.get();
    check(s.phase == KILN_WINDOW_POST && kiln_window_world_safe(&s, 77), "post after end");
    const std::vector<std::string> want = {"dec.begin@2:1/10", "chg.begin@2:1/10", "chg.end@4:1/10", "dec.end@4:1/10"};
    check(g_log == want, "begin: decisions then changes in phase 2; end: changes then decisions in phase 4");
    if (g_log != want)
        for (auto& l : g_log) std::printf("  saw %s\n", l.c_str());

    // a second step keeps the epoch
    g_log.clear();
    g_core.pre(11, 77);
    g_core.begin(true, both);
    g_core.end(both);
    check(g_log.size() == 4 && g_log[0] == "dec.begin@2:1/11" && g_log[3] == "dec.end@4:1/11", "second step");

    // world stop and a new world: epoch +1 per KILN_WORLD_STARTED, the session stays
    g_core.stopping();
    s = g_core.get();
    check(s.phase == KILN_WINDOW_STOPPING && !kiln_window_world_safe(&s, 77), "stopping is for teardown only");
    g_core.stopped();
    check(g_core.get().phase == KILN_WINDOW_NONE && g_core.get().world_thread == 0, "stopped");
    g_core.world_started();
    g_core.pre(12, 90);
    s = g_core.get();
    check(s.world_epoch == 2 && s.world_thread == 90 && std::memcmp(s.process_session, session, 16) == 0, "epoch 2, same session");

    // without WaitWorldJobs: no window call, phase goes 1 -> 3 and stays there until the next Pre
    g_log.clear();
    check(g_core.begin(false, both) == 0 && g_log.empty() && g_core.get().phase == KILN_WINDOW_SIM, "no windows without wait_jobs");
    g_core.pre(13, 90);
    check(g_core.get().phase == KILN_WINDOW_PRE, "next pre");

    // one service only; a table too short for the window members is left out
    g_log.clear();
    const wc::Windows only_changes = wc::Windows::from(nullptr, &chg);
    g_core.begin(true, only_changes);
    g_core.end(only_changes);
    check(g_log == std::vector<std::string>({"chg.begin@2:2/13", "chg.end@4:2/13"}), "component_changes alone");
    KilnDecisionsApiV1 short_table = dec;
    short_table.header.size = offsetof(KilnDecisionsApiV1, window_begin);
    const wc::Windows none = wc::Windows::from(&short_table, nullptr);
    check(!none.any(), "a table without the window members gets no call");
    check(both.any() && only_changes.any(), "any()");

    // a faulting call is reported by service and does not stop the others or the phase
    g_log.clear();
    g_fault_dec_begin = true;
    g_core.pre(14, 90);
    check(g_core.begin(true, both, &faulting_caller) == 1, "fault mask names decisions");
    check(g_log == std::vector<std::string>({"chg.begin@2:2/14"}) && g_core.get().phase == KILN_WINDOW_SIM, "the other service still ran; phase reached sim");

    // kiln.decisions with nothing to do (no listener, nothing held: window_wanted 0) gets no window call, and the step
    // need not wait for the world's jobs on its account (begun() is 0); component_changes is unaffected
    KilnDecisionsApiV1 lazy = dec;
    lazy.window_wanted = &dec_wanted;
    const wc::Windows lazy_both = wc::Windows::from(&lazy, &chg);
    const wc::Windows lazy_only = wc::Windows::from(&lazy, nullptr);
    g_log.clear();
    g_want = 0;
    g_asked = 0;
    g_core.pre(15, 90);
    check(g_core.begin(true, lazy_only) == 0 && g_asked == 1 && g_log.empty() && g_core.begun() == 0, "nothing wanted: no begin, nothing begun");
    check(g_core.get().phase == KILN_WINDOW_SIM, "nothing wanted: the phase still reaches sim");
    check(g_core.end(lazy_only) == 0 && g_log.empty() && g_core.get().phase == KILN_WINDOW_POST, "nothing wanted: no end call either");
    g_log.clear();
    g_core.pre(16, 90);
    g_core.begin(true, lazy_both);
    check(g_core.begun() == 2, "nothing wanted by decisions: only component_changes begun");
    g_core.end(lazy_both);
    check(g_log == std::vector<std::string>({"chg.begin@2:2/16", "chg.end@4:2/16"}), "nothing wanted: component_changes alone runs");
    // something to do again (a listener, an open request): both calls, in order
    g_log.clear();
    g_want = 1;
    g_core.pre(17, 90);
    g_core.begin(true, lazy_only);
    check(g_core.begun() == 1, "wanted: decisions begun");
    g_core.end(lazy_only);
    check(g_log == std::vector<std::string>({"dec.begin@2:2/17", "dec.end@4:2/17"}), "wanted: begin and end");
    // an end without its begin in this step (the windows were off) calls nothing
    g_log.clear();
    g_core.pre(18, 90);
    g_core.begin(false, lazy_only);
    g_core.end(lazy_only);
    check(g_log.empty(), "no begin this step: no end");
    // a table without window_wanted (older, shorter): wanted every step, as before
    check(both.decisions_wanted == nullptr && lazy_both.decisions_wanted == &dec_wanted, "window_wanted read only when the table has it");

    // a faulting decisions window call: the service is told once (window_faulted) and gets no end call in that step
    KilnDecisionsApiV1 told = dec;
    told.window_faulted = &dec_faulted;
    const wc::Windows told_w = wc::Windows::from(&told, &chg);
    g_log.clear();
    g_fault_dec_begin = true;
    g_core.pre(19, 90);
    const uint32_t mask = g_core.begin(true, told_w, &faulting_caller);
    check(mask == 1 && (g_core.begun() & 1) == 0, "a faulted begin is not begun");
    check(wc::tell_faulted(mask, 0, told_w, 0xC0000005u) == 1 && g_told == std::vector<uint32_t>({0xC0000005u}), "the service is told with the code");
    check(wc::tell_faulted(mask, 1, told_w, 0xC0000005u) == 0 && g_told.size() == 1, "told once");
    check(wc::tell_faulted(2, 0, told_w, 1) == 0 && g_told.size() == 1, "a fault of component_changes does not tell decisions");
    check(wc::tell_faulted(1, 0, both, 1) == 0, "a table without window_faulted is not told");
    g_core.end(told_w);
    check(g_log == std::vector<std::string>({"chg.begin@2:2/19", "chg.end@4:2/19"}), "no end call for the faulted service in that step");
    g_fault_dec_begin = false;

    // since 0.2.0-dev.29 kiln.component_changes says too whether a step needs it (window_wanted): with no watch it gets
    // neither call and no step waits for the world's jobs on its account (07 S7: nothing runs for a player whose mods
    // watch nothing)
    KilnComponentChangesApiV1 chg_lazy = chg;
    chg_lazy.window_wanted = &chg_wanted;
    chg_lazy.window_faulted = &chg_faulted;
    const wc::Windows changes_lazy = wc::Windows::from(&lazy, &chg_lazy);
    check(changes_lazy.changes_wanted == &chg_wanted && changes_lazy.changes_faulted == &chg_faulted, "the appended members are read");
    g_log.clear();
    g_want = 0;
    g_chg_want = 0;
    g_chg_asked = 0;
    g_core.pre(20, 90);
    check(g_core.begin(true, changes_lazy) == 0 && g_chg_asked == 1 && g_core.begun() == 0 && g_log.empty(),
          "no watch: component_changes is asked and not begun");
    g_core.end(changes_lazy);
    check(g_log.empty(), "no watch: no end call either");
    g_chg_want = 1;
    g_core.pre(21, 90);
    g_core.begin(true, changes_lazy);
    check(g_core.begun() == 2, "a watch: component_changes begun");
    g_core.end(changes_lazy);
    check(g_log == std::vector<std::string>({"chg.begin@2:2/21", "chg.end@4:2/21"}), "a watch: both calls");
    // a component_changes table that is too short for window_wanted: called every step, as before
    KilnComponentChangesApiV1 chg_short = chg_lazy;
    chg_short.header.size = offsetof(KilnComponentChangesApiV1, window_wanted);
    check(wc::Windows::from(nullptr, &chg_short).changes_wanted == nullptr, "an older table: window_wanted not read");
    // its fault is told to it (window_faulted), once
    g_chg_told.clear();
    check(wc::tell_faulted(2, 0, changes_lazy, 0xC0000005u) == 2 && g_chg_told == std::vector<uint32_t>({0xC0000005u}),
          "component_changes is told of its own fault");
    check(wc::tell_faulted(2, 2, changes_lazy, 1) == 0 && g_chg_told.size() == 1, "told once");
    check(wc::tell_faulted(3, 0, told_w, 1) == 1, "both faulted, only decisions has window_faulted: only it is told");

    std::printf("window_clock_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

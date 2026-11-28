// SPDX-License-Identifier: MIT
// Host test of the overlay crash guard's runtime (overlay_guard.cpp) over host stand-ins for its Windows calls
// (guard_host/windows.h). kiln/state/overlay-start.guard lives in a temporary folder; each game start runs in a child
// process of its own (the guard keeps its state per process, as the game does), and the parent reads the record
// between starts. A start that returns without overlay_guard_exit is a game that died (or was killed): no exit step ran.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdarg>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../overlay_guard.cpp"

namespace kui {
const KilnApi* g_api = nullptr;
std::vector<std::string> g_log;
void logf(int, const char* fmt, ...) {
    char b[2048];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    g_log.push_back(b);
}
uint64_t g_presents = 0;
uint64_t vk_presents() { return g_presents; }
std::atomic<bool> g_overlay_failed{false};  // ui.cpp: the overlay's drawing turned off during the session
bool g_drawing = false;                     // vk.cpp: a frame of the overlay is being made
bool vk_drawing() { return g_drawing; }
unsigned g_fell_back_calls = 0;             // game.cpp: the Mods entry's caption, the main menu's page left
void game_menu_fell_back() { ++g_fell_back_calls; }
}  // namespace kui

namespace {
using kui::overlay_guard::Stage;
std::filesystem::path g_root, g_state;
unsigned g_checks = 0, g_failures = 0;

KilnResult KILN_CALL fake_path(int32_t kind, char* buffer, size_t capacity, size_t* out_len) {
    const auto game = g_root / "game";
    const std::filesystem::path p = kind == KILN_PATH_GAME ? game
        : kind == KILN_PATH_KILN                           ? game / "kiln"
        : kind == KILN_PATH_STATE                          ? g_state
                                                           : std::filesystem::path();
    if (p.empty()) return KILN_ERR_NOT_FOUND;
    const std::string s = p.string();
    *out_len = s.size();
    if (!buffer || capacity < s.size()) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(buffer, s.data(), s.size());
    return KILN_OK;
}

// The running Kiln's version (KilnApi::version): what a release build reports
const char* g_version = "0.2.0-dev.29.19";
KilnStr KILN_CALL fake_version() { return {g_version, std::strlen(g_version)}; }

bool expect(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
    return ok;
}
std::string record_text() {
    std::ifstream in(g_state / "overlay-start.guard", std::ios::binary);
    return in ? std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::string("(none)");
}
bool logged(const std::string& part) {
    for (const auto& line : kui::g_log)
        if (line.find(part) != std::string::npos) return true;
    return false;
}
unsigned count_logged(const std::string& part) {
    unsigned n = 0;
    for (const auto& line : kui::g_log) n += line.find(part) != std::string::npos;
    return n;
}

// One game start in a child process; `body` runs its calls and checks. The child's checks are counted in the parent.
template <class Body>
void start(const char* name, Body body) {
    std::fflush(stdout);
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) std::abort();
    const pid_t pid = fork();
    if (pid == 0) {
        close(pipe_fds[0]);
        g_checks = g_failures = 0;
        body();
        const unsigned counts[2] = {g_checks, g_failures};
        if (write(pipe_fds[1], counts, sizeof counts) != sizeof counts) _exit(2);
        std::fflush(stdout);
        _exit(0);  // no exit handler of any kind: what a dead game leaves is what is on the disk
    }
    close(pipe_fds[1]);
    unsigned counts[2] = {0, 0};
    const bool got = read(pipe_fds[0], counts, sizeof counts) == sizeof counts;
    close(pipe_fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    g_checks += counts[0];
    g_failures += counts[1];
    expect(got && WIFEXITED(status) && WEXITSTATUS(status) == 0, std::string(name) + ": the start ran to its end");
}

// A start with the overlay's drawing on, through Kiln's Vulkan hooks up to `last` (vk.cpp's stages).
void stages(Stage last) {
    for (Stage s : {Stage::begun, Stage::hooks, Stage::instance, Stage::device, Stage::swapchain})
        if (s <= last) kui::overlay_guard_stage(s);
}
// The overlay's first draw and 60 presented frames with it drawn (vk.cpp hk_present).
void draw_frames() {
    kui::overlay_guard_first_draw();
    for (int i = 0; i < 60; ++i) kui::overlay_guard_drawn();
}
}  // namespace

int main() {
    unsetenv("KILN_UI_OVERLAY");
    g_root = std::filesystem::temp_directory_path() / ("kiln-guard-host-" + std::to_string(getpid()));
    g_state = g_root / "game/kiln/state";
    std::filesystem::create_directories(g_state);
    KilnApi api{};
    api.size = sizeof api;
    api.path = &fake_path;
    api.version = &fake_version;
    kui::g_api = &api;
    const std::string id = kui::overlay_guard::detect_wrappers(g_root / "game").identity;
    auto rec = [&](const char* fields) { return std::string("4 ") + fields + " 0.2.0-dev.29.19 " + id + "\n"; };
    auto reset = [&] { std::filesystem::remove(g_state / "overlay-start.guard"); };

    // The owner's sequence, 0.2.0-dev.29.18 candidate on Proton, 2026-11-19: the game died while the overlay was
    // starting, the next start ran without the overlay's drawing and quit normally from the main menu, and the start
    // after it must try the overlay again (it stayed off: a guarded start had to run a minute).
    start("died at hooks installed", [] {
        expect(!kui::overlay_guard_start(), "no record: the overlay's drawing is on");
        stages(Stage::hooks);
    });
    expect(record_text() == rec("1 1 0 0 0"), "the dead start's record: trying at hooks installed, got " + record_text());
    start("guarded, quits normally", [&] {
        expect(kui::overlay_guard_start(), "the start after the death leaves the drawing off");
        expect(record_text() == rec("2 1 1 1 0"), "guarded is written at once, got " + record_text());
        expect(logged("the overlay's drawing stays off for this start, and the next start tries it again"),
               "the warning says the next start tries again");
        expect(guard_host::timers.empty(), "a guarded start sets no timer (no minute to wait for)");
        stages(Stage::swapchain);  // nothing of the overlay runs in it; were a stage reported, it changes nothing
        expect(record_text() == rec("2 1 1 1 0"), "a guarded start arms no attempt of its own, got " + record_text());
        kui::overlay_guard_exit();
        expect(record_text() == rec("3 1 1 1 0"), "its normal exit writes retry, got " + record_text());
        expect(logged("the guard lets the next start try the overlay again"), "the exit says so");
    });
    start("the start after tries again", [&] {
        expect(!kui::overlay_guard_start(), "the overlay's drawing is on again");
        stages(Stage::begun);
        expect(record_text() == rec("1 0 1 1 0"), "an attempt, the first death kept, got " + record_text());
        stages(Stage::swapchain);
        draw_frames();
        expect(record_text() == rec("0 6 1 0 0"), "60 drawn frames complete it and clear the history, got " + record_text());
        kui::overlay_guard_exit();
        expect(record_text() == rec("0 6 1 0 0"), "the exit of a complete start writes nothing");
    });

    // The record the candidate left on the owner's disk ("2 2 1 1 1 0 w2-...") heals at the next start.
    std::ofstream(g_state / "overlay-start.guard", std::ios::binary) << rec("2 1 1 1 0");
    start("the candidate's stuck record", [] {
        expect(!kui::overlay_guard_start(), "a guarded record left by an earlier start: the overlay is tried");
    });

    // A guarded start that dies too (chosen: its death is not the overlay's, no Vulkan hook of Kiln's ran in it): the
    // start after tries the overlay again with the first death kept, and a second death at that stage locks.
    reset();
    start("died at instance seen", [] {
        kui::overlay_guard_start();
        stages(Stage::instance);
    });
    start("guarded, dies too", [&] {
        expect(kui::overlay_guard_start(), "guarded");
        expect(record_text() == rec("2 2 2 1 0"), "guarded is written, got " + record_text());
    });
    expect(record_text() == rec("2 2 2 1 0"), "the dead guarded start's record is unchanged");
    start("tries again, dies at the same stage", [&] {
        expect(!kui::overlay_guard_start(), "the start after a guarded start that died tries the overlay again");
        stages(Stage::instance);
    });
    start("locked, quits normally", [&] {
        expect(kui::overlay_guard_start(), "two deaths at one stage: the drawing stays off");
        expect(record_text() == rec("4 2 2 2 0"), "locked, got " + record_text());
        expect(logged("it failed twice at this stage") && !logged("the next start tries it again"),
               "the warning names the lock and promises no retry");
        kui::overlay_guard_exit();
        expect(record_text() == rec("4 2 2 2 0"), "a normal exit keeps the lock, got " + record_text());
    });
    start("still locked", [] { expect(kui::overlay_guard_start(), "locked until the record is deleted"); });

    // Nothing writes the record after the normal exit: a failure of the overlay or its first draw on another thread
    // in the moment before the process ends must not arm it again (before, either could leave trying behind).
    reset();
    start("fine without drawing, late calls after the exit", [&] {
        kui::overlay_guard_start();
        kui::g_presents = 0;
        stages(Stage::swapchain);
        kui::g_presents = 100;
        expect(guard_host::fire(kui::kIdleProofMs) == 1, "the idle watch runs");
        expect(record_text() == rec("0 4 0 0 0"), "presented through the hooks with nothing drawn: fine, got " + record_text());
        kui::overlay_guard_exit();
        kui::overlay_guard_first_draw();
        kui::overlay_guard_failed();
        draw_frames();
        guard_host::fire(kui::kDrawnProofMs);
        guard_host::fire(kui::kFailedProofMs);
        expect(record_text() == rec("0 4 0 0 0"), "nothing after the exit arms the record, got " + record_text());
    });
    start("drawn, failed late after the exit", [&] {
        expect(!kui::overlay_guard_start(), "on");
        stages(Stage::swapchain);
        draw_frames();
        expect(record_text() == rec("0 6 0 0 0"), "complete, got " + record_text());
        kui::overlay_guard_exit();
        kui::overlay_guard_failed();
        expect(record_text() == rec("0 6 0 0 0"), "a failure after the exit arms nothing, got " + record_text());
    });
    // ... while the same failure before the exit still arms it, and a death in its window guards the next start
    start("failed, then died", [&] {
        kui::overlay_guard_start();
        stages(Stage::swapchain);
        draw_frames();
        kui::overlay_guard_failed();
        expect(record_text() == rec("1 5 0 0 0"), "the failure arms the record again (the overlay's drawing), got " + record_text());
    });
    start("guarded after the failure", [&] {
        expect(kui::overlay_guard_start(), "the death after the failure guards this start");
        kui::overlay_guard_exit();
    });
    // A start still trying that quits normally (the main menu, nothing of Kiln's opened) counts as fine.
    start("quits before anything is drawn", [&] {
        expect(!kui::overlay_guard_start(), "on after the guarded start");
        stages(Stage::swapchain);
        kui::overlay_guard_exit();
        expect(record_text() == rec("0 4 5 1 0"), "complete, the history kept, got " + record_text());
    });
    start("on after a quiet quit", [] { expect(!kui::overlay_guard_start(), "on"); });

    // Inbox 97: a lock written by 0.2.0-dev.29.17 (old bugs, "failed twice at this stage") outlived the update to a Kiln
    // that fixed them. A record of another Kiln, or one without a version (every Kiln before 0.2.0-dev.29.19), is
    // cleared at the first start of the new one, and kiln.log says why.
    std::ofstream(g_state / "overlay-start.guard", std::ios::binary) << "2 4 1 1 2 0 " + id + "\n";
    start("29.17's lock after the update", [&] {
        expect(!kui::overlay_guard_start(), "a lock from an older Kiln: the overlay's drawing is on");
        expect(logged("Kiln was updated since the record was written (by a Kiln that did not note its version)"),
               "kiln.log says the guard was cleared because Kiln was updated");
        expect(!logged("failed twice") && !logged("graphics wrappers changed"), "and nothing else about the old record");
        expect(record_text() == rec("0 0 0 0 0"), "the cleared record is written with this Kiln's version, got " + record_text());
        stages(Stage::hooks);
        expect(record_text() == rec("1 1 0 0 0"), "an attempt of this Kiln, got " + record_text());
    });
    start("guarded after its own death", [&] {
        expect(kui::overlay_guard_start(), "a death of this Kiln's attempt still guards");
        expect(!logged("Kiln was updated"), "the record is this Kiln's: no update line");
    });
    std::ofstream(g_state / "overlay-start.guard", std::ios::binary) << "3 4 2 2 2 0 0.2.0-dev.29.18 " + id + "\n";
    start("29.18's lock after the update", [&] {
        expect(!kui::overlay_guard_start(), "a lock from another version: the overlay's drawing is on");
        expect(logged("Kiln was updated since the record was written (by Kiln 0.2.0-dev.29.18, this is 0.2.0-dev.29.19)"),
               "the line names both versions");
    });
    std::ofstream(g_state / "overlay-start.guard", std::ios::binary) << rec("4 2 2 2 0");
    start("this Kiln's lock", [&] {
        expect(kui::overlay_guard_start(), "a lock from this very Kiln holds");
        expect(!logged("Kiln was updated") && logged("it failed twice at this stage"), "the lock's own warning");
    });
    // a complete record of an older Kiln is rewritten quietly: nothing it said keeps the overlay off
    std::ofstream(g_state / "overlay-start.guard", std::ios::binary) << "2 0 5 0 0 0 " + id + "\n";
    start("29.17's complete record", [&] {
        expect(!kui::overlay_guard_start(), "on");
        expect(!logged("Kiln was updated"), "no line for a record that kept nothing off");
        expect(record_text() == rec("0 0 0 0 0"), "written with this Kiln's version, got " + record_text());
    });

    // ---- 0.2.0-dev.30.2: ReShade's Vulkan layer kept (--keep-vulkan-layers; Eris, netjunk) ----
    // Regression replay. ReShade compiles its effects on the first frames, so the game presented only a few frames
    // through Kiln's hooks in the 30 s after its swapchain. That one window was all the guard watched (since 29.18 the
    // overlay's frames alone completed a start): the record stayed trying at "swapchain created" for the whole session,
    // and two ends that were not a normal exit (the game killed, a crash of something else) locked the overlay off. The
    // windows repeat now, and the start counts as fine once the game presents normally.
    reset();
    for (int session = 1; session <= 3; ++session) {
        start("a ReShade-like stalled start, then normal, then killed", [&] {
            const std::string s = " (session " + std::to_string(session) + ")";
            expect(!kui::overlay_guard_start(), "the overlay's drawing is on" + s);
            kui::g_presents = 0;
            stages(Stage::swapchain);
            kui::g_presents = 9;  // ReShade compiling: a few frames in the first 30 s
            expect(guard_host::fire(kui::kIdleProofMs) == 1, "the first window after the swapchain ends" + s);
            expect(record_text() == rec("1 4 0 0 0"), "still watched, trying at swapchain created, got " + record_text());
            expect(logged("only 9 frames presented in 30 s since the swapchain; the start is still being watched"), "the slow window is logged" + s);
            kui::g_presents += 40;  // still compiling
            expect(guard_host::fire(kui::kIdleProofMs) == 1, "watched again: a second window" + s);
            expect(record_text() == rec("1 4 0 0 0"), "still trying, got " + record_text());
            kui::g_presents += 1800;  // then 60 frames a second
            expect(guard_host::fire(kui::kIdleProofMs) == 1, "watched again: a third window" + s);
            expect(record_text() == rec("0 4 0 0 0"), "fine once the game presents normally, got " + record_text());
            expect(logged("the game presented 1800 frames through Kiln's hooks in 30 s with nothing drawn yet (after slower first frames)"),
                   "the line says so" + s);
            expect(count_logged("frames presented in 30 s since the swapchain") == 1, "the slow windows are logged once" + s);
            expect(guard_host::fire(kui::kIdleProofMs) == 0, "the watch has ended" + s);
            // then the game is killed (Steam's Stop, a crash elsewhere): no exit step runs
        });
    }
    start("after three killed sessions", [] {
        expect(!kui::overlay_guard_start(), "the overlay's drawing is on: neither guarded nor locked");
    });

    // A start that never presents enough through Kiln's hooks (or is killed in its first windows) stays trying at
    // "swapchain created". Nothing of the overlay's own work ran in it: such an end guards the next start only, and never
    // locks. The overlay's first draw (vk.cpp draw(), past its checks) makes a death count again.
    reset();
    start("no frames after the swapchain, killed", [&] {
        kui::overlay_guard_start();
        kui::g_presents = 0;
        stages(Stage::swapchain);
        for (int i = 0; i < 4; ++i) expect(guard_host::fire(kui::kIdleProofMs) == 1, "watched window after window");
        expect(record_text() == rec("1 4 0 0 0"), "trying at swapchain created, got " + record_text());
    });
    start("guarded once", [&] {
        expect(kui::overlay_guard_start(), "the start after it leaves the drawing off");
        expect(record_text() == rec("2 4 0 0 0"), "guarded, no failure counted, got " + record_text());
        expect(logged("after its swapchain was made, before Kiln's overlay drew anything (stage: swapchain created)") &&
               logged("the next start tries it again") && logged("never turns the overlay off for good"), "the warning says so");
        kui::overlay_guard_exit();
        expect(record_text() == rec("3 4 0 0 0"), "its normal exit writes retry, got " + record_text());
    });
    start("tries again, killed the same way", [&] {
        expect(!kui::overlay_guard_start(), "the start after the guarded one tries again");
        stages(Stage::swapchain);
    });
    start("guarded again, never locked", [&] {
        expect(kui::overlay_guard_start(), "guarded");
        expect(record_text() == rec("2 4 0 0 0"), "guarded again, got " + record_text());
        expect(!logged("failed twice"), "no lock");
    });
    start("on, and the overlay's first draw", [&] {
        expect(!kui::overlay_guard_start(), "on: never locked at swapchain created");
        stages(Stage::swapchain);
        kui::overlay_guard_first_draw();  // vk.cpp draw(): the overlay's own work begins
        expect(record_text() == rec("1 5 0 0 0"), "the first draw moves the start on, got " + record_text());
        expect(guard_host::fire(kui::kIdleProofMs) == 1 && record_text() == rec("1 5 0 0 0"),
               "the idle watch ends at the first draw: the overlay's frames decide");
        // killed during its first draw
    });
    start("guarded after a death at the first draw", [&] {
        expect(kui::overlay_guard_start(), "guarded");
        expect(record_text() == rec("2 5 5 1 0"), "counted at the overlay's first draw, got " + record_text());
        expect(logged("while Kiln's overlay was starting (stage: the overlay's first draw)"), "named");
    });
    start("tries again, dies at the first draw again", [&] {
        expect(!kui::overlay_guard_start(), "tries");
        stages(Stage::swapchain);
        kui::overlay_guard_first_draw();
    });
    start("locked after two deaths at the first draw", [&] {
        expect(kui::overlay_guard_start(), "locked");
        expect(record_text() == rec("4 5 5 2 0") && logged("it failed twice at this stage"), "locked, got " + record_text());
    });

    // Kiln's menu asked for in the overlay while no frame of the overlay reaches the screen (ReShade's Vulkan layer kept,
    // 0.2.0-dev.30.1: the Mods entry opened a menu nobody could see). About a second without a frame of the overlay drawn
    // and presented: the Mods entry falls back to the game's own Mods rows (logged once), and opens Kiln's menu again once
    // a frame of the overlay was presented since.
    reset();
    start("Kiln's menu that never shows", [&] {
        expect(!kui::overlay_guard_start(), "on");
        kui::g_presents = 0;
        stages(Stage::swapchain);
        expect(kui::overlay_menu_ask(), "the Mods entry opens Kiln's menu in the overlay");
        expect(kui::overlay_menu_ask(), "a second click within the second: the same watch");
        kui::g_presents = 40;  // the game presents, none of them with a frame of the overlay
        expect(!kui::g_menu_fell_back && kui::g_fell_back_calls == 0, "not before the second is up");
        expect(guard_host::fire(kui::kMenuShowMs) == 1, "one watch, ended after a second");
        expect(kui::g_menu_fell_back && kui::g_fell_back_calls == 1, "no frame of the overlay: the game's own Mods rows");
        expect(logged("no frame of Kiln's overlay was drawn and presented in the 1000 ms after (the game presented 40 frames"),
               "logged with what Kiln saw");
        expect(!kui::overlay_menu_ask(), "the next click: the game's own rows (the entry's caption)");
        expect(guard_host::fire(kui::kMenuShowMs) == 0, "and no new watch");
        draw_frames();  // a frame of the overlay was presented since (a toast, say)
        expect(kui::overlay_menu_ask() && !kui::g_menu_fell_back, "the next click opens Kiln's menu in the overlay again");
        kui::overlay_guard_drawn();  // its frame comes
        expect(guard_host::fire(kui::kMenuShowMs) == 1 && !kui::g_menu_fell_back && kui::g_fell_back_calls == 1, "shown: no fallback");
        expect(kui::overlay_menu_ask(), "asked again");
        expect(guard_host::fire(kui::kMenuShowMs) == 1 && kui::g_menu_fell_back && kui::g_fell_back_calls == 2,
               "no frame again: falls back again");
        expect(count_logged("no frame of Kiln's overlay was drawn and presented") == 1, "logged once");
        kui::overlay_guard_exit();
        expect(!kui::overlay_menu_ask(), "the game's own rows until the end");
    });
    start("the overlay still making its first frame", [&] {
        expect(!kui::overlay_guard_start(), "on");
        stages(Stage::swapchain);
        expect(kui::overlay_menu_ask(), "Kiln's menu asked for");
        kui::g_drawing = true;  // the first frame of the process: ImGui and the menu's code starting
        expect(guard_host::fire(kui::kMenuShowMs) == 1 && !kui::g_menu_fell_back, "a frame being made: not yet");
        expect(guard_host::fire(kui::kMenuBusyMs) == 1 && !kui::g_menu_fell_back, "looked at again");
        kui::g_drawing = false;
        kui::overlay_guard_first_draw();
        kui::overlay_guard_drawn();  // the frame was presented
        expect(guard_host::fire(kui::kMenuBusyMs) == 1 && !kui::g_menu_fell_back && kui::g_fell_back_calls == 0, "shown");
        // frames that are made but never presented: it falls back after 5 s at most
        expect(kui::overlay_menu_ask(), "asked again");
        kui::g_drawing = true;
        unsigned fires = guard_host::fire(kui::kMenuShowMs);
        for (int i = 0; i < 40 && !kui::g_menu_fell_back; ++i) fires += guard_host::fire(kui::kMenuBusyMs);
        expect(kui::g_menu_fell_back && fires == 1 + (kui::kMenuBusyMaxMs - kui::kMenuShowMs) / kui::kMenuBusyMs,
               "falls back after 5 s, got " + std::to_string(fires) + " looks");
        kui::overlay_guard_exit();
    });
    start("the overlay's drawing turned off during the session", [&] {
        expect(!kui::overlay_guard_start(), "on");
        stages(Stage::swapchain);
        expect(kui::overlay_menu_ask(), "Kiln's menu in the overlay");
        kui::g_overlay_failed = true;  // ui.cpp overlay_turned_off: a present from frame generation's own queue, say
        expect(!kui::overlay_menu_ask(), "from then on the game's own Mods rows");
        expect(guard_host::fire(kui::kMenuShowMs) == 1 && !kui::g_menu_fell_back && kui::g_fell_back_calls == 0,
               "the pending watch ends quietly: the drawing-off state says it all");
    });

    // Kiln's drawing steps aside on purpose for another layer on the game's presents (NVIDIA Streamline, OptiScaler with
    // its own menu or its frame generation): one line names the layer, and the Mods entry shows the game's own Mods rows,
    // never Kiln's menu.
    reset();
    std::ofstream(g_root / "game/sl.interposer.dll", std::ios::binary) << "Streamline";
    start("Streamline next to the game", [&] {
        expect(kui::overlay_guard_start(), "the overlay's drawing steps aside for Streamline");
        expect(count_logged("Kiln's overlay drawing steps aside for NVIDIA Streamline (sl.interposer.dll, a frame generation mod)") == 1,
               "one line names it");
        // (0.2.0-dev.31.25) the Mods entry names the setting that draws it anyway
        expect(logged("the Mods entry shows the game's own Mods rows (\"Mods: the overlay's drawing is off: NVIDIA Streamline is installed "
                      "(kiln.json ui.overlay \"on\" draws it anyway)\""),
               "and says what the Mods entry does");
        expect(!kui::overlay_menu_ask() && guard_host::timers.empty(), "the Mods entry: the game's own Mods rows, nothing watched");
    });
    std::filesystem::remove(g_root / "game/sl.interposer.dll");
    std::ofstream(g_root / "game/version.dll", std::ios::binary) << "MZ...OptiScaler.ini...";
    std::ofstream(g_root / "game/OptiScaler.ini", std::ios::binary) << "[Menu]\nOverlayMenu=auto\n";  // as it ships
    start("OptiScaler with its own overlay menu", [&] {
        expect(kui::overlay_guard_start(), "the overlay's drawing steps aside for OptiScaler's menu");
        expect(logged("Kiln's overlay drawing steps aside for OptiScaler (version.dll, its own overlay menu on)"), "named with its file");
        expect(!kui::overlay_menu_ask(), "the game's own Mods rows");
    });
    std::ofstream(g_root / "game/OptiScaler.ini", std::ios::binary) << "[Menu]\nOverlayMenu=false\n[FrameGen]\nFGInput=upscaler\nFGOutput=fsrfg\n";
    start("OptiScaler with its frame generation", [&] {
        expect(kui::overlay_guard_start(), "the overlay's drawing steps aside for OptiScaler's frame generation");
        expect(logged("Kiln's overlay drawing steps aside for OptiScaler (version.dll, its frame generation on)"), "named");
        expect(!kui::overlay_menu_ask(), "the game's own Mods rows");
    });
    std::filesystem::remove(g_root / "game/version.dll");
    std::filesystem::remove(g_root / "game/OptiScaler.ini");
    reset();
    start("KILN_UI_OVERLAY=0", [&] {
        setenv("KILN_UI_OVERLAY", "0", 1);  // this start's process only
        expect(kui::overlay_guard_start(), "off by the player's switch");
        expect(logged("ui: the overlay's drawing is off: KILN_UI_OVERLAY=0; key bindings and the Mods menu still work") &&
               !logged("steps aside"), "the switch's own line, no layer named");
        expect(!kui::overlay_menu_ask(), "the game's own Mods rows");
    });

    // ---- 0.2.0-dev.31.25 (Soligna, Nexus 301): kiln.json "ui": {"overlay": "on"}, the same as KILN_UI_OVERLAY=1 ----
    // Steam's launch options on Windows cannot set the variable, so the launcher writes the setting. One sequence, run once
    // with each switch, NVIDIA Streamline next to the game: the attempt dies at hooks installed twice (the crash guard
    // counts both, and locks), the start without the switch keeps the lock, the switch on again draws and completes it.
    // The records on the disk after each start, and what the drawing does in each, must be the same for both switches.
    const auto kiln_json = g_root / "game/kiln/kiln.json";
    const auto streamline = g_root / "game/sl.interposer.dll";
    auto set_json = [&](const char* text) {
        if (text) std::ofstream(kiln_json, std::ios::binary) << text;
        else std::filesystem::remove(kiln_json);
    };
    auto switched_on = [&](bool by_file) {
        std::vector<std::string> records;
        reset();
        std::ofstream(streamline, std::ios::binary) << "Streamline";
        const std::string sl_id = kui::overlay_guard::detect_wrappers(g_root / "game").identity;
        auto rec_sl = [&](const char* fields) { return std::string("4 ") + fields + " 0.2.0-dev.29.19 " + sl_id + "\n"; };
        const std::string who = by_file ? "kiln.json ui.overlay \"on\"" : "KILN_UI_OVERLAY=1";
        const std::string run = by_file ? " (kiln.json)" : " (KILN_UI_OVERLAY=1)";
        set_json(by_file ? R"({"ui": {"look": "game", "overlay": "on"}})" : nullptr);
        auto on = [by_file] {
            if (!by_file) setenv("KILN_UI_OVERLAY", "1", 1);  // this start's process only
        };
        start("switched on next to Streamline, dies at hooks installed", [&] {
            on();
            expect(!kui::overlay_guard_start(), "the overlay's drawing is on next to Streamline" + run);
            expect(logged("ui: " + who + ": trying overlay drawing despite Streamline; the crash guard records this attempt"),
                   "kiln.log names the switch" + run);
            expect(!logged("steps aside"), "no step-aside line" + run);
            stages(Stage::hooks);
        });
        records.push_back(record_text());
        expect(records.back() == rec_sl("1 1 0 0 0"), "the attempt is recorded, got " + records.back() + run);
        start("switched on after a death", [&] {
            on();
            expect(!kui::overlay_guard_start(), "the switch draws despite the interrupted start" + run);
            expect(logged("ui: " + who + ": trying overlay drawing despite an interrupted start; the crash guard records this attempt"),
                   "kiln.log says so" + run);
            expect(!logged("stays off for this start"), "no guarded start" + run);
            stages(Stage::hooks);
        });
        records.push_back(record_text());
        expect(records.back() == rec_sl("1 1 1 1 0"), "an attempt with the first death kept, got " + records.back() + run);
        start("switched on after two deaths at one stage", [&] {
            on();
            expect(!kui::overlay_guard_start(), "the switch draws despite the lock" + run);
            stages(Stage::hooks);
        });
        records.push_back(record_text());
        expect(records.back() == rec_sl("1 1 1 2 0"), "two deaths counted, got " + records.back() + run);
        set_json(nullptr);
        start("the switch taken away", [&] {
            expect(kui::overlay_guard_start(), "without the switch the lock holds" + run);
            expect(logged("it failed twice at this stage") && logged("kiln.json ui.overlay \"on\" or KILN_UI_OVERLAY=1 tries drawing at your own risk"),
                   "the lock's warning names both switches" + run);
            kui::overlay_guard_exit();
        });
        records.push_back(record_text());
        expect(records.back() == rec_sl("4 1 1 2 0"), "locked, got " + records.back() + run);
        set_json(by_file ? R"({"ui": {"overlay": "on"}})" : nullptr);
        start("switched on again, draws", [&] {
            on();
            expect(!kui::overlay_guard_start(), "on again" + run);
            stages(Stage::swapchain);
            draw_frames();
            kui::overlay_guard_exit();
        });
        records.push_back(record_text());
        expect(records.back() == rec_sl("0 6 1 0 0"), "60 drawn frames complete it and clear the history, got " + records.back() + run);
        std::filesystem::remove(streamline);
        set_json(nullptr);
        return records;
    };
    const auto by_environment = switched_on(false), by_file = switched_on(true);
    expect(by_environment == by_file, "kiln.json ui.overlay \"on\" and KILN_UI_OVERLAY=1 leave the same records at every start");

    // Precedence: KILN_UI_OVERLAY (0 or 1) wins over kiln.json ui.overlay, as it does over "off".
    reset();
    std::ofstream(streamline, std::ios::binary) << "Streamline";
    set_json(R"({"ui": {"overlay": "on"}})");
    start("KILN_UI_OVERLAY=0 over kiln.json \"on\"", [&] {
        setenv("KILN_UI_OVERLAY", "0", 1);
        expect(kui::overlay_guard_start(), "the variable turns the drawing off");
        expect(logged("ui: the overlay's drawing is off: KILN_UI_OVERLAY=0; key bindings and the Mods menu still work"), "its own line");
    });
    set_json(R"({"ui": {"overlay": "auto"}})");
    start("kiln.json \"auto\" next to Streamline", [&] {
        expect(kui::overlay_guard_start(), "Kiln decides: steps aside for Streamline");
        expect(logged("NVIDIA Streamline is installed (kiln.json ui.overlay \"on\" draws it anyway)"), "the reason names the setting");
    });
    std::filesystem::remove(streamline);
    set_json(R"({"ui": {"overlay": "off"}})");
    start("KILN_UI_OVERLAY=1 over kiln.json \"off\"", [&] {
        setenv("KILN_UI_OVERLAY", "1", 1);
        expect(!kui::overlay_guard_start(), "the variable turns the drawing on");
        expect(!logged("trying overlay drawing despite"), "nothing to try despite: no line");
    });
    // next to OptiScaler with its own menu: the setting decides, as the variable does
    std::ofstream(g_root / "game/version.dll", std::ios::binary) << "MZ...OptiScaler.ini...";
    std::ofstream(g_root / "game/OptiScaler.ini", std::ios::binary) << "[Menu]\nOverlayMenu=auto\n";
    set_json(R"({"ui": {"overlay": "on"}})");
    start("kiln.json \"on\" next to OptiScaler's menu", [&] {
        expect(!kui::overlay_guard_start(), "the setting draws next to OptiScaler's menu");
        expect(logged("ui: OptiScaler is installed (version.dll) with OverlayMenu=auto (on), no frame generation in OptiScaler.ini; "
                      "kiln.json ui.overlay \"on\" decides for Kiln's overlay"), "kiln.log says the setting decides");
        expect(!logged("steps aside"), "no step-aside line");
        expect(logged("ui: kiln.json ui.overlay \"on\": trying overlay drawing despite OptiScaler's own overlay"), "and that it tries");
    });
    set_json(nullptr);
    start("OptiScaler's menu, no setting", [&] {
        expect(kui::overlay_guard_start(), "steps aside");
        expect(logged("\"Mods: the overlay's drawing is off: OptiScaler's own overlay menu is on (kiln.json ui.overlay \"on\" draws it anyway)\""),
               "the Mods entry names the setting");
    });
    std::filesystem::remove(g_root / "game/version.dll");
    std::filesystem::remove(g_root / "game/OptiScaler.ini");

    std::filesystem::remove_all(g_root);
    std::printf("overlay_guard_host: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

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
    auto rec = [&](const char* fields) { return std::string("3 ") + fields + " 0.2.0-dev.29.19 " + id + "\n"; };
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
        expect(record_text() == rec("0 5 1 0 0"), "60 drawn frames complete it and clear the history, got " + record_text());
        kui::overlay_guard_exit();
        expect(record_text() == rec("0 5 1 0 0"), "the exit of a complete start writes nothing");
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
        expect(record_text() == rec("0 5 0 0 0"), "complete, got " + record_text());
        kui::overlay_guard_exit();
        kui::overlay_guard_failed();
        expect(record_text() == rec("0 5 0 0 0"), "a failure after the exit arms nothing, got " + record_text());
    });
    // ... while the same failure before the exit still arms it, and a death in its window guards the next start
    start("failed, then died", [&] {
        kui::overlay_guard_start();
        stages(Stage::swapchain);
        draw_frames();
        kui::overlay_guard_failed();
        expect(record_text() == rec("1 4 0 0 0"), "the failure arms the record again, got " + record_text());
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
        expect(record_text() == rec("0 4 4 1 0"), "complete, the history kept, got " + record_text());
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

    std::filesystem::remove_all(g_root);
    std::printf("overlay_guard_host: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

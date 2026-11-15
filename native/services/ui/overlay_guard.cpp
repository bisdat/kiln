// SPDX-License-Identifier: MIT
#include <windows.h>
#include <filesystem>
#include <atomic>
#include <mutex>
#include "services/kiln_caps.h"
#include "ui.h"
#include "overlay_guard_runtime.h"
namespace kui {
namespace {
std::mutex mu;
overlay_guard::Record record;
std::wstring path;
bool active = false, said_error = false, began_written = false, idle_done = false;
// set (under mu) by the overlay's first draw attempt of this process, before any of its work: from then on only the
// presented frame (or a normal exit) completes the start, never the idle timer
std::atomic<bool> draw_seen{false};
HANDLE ran_timer = nullptr, idle_timer = nullptr;
uint64_t presents_at_swapchain = 0;
// How long a guarded start (overlay off) must run before the next start may try the overlay again.
constexpr DWORD kRanFineMs = 60'000;
// How long the game must keep presenting through Kiln's hooks after its swapchain, with nothing drawn yet, for the start
// to count as fine (overlay_guard::idle), and how many frames it must present in that time (not hung).
constexpr DWORD kIdleProofMs = 30'000;
constexpr uint64_t kIdleProofPresents = 60;
void error() {
    if (!said_error) {
        said_error = true;
        logf(KILN_LOG_WARN, "ui: overlay crash guard could not read or write kiln/state/overlay-start.guard; continuing without that update");
    }
}
std::filesystem::path api_path(int kind) {
    char b[2048]; size_t n = 0;
    if (g_api->path(kind, b, sizeof b, &n) != KILN_OK || !n || n >= sizeof b) return {};
    return std::filesystem::path(std::u8string(b, b + n));
}
// Bounded known-wrapper set, no recursive disk scan. Presence and loaded presence use
// the same bit so loading an already installed DLL does not spuriously grant a retry.
std::string modules(bool& streamline) {
    auto game = api_path(KILN_PATH_GAME);
    std::string set = "m";
    const wchar_t* names[] = {L"sl.interposer.dll", L"nvngx_dlss.dll", L"dxgi.dll", L"ReShade64.dll", L"opengl32.dll", L"powrprof.dll"};
    // dxgi, opengl32 and powrprof are also Windows' own DLLs: only a FILE of that name next to the game is a wrapper.
    // Whether the system's copy is loaded yet when this runs differs from start to start and would reset the history.
    const bool system_name[] = {false, false, true, false, true, true};
    for (unsigned i = 0; i < 6; ++i) {
        bool found = !system_name[i] && GetModuleHandleW(names[i]) != nullptr;
        if (!game.empty()) {
            std::error_code ec;
            found |= std::filesystem::is_regular_file(game / names[i], ec);
            found |= std::filesystem::is_regular_file(game / L"Streamline" / names[i], ec);
        }
        if (found) set += char('a' + i);
        if (!i) streamline = found;
    }
    return set;
}
void save() {
    if (path.empty()) return;
    const auto text = overlay_guard::encode(record);
    const auto temp = path + L".tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) { error(); return; }
    DWORD n = 0;
    bool ok = WriteFile(h, text.data(), DWORD(text.size()), &n, nullptr) && n == text.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) error();
}
// A guarded start earns the next attempt by RUNNING, not by exiting cleanly: whether a client's normal quit reaches the
// process's exit call with code 0 is not confirmed (kiln_api.h, on_unload), so a rule that waits for a clean exit
// could leave the overlay off for good after one game closed during loading. A game that has run a minute without
// the overlay did not die of the overlay; the next start tries it again once.
VOID CALLBACK ran_fine(PVOID, BOOLEAN) {
    try {
        std::lock_guard<std::mutex> lock(mu);
        if (!active || record.phase != overlay_guard::Phase::guarded) return;
        record = overlay_guard::finish(record);
        save();
        active = false;
        logf(KILN_LOG_INFO, "ui: overlay crash guard: the game ran a minute without the overlay; the next start tries the overlay again");
    } catch (...) { error(); }
}
// Nothing has been drawn yet (the overlay draws once Kiln's menu or a mod's window is open), but the game has kept
// presenting through Kiln's hooks since its swapchain: the start did not die at the overlay. The first draw arms the
// record again (overlay_guard_first_draw).
VOID CALLBACK idle_proof(PVOID, BOOLEAN) {
    try {
        std::lock_guard<std::mutex> lock(mu);
        if (!active || draw_seen.load() || record.phase != overlay_guard::Phase::trying ||
            record.stage != overlay_guard::Stage::swapchain)
            return;
        const uint64_t presented = vk_presents() - presents_at_swapchain;
        if (presented < kIdleProofPresents) {
            logf(KILN_LOG_INFO, "ui: overlay crash guard: only %llu frames presented in %u s since the swapchain; the start is still being watched",
                 static_cast<unsigned long long>(presented), unsigned(kIdleProofMs / 1000));
            return;
        }
        record = overlay_guard::idle(record);
        idle_done = true;
        save();
        logf(KILN_LOG_INFO, "ui: overlay crash guard: the game presented %llu frames through Kiln's hooks with nothing drawn yet; the start counts as fine, and the first draw is watched when it comes",
             static_cast<unsigned long long>(presented));
    } catch (...) { error(); }
}

}
bool overlay_guard_start() noexcept {
    try {
        bool sl = false;
        const auto wrapper_set = modules(sl);
        char option[8] = {};
        const DWORD option_size = GetEnvironmentVariableA("KILN_UI_OVERLAY", option, sizeof option);
        const int environment = option_size == 1 && option[0] == '0' ? 0 : option_size == 1 && option[0] == '1' ? 1 : -1;
        auto apply_mode = [&](bool guarded) {
            g_overlay_mode = choose_overlay_mode(false, guarded, sl, environment);
            if (!overlay_drawing_on()) {
                g_overlay_reason = "the overlay's drawing is off: ";
                g_overlay_reason += environment == 0 ? "KILN_UI_OVERLAY=0" : guarded ? "an earlier overlay start was interrupted" : "NVIDIA Streamline is installed";
            }
        };
        apply_mode(false);
        auto dir = api_path(KILN_PATH_STATE);
        if (dir.empty()) { error(); return !overlay_drawing_on(); }
        path = (dir / L"overlay-start.guard").wstring();
        char b[513]; DWORD n = 0;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const bool ok = ReadFile(h, b, sizeof b, &n, nullptr);
            CloseHandle(h);
            if (!ok || !overlay_guard::parse(std::string(b, n), record)) error();
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) error();
        auto start = overlay_guard::begin(record, wrapper_set);
        apply_mode(start.off);
        record = start.record;
        if (environment == 1) {
            // Explicit opt-in bypasses OFF for this attempt, preserving failure history.
            record.phase = overlay_guard::Phase::trying;
            record.stage = overlay_guard::Stage::begun;
        }
        active = true;
        // An ON attempt is not recorded until vk_install, immediately before detours.
        if (start.off && !overlay_drawing_on()) {
            save();
            if (record.phase == overlay_guard::Phase::guarded &&
                !CreateTimerQueueTimer(&ran_timer, nullptr, &ran_fine, nullptr, kRanFineMs, 0, WT_EXECUTEONLYONCE))
                logf(KILN_LOG_WARN, "ui: overlay crash guard: no timer; the overlay stays off until kiln/state/overlay-start.guard is deleted");
            std::string why = "the game closed during the last start while Kiln's overlay was starting (stage: ";
            why += overlay_guard::stage_name(record.failed_stage);
            why += "); the overlay's drawing stays off for this start; key bindings and the Mods menu still work";
            if (record.phase == overlay_guard::Phase::locked) why += "; it failed twice at this stage; delete kiln/state/overlay-start.guard to try again";
            if (sl) why += "; NVIDIA Streamline (a frame generation mod) is installed; it wraps the same graphics calls";
            why += "; KILN_UI_OVERLAY=1 tries drawing at your own risk";
            logf(KILN_LOG_WARN, "ui: %s", why.c_str());

        }
        if (!overlay_drawing_on() && !start.off)
            logf(KILN_LOG_INFO, "ui: %s; key bindings and the Mods menu still work; KILN_UI_OVERLAY=1 tries drawing at your own risk", g_overlay_reason.c_str());
        else if (environment == 1 && (sl || start.off))
            logf(KILN_LOG_WARN, "ui: KILN_UI_OVERLAY=1: trying overlay drawing despite Streamline or an interrupted start; the crash guard records this attempt");
        return !overlay_drawing_on();
    } catch (...) { error(); return !overlay_drawing_on(); }
}
void overlay_guard_stage(overlay_guard::Stage stage) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mu);
        if (!active || record.phase != overlay_guard::Phase::trying || stage < record.stage || (began_written && stage == record.stage)) return;
        began_written = true;
        record.stage = stage;
        if (stage == overlay_guard::Stage::frame) {
            record = overlay_guard::finish(record);
            active = false;
            logf(KILN_LOG_INFO, "ui: overlay crash guard complete: first frame presented with overlay drawn");
        } else if (stage == overlay_guard::Stage::swapchain && !idle_timer) {
            presents_at_swapchain = vk_presents();
            if (!CreateTimerQueueTimer(&idle_timer, nullptr, &idle_proof, nullptr, kIdleProofMs, 0, WT_EXECUTEONLYONCE)) {
                idle_timer = nullptr;
                logf(KILN_LOG_WARN, "ui: overlay crash guard: no timer; until the overlay's first frame or a normal exit, a closed game counts as an interrupted overlay start");
            }
        }
        save();
    } catch (...) { error(); }
}
void overlay_guard_first_draw() noexcept {
    if (draw_seen.load(std::memory_order_relaxed)) return;  // every later draw: one atomic read
    try {
        std::lock_guard<std::mutex> lock(mu);
        if (draw_seen.exchange(true)) return;
        if (!active || !idle_done || record.phase != overlay_guard::Phase::complete) return;
        record = overlay_guard::rearm(record);
        save();
        logf(KILN_LOG_INFO, "ui: overlay crash guard: the overlay's first draw; watched until its frame is presented");
    } catch (...) { error(); }
}
void overlay_guard_exit() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mu);
        if (!active || record.phase != overlay_guard::Phase::trying) return;
        record = overlay_guard::idle(record);
        active = false;
        save();
        logf(KILN_LOG_INFO, "ui: overlay crash guard: the game exits normally; the overlay's start counts as fine");
    } catch (...) { error(); }
}
}

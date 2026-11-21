// SPDX-License-Identifier: MIT
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <atomic>
#include <mutex>
#include "services/kiln_caps.h"
#include "ui.h"
#include "overlay_guard_runtime.h"
#include "overlay_guard_file.h"
#include "overlay_wrappers.h"
#include "overlay_optiscaler.h"
#include "overlay_setting.h"
namespace kui {
std::atomic<bool> g_menu_fell_back{false};  // ui.h
namespace {
std::mutex mu;
overlay_guard::Record record;
std::wstring path;
bool active = false, said_error = false, error_unresolved = false, began_written = false, idle_done = false;
// set by overlay_guard_exit (the process exits with code 0): nothing writes the record after it, so a late failure or
// first draw on another thread before the process ends cannot arm it again behind the exit's back
bool exited = false;
bool found_optiscaler = false, found_streamline = false;  // overlay_guard_tools
// set (under mu) by the overlay's first draw attempt of this process, before any of its work: from then on only the
// presented frame (or a normal exit) completes the start, never the idle timer
std::atomic<bool> draw_seen{false};
HANDLE idle_timer = nullptr;
uint64_t presents_at_swapchain = 0;
unsigned idle_windows = 0;  // post-swapchain windows watched so far
// How long the game must keep presenting through Kiln's hooks after its swapchain, with nothing drawn yet, for the start
// to count as fine (overlay_guard::idle), and how many frames it must present in that time (not hung). Watched window
// after window until one has them (since 0.2.0-dev.30.2): a start whose first frames stall (ReShade's Vulkan layer
// compiles its effects there, a slow first load) counts as fine once the game presents normally. Before, the one window
// after the swapchain was all: a slower start stayed "trying" for its whole session, and any end of it that was not a
// normal exit read as a death at the overlay's start.
constexpr DWORD kIdleProofMs = 30'000;
constexpr uint64_t kIdleProofPresents = 60;
// Until 0.2.0-dev.29.18 the start counted as fine at the first present with the overlay drawn. Now it does after
// kDrawnFrames of them, or once the game has presented kIdleProofPresents frames in one kDrawnProofMs window after the
// first (the overlay may stop drawing sooner: a toast that went; the window is watched again until it has): a game that
// dies right after the overlay's first frames is caught as one that dies before them. A normal quit (code 0) counts as
// fine: kiln.ui's on_unload runs overlay_guard_exit as one of Kiln's own exit steps, under Wine and, since
// 0.2.0-dev.29.18, on native Windows too (only kiln.json "unloadAtExit": "off" turns it off). That the game's own quit on
// Windows exits with code 0 is not confirmed yet (kiln_api.h, on_unload); where it does not, a quit in the first second
// of the overlay's drawing counts as a closed start, which costs one start without the overlay.
constexpr uint64_t kDrawnFrames = 60;
constexpr DWORD kDrawnProofMs = 20'000;
// After the overlay's drawing turned off during a session because its own work failed (overlay_guard_failed; not for a
// present it would not draw on): the game must go on presenting kIdleProofPresents frames in a kFailedProofMs window
// (watched again until it has) for the failure not to count as a death at the overlay.
constexpr DWORD kFailedProofMs = 30'000;
std::atomic<uint64_t> drawn_presents{0};
HANDLE drawn_timer = nullptr, failed_timer = nullptr;
uint64_t presents_at_drawn = 0, presents_at_failed = 0;
// what: the step that failed ("read", "write", "replace", ...); code: its Win32 error, 0 when there is none.
void error(const char* what = "update", unsigned long code = 0) {
    error_unresolved = true;
    if (!said_error) {
        said_error = true;
        logf(KILN_LOG_WARN, "ui: overlay crash guard could not %s kiln/state/overlay-start.guard (Windows error %lu); continuing without that update",
             what, code);
    }
}
std::filesystem::path api_path(int kind) {
    size_t n = 0;
    if (g_api->path(kind, nullptr, 0, &n) != KILN_ERR_BUFFER_TOO_SMALL || !n || n > 128 * 1024) return {};
    std::u8string b(n, u8'\0');
    if (g_api->path(kind, reinterpret_cast<char*>(b.data()), b.size(), &n) != KILN_OK || !n || n > b.size()) return {};
    b.resize(n);
    return std::filesystem::path(b);
}
std::filesystem::path module_file(HMODULE h) {
    std::wstring file(32768, L'\0');
    const DWORD n = GetModuleFileNameW(h, file.data(), DWORD(file.size()));
    if (!n || n >= file.size()) return {};
    file.resize(n);
    return file;
}
// Only known wrapper names, with full loaded paths for copies outside the two install folders. OptiScaler is found by
// its contents (overlay_optiscaler.h): its DLL under one of its names next to the game, or a loaded copy.
std::string modules(bool& streamline, overlay_guard::OptiScaler& optiscaler) {
    overlay_guard::LoadedWrappers loaded;
    for (unsigned i = 0; i < overlay_guard::wrapper_names.size(); ++i) {
        if (i == 1 || overlay_guard::system_names[i]) continue;
        if (auto h = GetModuleHandleW(overlay_guard::wrapper_names[i])) {
            loaded[i] = overlay_guard::wrapper_names[i];
            if (auto file = module_file(h); !file.empty()) loaded[i] = file;
        }
    }
    const auto game = api_path(KILN_PATH_GAME);
    std::vector<std::filesystem::path> loaded_optiscaler;
    for (const wchar_t* name : overlay_guard::optiscaler_names)
        if (auto h = GetModuleHandleW(name))
            if (auto file = module_file(h); !file.empty()) loaded_optiscaler.push_back(file);
    optiscaler = overlay_guard::detect_optiscaler(game, loaded_optiscaler);
    std::vector<std::filesystem::path> extra;
    if (optiscaler.found()) extra.push_back(optiscaler.file);
    const auto found = overlay_guard::detect_wrappers(game, loaded, extra);
    streamline = found.streamline;
    return found.identity;
}
// This Kiln's version as the record keeps it (overlay_guard::for_kiln): a release build's full version, e.g.
// "0.2.0-dev.29.19".
std::string kiln_version() {
    const KilnStr v = g_api->version ? g_api->version() : KilnStr{};
    return overlay_guard::kiln_token(v.ptr ? std::string(v.ptr, v.len) : std::string());
}
std::string utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
// What OptiScaler.ini sets that decides for Kiln's overlay, for the log.
std::string optiscaler_settings(const overlay_guard::OptiScaler& o) {
    if (!o.ini.read) return "no OptiScaler.ini next to it, so its overlay menu is on (OptiScaler's default)";
    std::string s = o.ini.overlay_menu == 0 ? "OverlayMenu=false" : o.ini.overlay_menu == 1 ? "OverlayMenu=true" : "OverlayMenu=auto (on)";
    s += o.ini.frame_gen() ? ", frame generation set (FGInput=" + o.ini.fg_input + ", FGOutput=" + o.ini.fg_output + ")"
                           : ", no frame generation";
    return s + " in OptiScaler.ini";
}
// One flush of the record queued at a time; a save after its start queues the next.
std::atomic<bool> flush_queued{false};
// On a pool thread: the record (path is set once, before the first save) forced to the disk. Shared for delete, so the
// next save's rename over it never fails because of this handle.
DWORD WINAPI flush_record(void*) {
    flush_queued.store(false);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    return 0;
}
// The record's file operations (overlay_guard_file.h): nothing is forced to the disk on the caller's thread, the
// game's present call for most stages.
struct GuardFs {
    const char* failed = "write";
    DWORD code = 0;
    bool fail(const char* what) { failed = what; code = GetLastError(); return false; }
    bool write_new(const std::wstring& file, std::string_view text) {
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return fail("create the temporary file of");
        DWORD n = 0;
        const bool ok = WriteFile(h, text.data(), DWORD(text.size()), &n, nullptr) && n == text.size();
        if (!ok) fail("write the temporary file of");
        CloseHandle(h);
        return ok;
    }
    bool replace(const std::wstring& from, const std::wstring& to) {
        // Another process reading the record without delete sharing (a scanner, a backup) makes the rename fail for
        // that moment; one more try after a short wait (only on that failure: a normal save never waits).
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        Sleep(20);
        return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0 || fail("replace");
    }
    void flush_later(const std::wstring&) {
        if (flush_queued.exchange(true)) return;
        if (!QueueUserWorkItem(&flush_record, nullptr, WT_EXECUTEDEFAULT)) flush_queued.store(false);
    }
};
void save() {
    GuardFs fs;
    if (!overlay_guard::write_record(fs, path, overlay_guard::encode(record))) { error(fs.failed, fs.code); return; }
    if (error_unresolved) {
        error_unresolved = false;
        logf(KILN_LOG_INFO, "ui: overlay crash guard: kiln/state/overlay-start.guard written after the earlier failure; the guard is up to date again");
    }
}
// A periodic watch ends (its own callback, mu held): deleted without waiting for the callback that runs this.
void stop_timer(HANDLE& timer) {
    if (timer) DeleteTimerQueueTimer(nullptr, timer, nullptr);
    timer = nullptr;
}
// Nothing has been drawn yet (the overlay draws once Kiln's menu or a mod's window is open), but the game has kept
// presenting through Kiln's hooks since its swapchain: the start did not die at the overlay. The first draw arms the
// record again (overlay_guard_first_draw). Periodic: a window with too few frames is followed by the next one, until one
// has them, the overlay's first draw comes (its own watch decides then) or the game ends.
VOID CALLBACK idle_proof(PVOID, BOOLEAN) {
    try {
        std::lock_guard<std::mutex> lock(mu);
        if (!active || draw_seen.load() || record.phase != overlay_guard::Phase::trying ||
            record.stage != overlay_guard::Stage::swapchain)
            return stop_timer(idle_timer);
        ++idle_windows;
        const uint64_t now = vk_presents(), presented = now - presents_at_swapchain;
        if (presented < kIdleProofPresents) {
            if (idle_windows == 1)
                logf(KILN_LOG_INFO, "ui: overlay crash guard: only %llu frames presented in %u s since the swapchain; the start is still being watched, %u s at a time",
                     static_cast<unsigned long long>(presented), unsigned(kIdleProofMs / 1000), unsigned(kIdleProofMs / 1000));
            presents_at_swapchain = now;  // the next window
            return;
        }
        stop_timer(idle_timer);
        record = overlay_guard::idle(record);
        idle_done = true;
        save();
        logf(KILN_LOG_INFO, "ui: overlay crash guard: the game presented %llu frames through Kiln's hooks in %u s with nothing drawn yet%s; the start counts as fine, and the first draw is watched when it comes",
             static_cast<unsigned long long>(presented), unsigned(kIdleProofMs / 1000),
             idle_windows > 1 ? " (after slower first frames)" : "");
    } catch (...) { error(); }
}
bool drawn_slow_said = false, failed_slow_said = false;
// The overlay's first drawn frames were presented and the game went on presenting: the start is fine (overlay_guard_drawn).
// Periodic: a window with too few frames (loading) is followed by the next one, until one has them or the game ends.
VOID CALLBACK drawn_proof(PVOID, BOOLEAN) {
    try {
        std::lock_guard<std::mutex> lock(mu);
        // the overlay's drawing went off since: overlay_guard_failed's watch decides
        if (!active || record.phase != overlay_guard::Phase::trying || failed_timer) return stop_timer(drawn_timer);
        const uint64_t now = vk_presents(), presented = now - presents_at_drawn;
        if (presented < kIdleProofPresents) {
            if (!drawn_slow_said) {
                drawn_slow_said = true;
                logf(KILN_LOG_INFO, "ui: overlay crash guard: only %llu frames presented in %u s since the overlay's first frame; the start is still being watched",
                     static_cast<unsigned long long>(presented), unsigned(kDrawnProofMs / 1000));
            }
            presents_at_drawn = now;  // the next window
            return;
        }
        stop_timer(drawn_timer);
        record = overlay_guard::finish(record);
        active = false;
        save();
        logf(KILN_LOG_INFO, "ui: overlay crash guard complete: the game presented %llu frames in the %u s after the overlay's first frame",
             static_cast<unsigned long long>(presented), unsigned(kDrawnProofMs / 1000));
    } catch (...) { error(); }
}
// The overlay's drawing turned off during the session and the game went on presenting: it did not die of it
// (overlay_guard_failed). The failure history stays (overlay_guard::idle): only drawn frames clear it. Periodic, as above.
VOID CALLBACK failed_proof(PVOID, BOOLEAN) {
    try {
        std::lock_guard<std::mutex> lock(mu);
        if (!active || record.phase != overlay_guard::Phase::trying) return stop_timer(failed_timer);
        const uint64_t now = vk_presents(), presented = now - presents_at_failed;
        if (presented < kIdleProofPresents) {
            if (!failed_slow_said) {
                failed_slow_said = true;
                logf(KILN_LOG_INFO, "ui: overlay crash guard: only %llu frames presented in %u s since the overlay's drawing went off; the start is still being watched",
                     static_cast<unsigned long long>(presented), unsigned(kFailedProofMs / 1000));
            }
            presents_at_failed = now;  // the next window
            return;
        }
        stop_timer(failed_timer);
        record = overlay_guard::idle(record);
        active = false;
        save();
        logf(KILN_LOG_INFO, "ui: overlay crash guard: the game went on (%llu frames in %u s) after the overlay's drawing went off; the start counts as fine",
             static_cast<unsigned long long>(presented), unsigned(kFailedProofMs / 1000));
    } catch (...) { error(); }
}
// ---- Kiln's menu on the screen, or the game's own Mods rows instead (overlay_menu_ask) ----
// How long an ask for Kiln's menu waits for a frame of the overlay to be drawn and presented. While the overlay is still
// making a frame then (its first of the process starts ImGui and the menu's code), it is looked at again every
// kMenuBusyMs, up to kMenuBusyMaxMs after the ask.
constexpr DWORD kMenuShowMs = 1'000, kMenuBusyMs = 250, kMenuBusyMaxMs = 5'000;
HANDLE menu_timer = nullptr;                      // an ask being watched
uint64_t menu_asked_at = 0, menu_presents_at = 0;  // drawn_presents and vk_presents() at the ask
uint64_t menu_fell_back_at = 0;                   // drawn_presents when Kiln's menu fell back
DWORD menu_waited = 0;                            // how long the ask has been watched
bool menu_fallback_said = false;
// No frame of the overlay was drawn and presented since the ask: Kiln's menu falls back to the game's own Mods rows, as
// with the overlay's drawing off (game_menu_fell_back), until a frame of the overlay is presented and the menu is asked
// for again (overlay_menu_ask).
VOID CALLBACK menu_proof(PVOID, BOOLEAN) {
    bool first = false;
    uint64_t presents = 0;
    DWORD waited = 0;
    try {
        std::lock_guard<std::mutex> lock(mu);
        stop_timer(menu_timer);
        // shown (a frame of the overlay was presented since the ask), the game exits, or the drawing went off meanwhile (the
        // Mods entry is the drawing-off one then)
        if (exited || !overlay_draws() || drawn_presents.load(std::memory_order_relaxed) != menu_asked_at) return;
        menu_waited += menu_waited ? kMenuBusyMs : kMenuShowMs;
        if (vk_drawing() && menu_waited < kMenuBusyMaxMs &&
            CreateTimerQueueTimer(&menu_timer, nullptr, &menu_proof, nullptr, kMenuBusyMs, 0, WT_EXECUTEONLYONCE))
            return;  // the overlay is making a frame right now: a little longer
        menu_timer = nullptr;
        g_menu_fell_back.store(true);
        menu_fell_back_at = menu_asked_at;
        first = !menu_fallback_said;
        menu_fallback_said = true;
        presents = vk_presents() - menu_presents_at;
        waited = menu_waited;
    } catch (...) {
        return;
    }
    if (first)
        logf(KILN_LOG_WARN, "ui: Kiln's menu was opened, but no frame of Kiln's overlay was drawn and presented in the %lu ms after "
             "(the game presented %llu frames through Kiln's hooks meanwhile): the Mods entry shows the game's own Mods rows instead, "
             "as with the overlay's drawing off, and opens Kiln's menu again once a frame of the overlay was presented",
             static_cast<unsigned long>(waited), static_cast<unsigned long long>(presents));
    game_menu_fell_back();
}

// kiln.json "ui": {"overlay": "off"} (the launcher's Settings, "Kiln"): 0 = drawing off, -1 = Kiln decides
// (overlay_setting.h). Read once at start; a missing or unreadable file, section or key is -1.
int kiln_json_overlay() {
    try {
        const auto dir = api_path(KILN_PATH_KILN);
        if (dir.empty()) return -1;
        std::ifstream in(dir / L"kiln.json", std::ios::binary);
        if (!in) return -1;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return overlay_setting(text);
    } catch (...) {
        return -1;
    }
}

}
bool overlay_guard_start() noexcept {
    try {
        bool sl = false;
        overlay_guard::OptiScaler optiscaler;
        const auto wrapper_set = modules(sl, optiscaler);
        found_optiscaler = optiscaler.found();
        found_streamline = sl;
        // OptiScaler with its own overlay menu (or frame generation) patches the game's presents and swapchains too:
        // Kiln's drawing steps aside on its own, as for Streamline (overlay_optiscaler.h)
        const bool optiscaler_off = optiscaler.found() && !optiscaler.coexists();
        char option[8] = {};
        const DWORD option_size = GetEnvironmentVariableA("KILN_UI_OVERLAY", option, sizeof option);
        const int environment = option_size == 1 && option[0] == '0' ? 0 : option_size == 1 && option[0] == '1' ? 1 : -1;
        // kiln.json ui.overlay "off": the same as KILN_UI_OVERLAY=0, for players who cannot set the environment (Steam's
        // launch options on Windows); KILN_UI_OVERLAY wins over it
        const int setting = kiln_json_overlay();
        auto apply_mode = [&](bool guarded) {
            g_overlay_mode = choose_overlay_mode(false, guarded, sl || optiscaler_off, environment, setting);
            if (!overlay_drawing_on()) {
                g_overlay_reason = "the overlay's drawing is off: ";
                g_overlay_reason += environment == 0 ? "KILN_UI_OVERLAY=0"
                    : environment == -1 && setting == 0 ? "turned off in Kiln's settings (kiln.json ui.overlay)"
                    : guarded ? "an earlier overlay start was interrupted"
                    : sl ? "NVIDIA Streamline is installed"
                    : optiscaler.ini.overlay_menu != 0 ? "OptiScaler's own overlay menu is on" : "OptiScaler's frame generation is on";
            }
        };
        apply_mode(false);
        if (optiscaler.found()) {
            const std::string file = utf8(optiscaler.file.filename());
            if (environment != -1)  // KILN_UI_OVERLAY decides, with or without OptiScaler's menu (its own lines below)
                logf(KILN_LOG_INFO, "ui: OptiScaler is installed (%s) with %s; KILN_UI_OVERLAY=%d decides for Kiln's overlay",
                     file.c_str(), optiscaler_settings(optiscaler).c_str(), environment);
            else if (optiscaler.coexists())  // the crash guard may still keep drawing off for this start (its own line)
                logf(KILN_LOG_INFO, "ui: OptiScaler is installed (%s) with %s: it leaves the game's presents alone, so it does not turn Kiln's overlay drawing off",
                     file.c_str(), optiscaler_settings(optiscaler).c_str());
            else if (optiscaler.ini.overlay_menu != 0)
                logf(KILN_LOG_INFO, "ui: OptiScaler is installed (%s) with %s: it draws its own overlay in the game's presents and opens its menu with Insert, "
                     "so Kiln's overlay drawing steps aside (keys and the Mods menu still work). For Kiln's overlay, set OverlayMenu=false under [Menu] "
                     "in OptiScaler.ini and leave FGInput and FGOutput under [FrameGen] at auto (OptiScaler's menu and frame generation off, its "
                     "upscaling on), or KILN_UI_OVERLAY=1 to try both at your own risk",
                     file.c_str(), optiscaler_settings(optiscaler).c_str());
            else  // its menu off, its frame generation on: its nightly builds since October 2026 run it without the menu
                logf(KILN_LOG_INFO, "ui: OptiScaler is installed (%s) with %s: with frame generation the game's frames are presented from OptiScaler's "
                     "own queue and swapchain, where Kiln cannot draw, so Kiln's overlay drawing steps aside (keys and the Mods menu still work). "
                     "For Kiln's overlay, set FGInput and FGOutput under [FrameGen] in OptiScaler.ini to auto (frame generation off, its upscaling "
                     "stays), or KILN_UI_OVERLAY=1 to try at your own risk",
                     file.c_str(), optiscaler_settings(optiscaler).c_str());
        } else if (optiscaler.ini_only) {
            logf(KILN_LOG_INFO, "ui: OptiScaler.ini is in the game folder, but no OptiScaler DLL next to it; Kiln's overlay is not changed by it");
        }
        // Drawing off by the player's switch, or stepping aside for another layer on the game's presents: one line that
        // names it and says what the Mods entry does then (the game's own Mods rows, never a menu Kiln cannot draw). A
        // start the crash guard keeps off has its own warning below.
        auto say_off = [&](bool guarded) {
            if (overlay_drawing_on() || guarded) return;
            if (environment == 0 || (environment == -1 && setting == 0)) {
                logf(KILN_LOG_INFO, "ui: %s; key bindings and the Mods menu still work; KILN_UI_OVERLAY=1 tries drawing at your own risk", g_overlay_reason.c_str());
                return;
            }
            const std::string layer = sl ? std::string("NVIDIA Streamline (sl.interposer.dll, a frame generation mod)")
                : "OptiScaler (" + utf8(optiscaler.file.filename()) +
                      (optiscaler.ini.overlay_menu != 0 ? ", its own overlay menu on)" : ", its frame generation on)");
            logf(KILN_LOG_INFO, "ui: Kiln's overlay drawing steps aside for %s: it hooks the game's Vulkan presents too. Key bindings "
                 "still work, and the Mods entry shows the game's own Mods rows (\"Mods: %s\" once chosen); KILN_UI_OVERLAY=1 tries "
                 "drawing at your own risk", layer.c_str(), g_overlay_reason.c_str());
        };
        auto dir = api_path(KILN_PATH_STATE);
        if (dir.empty()) {
            error("find the folder of", 0);
            say_off(false);
            return !overlay_drawing_on();
        }
        path = (dir / L"overlay-start.guard").wstring();
        char b[513]; DWORD n = 0;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        bool have_record = false;
        if (h != INVALID_HANDLE_VALUE) {
            const bool ok = ReadFile(h, b, sizeof b, &n, nullptr);
            const DWORD read_error = ok ? 0 : GetLastError();
            CloseHandle(h);
            if (!ok) error("read", read_error);
            else if (!overlay_guard::parse(std::string(b, n), record)) error("understand", 0);
            else have_record = true;
        } else if (const DWORD e = GetLastError(); e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) error("open", e);
        // A record another Kiln wrote (or one from before 0.2.0-dev.29.19, without a version) starts fresh: an update
        // clears an old lock (inbox 97). Written now, as a complete record of this Kiln, so a start that installs no
        // Vulkan hook (the drawing off for another reason) does not read the old record again next time.
        const std::string kiln = kiln_version();
        if (have_record && record.kiln != kiln) {
            const bool held = record.phase != overlay_guard::Phase::complete || record.failures || record.wrapper_retry;
            if (held) {
                const std::string by = record.kiln.empty() ? std::string("by a Kiln that did not note its version")
                                                           : "by Kiln " + record.kiln + ", this is " + kiln;
                logf(KILN_LOG_INFO, "ui: overlay crash guard: Kiln was updated since the record was written (%s); its earlier overlay start failures%s are cleared, and the overlay's drawing is tried again",
                     by.c_str(), record.phase == overlay_guard::Phase::locked ? " and its lock" : "");
            }
            record = overlay_guard::for_kiln(record, kiln, wrapper_set);
            save();
        }
        record.kiln = kiln;  // a record that could not be read, or none: this Kiln's from its first save
        if (!record.modules.empty() && record.modules != wrapper_set)
            logf(KILN_LOG_INFO, record.wrapper_retry
                ? "ui: overlay crash guard: graphics wrappers changed again before an overlay frame succeeded; keeping earlier startup failures"
                : "ui: overlay crash guard: graphics wrappers changed or the guard was updated; allowing one fresh startup attempt");
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
            // Guarded (or locked) on the disk now, in place of the start that died: this start installs no Vulkan hook,
            // so it arms nothing, and the next one reads guarded and tries the overlay again however this one ends
            // (overlay_guard::begin). A normal exit writes retry (overlay_guard_exit).
            save();
            // the next start draws unless something else keeps the drawing off (Streamline, OptiScaler, the switches)
            const bool next_tries = record.phase == overlay_guard::Phase::guarded &&
                choose_overlay_mode(false, false, sl || optiscaler_off, environment, setting) == OverlayMode::on;
            // a death after the swapchain, before the overlay's first draw, counts towards no lock (overlay_guard::begin)
            const bool before_draw =
                record.phase == overlay_guard::Phase::guarded && record.stage == overlay_guard::Stage::swapchain;
            std::string why = before_draw
                ? "the game closed during the last start after its swapchain was made, before Kiln's overlay drew anything (stage: "
                : "the game closed during the last start while Kiln's overlay was starting (stage: ";
            why += overlay_guard::stage_name(record.stage);
            why += next_tries ? "); the overlay's drawing stays off for this start, and the next start tries it again"
                              : "); the overlay's drawing stays off for this start";
            if (before_draw) why += "; a start that ends so never turns the overlay off for good";
            why += "; key bindings and the Mods menu still work";
            if (record.phase == overlay_guard::Phase::locked) why += "; it failed twice at this stage; delete kiln/state/overlay-start.guard to try again";
            if (sl) why += "; NVIDIA Streamline (a frame generation mod) is installed; it wraps the same graphics calls";
            if (optiscaler.found()) why += "; OptiScaler is installed; it patches the same graphics calls";
            why += "; KILN_UI_OVERLAY=1 tries drawing at your own risk";
            logf(KILN_LOG_WARN, "ui: %s", why.c_str());

        }
        if (!overlay_drawing_on())
            say_off(start.off);
        else if (environment == 1 && (sl || optiscaler_off || start.off))
            logf(KILN_LOG_WARN, "ui: KILN_UI_OVERLAY=1: trying overlay drawing despite %s; the crash guard records this attempt",
                 start.off ? "an interrupted start" : sl ? "Streamline" : "OptiScaler's own overlay");
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
            logf(KILN_LOG_INFO, "ui: overlay crash guard complete: %llu frames presented with the overlay drawn",
                 static_cast<unsigned long long>(kDrawnFrames));
        } else if (stage == overlay_guard::Stage::swapchain && !idle_timer) {
            presents_at_swapchain = vk_presents();
            if (!CreateTimerQueueTimer(&idle_timer, nullptr, &idle_proof, nullptr, kIdleProofMs, kIdleProofMs, WT_EXECUTEDEFAULT)) {
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
        if (draw_seen.exchange(true) || !active) return;
        if (record.phase == overlay_guard::Phase::trying && record.stage < overlay_guard::Stage::drawing) {
            // still watched after its swapchain: from now on a death is one at the overlay's first draw (it counts towards
            // the lock), and the overlay's own frames decide (overlay_guard_drawn)
            record.stage = overlay_guard::Stage::drawing;
        } else if (idle_done && record.phase == overlay_guard::Phase::complete) {
            record = overlay_guard::rearm(record);
        } else {
            return;
        }
        save();
        logf(KILN_LOG_INFO, "ui: overlay crash guard: the overlay's first draw; watched until its frames are presented");
    } catch (...) { error(); }
}
bool overlay_menu_ask() noexcept {
    if (!overlay_draws()) return false;  // drawing off from the start (a switch, the guard, Streamline, OptiScaler) or since
    try {
        std::lock_guard<std::mutex> lock(mu);
        const uint64_t drawn = drawn_presents.load(std::memory_order_relaxed);
        if (g_menu_fell_back.load()) {
            if (drawn == menu_fell_back_at) return false;  // still no frame of the overlay since: the game's own rows
            g_menu_fell_back.store(false);
            logf(KILN_LOG_DEBUG, "ui: frames of Kiln's overlay were presented since Kiln's menu fell back to the game's own Mods rows: Kiln's menu is opened in the overlay again");
        }
        if (menu_timer) return true;  // an earlier ask is still watched (for a second or so)
        menu_asked_at = drawn;
        menu_presents_at = vk_presents();
        menu_waited = 0;
        if (!CreateTimerQueueTimer(&menu_timer, nullptr, &menu_proof, nullptr, kMenuShowMs, 0, WT_EXECUTEONLYONCE)) menu_timer = nullptr;
        return true;
    } catch (...) {
        return true;
    }
}
void overlay_guard_drawn() noexcept {
    const uint64_t n = drawn_presents.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n != 1 && n != kDrawnFrames) return;  // every other frame: one atomic add
    if (n == kDrawnFrames) {
        overlay_guard_stage(overlay_guard::Stage::frame);
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(mu);
        if (!active || record.phase != overlay_guard::Phase::trying || drawn_timer) return;
        presents_at_drawn = vk_presents();
        if (!CreateTimerQueueTimer(&drawn_timer, nullptr, &drawn_proof, nullptr, kDrawnProofMs, kDrawnProofMs, WT_EXECUTEDEFAULT)) {
            drawn_timer = nullptr;
            logf(KILN_LOG_WARN, "ui: overlay crash guard: no timer; the start counts as fine after %llu drawn frames",
                 static_cast<unsigned long long>(kDrawnFrames));
            return;
        }
        logf(KILN_LOG_INFO, "ui: overlay crash guard: the overlay's first frame was presented; watched for its first %llu frames or %u s",
             static_cast<unsigned long long>(kDrawnFrames), unsigned(kDrawnProofMs / 1000));
    } catch (...) { error(); }
}
void overlay_guard_failed() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mu);
        if (exited || failed_timer || path.empty()) return;
        if (record.phase == overlay_guard::Phase::complete) {
            // the start had already counted as fine (drawn frames, the idle proof): a death now is one at the overlay
            // again, for the window below
            record = overlay_guard::rearm(record);
            active = true;
            save();
        } else if (!active || record.phase != overlay_guard::Phase::trying) {
            return;  // a guarded or locked record, or no guard for this start
        }
        presents_at_failed = vk_presents();
        if (!CreateTimerQueueTimer(&failed_timer, nullptr, &failed_proof, nullptr, kFailedProofMs, kFailedProofMs, WT_EXECUTEDEFAULT)) {
            failed_timer = nullptr;
            logf(KILN_LOG_WARN, "ui: overlay crash guard: no timer; until a normal exit, a closed game counts as an interrupted overlay start");
            return;
        }
        logf(KILN_LOG_INFO, "ui: overlay crash guard: if the game closes in the next %u s, the next start leaves the overlay's drawing off",
             unsigned(kFailedProofMs / 1000));
    } catch (...) { error(); }
}
void overlay_guard_tools(bool* optiscaler, bool* streamline) noexcept {
    if (optiscaler) *optiscaler = found_optiscaler;
    if (streamline) *streamline = found_streamline;
}
void overlay_guard_exit() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mu);
        const bool was_active = active;
        exited = true;  // from here on nothing writes the record (overlay_guard_failed; the others need `active`)
        active = false;
        const auto phase = record.phase;
        if (!was_active || (phase != overlay_guard::Phase::trying && phase != overlay_guard::Phase::guarded)) return;
        record = overlay_guard::at_exit(record);
        save();
        logf(KILN_LOG_INFO, phase == overlay_guard::Phase::trying
            ? "ui: overlay crash guard: the game exits normally; the overlay's start counts as fine"
            : "ui: overlay crash guard: the game exits normally after a start with the overlay's drawing off; the guard lets the next start try the overlay again");
    } catch (...) { error(); }
}
}

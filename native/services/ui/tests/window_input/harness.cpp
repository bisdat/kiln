// SPDX-License-Identifier: MIT
// ui-window-input-test.exe: kiln.ui's production window procedure (input.cpp, wndproc_chain.h) on a message-only window
// under Wine, no display: subscribe_window_input (kiln_ui.h v5). Run by native/services/ui/tests/test-window-input-wine.sh.
//
// The "game" is this exe's own window procedure; it logs every message it gets. WM_INPUT carries made-up HRAWINPUT
// values that this exe's import of GetRawInputData (patched below; input.cpp is linked into this exe) answers with
// RAWINPUT records of the test's choice. Messages are sent with SendMessageW from the window's own thread, as the
// game's message loop would dispatch them. Checks:
//   - every record of each kind arrives once, unchanged and in the window's order (raw keys, window keys, raw and
//     message mouse buttons, focus loss), and nothing else does (characters, motion, the wheel);
//   - the game's procedure and Kiln's own key handling get exactly what they get without a subscriber, also while Kiln
//     captures input (menu open, the wheel) and for a key a Kiln binding consumes, and those records still arrive;
//   - one focus loss (WM_ACTIVATEAPP 0 and WM_KILLFOCUS) gives one record;
//   - one press counts once whatever the focus does (key_hold.h): a press's own key message or a repeat after a focus
//     loss is no second press, a press queued before the focus's return counts, an older queued repeat does not, a key
//     let go while away is not held once the focus is back (GetAsyncKeyState is the test's);
//   - unsubscribe stops delivery, waits for a call in flight on the window thread, works from inside the callback;
//     a faulting subscriber is removed and the others go on;
//   - the middle and side buttons and the wheel as key presses (mouse_press.h, since 0.2.0-dev.27): a raw record and
//     the message of the same notch or click give ONE press, in either order; a high-resolution wheel's small deltas
//     add up to one press per notch; a chord a key subscriber takes is kept from the game (record and message), one it
//     does not take passes; while Kiln captures input the press comes with captured 1 and the game gets nothing; over a
//     Kiln panel the wheel is the panel's; the left and right buttons are never presses; take_pressed_key stays
//     keyboard only; key_name names them; only subscribe_key_ex(KILN_UI_KEYS_MOUSE) subscribers get mouse presses
//     (keys.cpp, linked here): a subscribe_key subscriber gets none and cannot take one, and with no mouse subscriber
//     the mouse reaches the game untouched.
//   - kiln.ui.input (0.2.0-dev.31.13, input_own.cpp linked here): a press an owning subscriber takes is its own until the
//     release, for a key (raw record and key message, its repeats), the right button (Blueprint's gesture: message only,
//     raw record + message twins, a release record with motion), the middle button and a pad's button: the game gets
//     none of the press, its repeats or its release, and the button's MK_* bit leaves the game's mouse messages while
//     held; the pointer and key subscribers are not asked; focus loss, unsubscribe and a world change (a fake
//     kiln.client) end it; Alt+F4 and a mixed raw record without the GetRawInputData hook (stuck-press guard) let the
//     release reach the game; an owner that passes changes nothing; a faulting owner is removed.
//   - a key press a module of this process sends (SendInput, keybd_event: Kiln's hooks, text_keep.h) while a Kiln text
//     field has the keyboard types nothing into it, nor reaches it as a key; the player's own press of that key right
//     after does, as does input from no device that was not sent from this process, and a press sent while no field had
//     the keyboard.
// Lines: "PASS ...", "FAIL ...". Exit code 0 = no FAIL.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "key_names.h"
#include "services/kiln_client.h"
#include "services/kiln_ui_input.h"
#include "ui.h"
#include "raw_input_source.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "imgui_internal.h"

// ---- what input.cpp needs from ui.cpp ----
namespace kui {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
std::atomic<uint32_t> g_demand{0}, g_interactive{0}, g_render_demand{0}, g_hover_panels{0}, g_footer_demand{0}, g_graphics_demand{0};
std::atomic<bool> g_menu_open{false}, g_text_input{false}, g_want_mouse{false}, g_wheel_active{false}, g_page_capture{false};
std::atomic<bool> g_overlay_failed{false};  // ui.cpp: the overlay turned off during the session (never here)
std::atomic<bool> g_menu_fell_back{false};  // overlay_guard.cpp: Kiln's menu fell back to the game's own rows (never here)
std::atomic<uint32_t> g_chat_completion_demand{0};  // chat_input_probe.cpp: chat Tab completion's overlay demand (never here)
std::atomic<HWND> g_hwnd{nullptr};  // input_poll_window's (drawing off); never called here
thread_local CallbackLifetime* t_callback = nullptr;
int g_errors = 0;
void logf(int, const char* fmt, ...) {
    char b[1024];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    std::printf("KILN: %s\n", b);
}
void set_error(const char* fmt, ...) {
    char b[256];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    std::printf("KILN ERROR: %s\n", b);
    ++g_errors;
}
uint64_t now_ms() { return GetTickCount64(); }
KilnHandle next_handle() {
    static std::atomic<KilnHandle> h{1};
    return h++;
}
// Kiln's own key handling (the menu key, bindings, the wheel): what reaches it, and one key it consumes (with these
// modifiers only, unless g_consume_mods is ~0u). The real key subscribers (keys.cpp: subscribe_key, subscribe_key_ex)
// get it after, as in ui.cpp.
std::vector<std::string> g_kiln_keys;
uint32_t g_consume_vk = 0, g_consume_mods = ~0u;
bool dispatch_key(const KilnUiKey& k, bool mouse) {
    char b[96];
    std::snprintf(b, sizeof b, "vk=%u scan=%x mods=%u down=%u repeat=%u captured=%u", k.vk, k.scan, k.mods, k.down, k.repeat,
                  k.captured);
    g_kiln_keys.push_back(b);
    const bool subscribers = keys_dispatch(k, mouse);
    return subscribers || (k.down && k.vk == g_consume_vk && (g_consume_mods == ~0u || k.mods == g_consume_mods));
}
}  // namespace kui

namespace {

int g_fails = 0, g_checks = 0;
void check(bool ok, const char* what, ...) {
    char b[2048];
    va_list a;
    va_start(a, what);
    std::vsnprintf(b, sizeof b, what, a);
    va_end(a);
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", b);
    ++g_checks;
    if (!ok) ++g_fails;
}

// ---- the game: a window procedure of this exe that logs what it gets ----
HWND g_win = nullptr;
std::vector<std::string> g_game;
bool interesting(UINT m) {
    return m == WM_INPUT || (m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) ||
           m == WM_SETFOCUS || m == WM_KILLFOCUS || m == WM_ACTIVATEAPP;
}
LRESULT CALLBACK game_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (interesting(m)) {
        char b[160];
        std::snprintf(b, sizeof b, "%04x %llx %llx", m, static_cast<unsigned long long>(w), static_cast<unsigned long long>(l));
        if (m == WM_INPUT) {  // as the game does: read the record (the handle differs from run to run, its record not)
            alignas(8) unsigned char data[256];
            UINT size = sizeof data;
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, data, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
                const auto* ri = reinterpret_cast<const RAWINPUT*>(data);
                if (ri->header.dwType == RIM_TYPEKEYBOARD)
                    std::snprintf(b, sizeof b, "00ff keyboard %x %x %x", ri->data.keyboard.MakeCode, ri->data.keyboard.Flags, ri->data.keyboard.VKey);
                else
                    std::snprintf(b, sizeof b, "00ff mouse %x %ld %ld %d", ri->data.mouse.usButtonFlags, ri->data.mouse.lLastX,
                                  ri->data.mouse.lLastY, static_cast<SHORT>(ri->data.mouse.usButtonData));
            }
        }
        g_game.push_back(b);
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- WM_INPUT: made-up HRAWINPUT values 0x1000.. answered by this exe's patched GetRawInputData import ----
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
GetRawInputDataFn g_real_raw = nullptr;
std::vector<RAWINPUT> g_raw;
int g_raw_reads = 0;
constexpr uintptr_t kRawBase = 0x1000;
UINT WINAPI fake_raw(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header) {
    const uintptr_t i = reinterpret_cast<uintptr_t>(h) - kRawBase;
    if (i >= g_raw.size() || cmd != RID_INPUT || header != sizeof(RAWINPUTHEADER) || !size)
        return g_real_raw(h, cmd, data, size, header);
    ++g_raw_reads;
    const RAWINPUT& r = g_raw[i];
    if (!data) {
        *size = r.header.dwSize;
        return 0;
    }
    if (*size < r.header.dwSize) return static_cast<UINT>(-1);
    std::memcpy(data, &r, r.header.dwSize);
    return r.header.dwSize;
}
bool patch_import(const char* function, void* with, void** original) {
    auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image + dir.VirtualAddress); imp->Name; ++imp) {
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(image + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(image + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* named = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(image + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(named->Name), function)) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(with);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

LPARAM raw_key(USHORT make_code, USHORT flags, USHORT vkey) {
    RAWINPUT r{};
    r.header.dwType = RIM_TYPEKEYBOARD;
    r.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD);
    r.data.keyboard.MakeCode = make_code;
    r.data.keyboard.Flags = flags;
    r.data.keyboard.VKey = vkey;
    r.data.keyboard.Message = (flags & RI_KEY_BREAK) ? WM_KEYUP : WM_KEYDOWN;
    g_raw.push_back(r);
    return static_cast<LPARAM>(kRawBase + g_raw.size() - 1);
}
LPARAM raw_mouse(USHORT buttons, LONG dx = 0, LONG dy = 0, USHORT data = 0) {
    RAWINPUT r{};
    r.header.dwType = RIM_TYPEMOUSE;
    r.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE);
    r.data.mouse.usButtonFlags = buttons;
    r.data.mouse.usButtonData = data;
    r.data.mouse.lLastX = dx;
    r.data.mouse.lLastY = dy;
    g_raw.push_back(r);
    return static_cast<LPARAM>(kRawBase + g_raw.size() - 1);
}

// ---- subscribers ----
struct Got {
    std::vector<std::string> lines;
};
std::string text(const KilnUiWindowInput& r) {
    char b[160];
    switch (r.kind) {
        case KILN_UI_INPUT_RAW_KEY:
            std::snprintf(b, sizeof b, "raw %x %x %x", r.make_code, r.flags, r.vkey);
            break;
        case KILN_UI_INPUT_WINDOW_KEY:
            std::snprintf(b, sizeof b, "key %04x %llx %llx", r.message, static_cast<unsigned long long>(r.wparam),
                          static_cast<unsigned long long>(r.lparam));
            break;
        case KILN_UI_INPUT_MOUSE_BUTTON:
            std::snprintf(b, sizeof b, "button %u %u via %04x", r.button, r.down, r.message);
            break;
        case KILN_UI_INPUT_FOCUS_LOST:
            std::snprintf(b, sizeof b, "focus-lost %04x %llx", r.message, static_cast<unsigned long long>(r.wparam));
            break;
        default:
            std::snprintf(b, sizeof b, "kind %u", r.kind);
    }
    if (r.size != sizeof(KilnUiWindowInput)) std::snprintf(b + std::strlen(b), sizeof b - std::strlen(b), " size %u", r.size);
    return b;
}
void KILN_CALL collect(const KilnUiWindowInput* r, void* user) { static_cast<Got*>(user)->lines.push_back(text(*r)); }

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += "\n      " + x;
    return s.empty() ? " (none)" : s;
}

void send(UINT m, WPARAM w, LPARAM l) { SendMessageW(g_win, m, w, l); }

// lParam of a key message: repeat 1, scan code, extended, context (Alt), previous state, transition
LPARAM key_l(UINT scan, bool extended, bool up, bool alt = false) {
    return static_cast<LPARAM>(1u | (scan << 16) | (extended ? 1u << 24 : 0) | (alt ? 1u << 29 : 0) | (up ? 3u << 30 : 0));
}

// The sequence a player's input gives the window, and what a subscriber must get from it.
void play_everything() {
    send(WM_INPUT, RIM_INPUT, raw_key(0x43, RI_KEY_MAKE, VK_F9));
    send(WM_KEYDOWN, VK_F9, key_l(0x43, false, false));
    send(WM_CHAR, 'x', 1);                                             // no record
    send(WM_INPUT, RIM_INPUT, raw_key(0x43, RI_KEY_BREAK, VK_F9));
    send(WM_KEYUP, VK_F9, key_l(0x43, false, true));
    send(WM_SYSKEYDOWN, VK_MENU, key_l(0x38, false, false, true));
    send(WM_SYSKEYUP, VK_MENU, key_l(0x38, false, true, true));
    send(WM_INPUT, RIM_INPUT, raw_key(0x1d, RI_KEY_E0, VK_CONTROL));    // right Ctrl
    send(WM_INPUT, RIM_INPUT, raw_key(0x1d, RI_KEY_E0 | RI_KEY_BREAK, VK_CONTROL));
    send(WM_INPUT, RIM_INPUT, raw_key(0x1d, RI_KEY_E1, VK_PAUSE));      // Pause: the E1 record, then its key
    send(WM_INPUT, RIM_INPUT, raw_key(0x45, RI_KEY_MAKE, 0xff));
    send(WM_INPUT, RIM_INPUT, raw_key(0x1d, RI_KEY_E1 | RI_KEY_BREAK, VK_PAUSE));
    send(WM_INPUT, RIM_INPUT, raw_key(0x45, RI_KEY_BREAK, 0xff));
    send(WM_INPUT, RIM_INPUT, raw_key(0, RI_KEY_MAKE, VK_VOLUME_UP));   // a key without a make code: unchanged
    send(WM_INPUT, RIM_INPUT, raw_key(0, RI_KEY_BREAK, VK_VOLUME_UP));
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_DOWN));
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_BUTTON_5_UP));
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_WHEEL, 0, 0, 120));     // no record
    send(WM_INPUT, RIM_INPUT, raw_mouse(0, 5, -3));                    // motion: no record
    send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20));
    send(WM_LBUTTONUP, 0, MAKELPARAM(10, 20));
    send(WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(10, 20));
    send(WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(1, 2));
    send(WM_RBUTTONUP, 0, MAKELPARAM(1, 2));
    send(WM_MBUTTONDOWN, MK_MBUTTON, 0);
    send(WM_MBUTTONUP, 0, 0);
    send(WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), 0);
    send(WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), 0);
    send(WM_MOUSEMOVE, 0, MAKELPARAM(30, 40));                         // no record
    send(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);                // no record
    // One focus loss: both messages arrive, one record (the first message's).
    send(WM_ACTIVATEAPP, FALSE, 0);
    send(WM_KILLFOCUS, 0, 0);
    send(WM_KILLFOCUS, 0, 0);                                          // still the same loss
    send(WM_SETFOCUS, 0, 0);
    send(WM_KILLFOCUS, 0, 0);                                          // a new loss
    send(WM_ACTIVATEAPP, FALSE, 0);                                    // the same one
    send(WM_ACTIVATEAPP, TRUE, 0);
    send(WM_ACTIVATEAPP, FALSE, 0);                                    // a new loss
    send(WM_INPUT, RIM_INPUT, raw_key(0x1e, RI_KEY_MAKE, 'A'));         // a key while the window had lost the focus...
    send(WM_KILLFOCUS, 0, 0);                                          // ...so this loss clears it again
    send(WM_INPUT, RIM_INPUT, raw_key(0x1e, RI_KEY_BREAK, 'A'));
}
const std::vector<std::string> kEverything = {
    "raw 43 0 78",
    "key 0100 78 430001",
    "raw 43 1 78",
    "key 0101 78 c0430001",
    "key 0104 12 20380001",
    "key 0105 12 e0380001",
    "raw 1d 2 11",
    "raw 1d 3 11",
    "raw 1d 4 13",
    "raw 45 0 ff",
    "raw 1d 5 13",
    "raw 45 1 ff",
    "raw 0 0 af",
    "raw 0 1 af",
    "button 4 1 via 00ff",
    "button 1 1 via 00ff",
    "button 3 0 via 00ff",
    "button 5 0 via 00ff",
    "button 1 1 via 0201",
    "button 1 0 via 0202",
    "button 1 1 via 0203",
    "button 3 1 via 0204",
    "button 3 0 via 0205",
    "button 2 1 via 0207",
    "button 2 0 via 0208",
    "button 4 1 via 020b",
    "button 5 0 via 020c",
    "focus-lost 001c 0",
    "focus-lost 0008 0",
    "focus-lost 001c 0",
    "raw 1e 0 41",
    "focus-lost 0008 0",
    "raw 1e 1 41",
};

struct Run {
    std::vector<std::string> game, kiln, got;
    int raw_reads = 0;
};
template <class F>
Run run(F&& play, Got* got) {
    g_game.clear();
    kui::g_kiln_keys.clear();
    g_raw_reads = 0;
    if (got) got->lines.clear();
    play();
    return Run{g_game, kui::g_kiln_keys, got ? got->lines : std::vector<std::string>{}, g_raw_reads};
}

// The same input with and without a subscriber: the game and Kiln's own key handling see the same; the subscriber gets
// `want`.
void same_for_game(const char* what, void (*play)(), const std::vector<std::string>& want) {
    const Run before = run(play, nullptr);
    Got got;
    KilnHandle h = 0;
    check(kui::input_feed_subscribe(7, &collect, &got, &h) == KILN_OK && h, "%s: subscribed", what);
    const Run with = run(play, &got);
    check(with.got == want, "%s: the subscriber gets every record, unchanged and in order:%s\n    expected:%s", what,
          join(with.got).c_str(), join(want).c_str());
    check(with.game == before.game, "%s: the game's procedure gets the same %zu messages as without a subscriber:%s\n    without:%s",
          what, with.game.size(), join(with.game).c_str(), join(before.game).c_str());
    check(with.kiln == before.kiln, "%s: Kiln's own key handling gets the same %zu keys as without a subscriber", what, with.kiln.size());
    check(with.raw_reads == before.raw_reads, "%s: no extra GetRawInputData calls (%d, %d without)", what, with.raw_reads, before.raw_reads);
    check(kui::input_feed_unsubscribe(h) == KILN_OK, "%s: unsubscribed", what);
    const Run after = run(play, &got);
    check(after.got.empty() && after.game == before.game, "%s: after unsubscribe nothing is delivered (%zu records)", what, after.got.size());
}

bool contains(const std::vector<std::string>& v, const std::string& prefix) {
    for (const auto& x : v)
        if (x.rfind(prefix, 0) == 0) return true;
    return false;
}

void play_captured() {
    send(WM_INPUT, RIM_INPUT, raw_key(0x43, RI_KEY_MAKE, VK_F9));
    send(WM_KEYDOWN, VK_F9, key_l(0x43, false, false));
    send(WM_CHAR, 'x', 1);
    send(WM_INPUT, RIM_INPUT, raw_key(0x43, RI_KEY_BREAK, VK_F9));
    send(WM_KEYUP, VK_F9, key_l(0x43, false, true));
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_LEFT_BUTTON_DOWN));
    send(WM_LBUTTONDOWN, MK_LBUTTON, 0);
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_LEFT_BUTTON_UP));
    send(WM_LBUTTONUP, 0, 0);
}
const std::vector<std::string> kCaptured = {
    "raw 43 0 78", "key 0100 78 430001", "raw 43 1 78", "key 0101 78 c0430001",
    "button 1 1 via 00ff", "button 1 1 via 0201", "button 1 0 via 00ff", "button 1 0 via 0202",
};

void play_consumed() {  // Insert: the key a Kiln binding consumes here (the menu key's default)
    send(WM_INPUT, RIM_INPUT, raw_key(0x52, RI_KEY_E0, VK_INSERT));
    send(WM_KEYDOWN, VK_INSERT, key_l(0x52, true, false));
    send(WM_INPUT, RIM_INPUT, raw_key(0x52, RI_KEY_E0 | RI_KEY_BREAK, VK_INSERT));
    send(WM_KEYUP, VK_INSERT, key_l(0x52, true, true));
}
const std::vector<std::string> kConsumed = {"raw 52 2 2d", "key 0100 2d 1520001", "raw 52 3 2d", "key 0101 2d c1520001"};

void play_edge_keys() {
    send(WM_INPUT, RIM_INPUT, raw_key(0x2a, RI_KEY_E0, VK_SHIFT)); // Print Screen's fake extended Shift
    send(WM_INPUT, RIM_INPUT, raw_key(0x37, RI_KEY_E0, VK_SNAPSHOT));
    send(WM_INPUT, RIM_INPUT, raw_key(0x37, RI_KEY_E0 | RI_KEY_BREAK, VK_SNAPSHOT));
    send(WM_KEYUP, VK_SNAPSHOT, key_l(0x37, true, true));
    send(WM_INPUT, RIM_INPUT, raw_key(0x2a, RI_KEY_E0 | RI_KEY_BREAK, VK_SHIFT));
    send(WM_INPUT, RIM_INPUT, raw_key(0x38, RI_KEY_E0, VK_MENU)); // right Alt
    send(WM_INPUT, RIM_INPUT, raw_key(0x38, RI_KEY_E0 | RI_KEY_BREAK, VK_MENU));
    send(WM_INPUT, RIM_INPUT, raw_key(0x48, 0, VK_NUMPAD8));
    send(WM_INPUT, RIM_INPUT, raw_key(0x48, 0, VK_NUMPAD8)); // repeat is still a raw record
    send(WM_INPUT, RIM_INPUT, raw_key(0x48, RI_KEY_BREAK, VK_NUMPAD8));
    send(WM_INPUT, RIM_INPUT, raw_key(0x48, RI_KEY_E0, VK_UP)); // same make code, extended navigation key
    send(WM_INPUT, RIM_INPUT, raw_key(0x48, RI_KEY_E0 | RI_KEY_BREAK, VK_UP));
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP));
    send(WM_XBUTTONDBLCLK, MAKEWPARAM(MK_XBUTTON2, XBUTTON2), 0);
}
const std::vector<std::string> kEdgeKeys = {
    "raw 2a 2 10", "raw 37 2 2c", "raw 37 3 2c", "key 0101 2c c1370001", "raw 2a 3 10",
    "raw 38 2 12", "raw 38 3 12", "raw 48 0 68", "raw 48 0 68", "raw 48 1 68",
    "raw 48 2 26", "raw 48 3 26", "button 4 1 via 00ff", "button 4 0 via 00ff", "button 5 1 via 020d",
};

void raw_boundaries() {
    RAWINPUT hid{};
    hid.header.dwType = RIM_TYPEHID;
    hid.header.dwSize = 512; // only the size query succeeds; both readers' buffers are too small
    g_raw.push_back(hid);
    const LPARAM handle = static_cast<LPARAM>(kRawBase + g_raw.size() - 1);
    Got got;
    KilnHandle h = 0;
    kui::input_feed_subscribe(1, &collect, &got, &h);
    g_game.clear();
    g_raw_reads = 0;
    send(WM_INPUT, RIM_INPUT, handle);
    check(got.lines.empty() && g_game.size() == 1 && g_raw_reads == 2,
          "oversized HID record is not decoded, still reaches the game, and causes no oversized read");
    kui::input_feed_unsubscribe(h);

    const HWND other = CreateWindowExW(0, L"KilnWindowInputGame", L"other raw target", 0, 0, 0, 0, 0,
                                       HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    const RAWINPUTDEVICE target{1, 6, 0, other};
    check(other && RegisterRawInputDevices(&target, 1, sizeof target), "raw keyboard targets another message-only window");
    check(kui::raw_input::registered_for(6, other) && !kui::raw_input::registered_for(6, g_win),
          "source selection distinguishes the raw target from a replacement window");
    DestroyWindow(other);
    check(!kui::raw_input::registered_for(6, g_win), "destroyed raw target does not disable the replacement's legacy input");
    const RAWINPUTDEVICE restore{1, 6, 0, nullptr};
    check(RegisterRawInputDevices(&restore, 1, sizeof restore), "raw keyboard registration restored");
}

// The keys physically down, as GetAsyncKeyState tells (this exe's import of it answers from here; without a display
// Wine's says no key is down).
bool g_physical[256] = {};
bool g_async_patched = false;
SHORT WINAPI fake_async_key_state(int vk) { return vk > 0 && vk < 256 && g_physical[vk] ? static_cast<SHORT>(0x8000) : 0; }

size_t new_presses(uint32_t vk) {
    char want[24];
    std::snprintf(want, sizeof want, "vk=%u ", vk);
    size_t n = 0;
    for (const auto& x : kui::g_kiln_keys)
        if (x.rfind(want, 0) == 0 && x.find("down=1 repeat=0") != std::string::npos) ++n;
    return n;
}

// Posted, then delivered by the message loop below, one at a time (pump_one) or all (pump): as input that waits in the
// queue while the window's thread is busy. The focus messages are sent: Windows delivers them ahead of the queued input.
void post(UINT m, WPARAM w, LPARAM l) { PostMessageW(g_win, m, w, l); }
bool pump_one() {
    MSG msg;
    if (!PeekMessageW(&msg, g_win, 0, 0, PM_REMOVE)) return false;
    DispatchMessageW(&msg);
    return true;
}
void pump() {
    while (pump_one()) {
    }
}
LPARAM repeat_l(UINT scan) { return key_l(scan, false, false) | (1ll << 30); }  // a down whose key was down before
// Kiln's last line for vk says this ("repeat=1"...).
bool last_of(uint32_t vk, const char* want) {
    char key[24];
    std::snprintf(key, sizeof key, "vk=%u ", vk);
    for (auto it = kui::g_kiln_keys.rbegin(); it != kui::g_kiln_keys.rend(); ++it)
        if (it->rfind(key, 0) == 0) return it->find(want) != std::string::npos;
    return false;
}
void raw_registered(bool on) {
    const RAWINPUTDEVICE d{1, 6, static_cast<DWORD>(on ? 0 : RIDEV_REMOVE), nullptr};
    check(RegisterRawInputDevices(&d, 1, sizeof d), on ? "raw keyboard registered again" : "raw keyboard unregistered");
}
// input.cpp's repeat window, from the same system setting (key_hold.h window_for_delay; key_hold_test checks it)
uint32_t g_window = 1500;
uint32_t window_for_delay(UINT setting) { return std::max<uint32_t>(1500, (std::min<UINT>(setting, 3) + 1) * 250 + 750); }

// One press counts once, whatever the focus does (GitHub #39, Nexus 179900169: one press of F8 opened Kiln Auto Loot's
// window and shut it 40 ms later). key_hold.h: a down is a repeat when no release came since the key's last down,
// handled within the repeat window; the focus's return only says which keys are still held.
void held_focus_lost() {
    UINT delay = 1;
    SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0);
    g_window = window_for_delay(delay);
    check(g_async_patched, "this exe's GetAsyncKeyState import answers from the test (input_key_held at the focus's return)");
    kui::g_kiln_keys.clear();
    g_physical['J'] = true;
    send(WM_INPUT, RIM_INPUT, raw_key(0x24, RI_KEY_MAKE, 'J'));
    check(new_presses('J') == 1, "J goes down: a press");
    send(WM_KILLFOCUS, 0, 0);
    uint32_t mods = 0;
    check(kui::input_take_pressed(&mods) == 0, "focus loss discards a pending bind-capture press");
    g_physical['J'] = false;  // let go while the game had no focus: the release went to another window
    send(WM_SETFOCUS, 0, 0);
    check(!kui::input_key_held('J'), "a key let go while the game had no focus is not held once the focus is back");
    Sleep(g_window + 150);
    g_physical['J'] = true;
    post(WM_INPUT, RIM_INPUT, raw_key(0x24, RI_KEY_MAKE, 'J'));
    pump_one();
    check(new_presses('J') == 2 && last_of('J', "repeat=0"), "pressed again past the repeat window: a press (%zu)", new_presses('J'));

    // held all through a focus loss: its next down is a repeat
    send(WM_ACTIVATEAPP, FALSE, 0);
    check(kui::input_key_held('J'), "a key still down as the game loses the focus stays held");
    send(WM_ACTIVATEAPP, TRUE, 0);
    check(kui::input_key_held('J'), "and still held when the focus is back with it down");
    Sleep(30);
    post(WM_INPUT, RIM_INPUT, raw_key(0x24, RI_KEY_MAKE, 'J'));
    pump_one();
    check(new_presses('J') == 2 && last_of('J', "repeat=1"), "held through a focus loss: its next raw record is a repeat (%zu)",
          new_presses('J'));
    g_physical['J'] = false;
    post(WM_INPUT, RIM_INPUT, raw_key(0x24, RI_KEY_BREAK, 'J'));
    pump_one();
    check(!kui::input_key_held('J'), "its release ends it");

    // 18b: one press, its raw record, a focus loss, raw keyboard input gone for the window (key messages take over),
    // then the press's own key message 40 ms later: one press
    kui::g_kiln_keys.clear();
    g_physical[VK_F8] = true;
    send(WM_INPUT, RIM_INPUT, raw_key(0x42, RI_KEY_MAKE, VK_F8));
    check(new_presses(VK_F8) == 1, "F8's raw record: a press");
    Sleep(40);
    send(WM_KILLFOCUS, 0, 0);
    raw_registered(false);
    send(WM_KEYDOWN, VK_F8, key_l(0x42, false, false));  // the same press's message: the key was up before it
    check(new_presses(VK_F8) == 1 && last_of(VK_F8, "repeat=1"),
          "one press of F8, its raw record, a focus loss, then its key message 40 ms later: one press (%zu)%s",
          new_presses(VK_F8), join(kui::g_kiln_keys).c_str());
    send(WM_SETFOCUS, 0, 0);
    send(WM_KEYDOWN, VK_F8, repeat_l(0x42));
    check(new_presses(VK_F8) == 1, "and its repeat after the focus is back is no second press (%zu)", new_presses(VK_F8));
    g_physical[VK_F8] = false;
    send(WM_KEYUP, VK_F8, key_l(0x42, false, true));

    // key messages only: the previous-state bit marks a repeat, also of a key whose first down Kiln never saw
    kui::g_kiln_keys.clear();
    send(WM_KEYDOWN, 'L', repeat_l(0x26));
    check(new_presses('L') == 0, "a key message with the previous-state bit set is a repeat (%s)", join(kui::g_kiln_keys).c_str());
    send(WM_KEYUP, 'L', key_l(0x26, false, true));
    send(WM_KEYDOWN, 'L', key_l(0x26, false, false));
    check(new_presses('L') == 1, "after its release the next down is a press (%zu)", new_presses('L'));
    send(WM_KEYUP, 'L', key_l(0x26, false, true));
    raw_registered(true);
    std::memset(g_physical, 0, sizeof g_physical);
    send(WM_KILLFOCUS, 0, 0);
    send(WM_SETFOCUS, 0, 0);
}

// Input queued while the window's thread had not run yet, around the focus's return: neither a swallowed nor an invented
// press (the first review of dev29/uiinput: a GetAsyncKeyState snapshot at the return did both).
void queued_focus_return() {
    std::memset(g_physical, 0, sizeof g_physical);
    // A: F7 pressed after the focus came back, before the window's thread got to it: the press counts
    kui::g_kiln_keys.clear();
    send(WM_KILLFOCUS, 0, 0);
    post(WM_INPUT, RIM_INPUT, raw_key(0x41, RI_KEY_MAKE, VK_F7));
    post(WM_KEYDOWN, VK_F7, key_l(0x41, false, false));
    g_physical[VK_F7] = true;
    Sleep(30);
    send(WM_SETFOCUS, 0, 0);
    pump_one();
    check(new_presses(VK_F7) == 1, "A: a raw press queued before the focus's return was handled counts (%zu)%s",
          new_presses(VK_F7), join(kui::g_kiln_keys).c_str());
    pump_one();
    check(new_presses(VK_F7) == 1, "A: its key message is no second press (%zu)", new_presses(VK_F7));
    Sleep(30);
    post(WM_INPUT, RIM_INPUT, raw_key(0x41, RI_KEY_MAKE, VK_F7));
    post(WM_KEYDOWN, VK_F7, repeat_l(0x41));
    pump_one();
    check(new_presses(VK_F7) == 1 && last_of(VK_F7, "repeat=1"), "A: its repeat after that is a repeat (%zu)", new_presses(VK_F7));
    pump();
    g_physical[VK_F7] = false;
    post(WM_INPUT, RIM_INPUT, raw_key(0x41, RI_KEY_BREAK, VK_F7));
    post(WM_KEYUP, VK_F7, key_l(0x41, false, true));
    pump();
    check(!kui::input_key_held(VK_F7), "A: released, not held");

    // A with key messages only (no raw keyboard input)
    raw_registered(false);
    kui::g_kiln_keys.clear();
    send(WM_KILLFOCUS, 0, 0);
    post(WM_KEYDOWN, VK_F6, key_l(0x40, false, false));
    g_physical[VK_F6] = true;
    Sleep(30);
    send(WM_SETFOCUS, 0, 0);
    pump_one();
    check(new_presses(VK_F6) == 1, "A, key messages only: a press queued before the focus's return was handled counts (%zu)%s",
          new_presses(VK_F6), join(kui::g_kiln_keys).c_str());
    g_physical[VK_F6] = false;
    post(WM_KEYUP, VK_F6, key_l(0x40, false, true));
    pump();
    check(!kui::input_key_held(VK_F6), "A, key messages only: released, not held");
    raw_registered(true);

    // B: J held through the focus's return; its repeat and its release queued before the return was handled (which sees
    // J up): the old repeat is no new press, then its release, then a new press counts
    kui::g_kiln_keys.clear();
    g_physical['J'] = true;
    post(WM_INPUT, RIM_INPUT, raw_key(0x24, RI_KEY_MAKE, 'J'));
    post(WM_KEYDOWN, 'J', key_l(0x24, false, false));
    pump();
    check(new_presses('J') == 1, "B: J goes down: a press");
    send(WM_KILLFOCUS, 0, 0);
    post(WM_INPUT, RIM_INPUT, raw_key(0x24, RI_KEY_MAKE, 'J'));
    post(WM_KEYDOWN, 'J', repeat_l(0x24));
    post(WM_INPUT, RIM_INPUT, raw_key(0x24, RI_KEY_BREAK, 'J'));
    post(WM_KEYUP, 'J', key_l(0x24, false, true));
    g_physical['J'] = false;
    Sleep(30);
    send(WM_SETFOCUS, 0, 0);
    pump_one();
    check(new_presses('J') == 1 && last_of('J', "repeat=1"), "B: the queued older raw repeat is a repeat (%zu)%s", new_presses('J'),
          join(kui::g_kiln_keys).c_str());
    pump_one();
    check(new_presses('J') == 1, "B: its key message too (%zu)", new_presses('J'));
    pump_one();
    check(!kui::input_key_held('J'), "B: the queued release ends the hold");
    pump();
    Sleep(30);
    g_physical['J'] = true;
    post(WM_INPUT, RIM_INPUT, raw_key(0x24, RI_KEY_MAKE, 'J'));
    pump_one();
    check(new_presses('J') == 2 && last_of('J', "repeat=0"), "B: the next press after that counts (%zu)", new_presses('J'));
    g_physical['J'] = false;
    post(WM_INPUT, RIM_INPUT, raw_key(0x24, RI_KEY_BREAK, 'J'));
    pump();
    std::memset(g_physical, 0, sizeof g_physical);
    send(WM_KILLFOCUS, 0, 0);
    send(WM_SETFOCUS, 0, 0);
}

// The second review of dev29/uiinput: message times at the same tick as the return and stale ones of sent messages, a
// raw repeat handled ahead of the press's first key message, a raw record whose key message never came; plus fast taps,
// a long hold and the rule's two known misses (printed as NOTE, not checked).
void press_rule_cases() {
    std::memset(g_physical, 0, sizeof g_physical);
    // 1a: a press in the same tick as the focus's return, then its repeat: one press
    kui::g_kiln_keys.clear();
    send(WM_KILLFOCUS, 0, 0);
    send(WM_SETFOCUS, 0, 0);
    g_physical['K'] = true;
    post(WM_INPUT, RIM_INPUT, raw_key(0x25, RI_KEY_MAKE, 'K'));  // posted at once: the return's tick
    pump_one();
    check(new_presses('K') == 1, "a press at the focus's return's tick: a press (%zu)", new_presses('K'));
    Sleep(30);
    post(WM_INPUT, RIM_INPUT, raw_key(0x25, RI_KEY_MAKE, 'K'));
    pump_one();
    check(new_presses('K') == 1 && last_of('K', "repeat=1"), "its repeat 30 ms later is a repeat (%zu)", new_presses('K'));
    send(WM_INPUT, RIM_INPUT, raw_key(0x25, RI_KEY_MAKE, 'K'));  // sent: the message time is the last queued one's
    check(new_presses('K') == 1 && last_of('K', "repeat=1"), "a sent repeat (a stale message time) is a repeat (%zu)", new_presses('K'));
    // 1b: the focus back with K down, then K's release: not held, and a later focus loss does not make it held again
    send(WM_KILLFOCUS, 0, 0);
    send(WM_SETFOCUS, 0, 0);
    check(kui::input_key_held('K'), "the focus back with K down: K held");
    g_physical['K'] = false;
    send(WM_INPUT, RIM_INPUT, raw_key(0x25, RI_KEY_BREAK, 'K'));
    check(!kui::input_key_held('K'), "K's release: not held");
    send(WM_KILLFOCUS, 0, 0);
    check(!kui::input_key_held('K'), "a focus loss after it: still not held");
    send(WM_SETFOCUS, 0, 0);
    check(!kui::input_key_held('K'), "and the focus back: still not held");
    g_physical['K'] = true;
    send(WM_INPUT, RIM_INPUT, raw_key(0x25, RI_KEY_MAKE, 'K'));
    check(new_presses('K') == 2, "K's next down after its release: a press (%zu)", new_presses('K'));
    g_physical['K'] = false;
    send(WM_INPUT, RIM_INPUT, raw_key(0x25, RI_KEY_BREAK, 'K'));

    // 2: a raw repeat handled ahead of the press's first key message, raw keyboard input gone before that message
    kui::g_kiln_keys.clear();
    post(WM_INPUT, RIM_INPUT, raw_key(0x32, RI_KEY_MAKE, 'M'));
    post(WM_INPUT, RIM_INPUT, raw_key(0x32, RI_KEY_MAKE, 'M'));
    post(WM_KEYDOWN, 'M', key_l(0x32, false, false));
    post(WM_KEYDOWN, 'M', repeat_l(0x32));
    pump_one();
    check(new_presses('M') == 1, "M's raw record: a press (%zu)", new_presses('M'));
    pump_one();
    check(new_presses('M') == 1 && last_of('M', "repeat=1"), "M's raw repeat queued ahead of the key messages: a repeat (%zu)",
          new_presses('M'));
    raw_registered(false);
    pump_one();
    check(new_presses('M') == 1 && last_of('M', "repeat=1"),
          "the press's first key message after it (previous state up, key messages now): no second press (%zu)%s",
          new_presses('M'), join(kui::g_kiln_keys).c_str());
    pump_one();
    check(new_presses('M') == 1, "the repeat's key message: no press either (%zu)", new_presses('M'));
    send(WM_KEYUP, 'M', key_l(0x32, false, true));
    raw_registered(true);
    // the same with the review's times: raw down, a raw repeat 250 ms later, then the first key message
    kui::g_kiln_keys.clear();
    send(WM_INPUT, RIM_INPUT, raw_key(0x32, RI_KEY_MAKE, 'M'));
    Sleep(250);
    send(WM_INPUT, RIM_INPUT, raw_key(0x32, RI_KEY_MAKE, 'M'));
    raw_registered(false);
    send(WM_KEYDOWN, 'M', key_l(0x32, false, false));
    check(new_presses('M') == 1, "raw down, raw repeat 250 ms later, then the first key message: one press (%zu)%s",
          new_presses('M'), join(kui::g_kiln_keys).c_str());
    send(WM_KEYUP, 'M', key_l(0x32, false, true));
    raw_registered(true);

    // 3: a raw record whose key message never came, a focus loss, the release elsewhere, the focus back with raw
    // keyboard input gone, then a new key message past the repeat window: a press
    kui::g_kiln_keys.clear();
    g_physical['O'] = true;
    send(WM_INPUT, RIM_INPUT, raw_key(0x18, RI_KEY_MAKE, 'O'));
    check(new_presses('O') == 1, "O's raw record (no key message follows): a press");
    send(WM_KILLFOCUS, 0, 0);
    g_physical['O'] = false;
    send(WM_SETFOCUS, 0, 0);
    check(!kui::input_key_held('O'), "O let go while away: not held once the focus is back");
    raw_registered(false);
    Sleep(g_window + 150);
    g_physical['O'] = true;
    send(WM_KEYDOWN, 'O', key_l(0x18, false, false));
    check(new_presses('O') == 2, "a new key message for O past the repeat window: a press (%zu)%s", new_presses('O'),
          join(kui::g_kiln_keys).c_str());
    g_physical['O'] = false;
    send(WM_KEYUP, 'O', key_l(0x18, false, true));
    check(!kui::input_key_held('O'), "its release: not held");
    // the known miss: the same, 100 ms after the raw record
    kui::g_kiln_keys.clear();
    send(WM_KEYDOWN, 'P', key_l(0x19, false, false));
    send(WM_KILLFOCUS, 0, 0);
    send(WM_SETFOCUS, 0, 0);
    Sleep(100);
    send(WM_KEYDOWN, 'P', key_l(0x19, false, false));
    std::printf("NOTE known miss: a key let go while away (release not seen) and pressed again 100 ms later counts %s\n",
                new_presses('P') == 2 ? "a press" : "a repeat");
    send(WM_KEYUP, 'P', key_l(0x19, false, true));
    raw_registered(true);

    // fast taps with their releases seen: two presses
    kui::g_kiln_keys.clear();
    send(WM_INPUT, RIM_INPUT, raw_key(0x10, RI_KEY_MAKE, 'Q'));
    send(WM_INPUT, RIM_INPUT, raw_key(0x10, RI_KEY_BREAK, 'Q'));
    check(new_presses('Q') == 1 && !kui::input_key_held('Q'), "a tap of Q: one press, released (%zu)", new_presses('Q'));
    Sleep(40);
    send(WM_INPUT, RIM_INPUT, raw_key(0x10, RI_KEY_MAKE, 'Q'));
    check(new_presses('Q') == 2, "a second tap 40 ms later: a second press (%zu)", new_presses('Q'));
    send(WM_INPUT, RIM_INPUT, raw_key(0x10, RI_KEY_BREAK, 'Q'));

    // a long hold: the first repeat after a 600 ms repeat delay, then fast repeats: one press
    kui::g_kiln_keys.clear();
    send(WM_INPUT, RIM_INPUT, raw_key(0x13, RI_KEY_MAKE, 'R'));
    Sleep(600);
    send(WM_INPUT, RIM_INPUT, raw_key(0x13, RI_KEY_MAKE, 'R'));
    check(new_presses('R') == 1 && last_of('R', "repeat=1"), "a hold: the first repeat 600 ms later is a repeat (%zu)", new_presses('R'));
    Sleep(40);
    send(WM_INPUT, RIM_INPUT, raw_key(0x13, RI_KEY_MAKE, 'R'));
    check(new_presses('R') == 1, "and the next 40 ms later (%zu)", new_presses('R'));
    send(WM_INPUT, RIM_INPUT, raw_key(0x13, RI_KEY_BREAK, 'R'));
    check(!kui::input_key_held('R'), "released, not held");

    // the other known miss: a key held through a focus loss longer than the repeat window
    kui::g_kiln_keys.clear();
    g_physical['T'] = true;
    send(WM_INPUT, RIM_INPUT, raw_key(0x14, RI_KEY_MAKE, 'T'));
    send(WM_KILLFOCUS, 0, 0);
    Sleep(g_window + 150);
    send(WM_SETFOCUS, 0, 0);
    post(WM_INPUT, RIM_INPUT, raw_key(0x14, RI_KEY_MAKE, 'T'));
    pump_one();
    std::printf("NOTE known miss: a key held through a focus loss longer than the %u ms window: its next repeat counts %s\n",
                g_window, new_presses('T') == 2 ? "a press" : "a repeat");
    g_physical['T'] = false;
    send(WM_INPUT, RIM_INPUT, raw_key(0x14, RI_KEY_BREAK, 'T'));
    std::memset(g_physical, 0, sizeof g_physical);
    send(WM_KILLFOCUS, 0, 0);
    send(WM_SETFOCUS, 0, 0);
}

void raw_unregistered() {
    kui::g_consume_vk = 'J';
    const RAWINPUTDEVICE remove{1, 6, RIDEV_REMOVE, nullptr};
    check(RegisterRawInputDevices(&remove, 1, sizeof remove), "raw keyboard unregistered");
    kui::g_kiln_keys.clear();
    g_game.clear();
    send(WM_KEYDOWN, 'J', key_l(0x24, false, false));
    check(kui::input_key_held('J') && !contains(g_game, "0100 "),
          "legacy key after raw stops reaches Kiln's binding and is consumed");
    send(WM_KEYUP, 'J', key_l(0x24, false, true));
    check(!kui::input_key_held('J') && kui::g_kiln_keys.size() == 2,
          "legacy release after raw stops clears Kiln's held key (%zu callbacks)", kui::g_kiln_keys.size());
    const RAWINPUTDEVICE add{1, 6, 0, nullptr};
    check(RegisterRawInputDevices(&add, 1, sizeof add), "raw keyboard registered again");
    kui::g_consume_vk = 0;
}

// ---- lifetime ----
struct Blocking {
    HANDLE inside, release;
    std::atomic<int> calls{0};
};
void KILN_CALL blocking(const KilnUiWindowInput*, void* user) {
    auto* b = static_cast<Blocking*>(user);
    if (b->calls.fetch_add(1) == 0) {
        SetEvent(b->inside);
        WaitForSingleObject(b->release, 10000);
    }
}
struct Self {
    KilnHandle handle = 0;
    int calls = 0;
    KilnResult result = -1;
};
void KILN_CALL self_unsubscribe(const KilnUiWindowInput*, void* user) {
    auto* s = static_cast<Self*>(user);
    ++s->calls;
    s->result = kui::input_feed_unsubscribe(s->handle);
}
void KILN_CALL faulting(const KilnUiWindowInput*, void* user) {
    ++*static_cast<int*>(user);
    RaiseException(0xC0000005, 0, 0, nullptr);
}

struct Nested {
    KilnHandle outer_handle = 0, inner_handle = 0;
    bool inside = false, finished = false;
    int calls = 0;
    KilnResult result = -1;
};
void KILN_CALL outer_callback(const KilnUiWindowInput*, void* user) {
    auto* n = static_cast<Nested*>(user);
    ++n->calls;
    if (n->inside || n->finished) return;
    n->inside = true;
    send(WM_KEYDOWN, 'N', key_l(0x31, false, false));
    n->inside = false;
    n->finished = true;
}
void KILN_CALL inner_callback(const KilnUiWindowInput*, void* user) {
    auto* n = static_cast<Nested*>(user);
    if (n->inside && n->result == -1) n->result = kui::input_feed_unsubscribe(n->outer_handle);
}
void nested_unsubscribe() {
    Nested n;
    kui::input_feed_subscribe(1, &outer_callback, &n, &n.outer_handle);
    kui::input_feed_subscribe(2, &inner_callback, &n, &n.inner_handle);
    send(WM_KEYDOWN, 'M', key_l(0x32, false, false));
    check(n.finished && n.result == KILN_ERR_WRONG_THREAD,
          "a nested callback cannot drain an outer callback still on this thread's stack (result %d)", n.result);
    check(kui::input_feed_unsubscribe(n.outer_handle) == KILN_OK,
          "the canceled outer callback can be drained after it returns");
    send(WM_KEYUP, 'M', key_l(0x32, false, true));
    check(n.calls == 2, "the canceled outer callback gets no more records (%d calls)", n.calls);
    kui::input_feed_unsubscribe(n.inner_handle);
}

void KILN_CALL recursive_self(const KilnUiWindowInput*, void* user) {
    auto* n = static_cast<Nested*>(user);
    if (!n->inside) {
        n->inside = true;
        send(WM_KEYDOWN, 'P', key_l(0x19, false, false));
        n->inside = false;
    } else {
        n->result = kui::input_feed_unsubscribe(n->outer_handle);
    }
}
void recursive_unsubscribe() {
    Nested n;
    kui::input_feed_subscribe(1, &recursive_self, &n, &n.outer_handle);
    send(WM_KEYDOWN, 'O', key_l(0x18, false, false));
    check(n.result == KILN_ERR_WRONG_THREAD,
          "self-unsubscribe cannot drain an earlier invocation of the same callback (result %d)", n.result);
    check(kui::input_feed_unsubscribe(n.outer_handle) == KILN_OK, "recursive self-unsubscribe can be retried after return");
}

struct Mutation {
    KilnHandle victim = 0, added = 0;
    Got got;
    int calls = 0;
    KilnResult canceled = -1;
};
void KILN_CALL mutate(const KilnUiWindowInput*, void* user) {
    auto* m = static_cast<Mutation*>(user);
    if (m->calls++ != 0) return;
    kui::input_feed_subscribe(3, &collect, &m->got, &m->added);
    m->canceled = kui::input_feed_unsubscribe(m->victim);
}
void delivery_mutation() {
    Mutation m;
    Got victim;
    KilnHandle first = 0;
    kui::input_feed_subscribe(1, &mutate, &m, &first);
    kui::input_feed_subscribe(2, &collect, &victim, &m.victim);
    send(WM_KEYDOWN, 'Q', key_l(0x10, false, false));
    check(m.canceled == KILN_OK && victim.lines.empty(), "a callback cancels another queued callback without waiting");
    check(m.added && m.got.lines.empty(), "a subscriber added during delivery does not receive that record");
    send(WM_KEYUP, 'Q', key_l(0x10, false, true));
    check(m.got.lines.size() == 1 && victim.lines.empty(), "the new subscriber receives the next record, not the canceled one");
    kui::input_feed_unsubscribe(first);
    kui::input_feed_unsubscribe(m.added);

    Blocking b;
    b.inside = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    b.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    KilnHandle blocked = 0;
    kui::input_feed_subscribe(1, &blocking, &b, &blocked);
    KilnResult result = -1;
    std::thread other([&] {
        if (WaitForSingleObject(b.inside, 10000) == WAIT_OBJECT_0) {
            kui::CallbackLifetime caller;
            kui::CallbackLifetime::Invocation invocation(&caller);
            kui::t_callback = &caller;
            result = kui::input_feed_unsubscribe(blocked);
            kui::t_callback = nullptr;
        }
        SetEvent(b.release);
    });
    send(WM_KEYDOWN, 'R', key_l(0x13, false, false));
    other.join();
    check(result == KILN_ERR_WRONG_THREAD, "another thread's callback cancels without waiting for an active callback (%d)", result);
    check(kui::input_feed_unsubscribe(blocked) == KILN_OK, "a canceled cross-thread callback drains on retry outside callbacks");
    CloseHandle(b.inside);
    CloseHandle(b.release);
}

void lifetime() {
    // A call in flight on the window thread: unsubscribe from another thread waits for it, and nothing comes after.
    Blocking b;
    b.inside = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    b.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    KilnHandle h = 0;
    kui::input_feed_subscribe(1, &blocking, &b, &h);
    std::atomic<bool> returned{false};
    KilnResult result = -1;
    bool waited = false;
    std::thread other([&] {
        if (WaitForSingleObject(b.inside, 10000) != WAIT_OBJECT_0) return;
        std::thread unsub([&] {
            result = kui::input_feed_unsubscribe(h);
            returned = true;
        });
        Sleep(300);
        waited = !returned.load();
        SetEvent(b.release);
        unsub.join();
    });
    send(WM_INPUT, RIM_INPUT, raw_key(0x43, RI_KEY_MAKE, VK_F9));  // the window thread: blocks inside the callback
    other.join();
    check(waited, "unsubscribe from another thread waits while the callback runs on the window thread");
    check(returned && result == KILN_OK, "...and returns KILN_OK once it ended (%d)", result);
    const int calls = b.calls.load();
    send(WM_INPUT, RIM_INPUT, raw_key(0x43, RI_KEY_BREAK, VK_F9));
    send(WM_KEYUP, VK_F9, key_l(0x43, false, true));
    send(WM_KILLFOCUS, 0, 0);
    check(calls == 1 && b.calls.load() == 1, "no call after unsubscribe returned (%d calls)", b.calls.load());
    check(kui::input_feed_unsubscribe(h) == KILN_ERR_NOT_FOUND, "a second unsubscribe: not found");

    // From inside its own callback.
    Self s;
    kui::input_feed_subscribe(1, &self_unsubscribe, &s, &s.handle);
    send(WM_KEYDOWN, 'B', key_l(0x30, false, false));
    send(WM_KEYUP, 'B', key_l(0x30, false, true));
    check(s.calls == 1 && s.result == KILN_OK, "a callback can unsubscribe itself (%d calls, result %d)", s.calls, s.result);

    // A faulting subscriber is removed; the others go on.
    int faults = 0;
    Got got;
    KilnHandle hf = 0, hg = 0;
    kui::input_feed_subscribe(1, &faulting, &faults, &hf);
    kui::input_feed_subscribe(2, &collect, &got, &hg);
    const int errors = kui::g_errors;
    send(WM_KEYDOWN, 'C', key_l(0x2e, false, false));
    send(WM_KEYUP, 'C', key_l(0x2e, false, true));
    check(faults == 1 && kui::g_errors == errors + 1, "a faulting callback runs once and is removed, with an error line (%d)", faults);
    check(got.lines.size() == 2, "the other subscriber still gets both records (%zu)", got.lines.size());
    check(kui::input_feed_unsubscribe(hf) == KILN_ERR_NOT_FOUND, "the faulting one is gone");
    // Two subscribers get the same records.
    Got second;
    KilnHandle h2 = 0;
    kui::input_feed_subscribe(3, &collect, &second, &h2);
    got.lines.clear();
    send(WM_INPUT, RIM_INPUT, raw_key(0x2e, RI_KEY_MAKE, 'C'));
    check(got.lines == second.lines && got.lines.size() == 1, "two subscribers get the same records");
    // A new subscription starts with the focus as "not lost": its first loss gives a record.
    send(WM_KILLFOCUS, 0, 0);
    const auto old_loss = got.lines;
    Got late;
    KilnHandle hl = 0;
    kui::input_feed_subscribe(5, &collect, &late, &hl);
    send(WM_ACTIVATEAPP, FALSE, 0);  // the second notification of the same loss, with a new subscriber in between
    check(got.lines == old_loss, "adding a subscriber does not deliver the same focus loss twice to existing subscribers");
    check(late.lines == std::vector<std::string>{"focus-lost 001c 0"}, "the late subscriber gets its first loss notification");
    kui::input_feed_unsubscribe(hl);
    kui::input_feed_unsubscribe(hg);
    kui::input_feed_unsubscribe(h2);
    Got fresh;
    KilnHandle h3 = 0;
    kui::input_feed_subscribe(4, &collect, &fresh, &h3);
    send(WM_KILLFOCUS, 0, 0);
    check(fresh.lines == std::vector<std::string>{"focus-lost 0008 0"}, "a new subscriber gets its first focus loss:%s", join(fresh.lines).c_str());
    kui::input_feed_unsubscribe(h3);
    check(kui::input_feed_subscribe(1, nullptr, nullptr, &h3) == KILN_ERR_INVALID_ARGUMENT, "no fn: INVALID_ARGUMENT");
}

// ---- the middle and side buttons and the wheel as key presses (mouse_press.h) ----
size_t count_prefix(const std::vector<std::string>& v, const std::string& prefix) {
    size_t n = 0;
    for (const auto& x : v) n += x.rfind(prefix, 0) == 0;
    return n;
}
size_t count_if_has(const std::vector<std::string>& v, const std::string& part) {
    size_t n = 0;
    for (const auto& x : v) n += x.find(part) != std::string::npos;
    return n;
}
// Posted, then taken from the queue and dispatched as a game's message loop does: the message carries its own message
// time (GetMessageTime), unlike a sent one, which carries the time of the last message taken.
void post_pump(UINT m, WPARAM w, LPARAM l) {
    PostMessageW(g_win, m, w, l);
    MSG msg;
    while (PeekMessageW(&msg, g_win, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
}
// Kiln's key lines for one vk and transition ("vk=256 ... down=1"), presses only: a part of a wheel notch is asked as a
// repeat (would a subscriber keep the notch?), which `parts` counts.
size_t presses(const Run& r, uint32_t vk, bool down = true, bool repeat = false) {
    size_t n = 0;
    char want[32];
    std::snprintf(want, sizeof want, "down=%u repeat=%u", down ? 1u : 0u, repeat ? 1u : 0u);
    for (const auto& x : r.kiln) {
        unsigned v = 0;
        if (std::sscanf(x.c_str(), "vk=%u", &v) == 1 && v == vk && x.find(want) != std::string::npos) ++n;
    }
    return n;
}
size_t parts(const Run& r, uint32_t vk) { return presses(r, vk, true, true); }
size_t all_presses(const Run& r) { return r.kiln.size() - count_if_has(r.kiln, "repeat=1"); }
// The modifiers the window thread reads (GetKeyState): Ctrl and Shift held or not.
void hold(bool ctrl, bool shift = false) {
    BYTE ks[256] = {};
    GetKeyboardState(ks);
    ks[VK_CONTROL] = ks[VK_LCONTROL] = ctrl ? 0x80 : 0;
    ks[VK_SHIFT] = ks[VK_LSHIFT] = shift ? 0x80 : 0;
    SetKeyboardState(ks);
}
// The window forgets pending twins and notches with the focus (as after alt-tab): each case starts clean.
void fresh() {
    send(WM_KILLFOCUS, 0, 0);
    send(WM_SETFOCUS, 0, 0);
}
WPARAM wheel_w(int delta) { return MAKEWPARAM(0, static_cast<WORD>(static_cast<SHORT>(delta))); }
void raw_wheel(int delta, USHORT more = 0, LONG dx = 0, LONG dy = 0) {
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_WHEEL | more, dx, dy, static_cast<USHORT>(static_cast<SHORT>(delta))));
}
void msg_wheel(int delta) { send(WM_MOUSEWHEEL, wheel_w(delta), MAKELPARAM(10, 10)); }
std::string game_text(const Run& r) { return join(r.game); }
bool game_has_raw_wheel(const Run& r) {
    for (const auto& x : r.game) {
        unsigned flags = 0;
        if (std::sscanf(x.c_str(), "00ff mouse %x", &flags) == 1 && (flags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL))) return true;
    }
    return false;
}

void mouse_wheel_presses() {
    fresh();
    Run r = run([] { raw_wheel(120); msg_wheel(120); }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_UP) == 1 && r.kiln.size() == 1,
          "wheel: a raw record and the WM_MOUSEWHEEL of the same notch give ONE press (WheelUp):%s", join(r.kiln).c_str());
    check(r.kiln.size() == 1 && r.kiln[0].find("mods=0 down=1 repeat=0 captured=0") != std::string::npos,
          "wheel: the notch is a press without modifiers, no repeat, not captured: %s", r.kiln.empty() ? "" : r.kiln[0].c_str());
    check(game_has_raw_wheel(r) && contains(r.game, "020a "), "wheel: no key subscriber took it: the game got both:%s",
          game_text(r).c_str());
    r = run([] { msg_wheel(-120); raw_wheel(-120); }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_DOWN) == 1 && r.kiln.size() == 1,
          "wheel: the message before its raw record also gives ONE press (WheelDown):%s", join(r.kiln).c_str());
    r = run([] { raw_wheel(120); raw_wheel(120); msg_wheel(120); msg_wheel(120); }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_UP) == 2 && r.kiln.size() == 2,
          "wheel: two raw records drained ahead of their two messages give two presses:%s", join(r.kiln).c_str());
    fresh();
    r = run([] { raw_wheel(120); raw_wheel(120); raw_wheel(120); }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_UP) == 3, "wheel: raw records without messages: a press each (%zu)", presses(r, KILN_UI_VK_WHEEL_UP));
    fresh();
    r = run([] { msg_wheel(120); msg_wheel(120); }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_UP) == 2, "wheel: messages without raw records: a press each (%zu)", presses(r, KILN_UI_VK_WHEEL_UP));
    fresh();
    r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_HWHEEL, 0, 0, 120));
        send(WM_MOUSEHWHEEL, wheel_w(120), 0);
        send(WM_MOUSEHWHEEL, wheel_w(-120), 0);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_HWHEEL, 0, 0, static_cast<USHORT>(-120)));
    }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_RIGHT) == 1 && presses(r, KILN_UI_VK_WHEEL_LEFT) == 1 && r.kiln.size() == 2,
          "wheel: the horizontal wheel the same way (WheelRight, WheelLeft once each):%s", join(r.kiln).c_str());
    fresh();
    r = run([] { raw_wheel(240); msg_wheel(240); }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_UP) == 2, "wheel: a turn of two notches in one record gives two presses (%zu)",
          presses(r, KILN_UI_VK_WHEEL_UP));
    // Wine merges a queued WM_MOUSEWHEEL with the next one at the same cursor position, across the WM_INPUT records
    // between them (server/queue.c merge_mousewheel): two turns come as two records, then one +240 message. Posted
    // messages are not merged, so the harness delivers what Wine delivers after the merge.
    fresh();
    r = run([] { raw_wheel(120); raw_wheel(120); msg_wheel(240); }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_UP) == 2 && all_presses(r) == 2,
          "wheel: two records and one merged +240 message give two presses, not four:%s", join(r.kiln).c_str());
    r = run([] { raw_wheel(-240); msg_wheel(-120); msg_wheel(-120); }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_DOWN) == 2 && all_presses(r) == 2,
          "wheel: one -240 record and two -120 messages give two presses:%s", join(r.kiln).c_str());
    // A record and its message one Windows tick apart in message time (posted, so the message carries its own time)
    fresh();
    r = run([] {
        post_pump(WM_NULL, 0, 0);
        raw_wheel(120);  // carries the WM_NULL's time
        Sleep(20);
        post_pump(WM_MOUSEWHEEL, wheel_w(120), 0);
    }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_UP) == 1, "wheel: a record and its message 20 ms apart in message time give ONE press (%zu)",
          presses(r, KILN_UI_VK_WHEEL_UP));
}

void mouse_wheel_small_deltas() {
    fresh();
    Run r = run([] {
        for (int i = 0; i < 3; ++i) { raw_wheel(30); msg_wheel(30); }
    }, nullptr);
    check(all_presses(r) == 0 && parts(r, KILN_UI_VK_WHEEL_UP) == 3,
          "high-resolution wheel: three quarters of a notch give no press yet, each asked once as a part (repeat 1):%s",
          join(r.kiln).c_str());
    r = run([] { raw_wheel(30); msg_wheel(30); }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_UP) == 1 && r.kiln.size() == 1, "high-resolution wheel: the fourth quarter gives ONE press:%s",
          join(r.kiln).c_str());
    r = run([] {
        raw_wheel(60); msg_wheel(60);       // half a notch up...
        raw_wheel(-60); msg_wheel(-60);     // ...then down: a new notch begins
        raw_wheel(-60); msg_wheel(-60);
    }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_DOWN) == 1 && all_presses(r) == 1,
          "high-resolution wheel: a turn the other way starts a new notch (one WheelDown, no WheelUp):%s", join(r.kiln).c_str());
}

void mouse_wheel_bound() {
    fresh();
    kui::g_consume_vk = KILN_UI_VK_WHEEL_UP;  // a binding on Ctrl+WheelUp
    kui::g_consume_mods = KILN_UI_MOD_CTRL;
    hold(true);
    Run r = run([] { raw_wheel(120); msg_wheel(120); }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_UP) == 1 && r.kiln.size() == 1 && r.kiln[0].find("mods=1 ") != std::string::npos,
          "bound Ctrl+WheelUp: one press with Ctrl:%s", join(r.kiln).c_str());
    check(!game_has_raw_wheel(r) && !contains(r.game, "020a "),
          "bound Ctrl+WheelUp: kept from the game, raw record and message (the camera does not zoom):%s", game_text(r).c_str());
    r = run([] { msg_wheel(120); raw_wheel(120); }, nullptr);
    check(r.kiln.size() == 1 && !game_has_raw_wheel(r) && !contains(r.game, "020a "),
          "bound Ctrl+WheelUp, message first: one press, both kept from the game:%s", game_text(r).c_str());
    hold(false);
    r = run([] { raw_wheel(120); msg_wheel(120); }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_UP) == 1 && game_has_raw_wheel(r) && contains(r.game, "020a "),
          "the same wheel without Ctrl: one press no binding takes; the game gets both (the camera zooms):%s", game_text(r).c_str());
    hold(true);
    r = run([] { raw_wheel(-120); msg_wheel(-120); }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_DOWN) == 1 && game_has_raw_wheel(r) && contains(r.game, "020a "),
          "Ctrl+WheelDown, not bound: passes to the game:%s", game_text(r).c_str());
    // A taken press in a record with other input: the game reads the record with only the taken flags (and the wheel's
    // delta) taken out, through its own GetRawInputData import (input_install_iat hooked it).
    r = run([] { raw_wheel(120, 0, 5, -3); msg_wheel(120); }, nullptr);
    check(r.kiln.size() == 1 && r.game == std::vector<std::string>{"00ff mouse 0 5 -3 0"},
          "bound notch in a record with motion: the game gets the motion, without the notch:%s", game_text(r).c_str());
    r = run([] { raw_wheel(120, RI_MOUSE_LEFT_BUTTON_UP); msg_wheel(120); }, nullptr);
    check(r.kiln.size() == 1 && r.game == std::vector<std::string>{"00ff mouse 2 0 0 0"},
          "bound notch in a record with another button's release: the game gets the release, without the notch:%s",
          game_text(r).c_str());
    r = run([] { raw_wheel(120, RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_BUTTON_5_UP, -7, 4); msg_wheel(120); }, nullptr);
    check(r.kiln.size() == 2 && r.game == std::vector<std::string>{"00ff mouse 204 -7 4 0"},
          "...with a right press, a Mouse5 release and motion: all three stay, only the notch goes:%s", game_text(r).c_str());
    // Wine's merged message of two taken notches is kept as its two records are.
    fresh();
    r = run([] { raw_wheel(120); raw_wheel(120); msg_wheel(240); }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_UP) == 2 && r.game.empty(),
          "bound Ctrl+WheelUp, two records and one merged +240 message: two presses, none reaches the game:%s",
          game_text(r).c_str());
    // A high-resolution wheel on a bound chord: each part of a notch is asked (repeat 1) and kept like the notch.
    fresh();
    kui::g_consume_vk = KILN_UI_VK_WHEEL_DOWN;
    r = run([] {
        for (int i = 0; i < 8; ++i) { raw_wheel(-30); msg_wheel(-30); }
    }, nullptr);
    size_t to_game = count_prefix(r.game, "00ff ") + count_prefix(r.game, "020a ");
    check(presses(r, KILN_UI_VK_WHEEL_DOWN) == 2 && parts(r, KILN_UI_VK_WHEEL_DOWN) == 6 && to_game == 0,
          "bound Ctrl+WheelDown, high resolution: two presses, six parts asked; none of the 16 messages reaches the game "
          "(%zu)", to_game);
    // The binding gone: the very next part is asked again and passes
    kui::g_consume_vk = 0;
    r = run([] {
        for (int i = 0; i < 4; ++i) { raw_wheel(-30); msg_wheel(-30); }
    }, nullptr);
    to_game = count_prefix(r.game, "00ff ") + count_prefix(r.game, "020a ");
    check(presses(r, KILN_UI_VK_WHEEL_DOWN) == 1 && to_game == 8, "unbound again: every part reaches the game at once (%zu of 8)",
          to_game);
    // Review finding 3: Ctrl+WheelUp taken once, then the binding gone, then a high-resolution wheel going back and forth
    // with Ctrl (each turn the other way starts a new notch, none completes): nothing may stay kept.
    fresh();
    kui::g_consume_vk = KILN_UI_VK_WHEEL_UP;
    run([] { raw_wheel(120); msg_wheel(120); }, nullptr);
    kui::g_consume_vk = 0;
    r = run([] {
        for (int i = 0; i < 50; ++i) { raw_wheel(30); msg_wheel(30); raw_wheel(-30); msg_wheel(-30); }
    }, nullptr);
    check(r.game.size() == 200 && all_presses(r) == 0,
          "the binding gone, 100 parts back and forth with Ctrl: all 200 reach the game, no press (%zu reached it)", r.game.size());
    // Review finding 2: a twin carries the same message time. A raw record whose message never came, taken at one time,
    // and an unbound message 150 ms of message time later are two inputs: the second is dispatched and reaches the game.
    fresh();
    kui::g_consume_vk = KILN_UI_VK_WHEEL_UP;
    r = run([] {
        post_pump(WM_NULL, 0, 0);  // this thread's message time: now
        hold(true);
        raw_wheel(120);  // taken by Ctrl+WheelUp; its message never comes
        hold(false);
        Sleep(150);
        post_pump(WM_MOUSEWHEEL, wheel_w(120), 0);  // another turn, without Ctrl
    }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_UP) == 2 && r.game == std::vector<std::string>{"020a 780000 0"},
          "a message 150 ms after a lone taken record is not its twin: two presses, the unbound one reaches the game:%s%s",
          join(r.kiln).c_str(), game_text(r).c_str());
    hold(false);
    kui::g_consume_mods = ~0u;
}

// Without the GetRawInputData hook (the game's import not found): a record with a taken press and other input reaches the
// game whole, so its other input is never lost; its message is still kept. A record with only the press is kept whole.
void mouse_mixed_without_hook() {
    fresh();
    kui::g_consume_vk = KILN_UI_VK_WHEEL_UP;
    kui::g_consume_mods = KILN_UI_MOD_CTRL;
    hold(true);
    Run r = run([] { raw_wheel(120, 0, 5, -3); msg_wheel(120); }, nullptr);
    check(r.kiln.size() == 1 && r.game == std::vector<std::string>{"00ff mouse 400 5 -3 120"},
          "no import to edit: a taken notch with motion reaches the game whole (motion kept), its message is kept:%s",
          game_text(r).c_str());
    r = run([] { raw_wheel(120); msg_wheel(120); }, nullptr);
    check(r.kiln.size() == 1 && r.game.empty(), "no import to edit: a record with only the taken notch is still kept:%s",
          game_text(r).c_str());
    hold(false);
    kui::g_consume_vk = 0;
    kui::g_consume_mods = ~0u;
}

void mouse_buttons() {
    fresh();
    Run r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_DOWN));
        send(WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), 0);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_UP));
        send(WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    }, nullptr);
    check(presses(r, VK_XBUTTON1, true) == 1 && presses(r, VK_XBUTTON1, false) == 1 && r.kiln.size() == 2,
          "Mouse4 from raw input and WM_XBUTTONDOWN: ONE press and ONE release:%s", join(r.kiln).c_str());
    check(r.game.size() == 4, "Mouse4, not bound: the game gets all four (%zu):%s", r.game.size(), game_text(r).c_str());
    r = run([] {
        send(WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON2, XBUTTON2), 0);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_5_DOWN));
        send(WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), 0);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_5_UP));
    }, nullptr);
    check(presses(r, VK_XBUTTON2, true) == 1 && presses(r, VK_XBUTTON2, false) == 1 && r.kiln.size() == 2,
          "Mouse5, message before raw: ONE press and ONE release:%s", join(r.kiln).c_str());
    r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_MIDDLE_BUTTON_DOWN));
        send(WM_MBUTTONDOWN, MK_MBUTTON, 0);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_MIDDLE_BUTTON_UP));
        send(WM_MBUTTONUP, 0, 0);
        send(WM_XBUTTONDBLCLK, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), 0);  // a double click is a press, not a repeat
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_DOWN));
        send(WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_UP));
    }, nullptr);
    check(presses(r, VK_MBUTTON, true) == 1 && presses(r, VK_MBUTTON, false) == 1 && presses(r, VK_XBUTTON1, true) == 1 &&
              r.kiln.size() == 4 && !contains(r.kiln, "vk=5 scan=0 mods=0 down=1 repeat=1"),
          "MouseMiddle once each way; a double click of Mouse4 one press:%s", join(r.kiln).c_str());
    r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_LEFT_BUTTON_DOWN));
        send(WM_LBUTTONDOWN, MK_LBUTTON, 0);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP));
        send(WM_LBUTTONUP, 0, 0);
        send(WM_RBUTTONDOWN, MK_RBUTTON, 0);
        send(WM_RBUTTONUP, 0, 0);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_RIGHT_BUTTON_UP));
    }, nullptr);
    check(r.kiln.empty() && r.game.size() == 7, "left and right clicks are never key presses (%zu) and all reach the game",
          r.kiln.size());

    // Shift+Mouse4 bound
    fresh();
    kui::g_consume_vk = VK_XBUTTON1;
    kui::g_consume_mods = KILN_UI_MOD_SHIFT;
    hold(false, true);
    r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_DOWN));
        send(WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1 | MK_SHIFT, XBUTTON1), 0);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_UP));
        send(WM_XBUTTONUP, MAKEWPARAM(MK_SHIFT, XBUTTON1), 0);
    }, nullptr);
    check(presses(r, VK_XBUTTON1, true) == 1 && r.kiln.size() == 2 && r.kiln[0].find("mods=2 ") != std::string::npos,
          "bound Shift+Mouse4: one press with Shift, one release:%s", join(r.kiln).c_str());
    check(r.game == std::vector<std::string>{"00ff mouse 80 0 0 0", "020c 10004 0"},
          "bound Shift+Mouse4: the game gets neither press, both releases:%s", game_text(r).c_str());
    r = run([] {
        send(WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1 | MK_SHIFT, XBUTTON1), 0);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP));  // a fast click in one record
        send(WM_XBUTTONUP, MAKEWPARAM(MK_SHIFT, XBUTTON1), 0);
    }, nullptr);
    check(presses(r, VK_XBUTTON1, true) == 1 && r.game == std::vector<std::string>{"020c 10004 0"},
          "bound Shift+Mouse4, message first and a press with its release in one record: one press, the game gets "
          "the release message only:%s", game_text(r).c_str());
    hold(false);
    r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_DOWN));
        send(WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), 0);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_UP));
        send(WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    }, nullptr);
    check(presses(r, VK_XBUTTON1, true) == 1 && r.game.size() == 4, "Mouse4 without Shift: not taken, the game gets all four:%s",
          game_text(r).c_str());
    uint32_t mods = 0;
    kui::input_take_pressed(&mods);
    hold(false, true);
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_DOWN));
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_UP));
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_WHEEL, 0, 0, 120));
    check(kui::input_take_pressed(&mods) == 0, "take_pressed_key stays keyboard only (no mouse button, no notch)");
    hold(false);
    kui::g_consume_vk = 0;
    kui::g_consume_mods = ~0u;
}

void mouse_captured() {
    fresh();
    kui::g_menu_open = true;  // a Kiln screen has the input
    Run r = run([] {
        raw_wheel(120);
        msg_wheel(120);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_DOWN));
        send(WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), 0);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_UP));
        send(WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    }, nullptr);
    check(presses(r, KILN_UI_VK_WHEEL_UP) == 1 && presses(r, VK_XBUTTON1) == 1 && r.kiln.size() == 3 &&
              count_prefix(r.kiln, "vk=") == static_cast<size_t>(std::count_if(r.kiln.begin(), r.kiln.end(), [](const std::string& x) {
                  return x.find("captured=1") != std::string::npos;
              })),
          "Kiln captures input: one notch and one click, each once, with captured 1 (bindings do not fire on it; the Keys "
          "page takes it):%s", join(r.kiln).c_str());
    check(!game_has_raw_wheel(r) && !contains(r.game, "020a ") && !contains(r.game, "020b ") &&
              r.game == std::vector<std::string>{"00ff mouse 80 0 0 0", "020c 10000 0"},
          "Kiln captures input: the game gets no notch and no press, only the releases:%s", game_text(r).c_str());
    kui::g_menu_open = false;

    // a Kiln panel next to a game screen has the mouse over it: the wheel is the panel's, no press
    fresh();
    kui::g_hover_panels = 1;
    kui::g_want_mouse = true;
    kui::g_demand = 1;
    r = run([] { raw_wheel(120); msg_wheel(120); }, nullptr);
    check(r.kiln.empty() && r.game.empty(), "over a Kiln panel the wheel is the panel's: no press, not the game's:%s",
          join(r.kiln).c_str());
    kui::g_hover_panels = 0;
    kui::g_want_mouse = false;
    kui::g_demand = 0;
}

// ---- who gets mouse presses: subscribe_key (keyboard only, as before) and subscribe_key_ex(KILN_UI_KEYS_MOUSE) ----
struct KeyLog {
    std::vector<std::string> lines;
    uint32_t take_vk = 0;   // the press this subscriber takes
    bool take_all = false;  // a plugin that takes every press it gets
};
bool KILN_CALL key_collect(const KilnUiKey* k, void* user) {
    auto* log = static_cast<KeyLog*>(user);
    char b[48];
    std::snprintf(b, sizeof b, "vk=%u down=%u", k->vk, k->down);
    log->lines.push_back(b);
    return log->take_all || (k->down && k->vk == log->take_vk);
}
bool has_code_above_ff_or_button(const std::vector<std::string>& lines) {
    for (const auto& x : lines) {
        unsigned vk = 0;
        if (std::sscanf(x.c_str(), "vk=%u", &vk) == 1 && (vk > 0xFF || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2))
            return true;
    }
    return false;
}
void play_mouse_all() {
    raw_wheel(120);
    msg_wheel(120);
    raw_wheel(-120);
    msg_wheel(-120);
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_DOWN));
    send(WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), 0);
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_UP));
    send(WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_MIDDLE_BUTTON_DOWN));
    send(WM_MBUTTONDOWN, MK_MBUTTON, 0);
    send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_MIDDLE_BUTTON_UP));
    send(WM_MBUTTONUP, 0, 0);
}
struct Leaving {
    KilnHandle handle = 0;
    int calls = 0;
    KilnResult left = -1;
};
bool KILN_CALL take_and_leave(const KilnUiKey* k, void* user) {
    auto* l = static_cast<Leaving*>(user);
    ++l->calls;
    if (k->vk == VK_XBUTTON1 && k->down) {
        l->left = kui::keys_unsubscribe(l->handle);
        return true;
    }
    return false;
}
void mouse_subscribers() {
    fresh();
    // A plugin built before 0.2.0-dev.27 (subscribe_key) that takes every press it gets: alone, it changes nothing for
    // the mouse, and Kiln makes no mouse press at all.
    KeyLog old_plugin;
    old_plugin.take_all = true;
    KilnHandle old_h = 0, new_h = 0;
    check(kui::keys_subscribe(11, &key_collect, &old_plugin, 0, &old_h) == KILN_OK, "a subscribe_key subscriber");
    Run r = run(&play_mouse_all, nullptr);
    check(old_plugin.lines.empty(), "subscribe_key: no mouse press, no wheel code (%zu calls)", old_plugin.lines.size());
    check(r.kiln.empty(), "no KILN_UI_KEYS_MOUSE subscriber: no mouse press is made at all (%zu)", r.kiln.size());
    check(r.game.size() == 12 && count_prefix(r.game, "020a ") == 2 && game_has_raw_wheel(r),
          "...and the game gets every record and message untouched (%zu of 12):%s", r.game.size(), game_text(r).c_str());
    r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_key(0x43, RI_KEY_MAKE, VK_F9));
        send(WM_INPUT, RIM_INPUT, raw_key(0x43, RI_KEY_BREAK, VK_F9));
    }, nullptr);
    check(old_plugin.lines == std::vector<std::string>{"vk=120 down=1", "vk=120 down=0"} && r.game.size() == 1,
          "subscribe_key still gets keyboard keys and still takes the press it wants:%s", join(old_plugin.lines).c_str());

    // Kiln.Ui's way: subscribe_key_ex with KILN_UI_KEYS_MOUSE, next to the old plugin in the same process.
    KeyLog bindings;
    bindings.take_vk = KILN_UI_VK_WHEEL_UP;  // a binding on WheelUp
    check(kui::keys_subscribe(12, &key_collect, &bindings, KILN_UI_KEYS_MOUSE, &new_h) == KILN_OK,
          "a subscribe_key_ex(KILN_UI_KEYS_MOUSE) subscriber");
    old_plugin.lines.clear();
    r = run(&play_mouse_all, nullptr);
    check(old_plugin.lines.empty(), "the subscribe_key subscriber still gets no mouse press (%zu calls), so it cannot take one",
          old_plugin.lines.size());
    check(!has_code_above_ff_or_button(old_plugin.lines), "...no wheel code and no button code reached it");
    check(bindings.lines == std::vector<std::string>{"vk=256 down=1", "vk=257 down=1", "vk=5 down=1", "vk=5 down=0", "vk=4 down=1",
                                                     "vk=4 down=0"},
          "the KILN_UI_KEYS_MOUSE subscriber gets each notch and click once:%s", join(bindings.lines).c_str());
    check(r.game.size() == 10 && count_prefix(r.game, "020a ") == 1 && count_prefix(r.game, "00ff mouse 400 ") == 1,
          "the notch it took is kept from the game (record and message), the rest passes (%zu of 12):%s", r.game.size(),
          game_text(r).c_str());
    bindings.lines.clear();
    old_plugin.take_all = false;
    send(WM_INPUT, RIM_INPUT, raw_key(0x43, RI_KEY_MAKE, VK_F9));
    send(WM_INPUT, RIM_INPUT, raw_key(0x43, RI_KEY_BREAK, VK_F9));
    check(bindings.lines.size() == 2 && old_plugin.lines.size() == 2, "a keyboard key reaches both");
    KeyLog bad;
    KilnHandle bad_h = 0;
    check(kui::keys_subscribe(13, &key_collect, &bad, KILN_UI_KEYS_PAD, &bad_h) == KILN_OK &&
              kui::keys_unsubscribe(bad_h) == KILN_OK, "subscribe_key_ex accepts the controller flag");
    check(kui::keys_subscribe(13, &key_collect, &bad, 4, &bad_h) == KILN_ERR_INVALID_ARGUMENT &&
              kui::keys_subscribe(13, &key_collect, &bad, KILN_UI_KEYS_MOUSE | 0x80000000u, &bad_h) == KILN_ERR_INVALID_ARGUMENT &&
              kui::keys_subscribe(13, nullptr, &bad, KILN_UI_KEYS_MOUSE, &bad_h) == KILN_ERR_INVALID_ARGUMENT,
          "subscribe_key_ex refuses flags it does not know and a missing callback");

    // The last KILN_UI_KEYS_MOUSE subscriber gone: the mouse passes untouched again.
    check(kui::keys_unsubscribe(new_h) == KILN_OK && kui::keys_unsubscribe(new_h) == KILN_ERR_NOT_FOUND,
          "unsubscribe, once");
    fresh();
    r = run([] { raw_wheel(120); msg_wheel(120); }, nullptr);
    check(r.kiln.empty() && r.game.size() == 2, "no KILN_UI_KEYS_MOUSE subscriber left: no press, the game gets both (%zu)",
          r.game.size());

    // Review finding 5: the only mouse subscriber takes a raw Mouse4 press and leaves inside its callback. The press's
    // message comes after: the decision still stands, so the game gets neither form of the press.
    static Leaving leaving;
    leaving = {};
    check(kui::keys_subscribe(14, &take_and_leave, &leaving, KILN_UI_KEYS_MOUSE, &leaving.handle) == KILN_OK,
          "a sole mouse subscriber that leaves when it takes Mouse4");
    r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_DOWN));
        send(WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), 0);
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_BUTTON_4_UP));
        send(WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
        raw_wheel(120);
        msg_wheel(120);
    }, nullptr);
    check(leaving.calls == 1 && leaving.left == KILN_OK, "it took the press and unsubscribed inside its callback (%d calls, %d)",
          leaving.calls, leaving.left);
    check(r.game == std::vector<std::string>{"00ff mouse 80 0 0 0", "020c 10000 0", "00ff mouse 400 0 0 120", "020a 780000 a000a"},
          "the press's message is kept as its record was; the releases and a later notch reach the game:%s", game_text(r).c_str());
    kui::keys_unsubscribe(old_h);
}

void mouse_key_names() {
    const struct {
        uint32_t vk;
        const char* name;
    } names[] = {{VK_MBUTTON, "MouseMiddle"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
                 {KILN_UI_VK_WHEEL_UP, "WheelUp"}, {KILN_UI_VK_WHEEL_DOWN, "WheelDown"},
                 {KILN_UI_VK_WHEEL_LEFT, "WheelLeft"}, {KILN_UI_VK_WHEEL_RIGHT, "WheelRight"}};
    for (const auto& n : names) {
        char b[32];
        kui::key_name(n.vk, b, sizeof b);
        check(std::strcmp(b, n.name) == 0, "key_name(0x%x) = %s (%s)", n.vk, n.name, b);
    }
}

// GitHub issue 33: the key left of 1 on a UK layout (scan 0x29, VK_OEM_8) reaches Kiln's key handling with its scan
// code from raw input and from the window's key message alike, so a "Backquote" binding (key_names.h) can find its place;
// key_name names VK_OEM_8.
void layout_scan_codes() {
    kui::g_kiln_keys.clear();
    send(WM_INPUT, RIM_INPUT, raw_key(0x29, RI_KEY_MAKE, VK_OEM_8));
    send(WM_INPUT, RIM_INPUT, raw_key(0x29, RI_KEY_BREAK, VK_OEM_8));
    check(kui::g_kiln_keys.size() == 2 && kui::g_kiln_keys[0].rfind("vk=223 scan=29 ", 0) == 0,
          "raw input: the UK backquote key arrives as VK_OEM_8 with scan code 0x29 (%s)",
          kui::g_kiln_keys.empty() ? "nothing" : kui::g_kiln_keys[0].c_str());
    const RAWINPUTDEVICE remove{1, 6, RIDEV_REMOVE, nullptr};
    check(RegisterRawInputDevices(&remove, 1, sizeof remove), "raw keyboard unregistered (key messages only)");
    kui::g_kiln_keys.clear();
    send(WM_KEYDOWN, VK_OEM_8, key_l(0x29, false, false));
    send(WM_KEYUP, VK_OEM_8, key_l(0x29, false, true));
    check(kui::g_kiln_keys.size() == 2 && kui::g_kiln_keys[0].rfind("vk=223 scan=29 ", 0) == 0,
          "key message: the UK backquote key arrives as VK_OEM_8 with scan code 0x29 (%s)",
          kui::g_kiln_keys.empty() ? "nothing" : kui::g_kiln_keys[0].c_str());
    const RAWINPUTDEVICE add{1, 6, 0, nullptr};
    check(RegisterRawInputDevices(&add, 1, sizeof add), "raw keyboard registered again");
    char b[32];
    kui::key_name(VK_OEM_8, b, sizeof b);
    check(std::strcmp(b, "Oem8") == 0 && kui::keys::matches(0xC0, VK_OEM_8, 0x29) && !kui::keys::matches(0xC0, VK_OEM_3, 0x28),
          "key_name(VK_OEM_8) = Oem8 (%s), and Backquote is the key at 0x29, not UK's ' key", b);
}

}  // namespace

// ---- the overlay's replay into ImGui as a Kiln screen opens (input_capture_changed, input_replay) ----
int imgui_presses(int button) {
    int n = 0;
    for (const ImGuiInputEvent& e : ImGui::GetCurrentContext()->InputEventsQueue)
        if (e.Type == ImGuiInputEventType_MouseButton && e.MouseButton.Button == button && e.MouseButton.Down) ++n;
    return n;
}
std::string imgui_positions() {
    std::string s;
    for (const ImGuiInputEvent& e : ImGui::GetCurrentContext()->InputEventsQueue)
        if (e.Type == ImGuiInputEventType_MousePos) {
            char b[48];
            std::snprintf(b, sizeof b, " (%.0f, %.0f)", e.MousePos.PosX, e.MousePos.PosY);
            s += b;
        }
    return s;
}
void imgui_events_taken() { ImGui::GetCurrentContext()->InputEventsQueue.resize(0); }

void replay_screen_opens() {
    fresh();
    ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(1920, 1080);
    check(ImGui_ImplWin32_Init(g_win), "ImGui's Win32 backend on the test window");
    kui::input_replay();  // what the cases above left queued
    imgui_events_taken();

    // play: the game keeps its cursor at the centre for mouse look; an attack click, then a Kiln screen opens before
    // the overlay's next frame (or a hitch kept the frame back)
    send(WM_MOUSEMOVE, 0, MAKELPARAM(960, 540));
    send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(960, 540));
    send(WM_LBUTTONUP, 0, MAKELPARAM(960, 540));
    send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(960, 540));  // held as the screen opens
    kui::g_interactive = 1;
    kui::input_capture_changed();
    kui::input_replay();
    check(imgui_presses(0) == 0, "a screen that opens gets no click of play that was still queued (%d presses)", imgui_presses(0));
    imgui_events_taken();
    send(WM_LBUTTONUP, 0, MAKELPARAM(960, 540));  // the held button's release: up already, it clicks nothing
    send(WM_MOUSEMOVE, 0, MAKELPARAM(100, 100));
    send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
    send(WM_LBUTTONUP, 0, MAKELPARAM(100, 100));
    kui::input_replay();
    check(imgui_presses(0) == 1, "a click on the open screen reaches it (%d presses)", imgui_presses(0));
    imgui_events_taken();

    // a hitch: no frame for 300 ms, the mouse moved; raw records come ahead of the window's coalesced WM_MOUSEMOVE
    Sleep(300);
    send(WM_INPUT, RIM_INPUT, raw_mouse(0, 500, 40));
    send(WM_MOUSEMOVE, 0, MAKELPARAM(120, 100));
    kui::input_replay();
    const std::string hitch = imgui_positions();
    check(hitch == " (120, 100)", "after a hitch the cursor goes where the window says, not by raw deltas first:%s", hitch.c_str());
    imgui_events_taken();

    // raw motion only (Wine's pointer lock, no WM_MOUSEMOVE for 250 ms): the raw deltas still move Kiln's cursor
    Sleep(300);
    send(WM_INPUT, RIM_INPUT, raw_mouse(0, 30, 0));
    kui::input_replay();
    const std::string raw_only = imgui_positions();
    check(raw_only == " (150, 100)", "raw motion alone still moves Kiln's cursor:%s", raw_only.c_str());
    imgui_events_taken();

    kui::g_interactive = 0;
    kui::input_capture_changed();
    kui::input_replay();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

// ---- a key press a mod sends while a Kiln text field has the keyboard (text_keep.h; HadToRegister, 31.13: "t" typed
// "tb" in the Kiln menu's search, as ShroudLink's client add-on taps B when its MODS key T goes down) ----
std::string imgui_text() {
    std::string s;
    for (const ImGuiInputEvent& e : ImGui::GetCurrentContext()->InputEventsQueue)
        if (e.Type == ImGuiInputEventType_Text) s += static_cast<char>(e.Text.Char);
    return s;
}
int imgui_key_downs(ImGuiKey key) {
    int n = 0;
    for (const ImGuiInputEvent& e : ImGui::GetCurrentContext()->InputEventsQueue)
        if (e.Type == ImGuiInputEventType_Key && e.Key.Key == key && e.Key.Down) ++n;
    return n;
}
// What Windows gives the game window for one tap of a letter key: its raw records from a keyboard (or, for a press a
// module sent, from no device), the key messages, the character TranslateMessage made.
void tap(UINT vk, UINT scan, char c, bool keyboard = true) {
    const LPARAM down = raw_key(static_cast<USHORT>(scan), RI_KEY_MAKE, static_cast<USHORT>(vk));
    const LPARAM up = raw_key(static_cast<USHORT>(scan), RI_KEY_BREAK, static_cast<USHORT>(vk));
    if (keyboard) {  // a keyboard's records name their device; SendInput's name none
        g_raw[down - kRawBase].header.hDevice = reinterpret_cast<HANDLE>(0x10041);
        g_raw[up - kRawBase].header.hDevice = reinterpret_cast<HANDLE>(0x10041);
    }
    send(WM_INPUT, RIM_INPUT, down);
    send(WM_KEYDOWN, vk, key_l(scan, false, false));
    send(WM_CHAR, static_cast<WPARAM>(c), key_l(scan, false, false));
    send(WM_INPUT, RIM_INPUT, up);
    send(WM_KEYUP, vk, key_l(scan, false, true));
}
// ShroudLink's press_menu_key (client/src/input.rs): B by its scan code, down and up, in one SendInput call
UINT send_b() {
    INPUT in[2]{};
    for (INPUT& i : in) {
        i.type = INPUT_KEYBOARD;
        i.ki.wScan = 0x30;
        i.ki.dwFlags = KEYEVENTF_SCANCODE;
    }
    in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    return SendInput(2, in, sizeof(INPUT));
}
// A character typed from the game's process (KEYEVENTF_UNICODE: an on-screen keyboard or a paste), down and up
UINT send_char(wchar_t c) {
    INPUT in[2]{};
    for (INPUT& i : in) {
        i.type = INPUT_KEYBOARD;
        i.ki.wScan = static_cast<WORD>(c);
        i.ki.dwFlags = KEYEVENTF_UNICODE;
    }
    in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    return SendInput(2, in, sizeof(INPUT));
}
// The presses of `vk` Kiln's own key handling got (g_kiln_keys)
int kiln_downs(uint32_t vk) {
    char want[32];
    std::snprintf(want, sizeof want, "vk=%u ", vk);
    int n = 0;
    for (const auto& x : kui::g_kiln_keys)
        if (x.rfind(want, 0) == 0 && x.find(" down=1 ") != std::string::npos) ++n;
    return n;
}
void mod_sent_key_in_text_field() {
    fresh();
    ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(1920, 1080);
    check(ImGui_ImplWin32_Init(g_win), "text field: ImGui's Win32 backend on the test window");
    kui::input_replay();
    imgui_events_taken();
    kui::g_menu_open = true;  // the Kiln menu, its search field has the keyboard
    kui::g_text_input = true;
    kui::input_capture_changed();
    kui::input_replay();
    imgui_events_taken();

    // the player types "t t"; after each T the add-on taps B from the game's process (here SendInput reaches no window:
    // no desktop; the harness plays what Windows gives the game window for it)
    tap('T', 0x14, 't');
    send_b();
    tap('B', 0x30, 'b', false);
    tap(VK_SPACE, 0x39, ' ');
    tap('T', 0x14, 't');
    send_b();
    tap('B', 0x30, 'b', false);
    kui::input_replay();
    check(imgui_text() == "t t", "text field: a press a mod sends with SendInput types nothing (\"t t\", not \"tb tb\"): \"%s\"",
          imgui_text().c_str());
    check(imgui_key_downs(ImGuiKey_B) == 0, "text field: nor does its key reach the field (%d B downs)", imgui_key_downs(ImGuiKey_B));
    imgui_events_taken();

    // keybd_event, the older call, the same
    keybd_event('B', 0x30, 0, 0);
    keybd_event('B', 0x30, KEYEVENTF_KEYUP, 0);
    tap('B', 0x30, 'b', false);
    kui::input_replay();
    check(imgui_text().empty(), "text field: a press a mod sends with keybd_event types nothing: \"%s\"", imgui_text().c_str());
    imgui_events_taken();

    // the player's own B types, also right after a sent one
    send_b();
    tap('B', 0x30, 'b', false);
    tap('B', 0x30, 'b');
    tap('A', 0x1E, 'a');
    kui::input_replay();
    check(imgui_text() == "ba", "text field: the player's own B right after a sent one types (\"ba\"): \"%s\"", imgui_text().c_str());
    imgui_events_taken();

    // another program's input (an on-screen keyboard: from no device, never sent from the game's process) types
    tap('B', 0x30, 'b', false);
    kui::input_replay();
    check(imgui_text() == "b", "text field: another program's input types: \"%s\"", imgui_text().c_str());
    imgui_events_taken();

    // two Bs in a row, the sent one's and the player's, with both raw records ahead of the key messages (a message pump
    // takes the posted WM_INPUTs first): one types, and Kiln's key handling gets both
    kui::g_kiln_keys.clear();
    send_b();
    {
        const LPARAM sent_down = raw_key(0x30, RI_KEY_MAKE, 'B'), sent_up = raw_key(0x30, RI_KEY_BREAK, 'B');
        const LPARAM own_down = raw_key(0x30, RI_KEY_MAKE, 'B'), own_up = raw_key(0x30, RI_KEY_BREAK, 'B');
        g_raw[own_down - kRawBase].header.hDevice = reinterpret_cast<HANDLE>(0x10041);
        g_raw[own_up - kRawBase].header.hDevice = reinterpret_cast<HANDLE>(0x10041);
        send(WM_INPUT, RIM_INPUT, sent_down);
        send(WM_INPUT, RIM_INPUT, sent_up);
        send(WM_INPUT, RIM_INPUT, own_down);
        send(WM_KEYDOWN, 'B', key_l(0x30, false, false));
        send(WM_CHAR, 'b', key_l(0x30, false, false));
        send(WM_KEYUP, 'B', key_l(0x30, false, true));
        send(WM_INPUT, RIM_INPUT, own_up);
        send(WM_KEYDOWN, 'B', key_l(0x30, false, false));
        send(WM_CHAR, 'b', key_l(0x30, false, false));
        send(WM_KEYUP, 'B', key_l(0x30, false, true));
    }
    kui::input_replay();
    check(imgui_text() == "b" && imgui_key_downs(ImGuiKey_B) == 1,
          "text field: a sent B and the player's B, their raw records first: one types (\"b\", 1 B down): \"%s\", %d B downs",
          imgui_text().c_str(), imgui_key_downs(ImGuiKey_B));
    check(kiln_downs('B') == 2, "text field: Kiln's key handling gets both B presses (%d):%s", kiln_downs('B'),
          join(kui::g_kiln_keys).c_str());
    imgui_events_taken();

    // a sent B whose key message comes without a raw record although raw keyboard records came before (Proton need not
    // deliver every key as one): kept out all the same, and the player's A and B after it type, in that order
    send_b();
    send(WM_KEYDOWN, 'B', key_l(0x30, false, false));
    send(WM_CHAR, 'b', key_l(0x30, false, false));
    send(WM_KEYUP, 'B', key_l(0x30, false, true));
    tap('A', 0x1E, 'a');
    tap('B', 0x30, 'b');
    kui::input_replay();
    check(imgui_text() == "ab", "text field: a sent B without its raw record types nothing, the player's \"ab\" after it types: \"%s\"",
          imgui_text().c_str());
    imgui_events_taken();

    // a character typed from the game's process (KEYEVENTF_UNICODE: an on-screen keyboard or a paste, text by intent and
    // never a game key) types, with a raw record or without
    send_char(L'x');
    send(WM_INPUT, RIM_INPUT, raw_key(0, RI_KEY_MAKE, VK_PACKET));
    send(WM_KEYDOWN, VK_PACKET, key_l(0, false, false));
    send(WM_CHAR, 'x', key_l(0, false, false));
    send(WM_INPUT, RIM_INPUT, raw_key(0, RI_KEY_BREAK, VK_PACKET));
    send(WM_KEYUP, VK_PACKET, key_l(0, false, true));
    send_char(L'y');
    send(WM_KEYDOWN, VK_PACKET, key_l(0, false, false));
    send(WM_CHAR, 'y', key_l(0, false, false));
    send(WM_KEYUP, VK_PACKET, key_l(0, false, true));
    kui::input_replay();
    check(imgui_text() == "xy", "text field: characters typed from the game's process (KEYEVENTF_UNICODE) type (\"xy\"): \"%s\"",
          imgui_text().c_str());
    imgui_events_taken();

    // no text field has the keyboard: nothing is written down, a sent press is a key like any other
    kui::g_text_input = false;
    send_b();
    kui::g_text_input = true;
    tap('B', 0x30, 'b', false);
    kui::input_replay();
    check(imgui_text() == "b", "text field: a press a mod sent while no field had the keyboard is no sent press: \"%s\"",
          imgui_text().c_str());
    imgui_events_taken();

    kui::g_text_input = false;
    kui::g_menu_open = false;
    kui::input_capture_changed();
    kui::input_replay();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

// ---- subscribe_pointer_press (0.2.0-dev.30): left/right presses over the game's UI for the core's slot clicks ----
struct PointerLog {
    std::vector<KilnUiPointerPress> asked;
    uint32_t take_mods = ~0u;  // answer TAKE when the press's modifiers are exactly these
};
uint32_t KILN_CALL pointer_collect(const KilnUiPointerPress* p, void* user) {
    auto* log = static_cast<PointerLog*>(user);
    log->asked.push_back(*p);
    return p->mods == log->take_mods ? KILN_UI_POINTER_TAKE : KILN_UI_POINTER_PASS;
}
uint32_t KILN_CALL pointer_fault(const KilnUiPointerPress*, void*) {
    volatile int* null = nullptr;
    return static_cast<uint32_t>(*null);
}
void pointer_presses() {
    fresh();
    PointerLog log;
    log.take_mods = KILN_UI_MOD_CTRL;
    KilnHandle h = 0;
    check(kui::input_pointer_subscribe(11, &pointer_collect, &log, &h) == KILN_OK && h, "pointer: subscribed");
    hold(true);
    Run r = run([] {
        send(WM_LBUTTONDOWN, MK_LBUTTON | MK_CONTROL, MAKELPARAM(10, 20));
        send(WM_LBUTTONUP, MK_CONTROL, MAKELPARAM(10, 20));
    }, nullptr);
    check(log.asked.size() == 1 && log.asked[0].button == 1 && log.asked[0].mods == KILN_UI_MOD_CTRL &&
              log.asked[0].x == 10 && log.asked[0].y == 20 && log.asked[0].size == sizeof(KilnUiPointerPress),
          "pointer: Ctrl+left as a message: asked once, at the message's point, with Ctrl (%zu asks)", log.asked.size());
    check(!contains(r.game, "0201 ") && contains(r.game, "0202 "),
          "pointer: a taken press is kept from the game, its release reaches it:%s", game_text(r).c_str());
    hold(false);
    log.asked.clear();
    r = run([] {
        send(WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(30, 40));
        send(WM_RBUTTONUP, 0, MAKELPARAM(30, 40));
    }, nullptr);
    check(log.asked.size() == 1 && log.asked[0].button == 3 && log.asked[0].mods == 0 && contains(r.game, "0204 ") &&
              contains(r.game, "0205 "),
          "pointer: a plain right click is asked about (button 3) and passed: the game gets both:%s", game_text(r).c_str());
    // the raw record and its message are one press: never asked twice; both follow one answer
    fresh();
    log.asked.clear();
    hold(true);
    r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_LEFT_BUTTON_DOWN));
        send(WM_LBUTTONDOWN, MK_LBUTTON | MK_CONTROL, MAKELPARAM(10, 20));
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_LEFT_BUTTON_UP));
        send(WM_LBUTTONUP, MK_CONTROL, MAKELPARAM(10, 20));
    }, nullptr);
    const bool raw_kept = !contains(r.game, "00ff mouse 1 "), msg_kept = !contains(r.game, "0201 ");
    check(log.asked.size() <= 1 && raw_kept == msg_kept && raw_kept == (log.asked.size() == 1) &&
              contains(r.game, "00ff mouse 2 ") && contains(r.game, "0202 "),
          "pointer: raw record + message of one Ctrl+left: asked %zu time(s) (0 = no cursor position without a display: "
          "passed), both treated alike, releases reach the game:%s", log.asked.size(), game_text(r).c_str());
    // a raw record with motion: only the taken press flag goes (the import hook), motion stays
    if (log.asked.size() == 1) {
        fresh();
        r = run([] {
            send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_LEFT_BUTTON_DOWN, 4, -2));
            send(WM_LBUTTONDOWN, MK_LBUTTON | MK_CONTROL, MAKELPARAM(10, 20));
        }, nullptr);
        check(r.game == std::vector<std::string>{"00ff mouse 0 4 -2 0"},
              "pointer: a taken press in a record with motion: the game gets the motion only:%s", game_text(r).c_str());
    }
    hold(false);
    // the Kiln menu captures input: nobody is asked, the press is the menu's
    fresh();
    log.asked.clear();
    kui::g_menu_open = true;
    run([] { send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20)); send(WM_LBUTTONUP, 0, MAKELPARAM(10, 20)); }, nullptr);
    kui::g_menu_open = false;
    check(log.asked.empty(), "pointer: while Kiln captures input nobody is asked (%zu)", log.asked.size());
    // unsubscribed: never asked again
    check(kui::input_pointer_unsubscribe(h) == KILN_OK && kui::input_pointer_unsubscribe(h) == KILN_ERR_NOT_FOUND,
          "pointer: unsubscribed (once)");
    fresh();
    r = run([] { send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20)); }, nullptr);
    check(log.asked.empty() && contains(r.game, "0201 "), "pointer: after unsubscribe nobody is asked, the game gets it");
    // a faulting subscriber is removed; the press reaches the game
    KilnHandle f = 0;
    check(kui::input_pointer_subscribe(12, &pointer_fault, nullptr, &f) == KILN_OK, "pointer: a faulting subscriber");
    fresh();
    r = run([] { send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20)); }, nullptr);
    check(contains(r.game, "0201 ") && kui::input_pointer_unsubscribe(f) == KILN_ERR_NOT_FOUND,
          "pointer: a fault removes the subscriber, the press reaches the game:%s", game_text(r).c_str());
    check(kui::input_pointer_subscribe(13, nullptr, nullptr, &f) == KILN_ERR_INVALID_ARGUMENT, "pointer: no fn -> INVALID_ARGUMENT");
}

// ---- kiln.ui.input (0.2.0-dev.31.13): a press an owner takes is its own until the release ----
struct OwnLog {
    std::vector<std::string> lines;
    std::vector<uint32_t> take;  // codes it takes
    bool all = false;
    KilnHandle handle = 0;
    uint32_t disown_vk = 0;  // gives this press up inside its own call (an overlay that closes on it)
};
uint32_t KILN_CALL own_collect(const KilnUiOwnedInput* in, void* user) {
    auto* log = static_cast<OwnLog*>(user);
    static const char* kinds[] = {"?", "press", "repeat", "release", "end"};
    static const char* ends[] = {"", " focus", " world", " lost"};
    char b[96];
    std::snprintf(b, sizeof b, "%s %x%s%s%s", kinds[in->kind < 5 ? in->kind : 0], in->vk, in->end < 4 ? ends[in->end] : " ?",
                  (in->flags & KILN_UI_OWNED_LEAKED) ? " leaked" : "", in->controller ? (in->controller == 1 ? " @1" : " @?") : "");
    log->lines.push_back(b);
    if (in->size != sizeof(KilnUiOwnedInput)) log->lines.push_back("bad size");
    if (in->kind != KILN_UI_OWNED_PRESS) return KILN_UI_OWN_PASS;
    bool take = log->all;
    for (const uint32_t t : log->take) take = take || t == in->vk;
    if (take && in->vk == log->disown_vk && kui::own_disown(log->handle, in->vk, in->controller) != 1) log->lines.push_back("disown said 0");
    return take ? KILN_UI_OWN_TAKE : KILN_UI_OWN_PASS;
}
uint32_t KILN_CALL own_fault(const KilnUiOwnedInput*, void*) {
    volatile int* null = nullptr;
    return static_cast<uint32_t>(*null);
}
bool has_exact(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }
// the game's mouse message lines with this message id: their wParam
std::vector<unsigned long long> wparams(const Run& r, UINT msg) {
    std::vector<unsigned long long> out;
    for (const auto& x : r.game) {
        unsigned id = 0;
        unsigned long long w = 0, l = 0;
        if (std::sscanf(x.c_str(), "%x %llx %llx", &id, &w, &l) == 3 && id == msg) out.push_back(w);
    }
    return out;
}

// a fake kiln.client for the world poll: its generation is the test's
uint32_t g_generation = 1;
KilnResult KILN_CALL fake_client_state(KilnClientState* out) {
    if (!out || out->size < sizeof(KilnClientState)) return KILN_ERR_INVALID_ARGUMENT;
    *out = KilnClientState{};
    out->size = sizeof *out;
    out->generation = g_generation;
    return KILN_OK;
}
KilnClientApi g_fake_client{};
bool g_client_missing = false;  // kiln.client not registered yet
const void* KILN_CALL fake_service(KilnStr name, uint32_t) {
    return !g_client_missing && std::string(name.ptr, name.len) == KILN_CLIENT_SERVICE ? &g_fake_client : nullptr;
}
KilnApi g_fake_api{};

// kiln.client registered after kiln.ui.input first asked for it: it is asked again (at most once a second), so a world
// change still ends ownership (review 31.13).
void late_client() {
    g_fake_client.header = {sizeof g_fake_client, KILN_CLIENT_VERSION};
    g_fake_client.state = &fake_client_state;
    g_fake_api.service = &fake_service;
    g_client_missing = true;
    kui::g_api = &g_fake_api;
    OwnLog o;
    o.take = {KILN_UI_VK_PAD_A};
    check(kui::own_subscribe(39, KILN_UI_OWN_PAD, &own_collect, &o, &o.handle) == KILN_OK, "late kiln.client: a pad owner");
    kui::pad_key(0, KILN_UI_VK_PAD_A, true, 0, false);  // asks for kiln.client: not there yet
    kui::pad_key(0, KILN_UI_VK_PAD_A, false, 0, false);
    g_client_missing = false;  // registered now
    Sleep(1100);
    kui::pad_key(0, KILN_UI_VK_PAD_A, true, 0, false);
    kui::own_world_poll();
    g_generation = 2;  // the player left the world
    kui::own_world_poll();
    kui::pad_key(0, KILN_UI_VK_PAD_A, false, 0, false);
    check(o.lines == std::vector<std::string>{"press 20c", "release 20c", "press 20c", "end 20c world"} && !kui::own_claimed(),
          "late kiln.client: found later, a world change ends ownership:%s", join(o.lines).c_str());
    g_generation = 1;
    kui::g_api = nullptr;
    kui::own_unsubscribe(o.handle);
}

void owned_input() {
    fresh();
    PointerLog plog;  // the core's slot clicks (subscribe_pointer_press): a later handler, never asked for an owned press
    plog.take_mods = ~0u;
    KilnHandle ph = 0;
    check(kui::input_pointer_subscribe(21, &pointer_collect, &plog, &ph) == KILN_OK, "own: a pointer subscriber too");
    OwnLog bp;  // Blueprint: owns the right button
    bp.take = {VK_RBUTTON};
    check(kui::own_subscribe(30, 0, &own_collect, &bp, &bp.handle) == KILN_ERR_INVALID_ARGUMENT &&
              kui::own_subscribe(30, 16, &own_collect, &bp, &bp.handle) == KILN_ERR_INVALID_ARGUMENT &&
              kui::own_subscribe(30, KILN_UI_OWN_POINTER, nullptr, &bp, &bp.handle) == KILN_ERR_INVALID_ARGUMENT,
          "own: no source, an unknown source bit or no fn -> INVALID_ARGUMENT");
    check(kui::own_subscribe(30, KILN_UI_OWN_POINTER, &own_collect, &bp, &bp.handle) == KILN_OK && bp.handle,
          "own: Blueprint subscribes for the pointer");
    check(kui::input_table()->header.size == sizeof(KilnUiInputApi) && kui::input_table()->header.version == KILN_UI_INPUT_VERSION &&
              kui::input_table()->subscribe_owned && kui::input_table()->pad_state,
          "own: the kiln.ui.input table is v1, every slot set");

    // Blueprint's gesture as window messages: press, drag with the button held, release
    Run r = run([] {
        send(WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(30, 40));
        send(WM_MOUSEMOVE, MK_RBUTTON | MK_SHIFT, MAKELPARAM(35, 44));
        send(WM_MOUSEMOVE, MK_RBUTTON, MAKELPARAM(40, 48));
        send(WM_RBUTTONUP, 0, MAKELPARAM(40, 48));
        send(WM_MOUSEMOVE, 0, MAKELPARAM(41, 48));
    }, nullptr);
    check(bp.lines == std::vector<std::string>{"press 2", "release 2"},
          "own: Blueprint gets the right press and its release, once each:%s", join(bp.lines).c_str());
    check(!contains(r.game, "0204 ") && !contains(r.game, "0205 "),
          "own: the game gets neither the owned press nor its release:%s", game_text(r).c_str());
    check(wparams(r, WM_MOUSEMOVE) == std::vector<unsigned long long>{MK_SHIFT, 0, 0},
          "own: while Blueprint holds it, MK_RBUTTON leaves the game's WM_MOUSEMOVE (other bits stay):%s", game_text(r).c_str());
    check(plog.asked.empty() && !kui::own_owns(bp.handle, VK_RBUTTON, 0), "own: the pointer subscriber was not asked; nothing owned now");

    // the raw record and the message of each transition, with raw motion in between
    bp.lines.clear();
    fresh();
    r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_RIGHT_BUTTON_DOWN));
        send(WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(30, 40));
        send(WM_INPUT, RIM_INPUT, raw_mouse(0, 6, -2));
        send(WM_MOUSEMOVE, MK_RBUTTON, MAKELPARAM(36, 38));
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_RIGHT_BUTTON_UP, 3, 0));  // the release with motion: only the flag goes
        send(WM_RBUTTONUP, 0, MAKELPARAM(39, 38));
    }, nullptr);
    check(bp.lines == std::vector<std::string>{"press 2", "release 2"},
          "own: raw records + messages of one gesture: one press, one release:%s", join(bp.lines).c_str());
    check(r.game == std::vector<std::string>{"00ff mouse 0 6 -2 0", "0200 0 260024", "00ff mouse 0 3 0 0"},
          "own: the game gets the motion only (the release record without its flag, WM_MOUSEMOVE without MK_RBUTTON):%s",
          game_text(r).c_str());

    // a pass changes nothing: the left button is not Blueprint's
    bp.lines.clear();
    plog.asked.clear();
    fresh();
    r = run([] {
        send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20));
        send(WM_LBUTTONUP, 0, MAKELPARAM(10, 20));
    }, nullptr);
    check(bp.lines == std::vector<std::string>{"press 1"} && plog.asked.size() == 1 && contains(r.game, "0201 ") &&
              contains(r.game, "0202 "),
          "own: a press the owner passes goes on to the pointer subscriber and the game, its release too:%s", game_text(r).c_str());

    // focus loss mid-gesture: Blueprint hears it; the release that comes back later is the game's
    bp.lines.clear();
    fresh();
    r = run([] {
        send(WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(30, 40));
        send(WM_KILLFOCUS, 0, 0);
        send(WM_ACTIVATEAPP, FALSE, 0);
        send(WM_SETFOCUS, 0, 0);
        send(WM_MOUSEMOVE, MK_RBUTTON, MAKELPARAM(31, 40));
        send(WM_RBUTTONUP, 0, MAKELPARAM(31, 40));
    }, nullptr);
    check(bp.lines == std::vector<std::string>{"press 2", "end 2 focus"},
          "own: focus loss ends ownership once (End Focus):%s", join(bp.lines).c_str());
    check(!contains(r.game, "0204 ") && contains(r.game, "0205 ") && wparams(r, WM_MOUSEMOVE) == std::vector<unsigned long long>{MK_RBUTTON},
          "own: after the loss the game gets the release and the held bit (it never saw the press: nothing held):%s",
          game_text(r).c_str());

    // unload mid-gesture: no callback, the release reaches nobody, the next press is everyone's
    bp.lines.clear();
    fresh();
    r = run([&] {
        send(WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(30, 40));
        check(kui::own_unsubscribe(bp.handle) == KILN_OK && kui::own_unsubscribe(bp.handle) == KILN_ERR_NOT_FOUND,
              "own: Blueprint unsubscribes mid-gesture (once)");
        send(WM_MOUSEMOVE, MK_RBUTTON, MAKELPARAM(31, 40));
        send(WM_RBUTTONUP, 0, MAKELPARAM(31, 40));
        send(WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(32, 40));
        send(WM_RBUTTONUP, 0, MAKELPARAM(32, 40));
    }, nullptr);
    check(bp.lines == std::vector<std::string>{"press 2"}, "own: no callback after unsubscribe:%s", join(bp.lines).c_str());
    check(r.game == std::vector<std::string>{"0200 0 28001f", "0204 2 280020", "0205 0 280020"},
          "own: the orphan's release and held bit stay the game's secret; the next press reaches the game:%s",
          game_text(r).c_str());

    // a key: raw records and messages, repeats, the release
    OwnLog ov;  // an overlay that owns the B key and the middle button
    ov.take = {'B', VK_MBUTTON};
    check(kui::own_subscribe(31, KILN_UI_OWN_KEYS | KILN_UI_OWN_MOUSE, &own_collect, &ov, &ov.handle) == KILN_OK,
          "own: an overlay subscribes for keys and the mouse");
    fresh();
    r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_key(0x30, RI_KEY_MAKE, 'B'));
        send(WM_KEYDOWN, 'B', key_l(0x30, false, false));
        send(WM_INPUT, RIM_INPUT, raw_key(0x30, RI_KEY_MAKE, 'B'));       // the auto-repeat
        send(WM_KEYDOWN, 'B', key_l(0x30, false, false) | (1 << 30));
        send(WM_INPUT, RIM_INPUT, raw_key(0x30, RI_KEY_BREAK, 'B'));
        send(WM_KEYUP, 'B', key_l(0x30, false, true));
    }, nullptr);
    check(ov.lines == std::vector<std::string>{"press 42", "repeat 42", "release 42"},
          "own: the overlay gets the B press, its repeat and its release:%s", join(ov.lines).c_str());
    check(r.game.empty(), "own: the game gets nothing of the owned key (records, WM_KEYDOWN, WM_KEYUP):%s", game_text(r).c_str());
    check(presses(r, 'B', true) == 1 && presses(r, 'B', true, true) == 0 && presses(r, 'B', false) == 0,
          "own: Kiln's own key handling saw the press only (the menu key comes first), not the repeat or release:%s",
          join(r.kiln).c_str());

    // Alt+F4 always reaches the game: an owned F4's release does too (stuck-press guard), and the owner still hears it
    ov.lines.clear();
    ov.take = {'B', VK_MBUTTON, VK_F4};
    fresh();
    r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_key(0x3e, RI_KEY_MAKE, VK_F4));
        send(WM_SYSKEYDOWN, VK_F4, key_l(0x3e, false, false, true));
        send(WM_INPUT, RIM_INPUT, raw_key(0x3e, RI_KEY_BREAK, VK_F4));
        send(WM_SYSKEYUP, VK_F4, key_l(0x3e, false, true, true));
    }, nullptr);
    check(ov.lines == std::vector<std::string>{"press 73", "release 73 leaked"},
          "own: Alt+F4 owned: the owner hears the press and the release (marked leaked):%s", join(ov.lines).c_str());
    check(contains(r.game, "0104 73") && contains(r.game, "0105 73"),
          "own: the game gets WM_SYSKEYDOWN F4 (Alt+F4 closes the game) and so its release:%s", game_text(r).c_str());

    // the middle button: messages, held bit, release
    ov.lines.clear();
    fresh();
    r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_MIDDLE_BUTTON_DOWN));
        send(WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(5, 5));
        send(WM_MOUSEMOVE, MK_MBUTTON, MAKELPARAM(6, 5));
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_MIDDLE_BUTTON_UP));
        send(WM_MBUTTONUP, 0, MAKELPARAM(6, 5));
    }, nullptr);
    check(ov.lines == std::vector<std::string>{"press 4", "release 4"}, "own: the middle button: one press, one release:%s",
          join(ov.lines).c_str());
    check(r.game == std::vector<std::string>{"0200 0 50006"}, "own: the game gets only the motion, without MK_MBUTTON:%s",
          game_text(r).c_str());

    // a pad's B, owned on controller 1 (the XInput hook's callback, pad_key): hidden until the release, which is the owner's
    KeyLog pads;
    KilnHandle pads_h = 0;
    check(kui::keys_subscribe(32, &key_collect, &pads, KILN_UI_KEYS_PAD, &pads_h) == KILN_OK, "own: a pad key subscriber");
    OwnLog pad_owner;
    pad_owner.take = {KILN_UI_VK_PAD_B};
    check(kui::own_subscribe(33, KILN_UI_OWN_PAD, &own_collect, &pad_owner, &pad_owner.handle) == KILN_OK, "own: a pad owner");
    const bool kept_b = kui::pad_key(1, KILN_UI_VK_PAD_B, true, 0, false);
    const bool owns_b = kui::own_owns(pad_owner.handle, KILN_UI_VK_PAD_B, 1) && !kui::own_owns(pad_owner.handle, KILN_UI_VK_PAD_B, 0);
    KilnUiPadState ps{};
    ps.size = sizeof ps;
    const KilnResult pr = kui::input_pad_state(1, &ps);
    const bool kept_up = kui::pad_key(1, KILN_UI_VK_PAD_B, false, 0, false);
    const bool kept_a = kui::pad_key(1, KILN_UI_VK_PAD_A, true, 0, false);
    kui::pad_key(1, KILN_UI_VK_PAD_A, false, 0, false);
    check(kept_b && owns_b && !kept_up && !kept_a, "own: the owned B is kept from the game until its release; A is not owned");
    check(pad_owner.lines == std::vector<std::string>{"press 20d @1", "release 20d @1", "press 20c @1"},
          "own: the pad owner gets B's press and release on controller 1, and is offered A:%s", join(pad_owner.lines).c_str());
    check(pads.lines == std::vector<std::string>{"vk=524 down=1", "vk=524 down=0"},
          "own: the pad key subscriber never sees the owned B, only A:%s", join(pads.lines).c_str());
    check(pr == KILN_ERR_UNAVAILABLE && ps.size == sizeof ps && ps.controller == 1 && !ps.connected && ps.owned == 0x2000 &&
              ps.hidden == 0x2000,
          "own: pad_state without XInput hooked: UNAVAILABLE, but the owned B shows (owned 0x%x hidden 0x%x)", ps.owned, ps.hidden);
    ps.size = 8;
    check(kui::input_pad_state(1, &ps) == KILN_ERR_INVALID_ARGUMENT && kui::input_pad_state(4, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          "own: pad_state: a short record or controller 4 -> INVALID_ARGUMENT");
    kui::keys_unsubscribe(pads_h);

    // a world change (the fake kiln.client's generation): End World; the release still reaches nobody
    g_fake_client.header = {sizeof g_fake_client, KILN_CLIENT_VERSION};
    g_fake_client.state = &fake_client_state;
    g_fake_api.service = &fake_service;
    kui::g_api = &g_fake_api;
    ov.lines.clear();
    fresh();
    r = run([] {
        send(WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(5, 5));
        kui::own_world_poll();
        g_generation = 2;  // the player left the world
        kui::own_world_poll();
        kui::own_world_poll();
        send(WM_MBUTTONUP, 0, MAKELPARAM(5, 5));
    }, nullptr);
    check(ov.lines == std::vector<std::string>{"press 4", "end 4 world"}, "own: a world change ends ownership once:%s",
          join(ov.lines).c_str());
    check(r.game.empty(), "own: the press's release still never reaches the game:%s", game_text(r).c_str());
    // a disown as the overlay closes on its own press
    ov.take = {VK_ESCAPE};
    ov.lines.clear();
    fresh();
    r = run([&] {
        send(WM_INPUT, RIM_INPUT, raw_key(0x01, RI_KEY_MAKE, VK_ESCAPE));
        send(WM_KEYDOWN, VK_ESCAPE, key_l(0x01, false, false));
        check(kui::own_disown(ov.handle, VK_ESCAPE, 0) == 1 && !kui::own_owns(ov.handle, VK_ESCAPE, 0), "own: the overlay disowns its Escape");
        send(WM_INPUT, RIM_INPUT, raw_key(0x01, RI_KEY_BREAK, VK_ESCAPE));
        send(WM_KEYUP, VK_ESCAPE, key_l(0x01, false, true));
    }, nullptr);
    check(ov.lines == std::vector<std::string>{"press 1b"} && r.game.empty(),
          "own: a disowned press's release reaches nobody, not the game:%s", game_text(r).c_str());
    // the same, given up inside the press's own call: taken, and nobody's at once
    ov.take = {'Q'};
    ov.disown_vk = 'Q';
    ov.lines.clear();
    fresh();
    r = run([&] {
        send(WM_INPUT, RIM_INPUT, raw_key(0x10, RI_KEY_MAKE, 'Q'));
        send(WM_KEYDOWN, 'Q', key_l(0x10, false, false));
        check(!kui::own_owns(ov.handle, 'Q', 0), "own: a press disowned in its own call is not owned");
        send(WM_INPUT, RIM_INPUT, raw_key(0x10, RI_KEY_BREAK, 'Q'));
        send(WM_KEYUP, 'Q', key_l(0x10, false, true));
    }, nullptr);
    check(ov.lines == std::vector<std::string>{"press 51"} && r.game.empty(),
          "own: disowned in its own call: kept from the game, its release reaches nobody:%s%s", join(ov.lines).c_str(),
          game_text(r).c_str());
    kui::g_api = nullptr;
    kui::own_unsubscribe(ov.handle);
    kui::own_unsubscribe(pad_owner.handle);

    // a faulting owner is removed: the press goes on to the others
    KilnHandle f = 0;
    check(kui::own_subscribe(34, KILN_UI_OWN_POINTER, &own_fault, nullptr, &f) == KILN_OK, "own: a faulting owner");
    plog.asked.clear();
    fresh();
    r = run([] { send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20)); send(WM_LBUTTONUP, 0, MAKELPARAM(10, 20)); }, nullptr);
    check(contains(r.game, "0201 ") && contains(r.game, "0202 ") && plog.asked.size() == 1 && kui::own_unsubscribe(f) == KILN_ERR_NOT_FOUND,
          "own: a fault removes the owner; the press reaches the pointer subscriber and the game:%s", game_text(r).c_str());
    kui::input_pointer_unsubscribe(ph);
}

// A pad's B turned into Escape while Kiln has the input (input_synth_escape, from the XInput hook): Kiln's own Escape and
// the key subscribers get its press and its release; an owner of Escape is never offered it (its chance was the pad's B
// itself), so no claim on Escape is left behind (review 31.13).
void synthetic_escape() {
    fresh();
    check(!kui::own_claimed(), "synthetic Escape: nothing claimed before");
    KeyLog keys;
    KilnHandle kh = 0;
    check(kui::keys_subscribe(36, &key_collect, &keys, 0, &kh) == KILN_OK, "synthetic Escape: a key subscriber");
    OwnLog esc;
    esc.take = {VK_ESCAPE};
    check(kui::own_subscribe(37, KILN_UI_OWN_KEYS, &own_collect, &esc, &esc.handle) == KILN_OK, "synthetic Escape: an owner of Escape");
    kui::input_synth_escape();
    check(esc.lines.empty() && !kui::own_owns(esc.handle, VK_ESCAPE, 0) && !kui::own_claimed(),
          "synthetic Escape: the owner is not offered it and no claim is left on Escape:%s", join(esc.lines).c_str());
    check(keys.lines == std::vector<std::string>{"vk=27 down=1", "vk=27 down=0"},
          "synthetic Escape: the key subscriber gets its press and its release:%s", join(keys.lines).c_str());
    kui::own_unsubscribe(esc.handle);
    kui::keys_unsubscribe(kh);
}

// With raw keyboard input seen, a key whose first raw record never came (Proton): its WM_KEYDOWN reaches the game, then the
// raw record of its auto-repeat is a new press to Kiln, which an owner takes. The game saw the key go down, so it must get
// the WM_KEYUP too (the stuck-press guard: the claim is leaked), and the owner hears the release marked leaked (review 31.13).
size_t game_key_lines(const Run& r, UINT msg, uint32_t vk) {
    char want[16];
    std::snprintf(want, sizeof want, "%04x %x ", msg, vk);
    size_t n = 0;
    for (const auto& x : r.game) n += x.rfind(want, 0) == 0;
    return n;
}
void owned_key_game_saw() {
    fresh();
    OwnLog ow;
    ow.take = {'K'};
    check(kui::own_subscribe(38, KILN_UI_OWN_KEYS, &own_collect, &ow, &ow.handle) == KILN_OK, "own (game saw it): an owner of K");
    const Run r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_key(0x1e, RI_KEY_MAKE, 'A'));  // raw keyboard input is seen
        send(WM_KEYDOWN, 'A', key_l(0x1e, false, false));
        send(WM_INPUT, RIM_INPUT, raw_key(0x1e, RI_KEY_BREAK, 'A'));
        send(WM_KEYUP, 'A', key_l(0x1e, false, true));
        send(WM_KEYDOWN, 'K', key_l(0x25, false, false));             // K's first raw record never came
        send(WM_INPUT, RIM_INPUT, raw_key(0x25, RI_KEY_MAKE, 'K'));  // the auto-repeat's: a new press to Kiln
        send(WM_KEYDOWN, 'K', repeat_l(0x25));
        send(WM_INPUT, RIM_INPUT, raw_key(0x25, RI_KEY_BREAK, 'K'));
        send(WM_KEYUP, 'K', key_l(0x25, false, true));
    }, nullptr);
    const size_t downs = game_key_lines(r, WM_KEYDOWN, 'K'), ups = game_key_lines(r, WM_KEYUP, 'K');
    check(downs >= 1 && ups == 1, "own (game saw it): the game gets a WM_KEYUP for the WM_KEYDOWN it got (%zu down, %zu up):%s",
          downs, ups, game_text(r).c_str());
    check(game_key_lines(r, WM_KEYDOWN, 'A') == 1 && game_key_lines(r, WM_KEYUP, 'A') == 1, "own (game saw it): A is the game's");
    check(ow.lines == std::vector<std::string>{"press 41", "press 4b", "release 4b leaked"} && !kui::own_claimed(),
          "own (game saw it): the owner hears the press and the release, marked leaked; nothing claimed after:%s",
          join(ow.lines).c_str());
    kui::own_unsubscribe(ow.handle);
}

// Before the GetRawInputData hook: an owned press in a raw record with motion reaches the game whole, so its release must
// too (the stuck-press guard), although the owner keeps hearing it.
void owned_input_without_hook() {
    fresh();
    OwnLog bp;
    bp.take = {VK_RBUTTON};
    check(kui::own_subscribe(35, KILN_UI_OWN_POINTER, &own_collect, &bp, &bp.handle) == KILN_OK, "own (no hook): Blueprint subscribes");
    const Run r = run([] {
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_RIGHT_BUTTON_DOWN, 4, 0));
        send(WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(30, 40));
        send(WM_MOUSEMOVE, MK_RBUTTON, MAKELPARAM(34, 40));
        send(WM_INPUT, RIM_INPUT, raw_mouse(RI_MOUSE_RIGHT_BUTTON_UP));
        send(WM_RBUTTONUP, 0, MAKELPARAM(34, 40));
    }, nullptr);
    const bool asked = !bp.lines.empty();  // raw-led: the cursor position is asked of the system (none without a display)
    check(asked && bp.lines.back() == "release 2 leaked",
          "own (no hook): the owner hears the release, marked leaked:%s", join(bp.lines).c_str());
    check(contains(r.game, "00ff mouse 4 4 ") && contains(r.game, "00ff mouse 8 ") && contains(r.game, "0205 ") &&
              wparams(r, WM_MOUSEMOVE) == std::vector<unsigned long long>{MK_RBUTTON},
          "own (no hook): the game saw the press (whole record), so it gets the held bit and the release:%s", game_text(r).c_str());
    kui::own_unsubscribe(bp.handle);
}

int main() {
    if (!patch_import("GetRawInputData", reinterpret_cast<void*>(&fake_raw), reinterpret_cast<void**>(&g_real_raw))) {
        std::printf("FAIL this exe imports no GetRawInputData to stand in for\n");
        return 1;
    }
    void* real_async = nullptr;  // input.cpp settles held keys with it when the focus comes back
    g_async_patched = patch_import("GetAsyncKeyState", reinterpret_cast<void*>(&fake_async_key_state), &real_async);
    WNDCLASSW wc{};
    wc.lpfnWndProc = &game_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KilnWindowInputGame";
    RegisterClassW(&wc);
    // Message-only: no display needed, nothing reaches a desktop.
    g_win = CreateWindowExW(0, wc.lpszClassName, L"window input", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    check(g_win && !IsWindowVisible(g_win), "message-only window %p", static_cast<void*>(g_win));
    kui::input_attach(g_win);
    check(kui::input_attached(), "Kiln's window procedure is on the window");
    const RAWINPUTDEVICE keyboard{1, 6, 0, nullptr};
    check(RegisterRawInputDevices(&keyboard, 1, sizeof keyboard), "raw keyboard registered");

    same_for_game("play", &play_everything, kEverything);
    same_for_game("extended, repeated and double-click input", &play_edge_keys, kEdgeKeys);

    kui::g_menu_open = true;  // the Kiln menu: Kiln keeps presses, characters and clicks from the game
    same_for_game("menu open", &play_captured, kCaptured);
    const Run captured = run(&play_captured, nullptr);
    check(!contains(captured.game, "0100 ") && !contains(captured.game, "0201 "),
          "menu open: the game got no key press and no click (the records above were kept from it):%s", join(captured.game).c_str());
    kui::g_menu_open = false;
    kui::g_wheel_active = true;  // the radial wheel captures the same way
    same_for_game("wheel open", &play_captured, kCaptured);
    kui::g_wheel_active = false;

    kui::g_consume_vk = VK_INSERT;  // a key Kiln consumes (the menu key, a binding)
    same_for_game("consumed key", &play_consumed, kConsumed);
    const Run consumed = run(&play_consumed, nullptr);
    check(!contains(consumed.game, "0100 ") && contains(consumed.game, "0101 "),
          "consumed key: the game got its release only:%s", join(consumed.game).c_str());
    kui::g_consume_vk = 0;

    lifetime();
    raw_unregistered();
    held_focus_lost();
    queued_focus_return();
    press_rule_cases();
    nested_unsubscribe();
    recursive_unsubscribe();
    delivery_mutation();
    raw_boundaries();
    layout_scan_codes();

    const RAWINPUTDEVICE mouse{1, 2, 0, nullptr};  // the game registers raw mouse input too
    check(RegisterRawInputDevices(&mouse, 1, sizeof mouse), "raw mouse registered");
    // Kiln.Ui's key subscriber asks for mouse presses (subscribe_key_ex, KILN_UI_KEYS_MOUSE); its bindings are played by
    // dispatch_key's consume rule above.
    KeyLog kiln_ui;
    KilnHandle kiln_ui_h = 0;
    check(kui::keys_subscribe(10, &key_collect, &kiln_ui, KILN_UI_KEYS_MOUSE, &kiln_ui_h) == KILN_OK,
          "Kiln.Ui's key subscriber, with KILN_UI_KEYS_MOUSE");
    mouse_mixed_without_hook();
    owned_input_without_hook();
    kui::input_install_iat();  // as at the game's start: the game's import slots, GetRawInputData among them
    mouse_wheel_presses();
    mouse_wheel_small_deltas();
    mouse_wheel_bound();
    mouse_buttons();
    pointer_presses();
    late_client();
    owned_input();
    synthetic_escape();
    owned_key_game_saw();
    mouse_captured();
    kui::keys_unsubscribe(kiln_ui_h);
    mouse_subscribers();
    mouse_key_names();
    replay_screen_opens();
    mod_sent_key_in_text_field();

    std::printf("%s: %d checks, %d failed\n", g_fails ? "FAIL" : "PASS", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

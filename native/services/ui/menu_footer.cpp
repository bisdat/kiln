// SPDX-License-Identifier: MIT
// kiln.ui: Kiln's line in the main menu's footer, "Kiln <version> - <N> Mods Loaded" with Kiln's mark (feature
// ui.menu_footer; menu_footer.h has the rules, docs/re/game-ui.md "The main menu's footer" the game's code).
//
// The game's footer function (ui/MainMenuFooter, window "LegalLine") draws the build number and the legal line at the
// bottom of the main menu. XHL-Core 2.1.0 patches its call of the text constructor for the build number (a 5-byte call
// rewrite) and draws its "All mods loaded" line there. Kiln's line takes that place: an inline hook on the footer
// function runs Kiln's own copy of it (code_clone.h), made from the exe file's bytes at start, whose build-number call
// goes through Kiln first. So the game's two lines are drawn as the game draws them, Kiln's line right above the build
// number with the same style, size and alignment, and a line another module patched into the game's footer code is
// not drawn while Kiln's is, whichever of the two installed first (Kiln never writes to the footer's body, so XHL-Core's
// own checks and patches go in as before, and its other main-menu code is untouched).
//
// With XHL-Core (owner's decision, 2026-11-19; menu_footer.h): Kiln's line says "Kiln <version> with XHL-Core - ...",
// and while XHL-Core reports a problem its red line (its only warning channel) is drawn again and opens its diagnostics
// when clicked. How: the copy's two calls XHL-Core patches (the "LegalLine" window call and the build-number call) go
// through Kiln's thunks, which call the game's functions, or XHL-Core's patched targets while Kiln follows XHL-Core. So
// XHL-Core draws its own line, in its words, colour and language, and handles its clicks, keyboard and gamepad reach
// and diagnostics as it does without Kiln; Kiln reads only its state (one int, found through its getter's shape) to
// leave its green "All mods loaded" out. If that state cannot be found, XHL-Core's line is always drawn (a warning is
// never hidden). XHL-Core is known by the export XhlNativeGameBuild of the module its patched call leads to.
//
// Fail safe: a name that does not resolve, a copy that cannot be made or checked, the player's setting, no mod count
// yet, or a fault in Kiln's line: the game's own function runs (with XHL-Core's line where it is installed). Each such
// reason is one kiln.log line. The mark is drawn by the overlay at the line's place in the game's layout.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "code_clone.h"
#include "code_copy_win.h"
#include "imgui.h"
#include "kiln_hook.hpp"
#include "menu_footer.h"
#include "menu_footer_logo.h"
#include "ui.h"
#include "ui_text.h"

namespace kui {

std::atomic<uint32_t> g_footer_demand{0};

namespace {

struct GameText {  // the UI's string view: UTF-8, byte length, FNV-1a 64 of the bytes (sign-extended, like the game)
    const char* p;
    uint64_t n;
    uint64_t fnv;
};

struct Names {
    uintptr_t fn = 0, site = 0, win = 0, line = 0, draw = 0, align = 0, rect = 0, ui_app = 0;
    int64_t config = 0, font = 0, text = 0, node = 0, shown = 0, color = 0, flags = 0, bits = 0, color2 = 0, flags2 = 0,
            size = 0;
    int64_t frame_arg = 0, viewport = 0, ppu = 0;
    bool metrics = false;
} N;

using LineFn = void*(__fastcall*)(void* out, const GameText* text, const void* style, uint32_t mode);
using DrawFn = void(__fastcall*)(void* text);
using AlignFn = void(__fastcall*)(void* node, const uint16_t* align);
using RectFn = const float*(__fastcall*)(void* node);
using FooterFn = void(__fastcall*)();
using WindowFn = void*(__fastcall*)(void*, void*, uint64_t, uint64_t, uint64_t, uint64_t);

kiln::RawHook g_hook;
FooterFn g_copy = nullptr;           // Kiln's copy of the footer function (ready: it passed its check)
uint32_t g_size = 0;                 // the function's length
std::vector<uint8_t> g_file;         // its bytes as the exe file has them
std::atomic<bool> g_failed{false};   // Kiln's line faulted: the game's function from now on
std::atomic<bool> g_checked{false};  // the live code was compared at the first use
std::atomic<bool> g_shown{false};    // the player's setting (Kiln.Ui: menus-client.json mainMenuFooter); off until told
std::atomic<int64_t> g_mods{-1};     // the count Kiln.Ui reported, -1 until then
uintptr_t g_win_file = 0;            // the window call's target in the exe file (0: the copy does not send it through Kiln)

// XHL-Core in the footer (the UI thread writes, the thunks read on it too)
std::atomic<bool> g_xhl{false};          // XHL-Core patched the footer's build-number call
std::atomic<bool> g_follow{false};       // this frame, the copy calls XHL-Core's targets
std::atomic<uintptr_t> g_xhl_line{0};    // XHL-Core's patched target of the build-number call
std::atomic<uintptr_t> g_xhl_win{0};     // its patched target of the window call (0: not patched by XHL-Core)
uintptr_t g_xhl_state = 0;               // its state int (0: not found: its line always shows)
uintptr_t g_seen_line = 1, g_seen_win = 1;  // the live calls' targets last looked at
int g_said_state = -2;                   // the state kiln.log last named (-1: unreadable)
KilnHandle g_mark = 0;               // the mark (image_load)

// The line's text, interned for the process (the game may keep the pointer until the frame renders)
std::mutex g_text_mu;
std::deque<std::string> g_texts;
std::atomic<const std::string*> g_text{nullptr};
std::atomic<uint64_t> g_text_fnv{0};
std::string g_version;

// What the last line's place was (UI thread writes, render thread reads)
std::mutex g_place_mu;
float g_rect[4] = {NAN, NAN, NAN, NAN};
float g_vx = 0, g_vy = 0, g_ppu = 0;
uint64_t g_drawn_frame = 0;
std::atomic<bool> g_said_rect{false};

uint64_t fnv64(const char* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(p[i])));
        h *= 0x100000001b3ull;
    }
    return h;
}

std::string line(bool gap) {
    const auto& tt = text::table();
    const int64_t m = g_mods.load();
    const bool xhl = g_xhl.load();
    return menu_footer::line_text(g_version, m, gap, tt.plural(menu_footer::line_key(xhl), m), xhl);
}

void set_text() {
    const bool gap = overlay_drawing_on() && g_mark != 0;
    const std::string t = line(gap);
    std::lock_guard lk(g_text_mu);
    const std::string* cur = g_text.load();
    if (cur && *cur == t) return;
    if (t.empty()) {
        g_text.store(nullptr);
        return;
    }
    for (const std::string& s : g_texts)
        if (s == t) {
            g_text_fnv.store(fnv64(s.data(), s.size()));
            g_text.store(&s);
            return;
        }
    g_texts.push_back(t);
    g_text_fnv.store(fnv64(g_texts.back().data(), g_texts.back().size()));
    g_text.store(&g_texts.back());
    if (g_copy)
        logf(KILN_LOG_INFO, "ui: the main menu's footer says \"%s\"%s", line(false).c_str(),
             gap ? " after Kiln's mark" : "");
}

// The module (file name + offset) of an address, for the log.
void where(uintptr_t a, char* out, size_t cap) {
    std::snprintf(out, cap, "0x%llx (code of no module)", static_cast<unsigned long long>(a));
    HMODULE mod = nullptr;
    wchar_t path[1024];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(a), &mod) || !mod)
        return;
    const DWORD n = GetModuleFileNameW(mod, path, 1024);
    const wchar_t* name = path;
    for (DWORD i = 0; i < n && i < 1024; ++i)
        if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
    char narrow[128] = "";
    if (n && n < 1024) WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof narrow - 1, nullptr, nullptr);
    std::snprintf(out, cap, "%s+0x%llx", narrow[0] ? narrow : "a module",
                  static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(mod)));
}

void* __fastcall line_thunk(void* out, const GameText* text, const void* style, uint32_t mode);

// The footer's window call: the game's window function, or XHL-Core's patched target while Kiln follows XHL-Core (it
// turns the window's input on so its line takes clicks). Six integer arguments, two on the stack, as the call passes them.
void* __fastcall window_thunk(void* a, void* b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    const uintptr_t x = g_follow.load(std::memory_order_relaxed) ? g_xhl_win.load(std::memory_order_relaxed) : 0;
    return reinterpret_cast<WindowFn>(x ? x : g_win_file)(a, b, c, d, e, f);
}

// Kiln's copy of the footer function (code_copy_win.h): its window call sent through window_thunk and its build-number
// call through line_thunk. Without the window call (it is not one), only the build-number call: XHL-Core's line can then
// show but not be clicked.
bool make_copy(std::string* why) {
    code_copy::Copy c;
    N.win = N.fn + menu_footer::kWindowCallAt;
    const code_copy::Site both[] = {{N.win, reinterpret_cast<const void*>(&window_thunk)},
                                    {N.site, reinterpret_cast<const void*>(&line_thunk)}};
    std::string why2;
    if (code_copy::make(N.fn, both, 2, &c, &why2)) {
        int32_t rel = 0;
        std::memcpy(&rel, c.file.data() + menu_footer::kWindowCallAt + 1, 4);
        g_win_file = N.win + 5 + static_cast<intptr_t>(rel);
    } else {
        N.win = 0;
        if (!code_copy::make(N.fn, N.site, reinterpret_cast<const void*>(&line_thunk), &c, why)) return false;
        logf(KILN_LOG_INFO, "ui: the main menu's footer: its window call stays the game's (%s); XHL-Core's line, where "
                            "Kiln shows it, cannot be clicked", why2.c_str());
    }
    g_file = std::move(c.file);
    g_size = c.size;
    g_copy = reinterpret_cast<FooterFn>(c.fn);
    return true;
}

// n bytes from game memory; false when it faults. No C++ objects in this frame (__try).
bool copy_guarded(void* dst, uintptr_t src, size_t n) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A call-site patch usually calls a stub next to the image (XHL-Core's: an optional `mov r9, reg`, then `jmp [rip+0]`
// and the address): the module behind it, for the log. Else the address itself.
uintptr_t through_stub(uintptr_t t) {
    uint8_t b[24] = {};
    if (!copy_guarded(b, t, sizeof b)) return t;
    for (int i = 0; i <= 4; ++i)
        if (b[i] == 0xFF && b[i + 1] == 0x25 && b[i + 2] == 0 && b[i + 3] == 0 && b[i + 4] == 0 && b[i + 5] == 0) {
            uint64_t to = 0;
            std::memcpy(&to, b + i + 6, 8);
            return static_cast<uintptr_t>(to);
        }
    return t;
}

// Once, at the first use (UI thread): the live footer against the file's. A call another module patched is logged (its
// line is the one Kiln's replaces); any other change, apart from the function's first instructions (inline hooks,
// Kiln's own), leaves the game's function in charge: Kiln does not know what it is for.
bool live_check() {
    if (g_checked.exchange(true)) return g_copy != nullptr;
    std::vector<uint8_t> live(g_size);
    if (!copy_guarded(live.data(), N.fn, g_size)) return true;  // unreadable: the copy is the file's code all the same
    std::vector<uint32_t> starts;
    code_clone::scan(g_file.data(), g_size, N.fn, nullptr, &starts, nullptr);
    uint32_t head = 0;  // the instructions an inline hook on the entry overwrites (up to 14 bytes)
    for (uint32_t s : starts) {
        if (s >= 14) break;
        head = s;
    }
    for (const code_clone::Change& c : code_clone::changes(g_file.data(), live.data(), g_size, N.fn)) {
        if (c.at <= head) continue;
        char w[160];
        if (c.call) {
            where(through_stub(static_cast<uintptr_t>(c.live_target)), w, sizeof w);
            logf(KILN_LOG_INFO, "ui: the main menu's footer: a module patched the game's footer call at +0x%x to %s; Kiln's "
                                "copy calls it only when it is XHL-Core's, while XHL-Core reports a problem",
                 c.at, w);
            continue;
        }
        where(N.fn + c.at, w, sizeof w);
        logf(KILN_LOG_WARN, "ui: the main menu's footer: the game's footer code was changed at +0x%x (%u bytes, not a call) "
                            "by another module; Kiln leaves the footer to the game's own code and draws no line of its own",
             c.at, c.len);
        g_copy = nullptr;
        g_footer_demand.store(0, std::memory_order_relaxed);
        return false;
    }
    return true;
}

// The target of the near call at `site` in the live code, 0 when it is not one (or unreadable).
uintptr_t live_call(uintptr_t site) {
    uint8_t b[5] = {};
    if (!site || !copy_guarded(b, site, 5) || b[0] != 0xE8) return 0;
    int32_t rel = 0;
    std::memcpy(&rel, b + 1, 4);
    return site + 5 + static_cast<intptr_t>(rel);
}

HMODULE module_of(uintptr_t a) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(a), &mod))
        return nullptr;
    return mod;
}

// The footer's live calls changed (or were never looked at): is XHL-Core behind the build-number call, where are its
// targets and its state. UI thread.
void xhl_resolve(uintptr_t lt, uintptr_t wt) {
    bool xhl = false;
    uintptr_t line_to = 0, win_to = 0, state = 0;
    char w[160] = "";
    if (lt && lt != N.line) {
        const uintptr_t detour = through_stub(lt);
        const HMODULE mod = module_of(detour);
        if (mod && GetProcAddress(mod, "XhlNativeGameBuild")) {
            xhl = true;
            line_to = lt;
            where(detour, w, sizeof w);
            uint8_t code[menu_footer::kXhlDetourScan];
            size_t n = sizeof code;
            if (!copy_guarded(code, detour, n)) n = copy_guarded(code, detour, 0x40) ? 0x40 : 0;
            const std::vector<uint64_t> calls = code_clone::calls_in(code, n, detour);
            for (size_t i = 0; i < calls.size() && i < 8 && !state; ++i) {
                uint8_t g[menu_footer::kXhlGetterLen];
                uint64_t s = 0;
                if (copy_guarded(g, static_cast<uintptr_t>(calls[i]), sizeof g) &&
                    menu_footer::xhl_state_getter(g, sizeof g, calls[i], &s))
                    state = static_cast<uintptr_t>(s);
            }
            if (wt && g_win_file && wt != g_win_file && module_of(through_stub(wt)) == mod) win_to = wt;
        }
    }
    g_xhl_state = state;
    g_xhl_win.store(win_to);
    g_xhl_line.store(line_to);
    g_said_state = -2;
    const bool was = g_xhl.exchange(xhl);
    if (xhl)
        logf(KILN_LOG_INFO, "ui: the main menu's footer: XHL-Core's footer line is at %s; Kiln's line names XHL-Core, and "
                            "XHL-Core's line shows %s (%s)", w,
             state ? "while XHL-Core reports a problem" : "as XHL-Core draws it: its state was not found",
             win_to ? "it takes clicks" : g_win_file ? "XHL-Core did not patch the window call: no clicks"
                                                     : "no clicks: the window call is not Kiln's");
    else if (was)
        logf(KILN_LOG_INFO, "ui: the main menu's footer: XHL-Core's footer line is gone; Kiln's line is its own again");
    if (was != xhl) set_text();
}

// Each frame Kiln's copy runs (UI thread): XHL-Core's patches (they can come late: XHL-Core installs them from its own
// thread) and whether the copy calls XHL-Core's line in this frame.
void xhl_update() {
    const uintptr_t lt = live_call(N.site), wt = live_call(N.win);
    if (lt != g_seen_line || wt != g_seen_win) {
        g_seen_line = lt;
        g_seen_win = wt;
        xhl_resolve(lt, wt);
    }
    if (!g_xhl.load(std::memory_order_relaxed)) {
        g_follow.store(false, std::memory_order_relaxed);
        return;
    }
    int32_t st = 0;
    const bool known = g_xhl_state && copy_guarded(&st, g_xhl_state, 4);
    const bool follow = menu_footer::follow_xhl(true, known, st);
    g_follow.store(follow, std::memory_order_relaxed);
    const int said = known ? (st != 0 ? 1 : 0) : -1;
    if (said != g_said_state && g_xhl_state) {
        g_said_state = said;
        logf(KILN_LOG_INFO, "ui: the main menu's footer: XHL-Core %s", said == 1   ? "reports a problem: its red line shows under Kiln's"
                                                                       : said == 0 ? "reports no problem: Kiln's line only"
                                                                                   : "state unreadable: its line shows");
    }
}

uint8_t* field(uint8_t* obj, int64_t f) { return obj + (f - N.text); }

// Kiln's line: built and drawn as the game builds its build-number line (same style, mode, alignment, colour alpha,
// flags and size), right before it. No C++ objects in this frame (__try).
bool draw_line(const GameText* t, const void* style, uint32_t mode, float rect[4]) {
    __try {
        alignas(16) uint8_t obj[0x200];  // the game's text object is 0x150 bytes (its constructor writes through +0x140)
        std::memset(obj, 0, sizeof obj);
        reinterpret_cast<LineFn>(N.line)(obj, t, style, mode);
        void* node = *reinterpret_cast<void**>(field(obj, N.node));
        *field(obj, N.shown) = 1;
        const uint16_t align = 0x301;  // the game's footer lines' alignment (the pattern holds the same constant)
        reinterpret_cast<AlignFn>(N.align)(node, &align);
        uint32_t c = 0;
        std::memcpy(&c, field(obj, N.color), 4);
        c = (c & 0xFFFFFFu) | 0x7F000000u;
        std::memcpy(field(obj, N.color), &c, 4);
        *field(obj, N.bits) |= 1;
        uint16_t f = 0;
        std::memcpy(&f, field(obj, N.flags), 2);
        f |= 0x48;
        std::memcpy(field(obj, N.flags), &f, 2);
        const uint16_t zero = 0;
        std::memcpy(field(obj, N.flags2), &zero, 2);
        std::memcpy(&c, field(obj, N.color2), 4);
        c = (c & 0xFFFFFFu) | 0x7F000000u;
        std::memcpy(field(obj, N.color2), &c, 4);
        const uintptr_t ctx = *reinterpret_cast<const uintptr_t*>(N.ui_app);
        const uintptr_t cfg = *reinterpret_cast<const uintptr_t*>(ctx + N.config);
        std::memcpy(field(obj, N.size), reinterpret_cast<const void*>(cfg + N.font), 4);
        reinterpret_cast<DrawFn>(N.draw)(obj);
        if (N.rect && node) {
            const float* r = reinterpret_cast<RectFn>(N.rect)(node);
            for (int i = 0; i < 4; ++i) rect[i] = r[i];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* __fastcall line_thunk(void* out, const GameText* text, const void* style, uint32_t mode) {
    const std::string* s = g_text.load();
    if (s && !s->empty() && !g_failed.load(std::memory_order_relaxed)) {
        const GameText t{s->data(), s->size(), g_text_fnv.load()};
        float r[4] = {NAN, NAN, NAN, NAN};
        if (draw_line(&t, style, mode, r)) {
            std::lock_guard lk(g_place_mu);
            for (int i = 0; i < 4; ++i) g_rect[i] = r[i];
            g_drawn_frame = game_frames() + 1;  // the frame being built
        } else if (!g_failed.exchange(true)) {
            g_footer_demand.store(0, std::memory_order_relaxed);
            logf(KILN_LOG_ERROR, "ui: Kiln's line in the main menu's footer faulted; the game's footer is drawn as the game "
                                 "draws it (with another mod's line where one is installed) for the rest of this game");
        }
    }
    // the build number: through XHL-Core's line first while Kiln follows XHL-Core (it draws its line, then calls the game's)
    const uintptr_t x = g_follow.load(std::memory_order_relaxed) ? g_xhl_line.load(std::memory_order_relaxed) : 0;
    return reinterpret_cast<LineFn>(x ? x : N.line)(out, text, style, mode);
}

void __fastcall hk_footer() {
    const auto take = menu_footer::take(g_copy != nullptr, g_shown.load(std::memory_order_relaxed),
                                        g_mods.load(std::memory_order_relaxed), g_failed.load(std::memory_order_relaxed));
    if (take == menu_footer::Take::copy && live_check() && g_copy) {
        xhl_update();
        g_copy();
        return;
    }
    g_hook.call<void>();
}

bool sym(const char* n, uintptr_t* out) { return g_api->symbol(KilnStr{n, std::strlen(n)}, out) == KILN_OK && *out; }
bool cst(const char* n, int64_t* out) { return g_api->constant(KilnStr{n, std::strlen(n)}, out) == KILN_OK; }
bool feat(const char* n) { return g_api->feature(KilnStr{n, std::strlen(n)}); }

}  // namespace

bool footer_install() {
    const KilnStr v = g_api->version();
    g_version = menu_footer::release_version(std::string_view(v.ptr ? v.ptr : "", v.ptr ? v.len : 0));
    const bool names = feat("ui.menu_footer") && sym("ui/MainMenuFooter", &N.fn) && sym("ui/MainMenuFooterBuildLine", &N.site) &&
                       sym("ui/NativeTextLine", &N.line) && sym("ui/NativeTextDraw", &N.draw) && sym("ui/UiNodeAlign", &N.align) &&
                       sym("ui/UiApp", &N.ui_app) && cst("ui/ui.config", &N.config) && cst("ui/cfg.footer_font", &N.font) &&
                       cst("ui/footer.text", &N.text) && cst("ui/footer.text_node", &N.node) &&
                       cst("ui/footer.text_shown", &N.shown) && cst("ui/footer.text_color", &N.color) &&
                       cst("ui/footer.text_flags", &N.flags) && cst("ui/footer.text_bits", &N.bits) &&
                       cst("ui/footer.text_color2", &N.color2) && cst("ui/footer.text_flags2", &N.flags2) &&
                       cst("ui/footer.text_size", &N.size);
    if (!names) {
        logf(KILN_LOG_INFO, "ui: the main menu's footer has no Kiln line on this game build (feature ui.menu_footer off)");
        return false;
    }
    // the fields lie inside the game's 0x150-byte text object at rbp+footer.text
    for (int64_t f : {N.node, N.shown, N.color, N.flags, N.bits, N.color2, N.flags2, N.size})
        if (f < N.text || f + 8 > N.text + 0x150) {
            logf(KILN_LOG_WARN, "ui: the main menu's footer has no Kiln line: the text object's fields are not where expected");
            return false;
        }
    // the line's place for the mark (optional): UiNodeRect and the canvas metrics
    if (!sym("ui/UiNodeRect", &N.rect)) N.rect = 0;
    N.metrics = cst("ui/sys.frame_arg", &N.frame_arg) && cst("ui/sys.viewport", &N.viewport) && cst("ui/sys.px_per_unit", &N.ppu);
    std::string why;
    if (!make_copy(&why)) {
        logf(KILN_LOG_WARN, "ui: the main menu's footer has no Kiln line: %s", why.c_str());
        return false;
    }
    g_hook.install(g_api, g_owner, N.fn, reinterpret_cast<void*>(&hk_footer), "ui/MainMenuFooter");
    if (!g_hook) {
        logf(KILN_LOG_WARN, "ui: the main menu's footer has no Kiln line: hooking the footer failed (%s)",
             g_hook.error() ? g_hook.error() : "no reason given");
        g_copy = nullptr;
        return false;
    }
    if (overlay_drawing_on() && N.rect && N.metrics)
        g_mark = image_load(g_owner, menu_footer::kMarkPng, sizeof menu_footer::kMarkPng);
    logf(KILN_LOG_INFO, "ui: the main menu's footer: Kiln's line goes above the build number (Kiln's copy of the footer, %u "
                        "bytes; mark %s)", g_size, g_mark ? "by the overlay" : "off: no overlay drawing or no layout read");
    return true;
}

void footer_set(int32_t mods, uint32_t flags) {
    const bool shown = (flags & KILN_UI_FOOTER_SHOWN) != 0;
    const int64_t m = mods < 0 ? -1 : mods;
    const bool was = g_shown.exchange(shown);
    const int64_t before = g_mods.exchange(m);
    if (was != shown && g_copy)
        logf(KILN_LOG_INFO, "ui: Kiln's line in the main menu's footer: %s", shown ? "shown" : "hidden (Kiln setting mainMenuFooter)");
    if (before != m || was != shown) set_text();
    g_footer_demand.store(shown && m >= 0 && g_copy && g_mark && !g_failed.load() ? 1u : 0u, std::memory_order_relaxed);
}

void footer_texts_changed() { set_text(); }

void footer_after_frame(uintptr_t sys10) {
    if (!g_mark || !N.metrics || !sys10) return;
    uint64_t drawn = 0;
    {
        std::lock_guard lk(g_place_mu);
        drawn = g_drawn_frame;
    }
    if (drawn != game_frames()) return;  // not drawn in the frame that just finished
    const uintptr_t sys = sys10 - static_cast<uintptr_t>(N.frame_arg);
    float vp[4] = {0, 0, 0, 0}, ppu = 0;
    if (!copy_guarded(vp, sys + N.viewport, sizeof vp) || !copy_guarded(&ppu, sys + N.ppu, 4)) return;
    std::lock_guard lk(g_place_mu);
    g_vx = vp[0], g_vy = vp[1], g_ppu = ppu;
}

void footer_overlay() {
    if (!g_mark || !g_shown.load(std::memory_order_relaxed) || g_failed.load(std::memory_order_relaxed)) return;
    float r[4];
    float vx, vy, ppu;
    uint64_t drawn;
    {
        std::lock_guard lk(g_place_mu);
        for (int i = 0; i < 4; ++i) r[i] = g_rect[i];
        vx = g_vx, vy = g_vy, ppu = g_ppu;
        drawn = g_drawn_frame;
    }
    if (!menu_footer::recent(drawn, game_frames())) return;
    menu_footer::Box b;
    if (!menu_footer::mark_box(r, vx, vy, ppu, &b)) return;
    ImTextureRef tex;
    if (!image_texture(g_mark, &tex)) return;
    if (!g_said_rect.exchange(true))
        logf(KILN_LOG_INFO, "ui: the main menu's footer: Kiln's line at %.0f, %.0f (%.0f x %.0f UI units), its mark at "
                            "%.0f, %.0f, %.0f px", r[0], r[1], r[2], r[3], b.x0, b.y0, b.x1 - b.x0);
    ImGui::GetBackgroundDrawList()->AddImage(tex, ImVec2(b.x0, b.y0), ImVec2(b.x1, b.y1), ImVec2(0, 0), ImVec2(1, 1),
                                             IM_COL32(255, 255, 255, 0xC0));
}

}  // namespace kui

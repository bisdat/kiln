// SPDX-License-Identifier: MIT
// Storage chest -> footer -> action row. Draw a native entry before the row closes, not a vanilla input action.
#include <array>
#include "ui_values_internal.h"

namespace kiln::uivalues {
namespace {
using ChestWindowFn = void (*)(uint8_t);
using ChestFooterFn = void (*)(void*, uint16_t, void*, uint8_t, uint8_t);
ChestWindowFn g_chest_window_next = nullptr;
ChestFooterFn g_chest_footer_next = nullptr;
struct ChestLayout {
    uintptr_t app = 0, entry = 0;
    int64_t data = 0, play = 0, ui_state = 0, hud = 0, target = 0;
} g_chest_layout;
std::atomic<bool> g_chest_active{false};
constexpr size_t kChestButtons = 4;
struct ChestScope {
    KilnUiChestPress press{};
    unsigned footer_depth = 0;
    bool drawn = false;
    std::array<std::shared_ptr<Provider>, kChestButtons> pressed;
    size_t count = 0;
};
thread_local ChestScope* t_chest = nullptr;

bool read_chest(KilnUiChestPress* p) {
    ClientNow now{};
    uintptr_t app = 0, data = 0, state = 0, play = 0;
    uint8_t ui = 0, hud = 0;
    const auto& l = g_chest_layout;
    if (!env().client || !env().client(&now) || !now.session || !now.local_player ||
        !game_read(l.app, &app) || !game_read(app + l.data, &data) || !game_read(data, &state) ||
        !game_read(state + l.ui_state, &ui) || ui != KILN_UI_STATE_PLAY ||
        !game_read(state + l.play, &play) || !game_read(play + l.hud, &hud) || hud != 22 ||
        !game_read(play + l.target, &p->container) || !p->container) return false;
    p->size = sizeof *p; p->session = now.session; p->local_player = now.local_player;
    KilnUiScreen screen{};
    if (env().screen && env().screen(&screen)) p->frame = screen.frame + 1;
    return true;
}

void hk_chest_window(uint8_t mode) {
    if (!g_chest_active.load(std::memory_order_acquire) || !any(kListChest)) {
        g_chest_window_next(mode); return;
    }
    ChestScope scope;
    auto* previous = t_chest;
    t_chest = mode == 0 && read_chest(&scope.press) ? &scope : nullptr;
    struct Restore { ChestScope* previous; ~Restore() { t_chest = previous; } } restore{previous};
    g_chest_window_next(mode);
    t_chest = previous; // callbacks run after the native chest builder, outside its layout/input scopes
    for (size_t i = 0; i < scope.count; ++i) {
        auto& p = scope.pressed[i];
        ClientNow now{};
        if (!env().client || !env().client(&now) || now.session != scope.press.session ||
            now.local_player != scope.press.local_player) break;
        if (!provider_live(*p)) continue;
        KilnResult rc = KILN_ERR_INTERNAL;
        if (!env().call_chest || !env().call_chest(reinterpret_cast<KilnUiChestFn>(p->fn), &scope.press, p->user, &rc) ||
            rc == KILN_ERR_INTERNAL) fault(*p, "native chest button");
        else if (rc == KILN_OK) succeeded(*p);
    }
}

void hk_chest_footer(void* span, uint16_t alignment, void* padding, uint8_t background, uint8_t stretch) {
    ChestScope* scope = t_chest;
    if (scope) ++scope->footer_depth;
    struct Restore { ChestScope* scope; ~Restore() { if (scope) --scope->footer_depth; } } restore{scope};
    g_chest_footer_next(span, alignment, padding, background, stretch);
}

void KILN_CALL on_chest_row_end(KilnRegisters*, void*) {
    auto* scope = t_chest;
    if (!g_chest_active.load(std::memory_order_acquire) || !any(kListChest) || !scope ||
        scope->footer_depth != 1 || scope->drawn) return;
    scope->drawn = true;
    try {
        const auto snapshot = providers(kListChest);
        for (const auto& p : *snapshot) {
            if (!provider_live(*p)) continue;
            text::GameText caption{};
            if (!text::game_text(p->label, &caption)) continue;
            bool pressed = false;
            if (!env().draw_chest || !env().draw_chest(g_chest_layout.entry, &caption, &pressed)) {
                disable_kind(kListChest, "native chest entry renderer faulted"); return;
            }
            if (pressed && scope->count < scope->pressed.size()) scope->pressed[scope->count++] = p;
        }
    } catch (...) { disable_kind(kListChest, "native chest row failed"); }
}

bool install_chest(uint32_t) {
    uintptr_t window = 0, footer = 0, row = 0, end = 0;
    ChestLayout l;
    if (!sig_symbol("ui/ChestWindow", &window) || !sig_symbol("ui/ChestFooter", &footer) ||
        !sig_symbol("ui/ActionRow", &row) || !sig_symbol("ui/ActionRowEnd", &end) || end <= row || end - row >= 0x400 ||
        !sig_symbol("ui/SystemMenuEntry", &l.entry) || !sig_symbol("ui/UiApp", &l.app) ||
        !sig_constant("ui/ui.data", &l.data) || !sig_constant("ui/state.play", &l.play) ||
        !sig_constant("ui/state.ui_state", &l.ui_state) || !sig_constant("ui/play.hud_screen", &l.hud) ||
        !sig_constant("ui/play.target", &l.target) || l.data < 0 || l.play < 0 || l.ui_state < 0 || l.hud < 0 || l.target < 0)
        return false;
    g_chest_layout = l;
    auto rc = env().hook_raw(window, "ui.native/ChestWindow", 0, reinterpret_cast<void*>(&hk_chest_window),
                             reinterpret_cast<void**>(&g_chest_window_next));
    if (rc == KILN_OK) rc = env().hook_raw(footer, "ui.native/ChestFooter", 0, reinterpret_cast<void*>(&hk_chest_footer),
                                          reinterpret_cast<void**>(&g_chest_footer_next));
    if (rc == KILN_OK) rc = env().hook_mid(end, "ui.native/ChestRowEnd", &on_chest_row_end);
    const bool ok = rc == KILN_OK && g_chest_window_next && g_chest_footer_next;
    g_chest_active.store(ok, std::memory_order_release);
    return ok;
}
struct ChestInstaller { ChestInstaller() { set_installer(kListChest, &install_chest); } } g_chest_installer;

KilnResult KILN_CALL chest_subscribe(KilnOwner owner, KilnStr caption, KilnUiChestFn fn, void* user, KilnUiRegistration* out) {
    if (!out || out->size < sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    out->ok = out->reason = out->reserved = 0; out->handle = 0;
    if (!owner || !fn || !caption.ptr || !caption.len || caption.len > 128 ||
        !text::valid_utf8(std::string_view(caption.ptr, caption.len))) return KILN_ERR_INVALID_ARGUMENT;
    if (!env().client_side || !env().client_side()) return KILN_ERR_UNAVAILABLE;
    if (!env().owner_alive(owner)) return KILN_ERR_REVOKED;
    try {
        if (!ensure_installed(kListChest)) return KILN_ERR_UNAVAILABLE;
        return add(owner, kListChest, 0, reinterpret_cast<void*>(fn), user, out, 0,
                   std::string_view(caption.ptr, caption.len));
    } catch (...) { return KILN_ERR_INTERNAL; }
}
KilnResult KILN_CALL chest_unsubscribe(KilnHandle h) {
    try { return remove(h); } catch (...) { return KILN_ERR_INTERNAL; }
}
const KilnUiChestApi g_chest_api = {{sizeof(KilnUiChestApi), KILN_UI_CHEST_VERSION}, &chest_subscribe, &chest_unsubscribe};
} // namespace
const KilnUiChestApi* chest_table() { return &g_chest_api; }
} // namespace kiln::uivalues

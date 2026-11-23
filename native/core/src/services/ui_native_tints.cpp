// SPDX-License-Identifier: MIT
// The game's SlotAppearance copies descriptor+0x48's {ABGR, strength} to generated options+0x30.
// Change only a private descriptor copy before calling on: no borrowed memory, persistent game writes or overlay.
// Static contract: docs/re/game-ui.md, client 5d9ef2f1; ensure_installed refuses every unmeasured layout.
#include <array>
#include <cstring>
#include "ui_values_internal.h"

namespace kiln::uivalues {
namespace {
using TintOptionsFn = void (*)(void*, void*, void*, void*);
TintOptionsFn g_tint_next = nullptr;
ViewLayout g_tint_layout{};
thread_local bool t_tint = false;

uint32_t tint_swap(uint32_t n) {
    return (n >> 24) | ((n >> 8) & 0xff00) | ((n << 8) & 0xff0000) | (n << 24);
}

void hk_tint_options(void* out, void* descriptor, void* state, void* context) {
    alignas(16) std::array<uint8_t, 0x90> copy{};
    uint8_t raw[kViewBytes]{};
    uintptr_t view = 0;
    if (t_tint || !any(kListTints) || !env().call_tint ||
        !env().read(copy.data(), reinterpret_cast<uintptr_t>(descriptor), copy.size()) ||
        !game_read(reinterpret_cast<uintptr_t>(descriptor), &view) || !env().read(raw, view, sizeof raw)) {
        g_tint_next(out, descriptor, state, context);
        return;
    }
    struct Recursion { Recursion() { t_tint = true; } ~Recursion() { t_tint = false; } } recursion;
    try {
        KilnUiItemView v{};
        complete_slot_view(&v);
        fill_view(raw, g_tint_layout, 0, &v);
        uint32_t game = 0;
        std::memcpy(&game, copy.data() + 0x48, 4);
        uint32_t rgba = tint_swap(game);
        const auto snapshot = providers(kListTints);
        for (const auto& p : *snapshot) {
            if (!provider_live(*p) || (p->item && p->item != v.item)) continue;
            uint32_t edit = rgba;
            KilnResult rc = KILN_ERR_INTERNAL;
            if (!env().call_tint(reinterpret_cast<KilnUiTintFn>(p->fn), &v, &edit, p->user, &rc) || rc == KILN_ERR_INTERNAL) {
                fault(*p, "slot tint");
                continue;
            }
            if (rc != KILN_OK) continue;
            rgba = edit;
            succeeded(*p);
        }
        game = tint_swap(rgba);
        std::memcpy(copy.data() + 0x48, &game, 4);
    } catch (...) {
        g_tint_next(out, descriptor, state, context);
        return;
    }
    g_tint_next(out, copy.data(), state, context);
}

bool install_tints(uint32_t) {
    uintptr_t site = 0;
    if (!load_view_layout(&g_tint_layout) || !sig_symbol("ui/SlotAppearance", &site)) return false;
    load_charge_layout();
    return env().hook_raw(site, "ui.native/SlotAppearance", 0, reinterpret_cast<void*>(&hk_tint_options),
                          reinterpret_cast<void**>(&g_tint_next)) == KILN_OK && g_tint_next;
}
struct TintInstaller { TintInstaller() { set_installer(kListTints, &install_tints); } } g_tint_installer;

KilnResult KILN_CALL tint_subscribe(KilnOwner owner, uint32_t item, KilnUiTintFn fn, void* user, KilnUiRegistration* out) {
    if (!out || out->size < sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    out->ok = out->reason = out->reserved = 0; out->handle = 0;
    if (!owner || !fn) return KILN_ERR_INVALID_ARGUMENT;
    try { return register_list(owner, kListTints, item, reinterpret_cast<void*>(fn), user, out); }
    catch (...) { return KILN_ERR_INTERNAL; }
}
KilnResult KILN_CALL tint_unsubscribe(KilnHandle h) {
    try { return remove(h); } catch (...) { return KILN_ERR_INTERNAL; }
}
const KilnUiTintsApi g_tints_api = {{sizeof(KilnUiTintsApi), KILN_UI_TINTS_VERSION}, &tint_subscribe, &tint_unsubscribe};
} // namespace
const KilnUiTintsApi* tints_table() { return &g_tints_api; }
} // namespace kiln::uivalues

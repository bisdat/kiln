// SPDX-License-Identifier: MIT
// gems.sockets on the client (kiln_ui_values.h, gemforge_sockets): the Gem Forge's WEAPONS list and its GEMS list's
// carrier pass build their item filter on the stack with a category mask of Weapons (client 26430625: `mov dword
// [rsp+0x6c], 4` at 0xcafc81 in ui/GemForgeLists, `mov dword [rsp+0x4c], 4` at 0xd27d78 in ui/GemForgeCandidates),
// then call the filtered-view builder 0xd24a50, whose category gate is a bit test of ItemInfo's category byte
// (0xd1c341). A mid hook right after each store ORs the mods' extra category bits into that stack slot. The slot
// displacements are sigdb captures (ui/gemforge.weaponsMask, ui/gemforge.carrierMask); nothing else is written.
//
// Owners: each owner has one mask; the effective extra bits are the OR over owners minus Weapons (the game's own).
// The hooks are installed once, at the first nonzero extra bit (a game without such a mod never gets them), and stay
// installed: while the union is 0 they return without touching the stack. An install that fails reports the row off
// and leaves both hooks inert.
#include <iterator>
#include <map>
#include <mutex>

#include "ui_values_internal.h"

namespace kiln::uivalues {
namespace {

constexpr char kRow[] = "gems.sockets";
constexpr uint32_t kWeapons = 1u << 2;     // ItemCategory Weapons
constexpr uint32_t kAllowed = 0x3fffu;     // categories 0..13: never WeaponGem (15) nor anything above
constexpr int64_t kListsScope = 0x600;     // the scopes the sigdb patterns were measured in
constexpr int64_t kCandidatesScope = 0x300;

struct Sockets {
    std::mutex mu;
    std::map<KilnOwner, uint32_t> masks;
    int state = 0;  // 0 not tried, 1 installed, -1 failed
};
Sockets& sockets() {
    static auto* s = new Sockets;  // never destroyed: a game thread may run a hook while the process ends
    return *s;
}
std::atomic<uint32_t> g_extra{0};
std::atomic<uint32_t> g_seen{0};
std::atomic<bool> g_active{false};  // both hooks installed and the captured displacements valid
int64_t g_weapons_disp = 0, g_carrier_disp = 0;

void widen(const KilnRegisters* r, const int64_t& disp) {
    const uint32_t extra = g_extra.load(std::memory_order_acquire);
    if (!extra || !r || !g_active.load(std::memory_order_acquire)) return;
    const uintptr_t at = r->rsp + static_cast<uintptr_t>(disp);  // read after g_active: published before it
    uint32_t mask = 0;
    if (!env().read || !env().read(&mask, at, sizeof mask)) return;
    const uint32_t next = mask | extra;
    if (next != mask && env().write) env().write(at, &next, sizeof next);
}
void KILN_CALL on_weapons(KilnRegisters* r, void*) { widen(r, g_weapons_disp); }
void KILN_CALL on_carriers(KilnRegisters* r, void*) { widen(r, g_carrier_disp); }

// A displacement of the filter's mask dword inside the caller's frame: a positive, aligned d8.
bool disp_ok(int64_t d) { return d > 0 && d <= 0x7c && (d & 3) == 0; }

bool install_locked(Sockets& s) {
    if (s.state) return s.state > 0;
    uintptr_t lists = 0, cands = 0, weapons = 0, carriers = 0;
    int64_t wd = 0, cd = 0;
    const char* why = nullptr;
    if (!env().layout_supported || !env().layout_supported())
        why = "the Gem Forge's stack layout is not measured for this client image";
    else if (!sig_constant("ui/gemforge.weaponsMask", &wd) || !sig_constant("ui/gemforge.carrierMask", &cd) ||
             !disp_ok(wd) || !disp_ok(cd))
        why = "the Gem Forge's mask displacements are missing or out of range";
    else if (!sig_symbol("ui/GemForgeLists", &lists) || !sig_symbol("ui/GemForgeCandidates", &cands) ||
             !sig_symbol("ui/GemForgeWeaponsMask", &weapons) || !sig_symbol("ui/GemForgeCarrierMask", &carriers) ||
             weapons <= lists || weapons - lists >= static_cast<uintptr_t>(kListsScope) || carriers <= cands ||
             carriers - cands >= static_cast<uintptr_t>(kCandidatesScope))
        why = "the Gem Forge's mask sites are missing or outside their functions";
    else if (!env().hook_mid)
        why = "kiln.hooks is not here";
    if (!why) {
        g_weapons_disp = wd;
        g_carrier_disp = cd;
        KilnResult rc = env().hook_mid(weapons, "ui.values/GemForgeWeaponsMask", &on_weapons);
        if (rc == KILN_OK) rc = env().hook_mid(carriers, "ui.values/GemForgeCarrierMask", &on_carriers);
        if (rc != KILN_OK) why = "kiln.hooks refused a Gem Forge mask hook";
    }
    s.state = why ? -1 : 1;
    g_active.store(!why, std::memory_order_release);
    if (why) {
        logf(KILN_LOG_ERROR, "ui.values: gems.sockets is off for this process: %s", why);
        if (env().row_off) env().row_off(kRow, why);
        return false;
    }
    logf(KILN_LOG_INFO, "ui.values: gems.sockets hooks installed (Gem Forge masks at +0x%llx and +0x%llx)",
         static_cast<unsigned long long>(wd), static_cast<unsigned long long>(cd));
    return true;
}

// Lock held. Drops owners the core no longer knows (hosts without the retirement notification) and republishes.
void recompute_locked(Sockets& s) {
    for (auto it = s.masks.begin(); it != s.masks.end();)
        it = env().owner_alive && !env().owner_alive(it->first) ? s.masks.erase(it) : std::next(it);
    uint32_t extra = 0;
    for (const auto& [owner, mask] : s.masks) extra |= mask;
    extra &= ~kWeapons;
    const uint32_t before = g_extra.exchange(extra, std::memory_order_acq_rel);
    g_seen.fetch_or(extra, std::memory_order_acq_rel);
    if (before != extra) logf(KILN_LOG_INFO, "ui.values: Gem Forge lists widened to category mask 0x%x", extra | kWeapons);
}

}  // namespace

KilnResult sockets_set(KilnOwner owner, uint32_t mask) {
    if (!env().client_side || !env().client_side()) return KILN_ERR_UNAVAILABLE;
    if (!owner || (mask & ~kAllowed)) return KILN_ERR_INVALID_ARGUMENT;
    if (!env().owner_alive || !env().owner_alive(owner)) return KILN_ERR_REVOKED;
    Sockets& s = sockets();
    std::lock_guard lk(s.mu);
    if (!mask) {  // a removal never needs the feature
        s.masks.erase(owner);
        recompute_locked(s);
        return KILN_OK;
    }
    if (!env().row_available || !env().row_available(kRow)) return KILN_ERR_UNAVAILABLE;
    if ((mask & ~kWeapons) && !install_locked(s)) return KILN_ERR_UNAVAILABLE;
    if (!env().owner_alive(owner)) return KILN_ERR_REVOKED;  // retirement may have won meanwhile
    s.masks[owner] = mask;
    recompute_locked(s);
    return KILN_OK;
}

void sockets_owner_retired(KilnOwner owner) {
    Sockets& s = sockets();
    std::lock_guard lk(s.mu);
    if (s.masks.erase(owner)) recompute_locked(s);
}

uint32_t sockets_effective() { return g_extra.load(std::memory_order_acquire); }
uint32_t sockets_seen() { return g_seen.load(std::memory_order_acquire); }

void sockets_reset_for_tests() {
    Sockets& s = sockets();
    std::lock_guard lk(s.mu);
    s.masks.clear();
    s.state = 0;
    g_extra = 0;
    g_seen = 0;
    g_active = false;
    g_weapons_disp = g_carrier_disp = 0;
}

}  // namespace kiln::uivalues

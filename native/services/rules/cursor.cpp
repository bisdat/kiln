// SPDX-License-Identifier: MIT
// kiln.rules' build cursor waivers (feature rules.cursor; docs/re/rules.md 2.1; docs/kiln/services/rules.md "Building"):
// the bits of the cursor's refusal flags (keen::ecs::CursorFlag) a player's rules waive are cleared where the game
// stores them.
//
// rules/CursorFlags (server 0x9fb20, client 0x29bfb0) computes the flags for both cursor programs: the main checks,
// then bit 0 from the actor state, then bit 12 (MissingMaterials) from the material check, stored with `mov [rbx], edx`.
// The one mid hook (rules/CursorFlagsStored, entry + 0xfa on both executables) is on the instruction right after that
// store, which every path through the function passes; after it the game only adds bit 8 (AreaOccupied, which no rule
// waives) before it returns. rbx holds the flags pointer and rsi the cursor context there (both loaded at the entry,
// callee-saved and not written in between; rules_sigdb_test.py checks this on both executables).
//
// Not at the entry since 0.2.0-dev.31.13. Building Tools (0.34.0) looks this function up by its first 42 bytes, 4 s after
// its DLL loads, once, and then redirects the game's calls to it; Kiln's jump at the entry (installed at load) hid that
// signature, so Building Tools' "overlap objects" stayed off. Kiln now leaves the first 0xfa bytes as the game wrote them
// (they also hold the end of Building Tools' MissingCall signature, at + 0xe2 .. 0xfa), so the two work in either load
// order: Building Tools' redirected calls run the whole function, Kiln's waivers inside it included
// (docs/kiln/engines/README.md "Mods that find game code by its bytes").
//
// The body runs under a structured-exception guard: a fault counts, and the game's flags stand.
#include <windows.h>

#include <atomic>
#include <cstring>

#include "creative.h"
#include "kiln_hook.hpp"
#include "safetyhook.hpp"  // safetyhook::Context: the mid handler's register view

namespace kiln_rules_cursor {
namespace {

kiln::MidHook2 h_stored;
std::atomic<uint64_t> g_cleared{0}, g_faults{0};

void body(uint32_t* flags, void* cctx) {
    KilnRulesPlayer s;
    if (!kiln_rules_bridge::cursor_rules(cctx, &s)) return;
    const uint32_t m = waived(s);
    if (*flags & m) {
        *flags &= ~m;
        g_cleared.fetch_add(1, std::memory_order_relaxed);
    }
}

bool guarded(uint32_t* flags, void* cctx) {
    __try {
        body(flags, cctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

uint32_t waived(const KilnRulesPlayer& s) {
    const uint32_t f = s.flags;
    uint32_t m = 0;
    if (f & KILN_RULES_BUILD_OUTSIDE_BASES) m |= kOutsideZone;
    if (f & KILN_RULES_IGNORE_NO_BUILD_ZONES) m |= kLenientInhibitor | kStrictInhibitor;
    if (f & KILN_RULES_IGNORE_ENEMIES) m |= kEnemies;
    if (f & KILN_RULES_IGNORE_SHROUD) m |= kInFog;
    if (f & KILN_RULES_IGNORE_BASE_PERMISSIONS) m |= kNoPermission;
    if (s.build_cost == 0.0f) m |= kMissingMaterials;
    return m;
}

// rbx: the flags (u32*), rsi: the cursor context.
void hk_stored(safetyhook::Context& c) {
    auto* flags = reinterpret_cast<uint32_t*>(c.rbx);
    void* cctx = reinterpret_cast<void*>(c.rsi);
    if (flags && cctx && !guarded(flags, cctx)) g_faults.fetch_add(1, std::memory_order_relaxed);
}

bool install(const KilnApi* api, KilnOwner owner) {
    uintptr_t site = 0;
    constexpr char kSite[] = "rules/CursorFlagsStored";
    if (api->symbol(KilnStr{kSite, sizeof kSite - 1}, &site) != KILN_OK || !site) return false;
    if (!h_stored.install(api, owner, site, &hk_stored, kSite)) {
        kiln_rules_bridge::log(KILN_LOG_ERROR, "hooking the cursor flags' store (rules/CursorFlagsStored) failed");
        return false;
    }
    return true;
}

uint64_t cleared() { return g_cleared.load(std::memory_order_relaxed); }
uint64_t faults() { return g_faults.load(std::memory_order_relaxed); }

}  // namespace kiln_rules_cursor

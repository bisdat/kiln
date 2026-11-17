// SPDX-License-Identifier: MIT
// ui.gemforge.sources (0.2.0-dev.29.15, GitHub issue 61): chest gems a server offered to this player, in the game's own
// GEMS grid at the Shroud Forge (kiln_ui_values.h gemforge_sources). Static evidence, client 26430625
// (docs/re/gems-forge.md, "Chest gems in the GEMS grid"):
//  - the grid ui/GemForgeGrid (0xcaefc0) draws every 0x48-byte view of the array its second argument names (it reads the
//    header's data and count only) and stores a clicked row's first 8 bytes, its InventorySlotId {page entity, slot},
//    at state+0x10;
//  - the next frame ui/GemForgeMenu resolves that key only in the local model's main views; both paths meet at
//    ui/GemForgeSelectResolve (0xcacfa4) with the selected view at rbp-0x20 (item at rbp-0x18) and rdi = state;
//  - the action panel ui/GemForgeActions (0xd23f90) tests the price against the material summed over the player's
//    Generic views: ui/GemForgeAffordSlot (0xd243dc) and ui/GemForgeAffordUpgrade (0xd246cc), esi = have, r15d = cost,
//    r10d = material, r13 = state;
//  - the action's UI command carries the selected view's key, and the command processor writes it into
//    PlayerInput.weaponGemAction.weaponGem: the server's handler sees the chest's real slot.
// Kiln appends the offered rows to the GEMS array (a copy; the game's array is untouched), fills the selected view for
// a key the model does not hold, and adds what the offer's payers hold to the price test. The hooks go in with the first
// rows a server sent (a client of a server that never offers stays vanilla) after each site's bytes were compared with
// the measured instructions; while no row is live every hook returns at once. The server decides everything.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "ui_values_internal.h"

namespace kiln::uivalues {
namespace {

constexpr char kSrcRow[] = "ui.gemforge.sources";
constexpr uint32_t kSrcMaxRows = 256, kSrcMaxPayers = 64, kSrcMaxArray = 4096;
constexpr int32_t kSrcGridPriority = -10;  // after ui.gemforge.inspect's link (priority 0): it captures the game's own list
using SrcGridFn = void (*)(void*, void*, void*, void*, float, uint8_t);
using SrcView = std::array<uint8_t, kViewBytes>;

struct SrcRow {
    KilnOwner owner = 0;
    KilnUiGemForgeSource s{};
};
struct SrcBook {
    std::mutex mu;
    std::vector<SrcRow> rows;
    std::vector<std::pair<KilnOwner, KilnUiGemForgePayer>> payers;
    uint64_t session = 0;     // the client world the rows were set in
    uint32_t player = 0;
    int state = 0;            // 0 not tried, 1 installed, -1 off
    char why[128] = "";
    std::vector<uint32_t> wanted;  // materials the price test asked about that no payer row covers (at most 8)
};
SrcBook& src_book() {
    static auto* s = new SrcBook;  // never destroyed: a game thread may run a hook while the process ends
    return *s;
}
std::atomic<bool> g_src_active{false};   // installed and not off
std::atomic<uint32_t> g_src_live{0};     // rows set (the hooks' first, lock-free test)
std::atomic<uint64_t> g_injected{0}, g_resolved{0}, g_priced{0};
SrcGridFn g_src_grid_next = nullptr;
thread_local std::vector<uint8_t> t_src_buf;

bool src_row_valid(const KilnUiGemForgeSource& r) {
    return r.size >= sizeof(KilnUiGemForgeSource) && r.page != 0 && r.slot <= 7 && r.item != 0 && r.pide != 0 &&
           r.reserved0 == 0 && r.reserved1 == 0 && r.reserved2 == 0;
}
bool src_payer_valid(const KilnUiGemForgePayer& p) {
    return p.size >= sizeof(KilnUiGemForgePayer) && p.group != 0 && p.item != 0 && !p.reserved[0] && !p.reserved[1];
}

// The view the game would hold for the gem in that slot: key {page, slot}, item, count 1, item entity, level, rarity.
SrcView src_view_of(const KilnUiGemForgeSource& r) {
    SrcView v{};
    std::memcpy(v.data() + 0x00, &r.page, 4);
    v[0x04] = r.slot;
    std::memcpy(v.data() + 0x08, &r.item, 4);
    const uint32_t one = 1;
    std::memcpy(v.data() + 0x10, &one, 4);
    std::memcpy(v.data() + 0x14, &r.pide, 4);
    std::memcpy(v.data() + 0x24, &r.level, 4);
    v[0x2c] = r.rarity;
    return v;
}

// Lock held: the rows are of this client world and live; drops them otherwise. The live rows' count.
uint32_t src_prune_locked(SrcBook& s) {
    ClientNow now{};
    const bool here = env().client && env().client(&now) && now.local_player && now.session == s.session &&
                      now.local_player == s.player;
    const uint64_t ms = env().tick_ms ? env().tick_ms() : 0;
    for (auto it = s.rows.begin(); it != s.rows.end();)
        it = !here || it->s.expires_ms < ms || (env().owner_alive && !env().owner_alive(it->owner)) ? s.rows.erase(it) : std::next(it);
    if (s.rows.empty()) s.payers.clear();
    g_src_live.store(static_cast<uint32_t>(s.rows.size()), std::memory_order_release);
    return static_cast<uint32_t>(s.rows.size());
}

// The live row for a key; false when none. Takes the lock.
bool src_find_row(uint32_t page, uint8_t slot, KilnUiGemForgeSource* out) {
    SrcBook& s = src_book();
    std::lock_guard lk(s.mu);
    if (!src_prune_locked(s)) return false;
    for (const SrcRow& r : s.rows)
        if (r.s.page == page && r.s.slot == slot) {
            *out = r.s;
            return true;
        }
    return false;
}

// ---- the hooks ----
// The array the grid should draw: the game's (`header`), or a copy with the live chest rows appended in `mine`.
void* src_grid_array(void* header, uint64_t* mine) {
    std::vector<SrcView> extra;
    {
        SrcBook& s = src_book();
        std::lock_guard lk(s.mu);
        if (src_prune_locked(s))
            for (const SrcRow& r : s.rows) extra.push_back(src_view_of(r.s));
    }
    uint64_t data = 0, count = 0;
    const auto h = reinterpret_cast<uintptr_t>(header);
    if (extra.empty() || !env().read || !env().read(&data, h, 8) || !env().read(&count, h + 8, 8) || count > kSrcMaxArray ||
        (count && !data))
        return header;
    std::vector<uint8_t>& buf = t_src_buf;
    buf.assign(static_cast<size_t>(count + extra.size()) * kViewBytes, 0);
    if (count && !env().read(buf.data(), static_cast<uintptr_t>(data), static_cast<size_t>(count) * kViewBytes)) return header;
    uint64_t n = count;
    for (const SrcView& v : extra) {
        bool dup = false;  // the player's own list holds this key already: never twice
        for (uint64_t i = 0; i < count && !dup; ++i) dup = std::memcmp(buf.data() + i * kViewBytes, v.data(), 5) == 0;
        if (dup) continue;
        std::memcpy(buf.data() + n * kViewBytes, v.data(), kViewBytes);
        ++n;
    }
    if (n == count) return header;
    // a header of our own for the copy: data, count, capacity; no grow callback (the grid only reads data and count)
    mine[0] = reinterpret_cast<uint64_t>(buf.data());
    mine[1] = n;
    mine[2] = n;
    g_injected.fetch_add(1, std::memory_order_relaxed);
    return mine;
}

void hk_src_grid(void* state, void* header, void* rect, void* actions, float lower, uint8_t weapons) {
    if (weapons || !g_src_active.load(std::memory_order_acquire) || !g_src_live.load(std::memory_order_acquire))
        return g_src_grid_next(state, header, rect, actions, lower, weapons);
    alignas(16) uint64_t mine[8] = {};
    void* use = header;
    try {
        use = src_grid_array(header, mine);
    } catch (...) {  // an allocation failed: the game's own list, exactly once
        use = header;
    }
    g_src_grid_next(state, use, rect, actions, lower, weapons);
}

// The selected-row resolve: the key at state+0x10 found no view in the player's model (item 0 at rbp-0x18).
void KILN_CALL on_resolve(KilnRegisters* r, void*) {
    if (!r || !g_src_active.load(std::memory_order_acquire) || !g_src_live.load(std::memory_order_acquire)) return;
    uint64_t key = 0;
    uint32_t item = 0;
    if (!env().read(&key, r->rdi + 0x10, 8) || !env().read(&item, r->rbp - 0x18, 4) || item != 0) return;
    const auto page = static_cast<uint32_t>(key);
    const auto slot = static_cast<uint8_t>(key >> 32);
    KilnUiGemForgeSource row{};
    if (!page || !src_find_row(page, slot, &row)) return;
    const SrcView v = src_view_of(row);
    if (!env().write || !env().write(r->rbp - 0x20, v.data(), v.size())) return;
    r->rcx = row.item;  // as the found path leaves it: the item the next call looks up
    g_resolved.fetch_add(1, std::memory_order_relaxed);
}

// A price test: esi = what the player's pages hold of r10d, r15d the cost; r13 = state (the selected key at +0x10).
void KILN_CALL on_price(KilnRegisters* r, void*) {
    if (!r || !g_src_active.load(std::memory_order_acquire) || !g_src_live.load(std::memory_order_acquire)) return;
    uint64_t key = 0;
    if (!env().read(&key, r->r13 + 0x10, 8)) return;
    KilnUiGemForgeSource row{};
    if (!src_find_row(static_cast<uint32_t>(key), static_cast<uint8_t>(key >> 32), &row) || !row.group) return;
    const auto material = static_cast<uint32_t>(r->r10);
    SrcBook& s = src_book();
    std::lock_guard lk(s.mu);
    for (const auto& [owner, p] : s.payers)
        if (p.group == row.group && p.item == material) {
            const uint64_t have = (p.player_first ? static_cast<uint32_t>(r->rsi) : 0ull) + p.count;
            r->rsi = static_cast<uint32_t>(std::min<uint64_t>(have, 0x7fffffffu));
            g_priced.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    if (s.wanted.size() < 8 && std::find(s.wanted.begin(), s.wanted.end(), material) == s.wanted.end()) s.wanted.push_back(material);
}

// The measured instruction bytes at each site (client 26430625): a site that differs (another build, another detour)
// leaves the feature off.
struct SrcSite {
    const char* symbol;
    std::array<uint8_t, 12> bytes;
    size_t n;
};
const SrcSite kSrcSites[] = {
    {"ui/GemForgeSelectResolve", {0x48, 0x8B, 0x05}, 3},  // mov rax, [rip+UiApp] (the meeting point)
    {"ui/GemForgeAffordSlot", {0x41, 0x3B, 0xF7, 0x0F, 0x93, 0x44, 0x24, 0x51}, 8},                  // cmp esi,r15d; setae [rsp+0x51]
    {"ui/GemForgeAffordUpgrade", {0x45, 0x3B, 0x66, 0x08, 0x73, 0x09, 0x41, 0x3B, 0xF7, 0x72, 0x04}, 11},  // cmp r12d,[r14+8]; jae; cmp esi,r15d; jb
};
// The resolve loop's view copy right before the meeting point: the selected view at rbp-0x20, its item at rbp-0x18,
// the key read from state+0x10 (the bytes the mid hook relies on).
constexpr uint8_t kSrcResolveTail[] = {0xF2, 0x0F, 0x10, 0x42, 0x40, 0xF2, 0x0F, 0x11, 0x45, 0x20};

bool src_site_ok(uintptr_t at, const uint8_t* want, size_t n) {
    uint8_t got[16]{};
    return at && n <= sizeof got && env().read && env().read(got, at, n) && std::memcmp(got, want, n) == 0;
}

void src_off_locked(SrcBook& s, const char* why) {
    s.state = -1;
    std::snprintf(s.why, sizeof s.why, "%s", why);
    g_src_active.store(false, std::memory_order_release);
    logf(KILN_LOG_ERROR, "ui.values: ui.gemforge.sources is off for this process: %s (the forge stays vanilla)", why);
    if (env().row_off) env().row_off(kSrcRow, why);
}

bool src_install_locked(SrcBook& s) {
    if (s.state) return s.state > 0;
    uintptr_t grid = 0, resolve = 0, slot = 0, upgrade = 0, menu = 0, actions = 0;
    if (!env().layout_supported || !env().layout_supported()) return src_off_locked(s, "the Gem Forge's layout is not measured for this client image"), false;
    if (!sig_symbol("ui/GemForgeGrid", &grid) || !sig_symbol("ui/GemForgeSelectResolve", &resolve) ||
        !sig_symbol("ui/GemForgeAffordSlot", &slot) || !sig_symbol("ui/GemForgeAffordUpgrade", &upgrade) ||
        !sig_symbol("ui/GemForgeMenu", &menu) || !sig_symbol("ui/GemForgeActions", &actions))
        return src_off_locked(s, "a Gem Forge site did not resolve"), false;
    if (resolve <= menu || resolve - menu >= 8192 || slot <= actions || slot - actions >= 2400 || upgrade <= actions ||
        upgrade - actions >= 2400)
        return src_off_locked(s, "a Gem Forge site is outside its function"), false;
    int64_t stride = 0, selected = 0;
    if (!sig_constant("ui/view.stride", &stride) || stride != static_cast<int64_t>(kViewBytes) ||
        !sig_constant("ui/gemforge.selected", &selected) || selected != 0x10)
        return src_off_locked(s, "the Gem Forge's view or selection layout changed"), false;
    const uintptr_t at[] = {resolve, slot, upgrade};
    for (size_t i = 0; i < std::size(kSrcSites); ++i)
        if (!src_site_ok(at[i], kSrcSites[i].bytes.data(), kSrcSites[i].n))
            return src_off_locked(s, "a Gem Forge site's bytes are not the measured instructions (another build or another hook)"), false;
    if (!src_site_ok(resolve - sizeof kSrcResolveTail, kSrcResolveTail, sizeof kSrcResolveTail))
        return src_off_locked(s, "the selected-row resolve before the hook is not the measured copy"), false;
    // The grid's entry: untouched by anyone (sigdb's fingerprint, file bytes), or already Kiln's own (the GEMS snapshot).
    if (!gemforge_grid_hooked() && (!env().gemforge_abi || !env().gemforge_abi()))
        return src_off_locked(s, "the GEMS grid's entry is not the measured function (another build or another hook)"), false;
    if (!env().hook_raw || !env().hook_mid) return src_off_locked(s, "kiln.hooks is not here"), false;
    KilnResult rc = env().hook_raw(grid, "ui.values/GemForgeSources", kSrcGridPriority, reinterpret_cast<void*>(&hk_src_grid),
                                   reinterpret_cast<void**>(&g_src_grid_next));
    if (rc == KILN_OK && !g_src_grid_next) rc = KILN_ERR_INTERNAL;
    if (rc == KILN_OK) rc = env().hook_mid(resolve, "ui.values/GemForgeSelectResolve", &on_resolve);
    if (rc == KILN_OK) rc = env().hook_mid(slot, "ui.values/GemForgeAffordSlot", &on_price);
    if (rc == KILN_OK) rc = env().hook_mid(upgrade, "ui.values/GemForgeAffordUpgrade", &on_price);
    if (rc != KILN_OK) return src_off_locked(s, "kiln.hooks refused a Gem Forge hook"), false;  // what went in stays inert
    s.state = 1;
    g_src_active.store(true, std::memory_order_release);
    logf(KILN_LOG_INFO, "ui.values: ui.gemforge.sources hooks installed (GEMS grid, selected-row resolve, two price tests): "
         "a server offered chest gems");
    return true;
}

}  // namespace

KilnResult sources_set(KilnOwner owner, const KilnUiGemForgeSource* rows, uint32_t count, const KilnUiGemForgePayer* payers,
                       uint32_t payer_count) {
    if (!env().client_side || !env().client_side()) return KILN_ERR_UNAVAILABLE;
    if (!owner || count > kSrcMaxRows || payer_count > kSrcMaxPayers || (count && !rows) || (payer_count && !payers))
        return KILN_ERR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < count; ++i)
        if (!src_row_valid(rows[i])) return KILN_ERR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < payer_count; ++i)
        if (!src_payer_valid(payers[i])) return KILN_ERR_INVALID_ARGUMENT;
    if (!env().owner_alive || !env().owner_alive(owner)) return KILN_ERR_REVOKED;
    SrcBook& s = src_book();
    std::lock_guard lk(s.mu);
    auto drop = [&] {
        s.rows.erase(std::remove_if(s.rows.begin(), s.rows.end(), [&](const SrcRow& r) { return r.owner == owner; }), s.rows.end());
        s.payers.erase(std::remove_if(s.payers.begin(), s.payers.end(), [&](const auto& p) { return p.first == owner; }), s.payers.end());
    };
    if (!count) {  // a removal never needs the feature
        drop();
        src_prune_locked(s);
        return KILN_OK;
    }
    if (!env().row_available || !env().row_available(kSrcRow)) return KILN_ERR_UNAVAILABLE;
    if (!src_install_locked(s)) return KILN_ERR_UNAVAILABLE;
    ClientNow now{};
    if (!env().client || !env().client(&now) || !now.local_player) return KILN_ERR_UNAVAILABLE;  // no client world now
    if (now.session != s.session || now.local_player != s.player) {
        s.rows.clear();
        s.payers.clear();
        s.wanted.clear();
        s.session = now.session;
        s.player = now.local_player;
    }
    drop();
    for (uint32_t i = 0; i < count; ++i) {
        SrcRow r;
        r.owner = owner;
        r.s = rows[i];
        r.s.size = sizeof r.s;
        s.rows.push_back(r);
    }
    for (uint32_t i = 0; i < payer_count; ++i) {
        KilnUiGemForgePayer p = payers[i];
        p.size = sizeof p;
        s.payers.push_back({owner, p});
        s.wanted.erase(std::remove(s.wanted.begin(), s.wanted.end(), p.item), s.wanted.end());
    }
    src_prune_locked(s);
    return KILN_OK;
}

KilnResult sources_state(KilnUiGemForgeSourcesState* out) {
    if (!out || out->size < sizeof(KilnUiGemForgeSourcesState)) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    SrcBook& s = src_book();
    std::lock_guard lk(s.mu);
    out->installed = s.state > 0;
    out->off = s.state < 0;
    out->rows = src_prune_locked(s);
    out->wanted_count = static_cast<uint32_t>(std::min<size_t>(s.wanted.size(), 8));
    for (uint32_t i = 0; i < out->wanted_count; ++i) out->wanted[i] = s.wanted[i];
    out->injected = g_injected.load();
    out->resolved = g_resolved.load();
    out->priced = g_priced.load();
    std::snprintf(out->why, sizeof out->why, "%s", s.why);
    return KILN_OK;
}

void sources_owner_retired(KilnOwner owner) {
    SrcBook& s = src_book();
    std::lock_guard lk(s.mu);
    s.rows.erase(std::remove_if(s.rows.begin(), s.rows.end(), [&](const SrcRow& r) { return r.owner == owner; }), s.rows.end());
    s.payers.erase(std::remove_if(s.payers.begin(), s.payers.end(), [&](const auto& p) { return p.first == owner; }), s.payers.end());
    src_prune_locked(s);
}

void sources_reset_for_tests() {
    SrcBook& s = src_book();
    std::lock_guard lk(s.mu);
    s.rows.clear();
    s.payers.clear();
    s.wanted.clear();
    s.session = 0;
    s.player = 0;
    s.state = 0;
    s.why[0] = 0;
    g_src_active = false;
    g_src_live = 0;
    g_injected = g_resolved = g_priced = 0;
    g_src_grid_next = nullptr;
}

}  // namespace kiln::uivalues

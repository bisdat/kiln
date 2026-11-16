// SPDX-License-Identifier: MIT
// Read-only GEMS grid entry capture. The game's temporary pointers never leave the wrapper.
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "ui_values_internal.h"

namespace kiln::uivalues {
namespace {
constexpr uint32_t kGemMax = 4096;
constexpr size_t kGemBytes = 3 * 1024 * 1024; // copied input, output and guarded-read work together
using GemRawView = std::array<uint8_t, kViewBytes>;
using GemGridFn = void (*)(void*, void*, void*, void*, float, uint8_t);
using GemFrameFn = void (*)(void*, void*);
GemGridFn g_gem_grid_next = nullptr;
GemFrameFn g_gem_frame_next = nullptr;
ViewLayout g_gem_view;
uintptr_t g_gem_app = 0;
std::atomic<bool> g_gem_failed{false};
struct GemFrame {
    KilnUiGemForgeFrame meta{};
    std::vector<KilnUiGemForgeCandidate> rows;
};
struct GemRing {
    std::mutex mu;
    std::shared_ptr<const GemFrame> latest, previous;
    uint64_t generation = 0;
    bool drawing = false;
};
GemRing& gem_ring() { static auto* r = new GemRing; return *r; }
struct GemSelection {
    uint8_t mode = 0;
    uint32_t page = 0;
    uint8_t slot = 0;
    GemRawView weapon{};
    bool operator==(const GemSelection&) const = default;
};
struct GemCapture {
    GemFrame frame;
    ClientNow client{};
    GemSelection selection;
    uint64_t before_frame = 0;
    bool seen = false, menu_checked = false;
};
thread_local GemCapture* t_gem = nullptr;

bool gem_on() {
    return !g_gem_failed.load(std::memory_order_acquire) && env().row_available &&
        env().row_available("ui.gemforge.inspect") && env().row_available("ui.screens");
}
void gem_clear_locked(GemRing& r) {
    if (r.latest || r.previous) ++r.generation;
    r.latest.reset(); r.previous.reset();
}
// Why a drawn GEMS grid published no snapshot, once per cause and process: an open forge with no list must be
// explainable from kiln.log (the real client showed a silent refusal on 2026-11-16).
void gem_note(const char* why) {
    static std::mutex mu;
    static std::vector<const char*> said;
    {
        std::lock_guard lk(mu);
        if (std::find(said.begin(), said.end(), why) != said.end() || said.size() >= 32) return;
        said.push_back(why);
    }
    logf(KILN_LOG_INFO, "ui.values: a Gem Forge frame is unavailable: %s (said once)", why);
}
void gem_fail(const char* why) {
    g_gem_failed.store(true, std::memory_order_release);
    { auto& r = gem_ring(); std::lock_guard lk(r.mu); gem_clear_locked(r); }
    disable_kind(kListGemForge, why);
}
// Includes the final ABI records in the budget; every memory operation checks the entire address range.
struct GemReader {
    size_t remaining = kGemBytes;
    bool charge(size_t n) { if (n > remaining) return false; remaining -= n; return true; }
    static bool range(uintptr_t p, uint64_t n, size_t stride) {
        return n <= SIZE_MAX / stride && (!n || (p && p <= UINTPTR_MAX - static_cast<size_t>(n) * stride));
    }
    bool bytes(uintptr_t base, size_t off, void* out, size_t n) {
        if (!base || off > UINTPTR_MAX - base || !range(base + off, n, 1) || !charge(n)) return false;
        return env().read && env().read(out, base + off, n);
    }
    template<class T> bool get(uintptr_t base, size_t off, T& out) { return bytes(base, off, &out, sizeof out); }
};
struct GemContext { uintptr_t play = 0, model = 0; uint32_t player = 0; };
// 0: normally not in the forge; -1: broken measured contract; 1: forge and local model.
int gem_context(GemReader& rd, const ClientNow& now, GemContext& c) {
    uintptr_t app = 0, data = 0, state = 0;
    uint8_t ui = 0, hud = 0;
    if (!rd.get(g_gem_app, 0, app) || !rd.get(app, 0xd0, data) || !rd.get(data, 0, state) ||
        !rd.get(state, 0xc, ui)) return -1;
    if (ui != KILN_UI_STATE_PLAY) return 0;
    if (!rd.get(state, 0x38, c.play) || !rd.get(c.play, 0x1cd8, hud)) return -1;
    if (hud != 45) return 0;
    if (!rd.get(c.play, 0x18, c.player) || c.player != now.local_player || !c.player) return -1;
    uintptr_t models = 0; uint64_t count = 0;
    if (!rd.get(c.play, 0x3060, models) || !rd.get(c.play, 0x3068, count) || count > 64 ||
        !GemReader::range(models, count, 0x8f70)) return -1;
    // Like the game, choose the first model with the local owner. Never scan other owners' views.
    for (uint64_t i = 0; i < count; ++i) {
        const uintptr_t model = models + static_cast<size_t>(i) * 0x8f70;
        uint32_t owner = 0;
        if (!rd.get(model, 0, owner)) return -1;
        if (owner == c.player) { c.model = model; return 1; }
    }
    return -1;
}
bool gem_selection(GemReader& rd, uintptr_t state, GemSelection& s) {
    return rd.get(state, 0, s.mode) && s.mode <= 4 && rd.get(state, 0x10, s.page) &&
        rd.get(state, 0x14, s.slot) && (!s.page || s.slot < 8) && rd.get(state, 0x18, s.weapon);
}
struct GemArray { uintptr_t data = 0; uint64_t count = 0, capacity = 0; bool operator==(const GemArray&) const = default; };
bool gem_array(GemReader& rd, uintptr_t header, GemArray& a) {
    return rd.get(header, 0, a.data) && rd.get(header, 8, a.count) && rd.get(header, 16, a.capacity) &&
        a.count <= kGemMax && a.count <= a.capacity && GemReader::range(a.data, a.capacity, kViewBytes);
}
KilnUiItemView gem_view(const GemRawView& raw) {
    KilnUiItemView v{}; v.size = sizeof v;
    fill_view(raw.data(), g_gem_view, 0, &v);
    v.units_per_charge = 1; // gems and their carriers have no water-charge conversion
    return v;
}
// Kiln's catalog entry for an item: 1 known (cat set), 0 not in the catalog, -1 no catalog. An item outside the
// catalog is normal: a mod's item while its mod is not loaded stays in a save as a placeholder (inbox: a Kiln
// Equipment wand selected in the forge disabled the feature on the real client).
int gem_lookup(uint32_t id, uint8_t& cat) {
    return id && env().item_category ? env().item_category(id, &cat) : -1;
}
enum GemRows { kRowsBroken = -1, kRowsNoCatalog = 0, kRowsOk = 1 };
// The producer lists only loose WeaponGem (15) items and Weapons (2) that hold a gem (docs/re: 0xd27cb0). A row the
// catalog does not know is classified like the game did: a weapon when the model has a socket gem for its pide,
// otherwise a loose gem. A known item of another category, or a known weapon without its gem, breaks the contract.
GemRows gem_rows(GemReader& rd, const GemContext& c, const GemArray& a, GemFrame& frame) {
    uintptr_t sockets = 0; uint64_t count = 0;
    if (!rd.get(c.model, 0x6b40, sockets) || !rd.get(c.model, 0x6b48, count) || count > kGemMax ||
        !GemReader::range(sockets, count, kViewBytes) || !rd.charge(static_cast<size_t>(a.count) * sizeof(KilnUiGemForgeCandidate)))
        return kRowsBroken;
    std::unordered_map<uint32_t, KilnUiItemView> gems;
    for (uint64_t i = 0; i < count; ++i) {
        GemRawView raw{};
        if (!rd.get(sockets, static_cast<size_t>(i) * kViewBytes, raw)) return kRowsBroken;
        auto v = gem_view(raw);
        if (!v.inventory_page || v.slot_index || !gems.emplace(v.inventory_page, v).second) return kRowsBroken;
    }
    frame.rows.reserve(static_cast<size_t>(a.count));
    for (uint32_t i = 0; i < a.count; ++i) {
        GemRawView raw{};
        if (!rd.get(a.data, static_cast<size_t>(i) * kViewBytes, raw)) return kRowsBroken;
        KilnUiGemForgeCandidate row{}; row.size = sizeof row; row.index = i;
        row.source_row = gem_view(raw);
        const auto& v = row.source_row;
        if (!v.inventory_page || v.slot_index >= 8 || !v.item || !v.count) return kRowsBroken;
        uint8_t cat = 0;
        const int known = gem_lookup(v.item, cat);
        if (known < 0) return kRowsNoCatalog;
        if (known && cat != 15 && cat != 2) return kRowsBroken;
        const auto it = v.client_entity ? gems.find(v.client_entity) : gems.end();
        const bool loose = known ? cat == 15 : it == gems.end();
        if (loose) {
            row.displayed_gem = v;
        } else {
            if (!v.client_entity || it == gems.end() || it->second.count != 1 || !it->second.client_entity) return kRowsBroken;
            uint8_t gem_cat = 0;
            const int gem_known = gem_lookup(it->second.item, gem_cat);
            if (gem_known < 0) return kRowsNoCatalog;
            if (gem_known && gem_cat != 15) return kRowsBroken;
            row.has_weapon_carrier = 1; row.weapon_carrier = v; row.displayed_gem = it->second;
        }
        frame.rows.push_back(row);
    }
    return kRowsOk;
}
void gem_capture_before(GemCapture& cap, uintptr_t state, uintptr_t header, GemReader& rd,
                        GemSelection& selection, GemContext& context, GemArray& array) {
    if (cap.seen) { gem_fail("more than one GEMS grid in a frame"); return; }
    cap.seen = true;
    if (!env().client || !env().client(&cap.client) || !cap.client.local_player) { gem_note("no client world or local player"); return; }
    KilnUiScreen before{};
    if (!env().screen || !env().screen(&before)) { gem_note("no screen state"); return; }
    cap.before_frame = before.frame;
    const int in_forge = gem_context(rd, cap.client, context);
    if (!in_forge) { gem_note("the GEMS grid drew outside the Play state's forge screen"); return; }
    const GemRows rows = in_forge < 0 || !gem_selection(rd, state, selection) || !gem_array(rd, header, array)
        ? kRowsBroken : gem_rows(rd, context, array, cap.frame);
    if (rows == kRowsBroken) { gem_fail("Gem Forge list, selection or local model is unreadable or invalid"); return; }
    if (rows == kRowsNoCatalog) { // this frame stays unavailable; the next may copy
        gem_note("Kiln's item catalog could not be asked about a row"); cap.frame.rows.clear(); return;
    }
    auto& m = cap.frame.meta;
    cap.selection = selection;
    m.size = sizeof m; m.ok = 1; m.count = static_cast<uint32_t>(array.count);
    m.local_player = context.player; m.mode = selection.mode;
    m.has_selected_row = selection.page != 0;
    m.selected_page = selection.page; m.selected_index = selection.slot;
    m.selected_weapon = gem_view(selection.weapon);
    m.has_selected_weapon = m.selected_weapon.item != 0;
    if (m.has_selected_weapon) {
        uint8_t cat = 0;
        const int known = gem_lookup(m.selected_weapon.item, cat);
        if (known < 0) {
            gem_note("Kiln's item catalog could not be asked about the selected weapon");
            m = {}; cap.frame.rows.clear(); // no catalog: this frame stays unavailable
        } else if ((known && cat != 2) || !m.selected_weapon.client_entity) {
            gem_fail("Gem Forge selected weapon is incoherent"); m.ok = 0;
        }
    }
}
void hk_gem_grid(void* state, void* header, void* rect, void* actions, float lower, uint8_t weapons) {
    GemCapture* cap = t_gem;
    GemReader rd; GemSelection before; GemContext context; GemArray array;
    const bool capture = cap && !weapons && gem_on();
    if (capture) {
        try { gem_capture_before(*cap, reinterpret_cast<uintptr_t>(state), reinterpret_cast<uintptr_t>(header), rd, before, context, array); }
        catch (...) { gem_fail("Gem Forge copy could not be allocated"); }
    }
    g_gem_grid_next(state, header, rect, actions, lower, weapons); // exactly once, including every refusal
    if (capture && cap->frame.meta.ok && gem_on()) {
        GemSelection after; GemArray after_array; GemContext after_context; ClientNow now{};
        if (!gem_selection(rd, reinterpret_cast<uintptr_t>(state), after) ||
            !gem_array(rd, reinterpret_cast<uintptr_t>(header), after_array)) {
            gem_fail("Gem Forge context became unreadable");
        } else if (!(after == before)) {
            gem_note("the selection or mode changed during the GEMS grid");
            cap->frame.meta.ok = 0; cap->frame.meta.reason = KUI_STALE_FRAME;
        } else if (!(after_array == array)) {
            gem_fail("Gem Forge input array changed during its grid call");
        } else if (!env().client(&now) || now.session != cap->client.session || now.local_player != cap->client.local_player) {
            gem_note("the session or local player changed during the GEMS grid");
            cap->frame.meta.ok = 0; cap->frame.meta.reason = KUI_STALE_FRAME;
        } else {
            const int valid = gem_context(rd, now, after_context);
            if (valid < 0) gem_fail("Gem Forge local model became unreadable");
            if (valid != 1 || context.play != after_context.play || context.model != after_context.model) {
                gem_note("the forge's play state or local model changed during the GEMS grid");
                cap->frame.meta.ok = 0; cap->frame.meta.reason = KUI_STALE_FRAME;
            }
        }
    }
}
// RDI still names the selector before the menu restores its saved registers. Later grids and menu actions can
// change the selection after GEMS returns. Copy and compare here; never retain the selector pointer.
void KILN_CALL on_gem_end(KilnRegisters* r, void*) {
    GemCapture* cap = t_gem;
    if (!cap || !cap->frame.meta.ok || !gem_on()) return;
    GemReader rd; GemSelection after;
    if (!gem_selection(rd, r->rdi, after)) {
        gem_fail("Gem Forge final selection became unreadable");
    } else if (!(after == cap->selection)) {
        gem_note("the selection or mode changed after the GEMS grid, before the menu ended");
        cap->frame.meta.ok = 0; cap->frame.meta.reason = KUI_STALE_FRAME;
    } else {
        cap->menu_checked = true;
    }
}
bool gem_screen(const KilnUiScreen& s) { return s.available && s.ui_state == KILN_UI_STATE_PLAY && s.hud_screen == 45; }
bool gem_same(const GemFrame& a, const GemFrame& b) {
    // Frame stamps and C struct padding are not list identity.
    auto key = [](const KilnUiItemView& v) {
        return std::tie(v.flags, v.client_entity, v.server_entity, v.item, v.inventory_page, v.slot_index, v.count,
            v.durability, v.durability_max, v.fuel, v.fuel_max, v.units_per_charge, v.level, v.rarity);
    };
    const auto& am = a.meta; const auto& bm = b.meta;
    if (std::tie(am.local_player, am.mode, am.role, am.has_selected_row, am.has_selected_weapon, am.selected_page, am.selected_index) !=
        std::tie(bm.local_player, bm.mode, bm.role, bm.has_selected_row, bm.has_selected_weapon, bm.selected_page, bm.selected_index) ||
        key(am.selected_weapon) != key(bm.selected_weapon) || a.rows.size() != b.rows.size()) return false;
    for (size_t i = 0; i < a.rows.size(); ++i) {
        const auto& ar = a.rows[i]; const auto& br = b.rows[i];
        if (ar.index != br.index || ar.has_weapon_carrier != br.has_weapon_carrier ||
            key(ar.source_row) != key(br.source_row) || key(ar.displayed_gem) != key(br.displayed_gem) ||
            key(ar.weapon_carrier) != key(br.weapon_carrier)) return false;
    }
    return true;
}
void gem_publish(GemCapture& cap) {
    auto f = std::make_shared<GemFrame>(std::move(cap.frame));
    auto& m = f->meta;
    KilnUiScreen screen{}; ClientNow now{};
    const bool have_screen = env().screen && env().screen(&screen);
    const bool have_client = env().client && env().client(&now);
    const bool valid = gem_on() && m.ok && cap.menu_checked && have_screen && gem_screen(screen) &&
        have_client && now.session == cap.client.session && now.local_player == cap.client.local_player &&
        screen.frame == cap.before_frame + 1;
    m.size = sizeof m;
    m.session = now.session; m.frame = screen.frame;
    m.capture_ms = env().tick_ms ? env().tick_ms() : 0;
    if (!valid && cap.seen && m.ok) { // a coherent capture that publication refused: say which check
        gem_note(!gem_on() ? "the feature went off during the frame" :
                 !cap.menu_checked ? "the menu's end check did not run after the GEMS grid" :
                 !have_screen || !gem_screen(screen) ? "the screen was no longer the forge at publication" :
                 !have_client || now.session != cap.client.session || now.local_player != cap.client.local_player ?
                     "the session or local player changed before publication" :
                 "the UI frame counter did not advance by one");
    }
    if (!valid) {
        const uint32_t reason = m.reason ? m.reason : uint32_t(KUI_NO_FRAME);
        m = {}; m.size = sizeof m; m.reason = reason; m.session = now.session; m.frame = screen.frame;
        f->rows.clear();
    }
    {
        auto& r = gem_ring(); std::lock_guard lk(r.mu);
        if (!valid || !r.latest || r.latest->meta.session != m.session || !gem_same(*f, *r.latest)) {
            gem_clear_locked(r); ++r.generation;
        }
        m.generation = r.generation;
        auto stamp = [&](KilnUiItemView& v) { v.local_player = m.local_player; v.screen = screen; v.session = m.session; v.frame = m.frame; };
        if (valid) {
            if (m.has_selected_weapon) stamp(m.selected_weapon);
            for (auto& row : f->rows) { stamp(row.source_row); stamp(row.displayed_gem); if (row.has_weapon_carrier) stamp(row.weapon_carrier); }
            r.previous = std::move(r.latest); r.latest = f;
        }
        r.drawing = false;
    }
    const auto subs = providers(kListGemForge);
    for (const auto& p : *subs) {
        if (!provider_live(*p)) continue;
        if (env().call_gemforge && env().call_gemforge(reinterpret_cast<KilnUiGemForgeFn>(p->fn), &m, f->rows.data(), m.count, p->user)) succeeded(*p);
        else fault(*p, "faulted");
    }
}
void hk_gem_frame(void* arg, void* other) {
    if (t_gem) { g_gem_frame_next(arg, other); return; }
    GemCapture cap;
    { auto& r = gem_ring(); std::lock_guard lk(r.mu); r.drawing = true; }
    t_gem = &cap;
    g_gem_frame_next(arg, other);
    t_gem = nullptr;
    try { gem_publish(cap); }
    catch (...) {
        gem_fail("Gem Forge frame could not be published");
        auto& r = gem_ring(); std::lock_guard lk(r.mu); r.drawing = false;
    }
}
bool install_gemforge(uint32_t list) {
    if (list != kListGemForge || !env().row_available || !env().row_available("ui.screens") ||
        !env().gemforge_abi || !env().gemforge_abi() || !load_view_layout(&g_gem_view)) {
        logf(KILN_LOG_ERROR, "ui.values: Gem Forge screen backend, untouched entry, ABI fingerprint or call edges unavailable");
        return false;
    }
    // Exact measured contract even with sigdb policy=any. No RVAs are used as a fallback.
    const std::pair<const char*, int64_t> fields[] = {
        {"ui/ui.data", 0xd0}, {"ui/state.play", 0x38}, {"ui/state.ui_state", 0xc},
        {"ui/play.hud_screen", 0x1cd8}, {"ui/play.localEntity", 0x18}, {"ui/play.models", 0x3060},
        {"ui/play.modelsCount", 0x3068}, {"ui/model.owner", 0}, {"ui/model.stride", 0x8f70},
        {"ui/view.stride", 0x48}, {"ui/view.inventoryPage", 0}, {"ui/view.slotIndex", 4}, {"ui/view.item", 8},
        {"ui/view.count", 0x10}, {"ui/view.pide", 0x14}, {"ui/view.durability", 0x18}, {"ui/view.durabilityMax", 0x1c},
        {"ui/view.fuel", 0x20}, {"ui/view.fuelMax", 0x22}, {"ui/view.level", 0x24}, {"ui/view.rarity", 0x2c},
        {"ui/gemforge.mode", 0}, {"ui/gemforge.selected", 0x10}, {"ui/gemforge.weapon", 0x18},
        {"ui/gemforge.sockets", 0x6b40}, {"ui/gemforge.socketCount", 0x6b48},
        {"ui/gemforge.arrayData", 0}, {"ui/gemforge.arrayCount", 8}, {"ui/gemforge.arrayCapacity", 16}};
    for (const auto& [name, expected] : fields) {
        int64_t actual = 0;
        if (!sig_constant(name, &actual) || actual != expected) { logf(KILN_LOG_ERROR, "ui.values: missing or changed %s", name); return false; }
    }
    uintptr_t grid = 0, frame = 0, end = 0, unused = 0;
    for (const char* name : {"ui/GemForgeMenu", "ui/GemForgeLists", "ui/GemForgeCandidates", "ui/GemForgeSourceRet",
         "ui/GemForgeSlotRet", "ui/GemForgeScreenCall", "ui/GemForgeListsCall", "ui/GemForgeGemsCall",
         "ui/GemForgeWeaponsCall", "ui/GemForgeSwitch", "ui/GemForgeLowerBoundary", "ui/PlayStateUi"})
        if (!sig_symbol(name, &unused)) return false;
    if (!sig_symbol("ui/UiApp", &g_gem_app) || !sig_symbol("ui/GemForgeGrid", &grid) ||
        !sig_symbol("ui/GemForgeEnd", &end) || !sig_symbol("ui/GameUiFrame", &frame)) return false;
    auto rc = env().hook_raw(grid, "ui.values/GemForgeGrid", 0, reinterpret_cast<void*>(&hk_gem_grid), reinterpret_cast<void**>(&g_gem_grid_next));
    if (rc == KILN_OK) rc = env().hook_mid(end, "ui.values/GemForgeEnd", &on_gem_end);
    if (rc == KILN_OK) rc = env().hook_raw(frame, "ui.values/GemForgeFrame", 110, reinterpret_cast<void*>(&hk_gem_frame), reinterpret_cast<void**>(&g_gem_frame_next));
    return rc == KILN_OK && g_gem_grid_next && g_gem_frame_next;
}
struct GemInstaller { GemInstaller() { set_installer(kListGemForge, &install_gemforge); } } g_gem_installer;
} // namespace

KilnResult gemforge_copy(KilnOwner owner, uint64_t session, uint64_t frame, KilnUiGemForgeFrame* out,
                        KilnUiGemForgeCandidate* rows, uint32_t capacity, uint32_t* required) {
    *required = 0;
    if (!env().client_side || !env().client_side()) return KILN_ERR_UNAVAILABLE;
    if (!env().owner_alive(owner)) return KILN_ERR_REVOKED;
    if (!ensure_installed(kListGemForge) || !gem_on()) {
        auto& r = gem_ring(); std::lock_guard lk(r.mu); gem_clear_locked(r);
        return KILN_ERR_UNAVAILABLE;
    }
    const uint32_t size = out->size, stride = capacity ? rows[0].size : 0;
    if (capacity && (stride > kGemBytes || !GemReader::range(reinterpret_cast<uintptr_t>(rows), capacity, stride))) return KILN_ERR_INVALID_ARGUMENT;
    *out = {}; out->size = size; out->reason = KUI_NO_FRAME;
    std::shared_ptr<const GemFrame> pick;
    {
        auto& r = gem_ring(); std::lock_guard lk(r.mu);
        // A frame in progress is temporarily unavailable, not an invalidation of retained copies.
        if (r.drawing) { out->reason = KUI_STALE_FRAME; return KILN_OK; }
        // Sample under the frame lock so a completed publication cannot overtake these stamps.
        ClientNow now{}; KilnUiScreen screen{};
        const bool current = env().client && env().client(&now) && now.local_player && env().screen && env().screen(&screen) && gem_screen(screen);
        const uint64_t ticks = env().tick_ms ? env().tick_ms() : 0;
        if (!current || (r.latest && (r.latest->meta.session != now.session || r.latest->meta.local_player != now.local_player ||
            r.latest->meta.frame != screen.frame || ticks < r.latest->meta.capture_ms || ticks - r.latest->meta.capture_ms > 1000))) {
            gem_clear_locked(r); out->reason = KUI_STALE_FRAME; return KILN_OK;
        }
        if (!session && !frame) pick = r.latest;
        else for (const auto& f : {r.latest, r.previous}) if (f && f->meta.session == session && f->meta.frame == frame) pick = f;
        if (pick && (ticks < pick->meta.capture_ms || ticks - pick->meta.capture_ms > 1000)) {
            out->reason = KUI_STALE_FRAME;
            return KILN_OK;
        }
    }
    if (!pick) { if (session || frame) out->reason = KUI_STALE_FRAME; return KILN_OK; }
    *out = pick->meta; out->size = size; *required = out->count;
    if (capacity < out->count) return KILN_ERR_BUFFER_TOO_SMALL;
    for (size_t i = 0; i < out->count; ++i) std::memcpy(reinterpret_cast<uint8_t*>(rows) + i * stride, &pick->rows[i], sizeof(*rows));
    return KILN_OK;
}
void gemforge_reset_for_tests() {
    auto& r = gem_ring(); std::lock_guard lk(r.mu);
    r.latest.reset(); r.previous.reset(); r.generation = 0; r.drawing = false;
    g_gem_failed = false; g_gem_app = 0; g_gem_grid_next = nullptr; g_gem_frame_next = nullptr; g_gem_view = {};
}
} // namespace kiln::uivalues

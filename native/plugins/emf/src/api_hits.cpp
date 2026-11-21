// SPDX-License-Identifier: MIT
// The EmfApi table, v31 on_hit (EMF 0.6.3): the server world's hits for EMF mods, as EMF's own core gives them.
//
// EMF's contract (sdk/include/emf/emf_api.h; EMF src/core/api.cpp api_on_hit, hit_event_delivered): `cb` after the
// server world's step, on its thread, once for each hit the server applied to an entity, from the game's HitEvent:
// the final numbers, EMF_HIT_KILLED on the killing blow, health_after = the entity's health then, health_before -1.
// Not for terrain or building hits; a joined player's game sees none. Observe only (const EmfHit*): changing a hit is
// on_hit_before and the hit rules, which Kiln does not carry yet (api_v40.cpp). At most 16 callbacks; false for a null
// callback or a 17th. EMF's Lua host (1.23+) builds emf.on_hit on this function.
//
// On Kiln: the same source. kiln-emf's own relay program for HitEvent on the server world (api_events.cpp, the one
// on_event uses: a kiln.world SYSTEM program, HOST_ONLY) copies the step's HitEvents, and after the step, on the tick
// thread, each one becomes an EmfHit (hits_model.cpp) with health_after from kiln.combat's health() and weapon_item
// from the weapon entity's ItemState. The relay is registered at on_ready only when an EMF mod is loaded, and its
// gate keeps it from reading the game's queue while no on_hit callback is switched on: a game with EMF mods that never
// ask for hits pays one call per server step, a game without EMF mods nothing (kiln-emf registers nothing then).
// on_hit may be called at any time (EMF: "so a mod or a Lua script can ask for hits at any time"): before on_ready a
// registration is kept, and after it a callback joins the running relay.
// What Kiln does not give (docs/kiln/emf-parity.md, and once in the mod's log at its first on_hit): fall, shroud, cold
// and drowning damage (EMF_HIT_KIND_FALL..DROWNING: EMF reads them from its own hooks on four game programs), and
// health_after of a placed object whose Health is gone is -1 (EMF answers the object's maximum).
#include <intrin.h>

#include "emf_internal.h"
#include "hits_model.h"

namespace kemf {

namespace {

hits::Observers g_observers;
// the relay: 0 = not decided yet (before on_ready), 1 = registered, -1 = off (g_off_why says why)
std::atomic<int> g_relay{0};
std::mutex g_why_mutex;
std::string g_off_why;
constexpr uint32_t kItemStateSize = 20;  // keen::ecs::ItemState {containedInSlotId +0, itemId +4, ...}

constexpr const char* kGapNote =
    "on_hit: hits of the server world arrive after its step, as on EMF; on Kiln entity hits only (fall, shroud, cold and "
    "drowning damage are not reported), and hits cannot be changed (on_hit_before and hit rules are not available yet)";

std::string off_why() {
    std::lock_guard lock(g_why_mutex);
    return g_off_why;
}

void set_off(const std::string& why) {
    {
        std::lock_guard lock(g_why_mutex);
        g_off_why = why;
    }
    g_relay.store(-1, std::memory_order_release);
}

bool hits_wanted() { return g_observers.live() > 0; }

// The record is read at EMF's offsets (hits_model.h). The game's reflected size of HitEvent and kiln.combat's per-build
// captures of the fields the game's own code reads (sigdb combat/hit.flags and combat/hit.health_change: a HitEvent is
// the HitCandidate's first 248 bytes) must agree with them. Empty when they do or are not known on this build; else
// why not (a game update moved the record: on_hit is then refused, as EMF refuses it where HitEvent is not usable).
std::string layout_mismatch() {
    using L = hits::HitEventLayout;
    const uint32_t real = type_size(fnv1a("keen::ecs::HitEvent"));
    if (real && real != L::size)
        return "the game's HitEvent has " + std::to_string(real) + " bytes, kiln-emf reads " + std::to_string(L::size);
    const struct {
        const char* name;
        size_t at;
    } fields[] = {{"combat/hit.flags", L::flags}, {"combat/hit.health_change", L::health_change}};
    for (const auto& f : fields) {
        int64_t v = -1;
        if (g_api && g_api->constant && g_api->constant(kstr(f.name), &v) == KILN_OK && v != static_cast<int64_t>(f.at))
            return std::string(f.name) + " is " + std::to_string(v) + " on this game build, kiln-emf reads " + std::to_string(f.at);
    }
    return {};
}

// mods that registered before on_ready: their true stands (EMF's own answer cannot be taken back), no hit comes
void tell_registered(const std::string& why) {
    g_observers.each(
        [&](hits::Observers::Entry& e) {
            mod_logf(static_cast<Mod*>(e.mod), KILN_LOG_WARN, "on_hit: no hits will arrive (%s)", why.c_str());
            return true;
        },
        [](hits::Observers::Entry&) {});
}

void switch_off(const std::string& why) {
    set_off(why);
    logf(KILN_LOG_WARN, "on_hit: %s; EMF mods get no hits", why.c_str());
    tell_registered(why);
}

int32_t health_of(uint32_t entity) {
    if (!entity || !S.combat || S.combat->header.size < offsetof(KilnCombatApi, health) + sizeof(void*) || !S.combat->health)
        return -1;
    KilnHealth h{};
    h.size = sizeof h;
    return S.combat->health(entity, &h) == KILN_OK ? h.current : -1;
}

uint32_t weapon_item_of(EmfWorld* w, uint32_t pide) {
    if (!pide) return 0;
    uint64_t size = 0;
    const void* p = component_of(w, fnv1a("keen::ecs::ItemState"), pide, &size);
    uint32_t item = 0;
    if (!p || size < kItemStateSize || !seh_read_u32(reinterpret_cast<uintptr_t>(p) + 4, &item)) return 0;
    return item;
}

struct HitCall {
    EmfHitCallback cb;
    EmfWorld* w;
    const EmfHit* hit;
    void* user;
};
void hit_thunk(void* p) {
    auto* c = static_cast<HitCall*>(p);
    c->cb(c->w, c->hit, c->user);
}

// One HitEvent copy of the server world, after its step on the tick thread (events_after_step).
void on_hit_event(EmfWorld* w, const void* event, uint32_t size, void*) {
    if (!g_observers.live()) return;
    EmfHit h{};
    if (!hits::hit_from_event(event, size, &h)) return;
    h.weapon_item = weapon_item_of(w, hits::weapon_pide(event, size));
    h.health_after = health_of(h.part);  // the entity the hit was applied to, else its actor (as EMF)
    if (h.health_after < 0 && h.target != h.part) h.health_after = health_of(h.target);
    g_observers.each(
        [&](hits::Observers::Entry& e) {
            Mod* m = static_cast<Mod*>(e.mod);
            HitCall c{e.cb, w, &h, e.user};
            ContextScope scope(m, w, true);
            const uint64_t t0 = qpc_ns();
            const bool ok = guarded(m, "an on_hit callback", &hit_thunk, &c);  // logs the fault under the mod
            account(m, qpc_ns() - t0);
            return ok;
        },
        [](hits::Observers::Entry&) {});
}

bool api_on_hit(EmfHitCallback cb, void* user) {
    Mod* m = cb ? mod_of_address(reinterpret_cast<const void*>(cb)) : nullptr;
    if (!m) m = caller_mod(_ReturnAddress());
    note_call("on_hit");
    if (!cb) return refuse(m, "on_hit", "no callback"), false;
    if (g_relay.load(std::memory_order_acquire) < 0) {
        unsupported(m, "on_hit", ("the server world's hits are not available: " + off_why()).c_str());
        return false;
    }
    if (!g_observers.add(m, cb, user)) return refuse(m, "on_hit", "16 callbacks already", true), false;
    bool first = true;  // the mod's first on_hit: one line about what Kiln gives
    {
        std::lock_guard lock(g_why_mutex);
        static std::vector<const Mod*> told;
        for (const Mod* x : told) first &= x != m;
        if (first) told.push_back(m);
    }
    if (first) mod_logf(m, KILN_LOG_INFO, "%s", kGapNote);
    return true;
}

}  // namespace

void hits_on_ready(bool any_mod_loaded) {
    if (!any_mod_loaded && !g_observers.count()) return set_off("no EMF mod is loaded");  // nothing to tell, nothing paid
    if (!S.world) return switch_off("kiln.world is not available");
    if (const std::string bad = layout_mismatch(); !bad.empty()) return switch_off("the HitEvent layout differs: " + bad);
    if (!events_add_internal_gated("HitEvent", hits::HitEventLayout::size, &on_hit_event, nullptr, &hits_wanted))
        return switch_off("the HitEvent relay could not be registered");
    g_relay.store(1, std::memory_order_release);
}

void hits_after_ready() {
    if (g_relay.load(std::memory_order_acquire) != 1) return;
    if (events_relay_registered("HitEvent")) {
        logf(KILN_LOG_INFO, "on_hit: the server world's HitEvents are relayed (%u callback(s) so far)", g_observers.count());
        return;
    }
    switch_off("kiln.world did not register the HitEvent relay, or the event queue layout is not resolved on this build");
}

void fill_hits(EmfApi* t) { t->on_hit = &api_on_hit; }

}  // namespace kemf

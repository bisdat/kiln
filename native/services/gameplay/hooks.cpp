// SPDX-License-Identifier: MIT
// Hooks inside the sim step (game worker threads, in parallel with other programs). They only (a) ask the zone table,
// which is an immutable snapshot, and (b) append a record under a mutex; Post reports the records on the tick thread.
//  - g38_building core: PlaceBlueprint / PlaceProp / DismantleBlock(2) / PickUpProp return the success flag the game
//    passes to BuildFinish (which commits or rolls back the inventory transaction and sends UiBuildingEvent); a zone
//    makes them return false without running, i.e. the game's own "could not build" path.
//  - TerraformImpact: the impact function of impact_nodes::Terraforming; a zone makes it return the impact error result
//    it returns itself when an argument is missing (code 0x18), before any voxel or inventory work.
//  - CraftRecipe: observed only (result code and recipe).
//  - QueueSpawn: called by random_spawner (identified by the call site, kiln.hooks caller()); a zone makes it report "no entity"
//    (*out = 0), which the spawner handles as it does the engine refusing a template.
#ifdef _WIN32
#include <windows.h>
#endif

#include "internal.h"
#include "services/kiln_caps.h"
#include "kiln_hook.hpp"
#include "demand_hooks.h"
#include "services/kiln_spawn.h"

namespace kg {
namespace {

kiln::RawHook h_block, h_prop, h_dis, h_dis2, h_pick;
FrameLink h_craft{KILN_GAMEPLAY_HOOK_CRAFT}, h_terra{KILN_GAMEPLAY_HOOK_TERRAFORM}, h_spawn{KILN_GAMEPLAY_HOOK_SPAWN};
const KilnSpawnApi* g_spawn = nullptr;

struct Record {
    uint32_t kind;
    uint32_t op;
    KilnEntity actor, target;
    uint32_t id, id2;
    int32_t result;
    uint32_t zone;
    KilnVec3 pos;
    uint8_t guid[16];
    uint32_t nout;
    KilnGameplayItem out[KILN_GAMEPLAY_MAX_OUTPUTS];
    bool replaced;  // SPAWN: a spawn_replace rule swapped the template
};

std::mutex g_mu;
std::vector<Record> g_recs;
std::atomic<uint32_t> g_dropped{0};

void push(const Record& r) {
    std::lock_guard lock(g_mu);
    if (g_recs.size() >= 8192) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_recs.push_back(r);
}

Record make(uint32_t kind, uint32_t op, KilnEntity actor) {
    Record r{};
    r.kind = kind;
    r.op = op;
    r.actor = actor;
    return r;
}

inline KilnEntity op_player(const uint8_t* op) {
    KilnEntity e = 0;
    std::memcpy(&e, op + L.op_player, 4);
    return e;
}

std::atomic<bool> g_build_installed{false};

inline bool want_build() {
    return g_build_installed.load(std::memory_order_acquire) && has_subscribers(KILN_GAMEPLAY_BUILD);
}

// ---- building ----
// All five return the success flag (al) that the caller passes on to BuildFinish.
uint8_t build_common(kiln::RawHook& h, uint32_t op_kind, uint32_t action, uint8_t* op, void* arg, uint32_t item,
                     double x, double y, double z, KilnEntity target) {
    if (!g_build_installed.load(std::memory_order_acquire)) return h.call<uint8_t>(op, arg, item);
    const KilnEntity player = op_player(op);
    const uint32_t zone = zone_check(player, x, y, z, action);
    if (zone) {
        S.denied.fetch_add(1, std::memory_order_relaxed);
        Record r = make(KILN_GAMEPLAY_BUILD, op_kind, player);
        r.id = item;
        r.target = target;
        r.result = -1;
        r.zone = zone;
        r.pos = KilnVec3{x, y, z};
        push(r);
        return 0;
    }
    const uint8_t ok = h.call<uint8_t>(op, arg, item);
    if (want_build()) {
        Record r = make(KILN_GAMEPLAY_BUILD, op_kind, player);
        r.id = item;
        r.target = target;
        r.result = ok ? 1 : 0;
        r.pos = KilnVec3{x, y, z};
        push(r);
    }
    return ok;
}

uint8_t __fastcall hk_place_block(uint8_t* op, const float* xf, uint32_t item) {
    if (!g_build_installed.load(std::memory_order_acquire)) return h_block.call<uint8_t>(op, const_cast<float*>(xf), item);
    return build_common(h_block, KILN_GAMEPLAY_BUILD_PLACE_BLOCK, KILN_GAMEPLAY_DENY_BUILD, op, const_cast<float*>(xf),
                        item, xf[0], xf[1], xf[2], 0);
}

uint8_t __fastcall hk_place_prop(uint8_t* op, const uint8_t* desc, uint32_t item) {
    if (!g_build_installed.load(std::memory_order_acquire)) return h_prop.call<uint8_t>(op, const_cast<uint8_t*>(desc), item);
    float p[3];
    std::memcpy(p, desc + 0x10, sizeof p);  // desc: +0 template GUID, +0x10 position (float metres), +0x1c rotation
    return build_common(h_prop, KILN_GAMEPLAY_BUILD_PLACE_PROP, KILN_GAMEPLAY_DENY_BUILD, op, const_cast<uint8_t*>(desc),
                        item, p[0], p[1], p[2], 0);
}

uint8_t __fastcall hk_dismantle(uint8_t* op, const float* xf, uint32_t item) {
    if (!g_build_installed.load(std::memory_order_acquire)) return h_dis.call<uint8_t>(op, const_cast<float*>(xf), item);
    return build_common(h_dis, KILN_GAMEPLAY_BUILD_DISMANTLE, KILN_GAMEPLAY_DENY_BREAK, op, const_cast<float*>(xf),
                        item, xf[0], xf[1], xf[2], 0);
}

uint8_t __fastcall hk_dismantle2(uint8_t* op, const float* xf, uint32_t item) {
    if (!g_build_installed.load(std::memory_order_acquire)) return h_dis2.call<uint8_t>(op, const_cast<float*>(xf), item);
    return build_common(h_dis2, KILN_GAMEPLAY_BUILD_DISMANTLE, KILN_GAMEPLAY_DENY_BREAK, op, const_cast<float*>(xf),
                        item, xf[0], xf[1], xf[2], 0);
}

uint8_t __fastcall hk_pick_up(uint8_t* op, const uint32_t* what, uint32_t item) {
    if (!g_build_installed.load(std::memory_order_acquire)) return h_pick.call<uint8_t>(op, const_cast<uint32_t*>(what), item);
    const KilnEntity prop = what[0];
    KilnVec3 p{};
    entity_position(prop, &p);  // a placed prop: static; read-only from a worker (guarded)
    return build_common(h_pick, KILN_GAMEPLAY_BUILD_PICK_UP_PROP, KILN_GAMEPLAY_DENY_BREAK, op,
                        const_cast<uint32_t*>(what), item, p.x, p.y, p.z, prop);
}

// ---- terraforming impact ----
struct TerraArgs {
    void** args;
    int64_t* ctx;
    uint32_t player;
    int64_t pos[3];
    uint32_t type;
    bool ok;
};

void read_terra_args(void* p) {
    auto* t = static_cast<TerraArgs*>(p);
    t->ok = false;
    const uint32_t* handles = *reinterpret_cast<uint32_t* const*>(t->args);
    const auto table = static_cast<uintptr_t>(t->ctx[0]);
    const auto base = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<const uint8_t*>(t->ctx) + L.arg_base);
    auto argp = [&](int i) -> const uint8_t* {
        const uint32_t h = handles[i];
        if (!h) return nullptr;
        const uint16_t off =
            *reinterpret_cast<const uint16_t*>(table + static_cast<uintptr_t>(L.arg_offset) + (h & 0xFFFFu) * L.arg_stride);
        return reinterpret_cast<const uint8_t*>(base + off);
    };
    const uint8_t* a0 = argp(0);
    const uint8_t* a1 = argp(1);
    const uint8_t* a2 = argp(2);
    if (!a0 || !a1) return;
    std::memcpy(&t->player, a0, 4);
    std::memcpy(t->pos, a1, sizeof t->pos);
    t->type = a2 ? *a2 : 0xFFu;
    t->ok = true;
}

uint8_t* __fastcall hk_terraform(uint8_t* out, void** args, int64_t* ctx) {
    TerraArgs t{args, ctx};
    unsigned long code = 0;
    const bool read = seh_call(&read_terra_args, &t, &code) && t.ok;
    if (!read) return h_terra.call<uint8_t*>(out, args, ctx);
    const double x = fixed_to_m(t.pos[0]), y = fixed_to_m(t.pos[1]), z = fixed_to_m(t.pos[2]);
    const uint32_t zone = zone_check(t.player, x, y, z, KILN_GAMEPLAY_DENY_TERRAFORM);
    if (zone) {
        S.denied.fetch_add(1, std::memory_order_relaxed);
        Record r = make(KILN_GAMEPLAY_BUILD, KILN_GAMEPLAY_BUILD_TERRAFORM, t.player);
        r.id2 = t.type;
        r.result = -1;
        r.zone = zone;
        r.pos = KilnVec3{x, y, z};
        push(r);
        // The function's own early exit when an argument is missing: {u8 code 0x18; u32 0 at +4; u8 0 at +8}.
        out[0] = 0x18;
        std::memset(out + 4, 0, 4);
        out[8] = 0;
        return out;
    }
    uint8_t* res = h_terra.call<uint8_t*>(out, args, ctx);
    if (has_subscribers(KILN_GAMEPLAY_BUILD)) {
        Record r = make(KILN_GAMEPLAY_BUILD, KILN_GAMEPLAY_BUILD_TERRAFORM, t.player);
        r.id2 = t.type;
        r.result = res && res[0] == 0 ? 1 : 0;
        r.pos = KilnVec3{x, y, z};
        push(r);
    }
    return res;
}

// ---- craft core ----
struct RecipeRead {
    const uint8_t* recipe;
    uint32_t id;
    uint32_t n;
    KilnGameplayItem out[KILN_GAMEPLAY_MAX_OUTPUTS];
};

void read_recipe(void* p) {
    auto* r = static_cast<RecipeRead*>(p);
    std::memcpy(&r->id, r->recipe + 0x10, 4);
    int32_t rel = 0;
    uint32_t count = 0;
    std::memcpy(&rel, r->recipe + 0x30, 4);  // outputs: BlobArray {i32 self-relative offset; u32 count}, 24-byte items
    std::memcpy(&count, r->recipe + 0x34, 4);
    r->n = 0;
    if (count > 64) return;
    const uint8_t* items = r->recipe + 0x30 + rel;
    for (uint32_t i = 0; i < count && r->n < KILN_GAMEPLAY_MAX_OUTPUTS; ++i) {
        std::memcpy(&r->out[r->n].item, items + i * 24 + 0x10, 4);
        std::memcpy(&r->out[r->n].count, items + i * 24 + 0x14, 4);
        ++r->n;
    }
}

int32_t __fastcall hk_craft(uint32_t* ctx, const uint8_t* recipe) {
    const int32_t rc = h_craft.call<int32_t>(ctx, recipe);
    // Automated stations (factory_station: operator == station, not a player) retry every step; report their
    // successes only. Craft requests (handle_crafting_events) and players report every outcome.
    const bool factory = ctx && ctx[0] == ctx[1] && !(ctx[0] >= 1 && ctx[0] <= 1023);
    if (has_subscribers(KILN_GAMEPLAY_CRAFTED) && ctx && recipe && (rc == 0 || !factory)) {
        Record r = make(KILN_GAMEPLAY_CRAFTED, 0, ctx[0]);
        r.target = ctx[1];
        r.id2 = ctx[2];
        r.result = rc;
        RecipeRead rr{recipe};
        unsigned long code = 0;
        if (seh_call(&read_recipe, &rr, &code)) {
            r.id = rr.id;
            r.nout = rr.n;
            std::memcpy(r.out, rr.out, sizeof r.out);
        }
        push(r);
    }
    return rc;
}

// ---- natural-spawn template replacement (spawn_replace) ----
// Written from any thread under the lock, read by the random spawner's worker under the lock (only while a rule exists).
struct Replace {
    KilnOwner owner;
    uint8_t from[16], to[16];
    float chance;
};
std::mutex g_replace_mu;
std::vector<Replace> g_replace;
std::atomic<bool> g_replace_any{false};
thread_local uint64_t t_rng = 0;

float rnd01() {
    if (!t_rng) t_rng = 0x9E3779B97F4A7C15ull ^ reinterpret_cast<uintptr_t>(&t_rng) ^ static_cast<uint64_t>(g_step);
    t_rng ^= t_rng << 13;
    t_rng ^= t_rng >> 7;
    t_rng ^= t_rng << 17;
    return static_cast<float>(t_rng >> 40) / static_cast<float>(1u << 24);
}

bool replacement(const uint8_t* guid, uint8_t out[16]) {
    if (!guid || !g_replace_any.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(g_replace_mu);
    for (const Replace& r : g_replace)
        if (std::memcmp(r.from, guid, 16) == 0 && rnd01() < r.chance) {
            std::memcpy(out, r.to, 16);
            return true;
        }
    return false;
}

// ---- random spawner's spawn ----
uint32_t* __fastcall hk_spawn(void* a, uint32_t* out, void* c, uint32_t d, const uint8_t* guid, const int64_t* xf) {
    if (h_spawn.caller() != fn.spawner_ret || !out || !xf)
        return h_spawn.call<uint32_t*>(a, out, c, d, guid, xf);
    const double x = fixed_to_m(xf[0]), y = fixed_to_m(xf[1]), z = fixed_to_m(xf[2]);
    const uint32_t zone = zone_check(0, x, y, z, KILN_GAMEPLAY_DENY_SPAWN);
    if (zone) {
        S.denied.fetch_add(1, std::memory_order_relaxed);
        *out = 0;
        Record r = make(KILN_GAMEPLAY_SPAWN, 0, 0);
        r.zone = zone;
        r.result = -1;
        r.pos = KilnVec3{x, y, z};
        if (guid) std::memcpy(r.guid, guid, 16);
        push(r);
        return out;
    }
    uint8_t swapped[16];
    const bool replaced = replacement(guid, swapped);
    if (replaced) S.spawns_replaced.fetch_add(1, std::memory_order_relaxed);
    uint32_t* res = h_spawn.call<uint32_t*>(a, out, c, d, replaced ? swapped : guid, xf);
    if (has_subscribers(KILN_GAMEPLAY_SPAWN)) {
        Record r = make(KILN_GAMEPLAY_SPAWN, 0, res ? *res : 0);
        r.result = 1;
        r.pos = KilnVec3{x, y, z};
        if (guid) std::memcpy(r.guid, replaced ? swapped : guid, 16);
        r.replaced = replaced;
        push(r);
    }
    return res;
}

bool hook(kiln::RawHook& h, uintptr_t target, void* detour, const char* what) {
    const std::string label = std::string("gameplay/") + what;  // the sigdb symbol name
    if (!h.install(g_api, g_owner, target, detour, label.c_str())) {
        KG_E("hooking %s at %p failed", what, reinterpret_cast<void*>(target));
        return false;
    }
    return true;
}

}  // namespace

void hooks_set_spawn_service(const void* p) { g_spawn = static_cast<const KilnSpawnApi*>(p); }

KilnResult hooks_spawn_replace(KilnOwner owner, const uint8_t* from, const uint8_t* to, float chance) {
    if (!F.spawn) return KILN_ERR_UNAVAILABLE;
    if (!from || !to || !(chance >= 0.f && chance <= 1.f)) return KILN_ERR_INVALID_ARGUMENT;
    if (chance>0 && !hooks_request(KILN_GAMEPLAY_HOOK_SPAWN,owner))
        return KILN_ERR_UNAVAILABLE;
    std::lock_guard lock(g_replace_mu);
    std::erase_if(g_replace, [&](const Replace& r) {
        return r.owner == owner && !std::memcmp(r.from, from, 16) && !std::memcmp(r.to, to, 16);
    });
    if (chance > 0.f) {
        Replace r{owner, {}, {}, chance};
        std::memcpy(r.from, from, 16);
        std::memcpy(r.to, to, 16);
        g_replace.push_back(r);
    }
    g_replace_any.store(!g_replace.empty(), std::memory_order_release);
    return KILN_OK;
}

uint32_t hooks_spawn_replace_clear(KilnOwner owner) {
    std::lock_guard lock(g_replace_mu);
    const size_t n = std::erase_if(g_replace, [&](const Replace& r) { return r.owner == owner; });
    g_replace_any.store(!g_replace.empty(), std::memory_order_release);
    return static_cast<uint32_t>(n);
}

namespace {
std::recursive_mutex& g_build_mu=hooks_publication();
uint32_t g_build_state = 0, g_build_mask = 0; // idle, installed, waiting, refused
KilnOwner g_build_requester = 0;
bool g_build_requested = false, g_build_lifecycle = false;
std::string g_build_reason = "resolved, not installed: no mod asked";
const char* build_names[] = {"gameplay/PlaceBlueprint", "gameplay/PlaceProp", "gameplay/DismantleBlock",
                            "gameplay/DismantleBlock2", "gameplay/PickUpProp"};

// Single installation-policy seam: replace waiting for the stop with checked raw installation at server Pre
// once kiln.hooks provides it. session() is an AnyThread snapshot. A STOPPING callback excludes only the world
// whose jobs the core has drained; every other world (including a hosting client's prediction world) must be gone.
bool build_may_install(uint32_t stopping) {
    KilnSessionInfo session{}; session.size = sizeof session;
    if (!g_api->session || g_api->session(&session) != KILN_OK) return false;
    if (stopping == KILN_WORLD_SERVER && !g_api->feature(kstr("tick.wait_jobs"))) return false;
    for (uint32_t k = 0; k < KILN_WORLD_KINDS; ++k)
        if (k != stopping && session.worlds[k]) return false;
    return true;
}
void build_publish() {
    const auto* caps = static_cast<const KilnCapsApi*>(g_api->service(kstr(KILN_CAPS_SERVICE), 1));
    if (caps && caps->report)
        caps->report(g_owner, kstr("gameplay.build_hooks"),
            g_build_state == 1 ? KILN_CAP_AVAILABLE : g_build_state == 3 ? KILN_CAP_UNAVAILABLE : KILN_CAP_PENDING,
            g_build_state == 1 ? KILN_CAP_REASON_NONE : g_build_state == 3 ? KILN_CAP_REASON_FAULTED : KILN_CAP_REASON_SESSION,
            kstr(g_build_reason));
}
}
void build_lifecycle(bool available) { std::lock_guard lock(g_build_mu); g_build_lifecycle = available; }
bool build_installed() { return g_build_installed.load(std::memory_order_acquire); }
uint32_t build_state() { std::lock_guard lock(g_build_mu); return g_build_state; }
uint32_t build_mask() { std::lock_guard lock(g_build_mu); return g_build_mask; }
std::string build_reason() { std::lock_guard lock(g_build_mu); return g_build_reason; }
void build_poll(uint32_t stopping) {
    std::lock_guard lock(g_build_mu);
    if (!g_build_requested || g_build_state == 1 || g_build_state == 3 || !build_may_install(stopping)) return;
    const uintptr_t targets[] = {fn.place_block, fn.place_prop, fn.dismantle, fn.dismantle2, fn.pick_up};
    kiln::RawHook* hooks[] = {&h_block, &h_prop, &h_dis, &h_dis2, &h_pick};
    void* detours[] = {reinterpret_cast<void*>(&hk_place_block), reinterpret_cast<void*>(&hk_place_prop),
        reinterpret_cast<void*>(&hk_dismantle), reinterpret_cast<void*>(&hk_dismantle2), reinterpret_cast<void*>(&hk_pick_up)};
    auto refuse = [&](const std::string& why) {
        g_build_state = 3;
        g_build_reason = "refused: " + why + "; installed target mask " + std::to_string(g_build_mask) +
                         " (all installed detours pass through; no build events or build/break denies)";
        for (unsigned i=0; i<5; ++i) if (g_build_mask & (1u << i)) g_build_reason += std::string("; ") + build_names[i];
        KG_W("kiln.gameplay: build hooks %s", g_build_reason.c_str()); build_publish();
    };
    if (!F.build) { refuse("gameplay.build symbols/layouts did not resolve"); return; }
    // Revalidate every resolved symbol and readable executable entry before the first write. The existing hook()
    // helper adds no byte validation of its own; sigdb resolution supplies the signature validation.
    for (unsigned i = 0; i < 5; ++i) {
        uintptr_t resolved = 0;
        if (!targets[i] || !g_api->symbol || g_api->symbol(kstr(build_names[i]), &resolved) != KILN_OK ||
            resolved != targets[i] || !seh_build_target(targets[i])) { refuse(std::string(build_names[i]) + " failed pre-check"); return; }
    }
    for (unsigned i = 0; i < 5; ++i) {
        if (!hook(*hooks[i], targets[i], detours[i], build_names[i] + 9)) {
            refuse(std::string(build_names[i]) + ": " + (hooks[i]->error() ? hooks[i]->error() : "install failed")); return;
        }
        g_build_mask |= 1u << i;
    }
    g_build_state = 1; g_build_reason = "installed";
    g_build_installed.store(true, std::memory_order_release);
    KG_I("kiln.gameplay: build hooks installed at first use by %llu", static_cast<unsigned long long>(g_build_requester));
    build_publish();
}
bool build_request(KilnOwner owner) {
    std::lock_guard lock(g_build_mu);
    if (g_build_state == 3) {
        if (!g_build_requested) KG_W("kiln.gameplay: build hooks %s (owner %llu)", g_build_reason.c_str(),
                                   static_cast<unsigned long long>(owner));
        g_build_requested = true;
        return false;
    }
    if (!g_build_requested) {
        g_build_requested = true; g_build_requester = owner;
        if (!build_may_install(KILN_WORLD_KINDS)) {
            KilnSessionInfo session{}; session.size = sizeof session;
            const bool observed = g_build_lifecycle && g_api->session && g_api->session(&session) == KILN_OK && session.lifecycle;
            g_build_state = observed ? 2 : 3;
            g_build_reason = "build hooks wait for the world to stop: asked while a world ran; building-piece events begin with the next world";
            if (!observed) g_build_reason = "refused: cannot observe world stop; build events unavailable in this process";
            KG_W("kiln.gameplay: %s (owner %llu)", g_build_reason.c_str(), static_cast<unsigned long long>(owner));
            build_publish();
        }
    }
    build_poll();
    return g_build_state != 3;
}

bool hooks_install() {
    { std::lock_guard lock(g_build_mu);
      if (!F.build) { g_build_state=3; g_build_reason="refused: gameplay.build symbols/layouts did not resolve"; }
      build_publish(); }
    hook_register(KILN_GAMEPLAY_HOOK_CRAFT,&h_craft,F.craft ? fn.craft : 0,"gameplay/CraftRecipe",&FrameLink::dispatch<&hk_craft>,
                  {KILN_SIG_PTR,KILN_SIG_PTR},KILN_SIG_S32);
    hook_register(KILN_GAMEPLAY_HOOK_TERRAFORM,&h_terra,F.terraform ? fn.terraform : 0,"gameplay/TerraformImpact",&FrameLink::dispatch<&hk_terraform>,
                  {KILN_SIG_PTR,KILN_SIG_PTR,KILN_SIG_PTR},KILN_SIG_PTR);
    hook_register(KILN_GAMEPLAY_HOOK_SPAWN,&h_spawn,F.spawn ? fn.queue_spawn : 0,"gameplay/QueueSpawn",&FrameLink::dispatch<&hk_spawn>,
                  {KILN_SIG_PTR,KILN_SIG_PTR,KILN_SIG_PTR,KILN_SIG_U32,KILN_SIG_PTR,KILN_SIG_PTR},KILN_SIG_PTR);
    return F.craft || F.build || F.terraform || F.spawn;
}

void hooks_post() {
    std::vector<Record> recs;
    {
        std::lock_guard lock(g_mu);
        recs.swap(g_recs);
    }
    if (const uint32_t d = g_dropped.exchange(0)) KG_W("%u hook record(s) dropped (more than 8192 in one step)", d);
    if (recs.empty()) return;
    S.hook_records += recs.size();
    for (auto& r : recs) {
        if (r.kind == KILN_GAMEPLAY_BUILD && r.op != KILN_GAMEPLAY_BUILD_TERRAFORM && !build_installed()) continue;
        KilnGameplayEvent ev;
        std::memset(&ev, 0, sizeof ev);
        ev.size = sizeof ev;
        ev.kind = r.kind;
        ev.step = g_step;
        ev.phase = KILN_GAMEPLAY_PHASE_POST;
        ev.actor = r.actor;
        ev.target = r.target;
        ev.id = r.id;
        ev.id2 = r.id2;
        ev.result = r.result;
        ev.op = r.op;
        ev.zone = r.zone;
        ev.position = r.pos;
        std::memcpy(ev.guid, r.guid, 16);
        ev.output_count = r.nout;
        std::memcpy(ev.outputs, r.out, sizeof ev.outputs);
        if (r.zone) ev.flags |= KILN_GAMEPLAY_F_DENIED | KILN_GAMEPLAY_F_CANCELED;
        if (r.kind == KILN_GAMEPLAY_CRAFTED) entity_position(r.target, &ev.position);
        if (r.kind == KILN_GAMEPLAY_SPAWN && !r.zone) ev.flags |= KILN_GAMEPLAY_F_CANCELABLE;
        if (r.replaced) ev.flags |= KILN_GAMEPLAY_F_REPLACED;
        const bool canceled = dispatch(&ev);
        if (r.kind == KILN_GAMEPLAY_SPAWN && !r.zone && canceled && r.actor) {
            const KilnResult rc = g_spawn ? g_spawn->destroy(r.actor) : KILN_ERR_UNAVAILABLE;
            if (rc == KILN_OK) ++S.canceled;
            KG_I("natural spawn %u at (%.1f, %.1f, %.1f) canceled: %s", r.actor, r.pos.x, r.pos.y, r.pos.z,
                 rc == KILN_OK ? "destroyed" : "could not destroy it");
        }
    }
}

}  // namespace kg

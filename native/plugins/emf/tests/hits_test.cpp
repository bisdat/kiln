// SPDX-License-Identifier: MIT
// Host test of kiln-emf's v31 on_hit (api_hits.cpp, hits_model.cpp) through the production event relay
// (api_events.cpp) with fake kiln.world, kiln.combat and components: no game, no Wine.
//   hits_test            the relay: registration before and after on_ready, the conversion, health_after, weapon_item,
//                        faults, the gate (no queue read while nobody listens), world stop, the 16-callback limit
//   hits_test no-mods    no EMF mod loaded: no relay is registered and on_hit says why
//   hits_test no-relay   kiln.world refuses the relay: a registration made before on_ready is told, later ones refused
//   hits_test mod-event  a mod's own on_event("HitEvent") keeps the relay reading while no on_hit callback listens
//   hits_test bad-layout a game build whose HitEvent fields moved (sigdb combat/hit.flags) or whose reflected size
//                        differs: no relay, a registration made before on_ready is told, later ones refused
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "emf_internal.h"
#include "hits_model.h"

static int failures = 0, checks = 0;
#define CHECK(x)                                                \
    do {                                                        \
        ++checks;                                               \
        if (!(x)) {                                             \
            ++failures;                                         \
            std::printf("FAIL %d: %s\n", __LINE__, #x);         \
        }                                                       \
    } while (0)

namespace kemf {
const KilnApi* g_api;
KilnOwner g_owner = 1;
uint32_t g_side = KILN_SIDE_SERVER;
Services S;
Paths P;
thread_local Context t_ctx;
static Mod mod_a, mod_b;
static Mod* active_mod = &mod_a;
static Mod* fault_mod = nullptr;  // guarded() reports a fault for this mod's callbacks (without calling them)
Mod* caller_mod(const void*) { return active_mod; }
Mod* mod_of_address(const void*) { return active_mod; }
std::vector<std::string> kiln_lines;
void logf(int, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kiln_lines.emplace_back(buf);
}
std::map<const Mod*, std::vector<std::string>> mod_lines;
void mod_logf(const Mod* m, int, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    mod_lines[m].emplace_back(buf);
    if (std::getenv("KEMF_TEST_VERBOSE")) std::printf("  log %s\n", buf);
}
int unsupported_calls = 0, refusals = 0;
std::set<std::string> unsupported_fns;
void unsupported(const Mod*, const char* fn, const char*) {
    ++unsupported_calls;
    if (fn) unsupported_fns.insert(fn);
}
void refuse(const Mod*, const char*, const char*, bool) { ++refusals; }
void note_call(const char*) {}
void account(Mod*, uint64_t) {}
uint64_t qpc_ns() { return 0; }
bool guarded(Mod* m, const char*, ThunkFn f, void* p) {
    if (m && m == fault_mod) return false;
    f(p);
    return true;
}
bool seh_copy(void* d, const void* s, size_t n) {
    if (!s || !d) return false;
    std::memcpy(d, s, n);
    return true;
}
bool seh_read_u32(uintptr_t p, uint32_t* v) { return seh_copy(v, reinterpret_cast<void*>(p), 4); }
bool seh_read_u64(uintptr_t p, uint64_t* v) { return seh_copy(v, reinterpret_cast<void*>(p), 8); }
bool seh_read_u16(uintptr_t p, uint16_t* v) { return seh_copy(v, reinterpret_cast<void*>(p), 2); }
bool seh_call(ThunkFn f, void* p, unsigned long*, uintptr_t*) {
    f(p);
    return true;
}
void keep_adapter_mapped() {}
uint32_t fnv1a(std::string_view s) {
    uint32_t h = 2166136261u;
    for (auto c : s) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return h;
}
static uint32_t hit_event_size = 248;
uint32_t type_size(uint32_t hash) { return hash == fnv1a("keen::ecs::HitEvent") ? hit_event_size : 0; }
std::string format(const char*, ...) { return {}; }
// ItemState of weapon entities: 900 holds item 0xABCD; 901's component is too small to be an ItemState
static uint8_t item_state[20], short_state[8];
const void* component_of(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    if (hash != fnv1a("keen::ecs::ItemState")) return nullptr;
    if (entity == 900) return *size = sizeof item_state, item_state;
    if (entity == 901) return *size = sizeof short_state, short_state;
    return nullptr;
}
}  // namespace kemf
using namespace kemf;

// ---- fakes ----
static uintptr_t worlds[3] = {0x1000, 0, 0x2000};
static std::map<std::string, KilnProgramDesc> programs;
static bool refuse_programs = false;
static int64_t hit_flags_at = -1;  // sigdb combat/hit.flags on this "build" (-1: not resolved)
static KilnCombatApi combat{};
static int health_calls = 0;

// One event queue as the game keeps it (combat/evq.*: first block index +0, blocks +8, count +16, next block +0 of a
// block, 256-byte blocks): one HitEvent per block, the blocks chained.
struct Queue {
    uint16_t first = 1;
    uint8_t pad[6]{};
    uintptr_t blocks = 0;
    uint32_t count = 0;
};
alignas(8) static uint8_t blocks[4][256];
static Queue queue;
static uint64_t iter[1];

struct Ev {
    uint32_t attacker = 1, source = 1, target = 7, root_target = 0, flags = 0, pide = 900, impact = 0x1234, program = 0x55,
             weapon_category = 0x77;
    int32_t health_change = -30, stamina_change = 0;
    uint16_t damage_types = 1u << 3, reaction = 2;
    uint8_t attacks = 1, faction = 4;
    float pos[3] = {1.5f, 2.5f, -3.5f};
};
static void put(int i, const Ev& e) {
    using L = hits::HitEventLayout;
    uint8_t* r = blocks[i] + 8;
    std::memset(r, 0xEE, 248);  // fields EMF does not read stay garbage
    auto w32 = [&](size_t off, uint32_t v) { std::memcpy(r + off, &v, 4); };
    w32(L::root_source, e.attacker);
    w32(L::source, e.source);
    w32(L::target, e.target);
    w32(L::root_target, e.root_target);
    w32(L::flags, e.flags);
    w32(L::weapon_pide, e.pide);
    w32(L::impact, e.impact);
    w32(L::program, e.program);
    w32(L::weapon_category, e.weapon_category);
    w32(L::health_change, static_cast<uint32_t>(e.health_change));
    w32(L::stamina_change, static_cast<uint32_t>(e.stamina_change));
    std::memcpy(r + L::damage_types, &e.damage_types, 2);
    std::memcpy(r + L::reaction, &e.reaction, 2);
    r[L::attack_category] = e.attacks;
    r[L::target_faction] = e.faction;
    std::memcpy(r + L::position, e.pos, 12);
}
static void fill(const std::vector<Ev>& evs) {
    for (size_t i = 0; i < evs.size(); ++i) {
        put(static_cast<int>(i), evs[i]);
        const uint16_t next = static_cast<uint16_t>(i + 2 <= evs.size() ? i + 2 : 0);
        std::memcpy(blocks[i], &next, 2);
    }
    queue.blocks = reinterpret_cast<uintptr_t>(blocks);
    queue.count = static_cast<uint32_t>(evs.size());
    iter[0] = reinterpret_cast<uintptr_t>(&queue);
}
static EmfWorld* server() { return reinterpret_cast<EmfWorld*>(worlds[0]); }
static void relay_step() {  // the relay program runs in the step; the copies go out after it
    auto& p = programs.at("emf_ev_HitEvent");
    p.each(nullptr, iter, 0, p.user);
}

struct Seen {
    std::vector<EmfHit> hits;
    std::vector<const Mod*> mods;
    std::vector<bool> world_cb;
};
static Seen seen_a, seen_b, seen_more;
static void cb_a(EmfWorld* w, const EmfHit* h, void*) {
    seen_a.hits.push_back(*h);
    seen_a.mods.push_back(t_ctx.mod);
    seen_a.world_cb.push_back(t_ctx.world_cb && t_ctx.world == w && w == server());
}
static void cb_b(EmfWorld*, const EmfHit* h, void*) {
    seen_b.hits.push_back(*h);
    seen_b.mods.push_back(t_ctx.mod);
}
static void cb_more(EmfWorld*, const EmfHit* h, void*) { seen_more.hits.push_back(*h); }
static int mod_events = 0;
static void mod_event_cb(EmfWorld*, const void*, uint32_t size, void*) { mod_events += size == 248; }

static int gap_lines(const Mod* m) {
    int n = 0;
    for (const auto& l : mod_lines[m]) n += l.find("entity hits only") != std::string::npos;
    return n;
}

static void setup(KilnApi& api, KilnWorldApi& world) {
    api.world = [](uint32_t k) { return worlds[k]; };
    api.constant = [](KilnStr n, int64_t* v) -> KilnResult {
        const std::string s(n.ptr, n.len);
        *v = s == "combat/evq.first" ? 0 : s == "combat/evq.blocks" ? 8 : s == "combat/evq.count" ? 16 : s == "combat/evq.next" ? 0
             : s == "combat/evq.block_shift" ? 8 : s == "combat/hit.flags" ? hit_flags_at
             : s == "combat/hit.health_change" ? 188 : -1;
        return *v < 0 ? KILN_ERR_NOT_FOUND : KILN_OK;
    };
    api.symbol = [](KilnStr, uintptr_t*) -> KilnResult { return KILN_ERR_NOT_FOUND; };
    g_api = &api;
    world.header.size = sizeof world;
    world.register_program = [](KilnOwner, const KilnProgramDesc* d, KilnHandle* h) -> KilnResult {
        if (refuse_programs) return KILN_ERR_UNAVAILABLE;
        programs[std::string(d->name.ptr, d->name.len)] = *d;
        *h = programs.size();
        return KILN_OK;
    };
    world.program_slot = [](KilnHandle, int32_t kind, KilnStr) -> int32_t { return kind == KILN_SLOT_EVENT_IN ? 0 : -1; };
    world.unregister_program = [](KilnHandle) -> KilnResult { return KILN_OK; };
    S.world = &world;
    combat.header.size = sizeof combat;
    combat.header.version = KILN_COMBAT_VERSION;
    combat.health = [](KilnEntity e, KilnHealth* out) -> KilnResult {
        ++health_calls;
        if (!out || out->size != sizeof(KilnHealth)) return KILN_ERR_INVALID_ARGUMENT;
        if (e == 7) return out->current = 42, out->max = 100, KILN_OK;
        if (e == 9) return out->current = 55, out->max = 60, KILN_OK;
        return KILN_ERR_NOT_FOUND;  // 8: a part without Health
    };
    S.combat = &combat;
    const uint32_t item = 0xABCD;
    std::memcpy(item_state + 4, &item, 4);
}

static int test_relay(EmfApi& t) {
    // ---- before on_ready: kept (EMF: any time; the Lua host asks at a script's first emf.on_hit) ----
    CHECK(!t.on_hit(nullptr, nullptr) && refusals == 1);
    CHECK(t.on_hit(&cb_a, nullptr));
    CHECK(gap_lines(&mod_a) == 1);
    hits_on_ready(true);
    events_on_ready();
    hits_after_ready();
    CHECK(programs.count("emf_ev_HitEvent") == 1);
    CHECK(programs.at("emf_ev_HitEvent").flags == (KILN_PROGRAM_SYSTEM | KILN_PROGRAM_HOST_ONLY));
    CHECK(programs.at("emf_ev_HitEvent").events_in.count == 1);

    // ---- one step, two HitEvents: a building hit (never passed, as EMF) and a melee hit on entity 7 ----
    Ev building;
    building.flags = EMF_HIT_BUILDING;
    Ev melee;
    melee.flags = EMF_HIT_CRIT | EMF_HIT_KILLED;
    fill({building, melee});
    relay_step();
    CHECK(seen_a.hits.empty());  // nothing before the step's end
    events_after_step(server(), EMF_WORLD_SERVER);
    CHECK(seen_a.hits.size() == 1);
    if (seen_a.hits.size() == 1) {
        const EmfHit& h = seen_a.hits[0];
        CHECK(h.size == sizeof(EmfHit) && h.kind == EMF_HIT_KIND_HIT);
        CHECK(h.attacker == 1 && h.source == 1 && h.target == 7 && h.part == 7);  // no root target: target = part
        CHECK(h.amount == 30 && h.stamina == 0);
        CHECK(h.flags == (EMF_HIT_CRIT | EMF_HIT_KILLED) && h.damage_types == (1u << 3) && h.attacks == 1);
        CHECK(h.weapon_category == 0x77 && h.weapon_item == 0xABCD && h.program == 0x55 && h.impact == 0x1234);
        CHECK(h.reaction == 2 && h.target_faction == 4 && h.from_mod == 0);
        CHECK(h.health_before == -1 && h.health_after == 42);
        CHECK(h.position[0] == 1.5f && h.position[1] == 2.5f && h.position[2] == -3.5f && h.cancel == 0);
        CHECK(seen_a.mods[0] == &mod_a && seen_a.world_cb[0]);  // inside a world callback of its mod
    }

    // ---- a part without Health (a weak spot): the actor's health; a damage() hit; healing; no weapon ----
    Ev part;
    part.target = 8;
    part.root_target = 9;
    part.impact = hits::kKilnImpactTag | 0x42;
    part.health_change = 12;  // healing: the game's change is positive
    part.pide = 0;
    Ev unknown;
    unknown.target = 10;  // no Health anywhere: -1
    unknown.pide = 901;   // a component too small for an ItemState: 0
    unknown.flags = EMF_HIT_TERRAIN;  // and a terrain hit: not passed
    Ev blocked;
    blocked.target = 10;
    blocked.pide = 901;
    blocked.flags = EMF_HIT_BLOCKED;
    blocked.health_change = 0;
    blocked.stamina_change = -9;
    fill({part, unknown, blocked});
    relay_step();
    events_after_step(server(), EMF_WORLD_SERVER);
    CHECK(seen_a.hits.size() == 3);
    if (seen_a.hits.size() == 3) {
        const EmfHit& p = seen_a.hits[1];
        CHECK(p.part == 8 && p.target == 9 && p.health_after == 55 && p.from_mod == 1 && p.amount == -12 && p.weapon_item == 0);
        const EmfHit& b = seen_a.hits[2];
        CHECK(b.target == 10 && b.health_after == -1 && b.weapon_item == 0 && b.amount == 0 && b.stamina == 9 &&
              b.flags == EMF_HIT_BLOCKED);
    }

    // ---- a world that stops: copies of its step never reach the next world ----
    fill({melee});
    relay_step();
    events_world_stopping();
    events_after_step(server(), EMF_WORLD_SERVER);
    CHECK(seen_a.hits.size() == 3);

    // ---- a callback that faults is switched off (guarded() logs it under the mod); nobody listens any more ----
    fault_mod = &mod_a;
    fill({melee});
    relay_step();
    events_after_step(server(), EMF_WORLD_SERVER);
    fault_mod = nullptr;
    fill({melee});
    relay_step();
    events_after_step(server(), EMF_WORLD_SERVER);
    CHECK(seen_a.hits.size() == 3);

    // ---- the gate: with no callback on, the relay leaves the game's queue alone (no copy to deliver later) ----
    const int health_before = health_calls;
    fill({melee});
    relay_step();
    active_mod = &mod_b;
    CHECK(t.on_hit(&cb_b, nullptr));  // after on_ready: joins the running relay
    CHECK(gap_lines(&mod_b) == 1);
    events_after_step(server(), EMF_WORLD_SERVER);
    CHECK(seen_b.hits.empty() && health_calls == health_before);
    relay_step();
    events_after_step(server(), EMF_WORLD_SERVER);
    CHECK(seen_b.hits.size() == 1 && seen_a.hits.size() == 3);
    if (!seen_b.mods.empty()) CHECK(seen_b.mods[0] == &mod_b);
    CHECK(t.on_hit(&cb_b, nullptr) && gap_lines(&mod_b) == 1);  // a second registration: the note is logged once

    // ---- at most 16 callbacks, as EMF (the faulted one still counts) ----
    for (int i = 3; i < 16; ++i) CHECK(t.on_hit(&cb_more, nullptr));
    const int refused = refusals;
    CHECK(!t.on_hit(&cb_more, nullptr) && refusals == refused + 1);
    relay_step();
    events_after_step(server(), EMF_WORLD_SERVER);
    CHECK(seen_b.hits.size() == 3 && seen_more.hits.size() == 13);
    CHECK(unsupported_calls == 0);
    return 0;
}

static void test_no_mods(EmfApi& t) {
    hits_on_ready(false);
    events_on_ready();
    hits_after_ready();
    CHECK(programs.count("emf_ev_HitEvent") == 0);  // nothing registered for a game without EMF mods
    CHECK(!t.on_hit(&cb_a, nullptr) && unsupported_fns.count("on_hit") == 1);
}

static void test_no_relay(EmfApi& t) {
    CHECK(t.on_hit(&cb_a, nullptr));  // before on_ready: EMF's answer for a feature that may still come
    refuse_programs = true;
    hits_on_ready(true);
    events_on_ready();
    hits_after_ready();
    int told = 0;
    for (const auto& l : mod_lines[&mod_a]) told += l.find("no hits will arrive") != std::string::npos;
    CHECK(told == 1);
    CHECK(!t.on_hit(&cb_a, nullptr) && unsupported_fns.count("on_hit") == 1);
}

static void test_bad_layout(EmfApi& t) {
    CHECK(t.on_hit(&cb_a, nullptr));
    hit_flags_at = 0x9C;  // the flags moved on this build
    hits_on_ready(true);
    events_on_ready();
    hits_after_ready();
    CHECK(programs.count("emf_ev_HitEvent") == 0);
    int told = 0;
    for (const auto& l : mod_lines[&mod_a]) told += l.find("no hits will arrive (the HitEvent layout differs") != std::string::npos;
    CHECK(told == 1);
    CHECK(!t.on_hit(&cb_a, nullptr) && unsupported_fns.count("on_hit") == 1);
    // the reflected size alone, with the sigdb captures agreeing
    CHECK(hits::HitEventLayout::flags == 152 && hits::HitEventLayout::health_change == 188);
}

static void test_mod_event(EmfApi& t) {
    // a mod's own HitEvent listener wants every copy; the on_hit gate cannot starve it
    CHECK(t.on_event(EMF_WORLD_SERVER, "HitEvent", 248, &mod_event_cb, nullptr));
    hits_on_ready(true);
    events_on_ready();
    hits_after_ready();
    Ev melee;
    fill({melee});
    relay_step();
    events_after_step(server(), EMF_WORLD_SERVER);
    CHECK(mod_events == 1 && health_calls == 0);  // no on_hit callback: no conversion, no health read
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    static KilnApi api{};
    static KilnWorldApi world{};
    setup(api, world);
    static EmfApi t{};
    fill_events(&t);
    fill_hits(&t);
    CHECK(t.on_hit != nullptr);
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "no-mods") test_no_mods(t);
    else if (mode == "no-relay") test_no_relay(t);
    else if (mode == "mod-event") test_mod_event(t);
    else if (mode == "bad-layout") test_bad_layout(t);
    else if (mode == "bad-size") {
        hit_event_size = 256;
        CHECK(t.on_hit(&cb_a, nullptr));
        hits_on_ready(true);
        events_on_ready();
        hits_after_ready();
        CHECK(programs.count("emf_ev_HitEvent") == 0 && !t.on_hit(&cb_a, nullptr));
    }
    else {
        hit_flags_at = 152;  // the sigdb captures of this build agree with EMF's layout
        test_relay(t);
    }
    std::printf("hits_test%s%s: %d checks, %d failures\n", mode.empty() ? "" : " ", mode.c_str(), checks, failures);
    return failures ? 1 : 0;
}

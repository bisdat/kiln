// SPDX-License-Identifier: MIT
// EmfApi v39 enemy orders and v40 move orders on Kiln (0.2.0-dev.31.13): the production adapter (api_orders.cpp and
// orders_model.cpp) against fake kiln.creatures, kiln.creature_control, kiln.spawn and kiln.world. No game.
// Covers: order, cancel, target, move with arrive and the guard hold at the waypoint, moving, every refusal with its
// `why`, the broker's lease (admission, Active, preemption, failure, release and unbinding), death, despawn, a mod
// that stopped, and the world closing.
#include "emf_internal.h"
#include "services/kiln_creature_control.h"
#include "services/kiln_creatures.h"
#include "v26_model.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

namespace kemf {
const KilnApi* g_api;
KilnOwner g_owner = 77;
uint32_t g_side = KILN_SIDE_SERVER;
Services S;
thread_local Context t_ctx;
static Mod mod_a, mod_b;
static Mod* calling = &mod_a;
Mod* caller_mod(const void*) { return calling; }
const char* mod_name(const Mod* m) { return m ? m->id.c_str() : "?"; }
static std::vector<std::string> refusals, unsupported_calls;
void unsupported(const Mod*, const char* fn, const char*) { unsupported_calls.push_back(fn); }
void refuse(const Mod*, const char* fn, const char* why, bool) { refusals.push_back(std::string(fn) + ": " + why); }
void mod_logf(const Mod*, int, const char*, ...) {}
void logf(int, const char*, ...) {}
void note_call(const char*) {}
void mark_status_dirty() {}
void fill_terrain(EmfApi*) {}  // api_terrain.cpp has its own tests
static uint64_t clock_ms = 1000;
uint64_t now_ms() { return clock_ms; }
bool guarded(Mod*, const char*, ThunkFn fn, void* p) { fn(p); return true; }
bool seh_copy(void* dst, const void* src, size_t n) {
    if (reinterpret_cast<uintptr_t>(src) < 0x10000) return false;  // a bogus pointer: never dereferenced
    std::memcpy(dst, src, n);
    return true;
}
}  // namespace kemf
using namespace kemf;

// enemy01::State (sigdb/types; docs/re/creatures.md)
enum : uint8_t { ST_IDLE = 2, ST_COMBAT = 6, ST_ROAMING = 15, ST_GUARD = 20 };
constexpr uint8_t kPlayerAlly = 11;
constexpr uint8_t kWaypoint[16] = {0x0d, 0xf7, 0x71, 0x6d, 0x2c, 0x60, 0x55, 0x4d,
                                   0x9d, 0xe2, 0x73, 0xe5, 0xb8, 0x41, 0x33, 0xb5};

// ------------------------------------------------------------------------------------------- the fake world --------
struct Ent {
    KilnVec3 pos{};
    bool ai = false, dead = false, player = false, boss = false;
    uint8_t faction = 4, state = ST_IDLE;
    uint32_t target = 0, guard = 0;
    KilnVec3 home{};
    bool waypoint = false;
};
static std::map<uint32_t, Ent> ents;
static uint32_t next_entity = 5000;
static const uint32_t kPlayerStateHash = 0x51515151;
static const uint32_t kBossTpl = 0xB0;

static bool KILN_CALL w_exists(KilnEntity e) { return ents.count(e) != 0; }
static KilnResult KILN_CALL w_position(KilnEntity e, KilnVec3* out) {
    auto it = ents.find(e);
    if (it == ents.end()) return KILN_ERR_NOT_FOUND;
    *out = it->second.pos;
    return KILN_OK;
}
static uint32_t KILN_CALL w_hash(KilnStr n) { return std::string(n.ptr, n.len) == "PlayerState" ? kPlayerStateHash : 0; }
static const void* KILN_CALL w_component(KilnEntity e, uint32_t hash, uint32_t* size) {
    static uint8_t blob[8];
    auto it = ents.find(e);
    if (it == ents.end() || hash != kPlayerStateHash || !it->second.player) return nullptr;
    if (size) *size = 8;
    return blob;
}

// kiln.spawn
static bool spawn_off = false;
static int spawns = 0, destroys = 0;
static std::vector<KilnSpawnRequest> spawn_reqs;
static KilnResult KILN_CALL sp_spawn(const KilnSpawnRequest* r, KilnEntity* out) {
    ++spawns;
    if (spawn_off) return KILN_ERR_UNAVAILABLE;
    spawn_reqs.push_back(*r);
    CHECK(std::memcmp(r->template_guid.b, kWaypoint, 16) == 0);
    const uint32_t e = next_entity++;
    Ent w;
    w.pos = r->position;
    w.waypoint = true;
    ents[e] = w;
    *out = e;
    return KILN_OK;
}
static KilnResult KILN_CALL sp_destroy(KilnEntity e) {
    ++destroys;
    return ents.erase(e) ? KILN_OK : KILN_ERR_NOT_FOUND;
}
static KilnResult KILN_CALL sp_template_get(const KilnGuid*, KilnTemplateInfo*) { return KILN_ERR_NOT_FOUND; }

// kiln.creatures
struct Cmd {
    uint32_t kind;
    KilnEntity entity, target;
    float value;
};
static std::vector<Cmd> cmds;
struct Prefer {
    KilnEntity e, target;
    float seconds;
};
static std::vector<Prefer> prefers;
static std::map<KilnEntity, KilnEntity> held;  // target_prefer's hold
static float guard_target_distance = 3.0f;
static KilnResult KILN_CALL cr_inspect(KilnEntity e, KilnCreatureInfo* out) {
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    auto it = ents.find(e);
    if (it == ents.end()) return KILN_ERR_NOT_FOUND;
    const Ent& x = it->second;
    std::memset(reinterpret_cast<uint8_t*>(out) + 4, 0, sizeof *out - 4);
    out->flags = (x.ai ? uint32_t{KILN_CREATURE_I_AI} : 0u) | (x.dead ? uint32_t{KILN_CREATURE_I_DEAD} : 0u);
    out->state = x.ai ? x.state : 0;
    out->faction = x.faction;
    out->target = x.target;
    out->guard = x.guard;
    out->home = x.home;
    out->health = x.dead ? 0 : 100;
    out->health_max = x.ai ? 100 : 0;
    if (x.boss) out->template_guid.b[0] = kBossTpl;
    return KILN_OK;
}
static KilnResult KILN_CALL cr_template_get(const KilnGuid* g, KilnCreatureTemplate* out) {
    if (!g || g->b[0] != kBossTpl) return KILN_ERR_NOT_FOUND;
    out->flags = KILN_CREATURE_T_BOSS;
    return KILN_OK;
}
static KilnResult KILN_CALL cr_command(const KilnCreatureCommand* c) {
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    cmds.push_back({c->kind, c->entity, c->target, c->value});
    return KILN_OK;
}
static KilnResult KILN_CALL cr_prefer(KilnOwner owner, KilnEntity e, KilnEntity target, float seconds) {
    CHECK(owner == g_owner);
    prefers.push_back({e, target, seconds});
    if (target) {
        held[e] = target;
        ents[e].target = target;
    } else {
        held.erase(e);
    }
    return KILN_OK;
}
static std::vector<std::pair<KilnEntity, KilnVec3>> homes;
static KilnResult KILN_CALL cr_set(KilnOwner owner, KilnEntity e, uint32_t field, uint32_t, const KilnCreatureValue* v) {
    CHECK(owner == 0 && field == KILN_CF_HOME);  // a direct write: the creature lives there afterwards
    homes.push_back({e, v->vec});
    ents[e].home = v->vec;
    return KILN_OK;
}
static KilnResult KILN_CALL cr_ai_get(KilnEntity, KilnStr comp, KilnStr path, char* buf, size_t cap, size_t* len, uint32_t*) {
    CHECK(std::string(comp.ptr, comp.len) == "Enemy01BehaviorState" && std::string(path.ptr, path.len) == "guardSetup.targetDistance");
    const int n = std::snprintf(buf, cap, "%g", guard_target_distance);
    if (len) *len = static_cast<size_t>(n);
    return KILN_OK;
}

// kiln.creature_control: a small broker. A lease is admitted Pending and granted at the next pre().
struct FakeLease {
    KilnControlBodyKey key{};
    uint32_t role = 0, state = KILN_CONTROL_PENDING, result = 0;
    uint64_t seq = 0;
    std::string reason;
};
static std::map<KilnHandle, FakeLease> leases;
static std::map<KilnEntity, uint64_t> bound;  // entity -> generation
static uint64_t next_lease = 900, next_gen = 1, next_req = 1, seq_counter = 10;
static std::string refuse_next;  // the broker refuses the next admission with this reason
static int unbinds = 0, releases = 0;
static uint32_t live_leases(KilnEntity e) {
    uint32_t n = 0;
    for (auto& [h, l] : leases)
        if (l.key.entity == e && l.state != KILN_CONTROL_RELEASED && l.state != KILN_CONTROL_FAILED) ++n;
    return n;
}
static KilnResult KILN_CALL cc_bind(KilnOwner owner, KilnEntity e, uint32_t kind, uint32_t retire, KilnControlBodyKey* out) {
    CHECK(owner == g_owner && kind == KILN_CONTROL_BASELINE_ENEMY && retire == 1);
    if (!ents.count(e)) return KILN_ERR_NOT_FOUND;
    if (!bound.count(e)) bound[e] = next_gen++;
    *out = KilnControlBodyKey{7, bound[e], e, 0};
    return KILN_OK;
}
static KilnResult KILN_CALL cc_next(KilnOwner, uint64_t* out) {
    *out = next_req++;
    return KILN_OK;
}
static void fill_state(KilnHandle h, const FakeLease& l, KilnControlState* out) {
    out->lease_state = l.state;
    out->result = l.result;
    out->requested_role = l.role;
    out->body = l.key;
    out->lease = h;
    out->transition_sequence = l.seq;
    std::snprintf(out->reason, sizeof out->reason, "%s", l.reason.c_str());
}
static KilnResult KILN_CALL cc_acquire(KilnOwner owner, const KilnControlRequest* r, KilnControlState* out) {
    CHECK(owner == g_owner && r->size == sizeof *r && r->request_id != 0);
    if (!refuse_next.empty()) {
        FakeLease l;
        l.state = KILN_CONTROL_FAILED;
        l.result = KILN_CONTROL_RESULT_BUSY;
        l.reason = refuse_next;
        refuse_next.clear();
        fill_state(0, l, out);
        out->lease = 0;
        return KILN_OK;
    }
    // the same owner's next base role replaces its old one at once (Replaced)
    const bool base = r->role != KILN_CONTROL_ROLE_PET_COMBAT;
    if (base)
        for (auto& [h, l] : leases)
            if (l.key.entity == r->body.entity && l.role != KILN_CONTROL_ROLE_PET_COMBAT && l.state <= KILN_CONTROL_PREEMPTED) {
                l.state = KILN_CONTROL_RELEASED;
                l.result = KILN_CONTROL_RESULT_CANCELED;
                l.reason = "Replaced";
            }
    FakeLease l;
    l.key = r->body;
    l.role = r->role;
    const KilnHandle h = next_lease++;
    leases[h] = l;
    fill_state(h, l, out);
    return KILN_OK;
}
static KilnResult KILN_CALL cc_query(KilnOwner, KilnHandle h, KilnControlState* out) {
    auto it = leases.find(h);
    if (it == leases.end()) return KILN_ERR_NOT_FOUND;
    fill_state(h, it->second, out);
    return KILN_OK;
}
static KilnResult KILN_CALL cc_can_drive(KilnOwner owner, KilnHandle h, KilnControlBodyKey key, uint64_t seq, uint32_t* out) {
    auto it = leases.find(h);
    *out = owner == g_owner && it != leases.end() && it->second.state == KILN_CONTROL_ACTIVE && it->second.seq == seq &&
           it->second.key.entity == key.entity && it->second.key.body_generation == key.body_generation;
    return KILN_OK;
}
static KilnResult KILN_CALL cc_release(KilnOwner, KilnHandle h) {
    CHECK(g_api->on_tick_thread());
    auto it = leases.find(h);
    if (it == leases.end()) return KILN_ERR_NOT_FOUND;
    ++releases;
    if (it->second.state <= KILN_CONTROL_PREEMPTED) {
        it->second.state = KILN_CONTROL_RELEASED;
        it->second.result = KILN_CONTROL_RESULT_RELEASED;
    }
    return KILN_OK;
}
static KilnResult KILN_CALL cc_release_body(KilnOwner, KilnControlBodyKey key) {
    if (!bound.count(key.entity)) return KILN_ERR_NOT_FOUND;
    if (live_leases(key.entity)) return KILN_ERR_INVALID_ARGUMENT;
    bound.erase(key.entity);
    ++unbinds;
    return KILN_OK;
}
static void broker_pre() {  // the broker's Pre: Pending leases are granted (AI-on: nothing to remove)
    for (auto& [h, l] : leases)
        if (l.state == KILN_CONTROL_PENDING) {
            l.state = KILN_CONTROL_ACTIVE;
            l.result = KILN_CONTROL_RESULT_GRANTED;
            l.seq = ++seq_counter;
        }
}
static FakeLease* lease_of(KilnEntity e, uint32_t role) {
    for (auto& [h, l] : leases)
        if (l.key.entity == e && l.role == role && l.state <= KILN_CONTROL_PREEMPTED) return &l;
    return nullptr;
}

// ------------------------------------------------------------------------------------------- the core ---------------
static bool tick_thread = true;
static uintptr_t server_ptr = 0x1000, client_ptr = 0x2000;
static uintptr_t KILN_CALL api_world(uint32_t k) { return k == KILN_WORLD_SERVER ? server_ptr : k == KILN_WORLD_CLIENT ? client_ptr : 0; }
static bool KILN_CALL api_tick() { return tick_thread; }
static KilnCreaturesApi creatures_api{};
static KilnCreatureControlApi control_api{};
static bool control_published = true;
static const void* KILN_CALL api_service(KilnStr name, uint32_t) {
    const std::string n(name.ptr, name.len);
    if (n == KILN_CREATURES_SERVICE) return &creatures_api;
    if (n == KILN_CREATURE_CONTROL_SERVICE) return control_published ? &control_api : nullptr;
    return nullptr;
}

static EmfWorld* W() { return reinterpret_cast<EmfWorld*>(server_ptr); }
static EmfWorld* CW() { return reinterpret_cast<EmfWorld*>(client_ptr); }
// one server step: the broker's Pre, then kiln-emf's after-step
static void step(uint64_t ms = 100) {
    clock_ms += ms;
    broker_pre();
    orders_after_step(W(), EMF_WORLD_SERVER);
}

struct Done {
    EmfWorld* world;
    uint32_t id, status;
};
static std::vector<Done> done;
static void move_cb(EmfWorld* w, uint32_t id, uint32_t status, void*) { done.push_back({w, id, status}); }

static uint32_t creature(double x, double z, uint8_t faction = 4) {
    const uint32_t e = next_entity++;
    Ent c;
    c.ai = true;
    c.pos = KilnVec3{x, 10, z};
    c.faction = faction;
    ents[e] = c;
    return e;
}
static uint32_t thing(double x, double z) {
    const uint32_t e = next_entity++;
    Ent c;
    c.pos = KilnVec3{x, 10, z};
    ents[e] = c;
    return e;
}
static size_t count_cmds(uint32_t kind, uint32_t e, uint32_t target) {
    size_t n = 0;
    for (const Cmd& c : cmds) n += c.kind == kind && c.entity == e && c.target == target;
    return n;
}

static EmfEnemyOrder order_req(uint32_t enemy, uint32_t target, float seconds = 0, uint32_t flags = 0, int32_t damage = 0) {
    EmfEnemyOrder o{};
    o.size = sizeof o;
    o.enemy = enemy;
    o.target = target;
    o.seconds = seconds;
    o.flags = flags;
    o.damage = damage;
    std::memset(o.why, 0x5a, sizeof o.why);
    return o;
}
static EmfPathPoint pts[17];
static EmfMoveOrder move_req(uint32_t enemy, uint32_t count, float arrive = 0, float seconds = 0, uint32_t flags = 0) {
    EmfMoveOrder o{};
    o.size = sizeof o;
    o.enemy = enemy;
    o.count = count;
    o.points = pts;
    o.arrive = arrive;
    o.seconds = seconds;
    o.flags = flags;
    std::memset(o.why, 0x5a, sizeof o.why);
    return o;
}

int main() {
    KilnApi api{};
    api.world = &api_world;
    api.on_tick_thread = &api_tick;
    api.service = &api_service;
    g_api = &api;
    KilnWorldApi world{};
    world.exists = &w_exists;
    world.position = &w_position;
    world.component = &w_component;
    world.component_hash = &w_hash;
    S.world = &world;
    KilnSpawnApi spawn{};
    spawn.spawn = &sp_spawn;
    spawn.destroy = &sp_destroy;
    spawn.template_get = &sp_template_get;
    S.spawn = &spawn;
    creatures_api.header.size = sizeof creatures_api;
    creatures_api.inspect = &cr_inspect;
    creatures_api.template_get = &cr_template_get;
    creatures_api.command = &cr_command;
    creatures_api.target_prefer = &cr_prefer;
    creatures_api.set = &cr_set;
    creatures_api.ai_get = &cr_ai_get;
    control_api.header.size = sizeof control_api;
    control_api.bind_body = &cc_bind;
    control_api.next_request = &cc_next;
    control_api.acquire_control = &cc_acquire;
    control_api.query_control = &cc_query;
    control_api.can_drive = &cc_can_drive;
    control_api.release_control = &cc_release;
    control_api.release_body = &cc_release_body;
    mod_a.id = "beastmaster";
    mod_a.status = EMF_MOD_LOADED;
    mod_b.id = "other";
    mod_b.status = EMF_MOD_LOADED;
    ents[1] = Ent{};
    ents[1].player = true;  // a player
    ents[1].pos = KilnVec3{0, 10, 0};

    EmfApi t{};
    fill_v40(&t);
    fill_orders(&t);
    CHECK(t.enemy_order && t.enemy_order_cancel && t.enemy_target && t.enemy_move && t.enemy_move_cancel && t.enemy_moving);
    if (!(t.enemy_order && t.enemy_order_cancel && t.enemy_target && t.enemy_move && t.enemy_move_cancel && t.enemy_moving)) {
        std::printf("orders_test: %d checks, %d failures (the six slots are not carried)\n", checks, failures);
        return 1;
    }
    ContextScope cb(&mod_a, W(), true);  // as inside the mod's on_tick on the server world

    // ------------------------------------------------------------------------------- enemy_order refusals ---------
    const uint32_t wolf = creature(5, 0, kPlayerAlly), boar = creature(8, 0), dummy = thing(12, 0);
    ents[wolf].guard = 1;  // a tamed beast guards its owner
    ents[wolf].state = ST_GUARD;
    {
        EmfEnemyOrder o = order_req(wolf, dummy);
        o.size = 8;  // too small: untouched, no why
        const EmfEnemyOrder saved = o;
        CHECK(t.enemy_order(W(), &o) == 0 && std::memcmp(&o, &saved, sizeof o) == 0);
        CHECK(t.enemy_order(W(), nullptr) == 0);
    }
    auto order_why = [&](EmfEnemyOrder o, EmfWorld* w = nullptr) {
        const uint32_t id = t.enemy_order(w ? w : W(), &o);
        CHECK(id == 0);
        return std::string(o.why);
    };
    {
        Context saved = t_ctx;
        t_ctx.world_cb = false;  // not inside a world callback
        CHECK(order_why(order_req(wolf, dummy, 0, EMF_ORDER_ALLIES)).find("world callback") != std::string::npos);
        t_ctx = saved;
        ContextScope client(&mod_a, CW(), true);  // a joined client's world callback
        CHECK(order_why(order_req(wolf, dummy, 0, EMF_ORDER_ALLIES), CW()) == "not a server world (host only)");
        CHECK(t.enemy_target(CW(), wolf) == 0 && t.enemy_moving(CW(), wolf) == 0);
    }
    CHECK(order_why(order_req(wolf, dummy, 0, 0x2)) == "unknown flags (a newer mod?)");
    CHECK(order_why(order_req(wolf, dummy, -1, EMF_ORDER_ALLIES)) == "seconds must be 0 or more (0: until it ends otherwise)");
    CHECK(order_why(order_req(wolf, dummy, NAN, EMF_ORDER_ALLIES)) == "seconds must be 0 or more (0: until it ends otherwise)");
    CHECK(order_why(order_req(wolf, dummy, 0, EMF_ORDER_ALLIES, 100001)) == "damage must be 0..100000");
    CHECK(order_why(order_req(wolf, wolf, 0, EMF_ORDER_ALLIES)) == "an enemy and another entity to go for");
    CHECK(order_why(order_req(0, dummy)) == "an enemy and another entity to go for");
    CHECK(order_why(order_req(1, dummy)) == "that is a player");
    CHECK(order_why(order_req(dummy, boar)) == "it has no enemy AI to send (DynamicAggro, SelectedTargets)");
    CHECK(order_why(order_req(wolf, dummy)) == "it is a player's ally (a tamed beast): pass EMF_ORDER_ALLIES to send it");
    CHECK(order_why(order_req(wolf, 999999, 0, EMF_ORDER_ALLIES)) == "no such target (or it has no position)");
    {
        const uint32_t corpse = creature(20, 0);
        ents[corpse].dead = true;
        CHECK(order_why(order_req(wolf, corpse, 0, EMF_ORDER_ALLIES)) == "the target is dead");
        const uint32_t troll = creature(30, 0);
        ents[troll].boss = true;
        CHECK(order_why(order_req(troll, wolf)) == "it is a boss: its fight and its leash stay the game's");
    }
    // Kiln carries no hit rules: an order with damage is refused, as EMF refuses it on a build without them
    CHECK(order_why(order_req(wolf, dummy, 0, EMF_ORDER_ALLIES, 200)) ==
          "its damage could not be added (hit rules are off on this game build)");
    refuse_next = "Busy: another owner holds the body (PetFollow)";
    CHECK(order_why(order_req(wolf, dummy, 0, EMF_ORDER_ALLIES)) ==
          "creature control: Busy: another owner holds the body (PetFollow)");
    CHECK(leases.empty() && bound.empty());  // a refused admission leaves no binding behind
    CHECK(prefers.empty() && cmds.empty() && spawns == 0);  // no refusal wrote anything
    control_published = false;  // (the services are looked up once; this only shows the order of the checks)

    // ------------------------------------------------------------------------------- enemy_order: the order -------
    control_published = true;
    EmfEnemyOrder o = order_req(wolf, dummy, 10, EMF_ORDER_ALLIES);
    const uint32_t o1 = t.enemy_order(W(), &o);
    CHECK(o1 == 1 && o.why[0] == 0);
    FakeLease* l1 = lease_of(wolf, KILN_CONTROL_ROLE_PET_COMBAT);
    CHECK(l1 && l1->state == KILN_CONTROL_PENDING);  // admitted under the broker: an AI-on combat lease
    if (!l1) {
        std::printf("orders_test: %d checks, %d failures (no order was admitted)\n", checks, failures);
        return 1;
    }
    CHECK(prefers.empty() && cmds.empty());           // nothing before the lease is Active
    step();
    CHECK(prefers.size() == 1 && prefers[0].e == wolf && prefers[0].target == dummy && prefers[0].seconds > 9);
    CHECK(count_cmds(KILN_CREATURE_CMD_ATTACK, wolf, dummy) == 1);  // AddThreatTargetEvent: an enemy at rest fights
    CHECK(t.enemy_target(W(), wolf) == dummy);                        // its pick (SelectedTargets slot 0)
    CHECK(t.enemy_target(W(), dummy) == 0 && t.enemy_target(W(), 999999) == 0 && t.enemy_target(nullptr, wolf) == 0);
    ents[wolf].state = ST_COMBAT;
    for (int i = 0; i < 20; ++i) step();  // 2 s: in a fight with its pick, no new threat
    CHECK(count_cmds(KILN_CREATURE_CMD_ATTACK, wolf, dummy) == 1 && prefers.size() == 1);
    ents[wolf].state = ST_GUARD;  // it dropped out of the fight: the threat again (every 3 s at most)
    for (int i = 0; i < 31; ++i) step();
    CHECK(count_cmds(KILN_CREATURE_CMD_ATTACK, wolf, dummy) == 2);
    // preempted (a ride): the hold is let go; resumed with a new transition: held again
    l1 = lease_of(wolf, KILN_CONTROL_ROLE_PET_COMBAT);
    l1->state = KILN_CONTROL_PREEMPTED;
    step();
    CHECK(prefers.back().target == 0 && held.count(wolf) == 0);
    l1->state = KILN_CONTROL_PENDING;
    step();
    CHECK(prefers.back().target == dummy && held[wolf] == dummy);
    // its time runs out: let go, the lease released, the body unbound
    for (int i = 0; i < 60; ++i) step();
    CHECK(held.count(wolf) == 0 && !lease_of(wolf, KILN_CONTROL_ROLE_PET_COMBAT));
    step();
    CHECK(bound.count(wolf) == 0 && !t.enemy_order_cancel(o1));

    // cancel inside a server world callback: at once
    o = order_req(wolf, dummy, 0, EMF_ORDER_ALLIES);
    const uint32_t o2 = t.enemy_order(W(), &o);
    CHECK(o2 == 2);
    step();
    CHECK(held[wolf] == dummy);
    CHECK(t.enemy_order_cancel(o2) && held.count(wolf) == 0 && !lease_of(wolf, KILN_CONTROL_ROLE_PET_COMBAT));
    CHECK(!t.enemy_order_cancel(o2) && !t.enemy_order_cancel(0) && !t.enemy_order_cancel(12345));
    // cancel from another thread: true now, it ends at the next step
    o = order_req(wolf, dummy, 0, EMF_ORDER_ALLIES);
    const uint32_t o3 = t.enemy_order(W(), &o);
    step();
    tick_thread = false;
    CHECK(t.enemy_order_cancel(o3) && held[wolf] == dummy);
    CHECK(t.enemy_target(W(), wolf) == 0);  // off the tick thread nothing is read
    tick_thread = true;
    step();
    CHECK(held.count(wolf) == 0 && !lease_of(wolf, KILN_CONTROL_ROLE_PET_COMBAT) && !t.enemy_order_cancel(o3));
    // a new order on the same enemy ends the old one; the target dying ends it
    const uint32_t rat = creature(15, 0);
    o = order_req(wolf, dummy, 0, EMF_ORDER_ALLIES);
    const uint32_t o4 = t.enemy_order(W(), &o);
    o = order_req(wolf, rat, 0, EMF_ORDER_ALLIES);
    const uint32_t o5 = t.enemy_order(W(), &o);
    CHECK(o5 == o4 + 1 && !t.enemy_order_cancel(o4));
    step();
    CHECK(held[wolf] == rat);
    ents[rat].dead = true;
    step();
    CHECK(held.count(wolf) == 0 && !t.enemy_order_cancel(o5) && !lease_of(wolf, KILN_CONTROL_ROLE_PET_COMBAT));
    // the enemy dies or despawns: the order ends and its lease goes
    o = order_req(boar, dummy);
    const uint32_t o6 = t.enemy_order(W(), &o);
    step();
    CHECK(held[boar] == dummy);
    ents[boar].dead = true;
    step();
    CHECK(!t.enemy_order_cancel(o6) && !lease_of(boar, KILN_CONTROL_ROLE_PET_COMBAT));
    ents[boar].dead = false;
    o = order_req(boar, dummy);
    const uint32_t o7 = t.enemy_order(W(), &o);
    step();
    ents.erase(boar);
    step();
    CHECK(!t.enemy_order_cancel(o7));
    const uint32_t boar2 = creature(8, 0);
    // the broker ends the lease (BodyNotReady): the order ends
    o = order_req(boar2, dummy);
    const uint32_t o8 = t.enemy_order(W(), &o);
    FakeLease* lb = lease_of(boar2, KILN_CONTROL_ROLE_PET_COMBAT);
    lb->state = KILN_CONTROL_FAILED;
    lb->result = KILN_CONTROL_RESULT_TIMED_OUT;
    lb->reason = "BodyNotReady";
    clock_ms += 100;
    orders_after_step(W(), EMF_WORLD_SERVER);
    CHECK(!t.enemy_order_cancel(o8) && held.count(boar2) == 0);
    // Beastmaster renews its training order every few seconds: the new order takes over the live lease (no release,
    // no new admission), the hold never lapses, and a new target is written at the next step
    {
        const uint32_t pup = creature(9, 0, kPlayerAlly), dummy2 = thing(14, 0);
        o = order_req(pup, dummy, 4, EMF_ORDER_ALLIES);
        const uint32_t r1 = t.enemy_order(W(), &o);
        step();
        const KilnHandle first = leases.rbegin()->first;
        const int rel = releases;
        const size_t lets_go = prefers.size();
        o = order_req(pup, dummy, 4, EMF_ORDER_ALLIES);
        const uint32_t r2 = t.enemy_order(W(), &o);
        CHECK(r2 == r1 + 1 && !t.enemy_order_cancel(r1) && releases == rel && leases.rbegin()->first == first);
        CHECK(held[pup] == dummy && prefers.size() == lets_go);  // not let go in between
        step();
        CHECK(prefers.back().target == dummy && prefers.back().seconds > 3.5f && held[pup] == dummy);
        o = order_req(pup, dummy2, 4, EMF_ORDER_ALLIES);
        const uint32_t r3 = t.enemy_order(W(), &o);
        step();
        CHECK(held[pup] == dummy2 && releases == rel && leases.rbegin()->first == first);
        CHECK(t.enemy_order_cancel(r3) && held.count(pup) == 0 && releases == rel + 1);
        ents.erase(pup);
        step();
    }

    // ------------------------------------------------------------------------------- enemy_move refusals ----------
    pts[0] = EmfPathPoint{25, 10, 0, 0};
    pts[1] = EmfPathPoint{25, 10, 20, 0};
    auto move_why = [&](EmfMoveOrder m) {
        const uint32_t id = t.enemy_move(W(), &m, &move_cb, nullptr);
        CHECK(id == 0);
        return std::string(m.why);
    };
    const int spawns_before = spawns;
    {
        EmfMoveOrder m = move_req(wolf, 1);
        m.size = 127;
        const EmfMoveOrder saved = m;
        CHECK(t.enemy_move(W(), &m, &move_cb, nullptr) == 0 && std::memcmp(&m, &saved, sizeof m) == 0);
    }
    CHECK(move_why(move_req(wolf, 1, 0, 0, EMF_MOVE_HOLD)) == "EMF_MOVE_HOLD is for villager_move");
    CHECK(move_why(move_req(wolf, 1, 0, 0, 0x40)) == "unknown flags (a newer mod?)");
    CHECK(move_why(move_req(wolf, 1, 0, 0, EMF_MOVE_ALLIES | EMF_MOVE_PEACEFUL)) ==
          "EMF_MOVE_PEACEFUL is not available on Kiln yet (no fight is cleared on the way)");
    CHECK(move_why(move_req(wolf, 1, 0, 0, EMF_MOVE_ALLIES | EMF_MOVE_PEACEFUL_ALL)) ==
          "EMF_MOVE_PEACEFUL_ALL is not available on Kiln yet (no fight is cleared on the way)");
    CHECK(move_why(move_req(wolf, 1, 0, 0, EMF_MOVE_ALLIES | EMF_MOVE_CLEAR)) ==
          "EMF_MOVE_CLEAR is not available on Kiln yet (no wall check at a point)");
    CHECK(move_why(move_req(wolf, 0, 0, 0, EMF_MOVE_ALLIES)) == "1 to 16 points");
    CHECK(move_why(move_req(wolf, 17, 0, 0, EMF_MOVE_ALLIES)) == "1 to 16 points");
    {
        EmfMoveOrder m = move_req(wolf, 1, 0, 0, EMF_MOVE_ALLIES);
        m.points = nullptr;
        CHECK(move_why(m) == "1 to 16 points");
        m.points = reinterpret_cast<const EmfPathPoint*>(uintptr_t{8});  // unreadable: refused, never dereferenced
        CHECK(move_why(m) == "its points cannot be read");
    }
    pts[2] = EmfPathPoint{NAN, 0, 0, 0};
    CHECK(move_why(move_req(wolf, 3, 0, 0, EMF_MOVE_ALLIES)) == "a point is not a number");
    CHECK(move_why(move_req(wolf, 1, 0.3f, 0, EMF_MOVE_ALLIES)) == "arrive must be 0.5 to 50 metres");
    CHECK(move_why(move_req(wolf, 1, 51, 0, EMF_MOVE_ALLIES)) == "arrive must be 0.5 to 50 metres");
    CHECK(move_why(move_req(wolf, 1, 0, 3601, EMF_MOVE_ALLIES)) == "seconds must be 0 (no limit) to 3600");
    {
        EmfMoveOrder m = move_req(wolf, 1, 0, 0, EMF_MOVE_ALLIES);
        m.stuck_seconds = 1;
        CHECK(move_why(m) == "stuck_seconds must be 0 (15) or 2 to 120");
        // a v40 mod's 128-byte order: the bytes after it are never read
        auto old = std::make_unique<uint8_t[]>(v26::kMoveOrderV40Size + 8);
        std::memset(old.get(), 0xff, v26::kMoveOrderV40Size + 8);
        std::memcpy(old.get(), &m, v26::kMoveOrderV40Size);
        auto* req = reinterpret_cast<EmfMoveOrder*>(old.get());
        req->size = v26::kMoveOrderV40Size;
        const uint32_t id = t.enemy_move(W(), req, nullptr, nullptr);
        CHECK(id != 0 && req->why[0] == 0);
        CHECK(t.enemy_move_cancel(id));
        CHECK(old[v26::kMoveOrderV40Size] == 0xff);
    }
    CHECK(move_why(move_req(0, 1)) == "no creature");
    CHECK(move_why(move_req(1, 1)) == "that is a player");
    CHECK(move_why(move_req(dummy, 1)) == "it has no enemy AI to move (DynamicEnemy01BehaviorState)");
    CHECK(move_why(move_req(wolf, 1)) == "it is a player's ally (a tamed beast): pass EMF_MOVE_ALLIES to move it");
    {
        const uint32_t troll = creature(40, 0);
        ents[troll].boss = true;
        CHECK(move_why(move_req(troll, 1)) == "it is a boss: its fight and its leash stay the game's");
        const uint32_t corpse = creature(41, 0);
        ents[corpse].dead = true;
        CHECK(move_why(move_req(corpse, 1)) == "it is dead");
    }
    refuse_next = "Busy: another owner holds the body (PetFollow)";
    CHECK(move_why(move_req(wolf, 1, 0, 0, EMF_MOVE_ALLIES)) == "creature control: Busy: another owner holds the body (PetFollow)");
    spawn_off = true;
    CHECK(move_why(move_req(wolf, 1, 0, 0, EMF_MOVE_ALLIES)) ==
          "its waypoint could not be spawned (spawn is off on this game build?)");
    spawn_off = false;
    CHECK(!lease_of(wolf, KILN_CONTROL_ROLE_ENGINE_GUARD));  // the refused move's lease went at once
    CHECK(done.empty() && t.enemy_moving(W(), wolf) == 0);
    CHECK(spawns == spawns_before + 2);  // the 128-byte order's waypoint, and the refused spawn

    // ------------------------------------------------------------------------------- enemy_move: arrive -----------
    step();
    step();  // the body from above is unbound
    cmds.clear();
    EmfMoveOrder m = move_req(wolf, 2, 2, 0, EMF_MOVE_ALLIES);
    const uint32_t m1 = t.enemy_move(W(), &m, &move_cb, nullptr);
    CHECK(m1 != 0 && m.why[0] == 0 && t.enemy_moving(W(), wolf) == m1);
    FakeLease* lg = lease_of(wolf, KILN_CONTROL_ROLE_ENGINE_GUARD);
    CHECK(lg && lg->state == KILN_CONTROL_PENDING);  // the game's own guard drives: an AI-on EngineGuard lease
    const uint32_t wp1 = next_entity - 1;
    CHECK(ents.count(wp1) && ents[wp1].waypoint && ents[wp1].pos.x == 25 && ents[wp1].pos.z == 0);
    CHECK(spawn_reqs.back().flags & KILN_SPAWN_NO_TRACK);
    CHECK(cmds.empty());  // no guard order before the lease is Active
    tick_thread = false;
    CHECK(t.enemy_moving(W(), wolf) == m1);  // any thread
    tick_thread = true;
    step();
    CHECK(count_cmds(KILN_CREATURE_CMD_GUARD, wolf, wp1) == 1);
    ents[wolf].guard = wp1;
    ents[wolf].state = ST_GUARD;
    // walking: 1 m closer every 1.5 s keeps it from being stuck
    for (int i = 0; i < 15; ++i) {
        ents[wolf].pos.x += 1;
        for (int k = 0; k < 15; ++k) step();
    }
    CHECK(done.empty() && t.enemy_moving(W(), wolf) == m1);
    // the guard hold: a guard stops its guard distance short of the waypoint (targetDistance 3 x up to 1.2, + 1 m);
    // 4.5 m short counts as reached although arrive is 2
    ents[wolf].pos.x = 25 - 4.5;
    step();
    const uint32_t wp2 = next_entity - 1;
    CHECK(wp2 != wp1 && !ents.count(wp1) && ents[wp2].pos.z == 20);  // on to the next point
    CHECK(count_cmds(KILN_CREATURE_CMD_GUARD, wolf, wp2) == 1 && done.empty());
    ents[wolf].guard = 0;  // not guarding the new waypoint yet: arrive alone (2 m) counts
    ents[wolf].pos = KilnVec3{25, 10, 20 - 3.0};
    step();
    CHECK(done.empty());
    ents[wolf].pos = KilnVec3{25.5, 11.5, 20 - 1.5};
    step();
    CHECK(done.size() == 1 && done[0].id == m1 && done[0].status == EMF_MOVE_ARRIVED && done[0].world == W());
    CHECK(t.enemy_moving(W(), wolf) == 0 && !ents.count(wp2) && !t.enemy_move_cancel(m1));
    // afterwards it guards whom it guarded before (its owner); no home is written for it
    CHECK(count_cmds(KILN_CREATURE_CMD_STOP_GUARD, wolf, wp2) == 1 && count_cmds(KILN_CREATURE_CMD_GUARD, wolf, 1) == 1);
    CHECK(homes.empty() && !lease_of(wolf, KILN_CONTROL_ROLE_ENGINE_GUARD));
    step();
    CHECK(bound.count(wolf) == 0);

    // a wild creature that arrives lives there (its home is the last point); arrive counts up and down too
    done.clear();
    const uint32_t vukah = creature(0, 50);
    ents[vukah].state = ST_ROAMING;
    pts[0] = EmfPathPoint{10, 10, 50, 0};
    m = move_req(vukah, 1, 1);
    const uint32_t m2 = t.enemy_move(W(), &m, &move_cb, nullptr);
    CHECK(m2 > m1);
    step();
    ents[vukah].pos = KilnVec3{10, 14, 50};  // 4 m up: beyond arrive 1 + 2
    step();
    CHECK(done.empty());
    ents[vukah].pos = KilnVec3{10, 12.5, 50.5};
    step();
    CHECK(done.size() == 1 && done[0].status == EMF_MOVE_ARRIVED);
    CHECK(homes.size() == 1 && homes[0].first == vukah && homes[0].second.x == 10 && homes[0].second.z == 50);

    // ------------------------------------------------------------------------------- stuck, timeout, gone ---------
    done.clear();
    m = move_req(vukah, 1);  // no progress for 15 s while not fighting: stuck
    const uint32_t m3 = t.enemy_move(W(), &m, &move_cb, nullptr);
    pts[0] = EmfPathPoint{60, 10, 50, 0};
    m = move_req(vukah, 1);
    const uint32_t m4 = t.enemy_move(W(), &m, &move_cb, nullptr);  // a new move ends the old one (cancelled), at once
    CHECK(done.size() == 1 && done[0].id == m3 && done[0].status == EMF_MOVE_CANCELLED && t.enemy_moving(W(), vukah) == m4);
    step();
    ents[vukah].state = ST_COMBAT;  // a fight on the way: the stuck clock waits
    ents[vukah].target = 1;
    for (int i = 0; i < 200; ++i) step();
    CHECK(done.size() == 1);
    ents[vukah].state = ST_IDLE;
    ents[vukah].target = 0;
    for (int i = 0; i < 149; ++i) step();
    CHECK(done.size() == 1);
    step();
    step();
    CHECK(done.size() == 2 && done[1].id == m4 && done[1].status == EMF_MOVE_STUCK && t.enemy_moving(W(), vukah) == 0);
    // stuck_seconds (v41): 5 s
    m = move_req(vukah, 1);
    m.stuck_seconds = 5;
    const uint32_t m5 = t.enemy_move(W(), &m, &move_cb, nullptr);
    for (int i = 0; i < 52; ++i) step();
    CHECK(done.size() == 3 && done[2].id == m5 && done[2].status == EMF_MOVE_STUCK);
    // timeout
    m = move_req(vukah, 1, 0, 2);
    const uint32_t m6 = t.enemy_move(W(), &m, &move_cb, nullptr);
    for (int i = 0; i < 21; ++i) {
        ents[vukah].pos.x += 0.5;  // progress: not stuck
        step();
    }
    CHECK(done.size() == 4 && done[3].id == m6 && done[3].status == EMF_MOVE_TIMEOUT);
    // the creature dies: gone, no guard order after it
    m = move_req(vukah, 1);
    const uint32_t m7 = t.enemy_move(W(), &m, &move_cb, nullptr);
    step();
    const size_t cmds_before = cmds.size();
    ents[vukah].dead = true;
    step();
    CHECK(done.size() == 5 && done[4].id == m7 && done[4].status == EMF_MOVE_GONE && cmds.size() == cmds_before);
    CHECK(!lease_of(vukah, KILN_CONTROL_ROLE_ENGINE_GUARD));
    ents[vukah].dead = false;
    // its waypoint vanishes: placed again (at most 3 times, every 3 s)
    m = move_req(vukah, 1);
    const uint32_t m8 = t.enemy_move(W(), &m, &move_cb, nullptr);
    step();
    for (int round = 0; round < 3; ++round) {
        ents.erase(next_entity - 1);
        const uint32_t before = next_entity;
        for (int i = 0; i < 31; ++i) {
            ents[vukah].pos.x += 0.1;
            step();
        }
        CHECK(next_entity == before + 1);
    }
    ents.erase(next_entity - 1);
    for (int i = 0; i < 31; ++i) step();
    CHECK(done.size() == 6 && done[5].id == m8 && done[5].status == EMF_MOVE_STUCK);
    // the lease fails later (BodyNotReady): the move ends stuck; the body goes: gone
    m = move_req(vukah, 1);
    const uint32_t m9 = t.enemy_move(W(), &m, &move_cb, nullptr);
    lg = lease_of(vukah, KILN_CONTROL_ROLE_ENGINE_GUARD);
    lg->state = KILN_CONTROL_FAILED;
    lg->result = KILN_CONTROL_RESULT_TIMED_OUT;
    lg->reason = "BodyNotReady";
    clock_ms += 100;
    orders_after_step(W(), EMF_WORLD_SERVER);
    CHECK(done.size() == 7 && done[6].id == m9 && done[6].status == EMF_MOVE_STUCK);
    m = move_req(vukah, 1);
    const uint32_t m10 = t.enemy_move(W(), &m, &move_cb, nullptr);
    lg = lease_of(vukah, KILN_CONTROL_ROLE_ENGINE_GUARD);
    lg->state = KILN_CONTROL_FAILED;
    lg->result = KILN_CONTROL_RESULT_BODY_GONE;
    clock_ms += 100;
    orders_after_step(W(), EMF_WORLD_SERVER);
    CHECK(done.size() == 8 && done[7].id == m10 && done[7].status == EMF_MOVE_GONE);

    // a ride preempts the move: no guard order, no arrival, the stuck clock waits; on resume the guard order again
    done.clear();
    {
        pts[0] = EmfPathPoint{90, 10, 50, 0};
        m = move_req(vukah, 1);
        const uint32_t mp = t.enemy_move(W(), &m, &move_cb, nullptr);
        step();
        const uint32_t wp = next_entity - 1;
        CHECK(count_cmds(KILN_CREATURE_CMD_GUARD, vukah, wp) == 1);
        lease_of(vukah, KILN_CONTROL_ROLE_ENGINE_GUARD)->state = KILN_CONTROL_PREEMPTED;
        const KilnVec3 before = ents[vukah].pos;
        ents[vukah].pos = KilnVec3{90, 10, 50};  // carried there by its rider
        for (int i = 0; i < 200; ++i) {
            clock_ms += 100;
            orders_after_step(W(), EMF_WORLD_SERVER);  // no broker Pre: it stays preempted
        }
        CHECK(done.empty() && t.enemy_moving(W(), vukah) == mp && count_cmds(KILN_CREATURE_CMD_GUARD, vukah, wp) == 1);
        ents[vukah].pos = before;
        lease_of(vukah, KILN_CONTROL_ROLE_ENGINE_GUARD)->state = KILN_CONTROL_PENDING;
        step();
        CHECK(count_cmds(KILN_CREATURE_CMD_GUARD, vukah, wp) == 2 && done.empty());
        CHECK(t.enemy_move_cancel(mp) && done.size() == 1 && done[0].status == EMF_MOVE_CANCELLED);
    }

    // ------------------------------------------------------------------------------- cancel and hand-overs --------
    done.clear();
    m = move_req(vukah, 1);
    const uint32_t m11 = t.enemy_move(W(), &m, &move_cb, nullptr);
    step();
    CHECK(t.enemy_move_cancel(m11) && done.size() == 1 && done[0].status == EMF_MOVE_CANCELLED);  // at once
    CHECK(!t.enemy_move_cancel(m11) && t.enemy_moving(W(), vukah) == 0);
    m = move_req(vukah, 1);
    const uint32_t m12 = t.enemy_move(W(), &m, &move_cb, nullptr);
    tick_thread = false;
    CHECK(t.enemy_move_cancel(m12) && done.size() == 1);  // another thread: at the next step
    CHECK(t.enemy_moving(W(), vukah) == 0);
    tick_thread = true;
    step();
    CHECK(done.size() == 2 && done[1].id == m12 && done[1].status == EMF_MOVE_CANCELLED && done[1].world == W());
    // an enemy order on a moving creature ends the move (cancelled), once the order holds
    m = move_req(vukah, 1);
    const uint32_t m13 = t.enemy_move(W(), &m, &move_cb, nullptr);
    o = order_req(vukah, dummy);
    const uint32_t o9 = t.enemy_order(W(), &o);
    CHECK(o9 != 0 && done.size() == 3 && done[2].id == m13 && done[2].status == EMF_MOVE_CANCELLED);
    CHECK(!lease_of(vukah, KILN_CONTROL_ROLE_ENGINE_GUARD) && lease_of(vukah, KILN_CONTROL_ROLE_PET_COMBAT));
    step();
    CHECK(held[vukah] == dummy);
    // a move on an ordered creature ends the order
    m = move_req(vukah, 1);
    const uint32_t m14 = t.enemy_move(W(), &m, &move_cb, nullptr);
    CHECK(m14 != 0 && !t.enemy_order_cancel(o9) && held.count(vukah) == 0 && !lease_of(vukah, KILN_CONTROL_ROLE_PET_COMBAT));
    // a callback may start the next move
    static uint32_t chained = 0;
    static EmfApi* tp = &t;
    struct Chain {
        static void cb(EmfWorld* w, uint32_t, uint32_t status, void* user) {
            done.push_back({w, 0, status});
            if (chained) return;
            EmfMoveOrder n = move_req(*static_cast<uint32_t*>(user), 1);
            chained = tp->enemy_move(w, &n, &move_cb, nullptr);
        }
    };
    static uint32_t vk = vukah;
    CHECK(t.enemy_move_cancel(m14));
    m = move_req(vukah, 1);
    const uint32_t m15 = t.enemy_move(W(), &m, &Chain::cb, &vk);
    CHECK(t.enemy_move_cancel(m15) && chained != 0 && t.enemy_moving(W(), vukah) == chained);

    // ------------------------------------------------------------------------------- a mod that stopped -----------
    calling = &mod_b;
    const uint32_t boar3 = creature(70, 0);
    o = order_req(boar3, dummy);
    const uint32_t o10 = t.enemy_order(W(), &o);
    step();
    CHECK(held[boar3] == dummy);
    mod_b.status = EMF_MOD_FAILED;
    step();
    CHECK(held.count(boar3) == 0 && !t.enemy_order_cancel(o10) && !lease_of(boar3, KILN_CONTROL_ROLE_PET_COMBAT));
    calling = &mod_a;

    // ------------------------------------------------------------------------------- 256 orders at most -----------
    {
        std::vector<uint32_t> ids;
        for (int i = 0; i < 260; ++i) {
            const uint32_t c = creature(100 + i, 100);
            EmfEnemyOrder x = order_req(c, dummy);
            const uint32_t id = t.enemy_order(W(), &x);
            if (id) ids.push_back(id);
            else CHECK(std::string(x.why) == "too many orders (256)");
        }
        CHECK(ids.size() == 256 - 0u);
        for (uint32_t id : ids) CHECK(t.enemy_order_cancel(id));
        step();
    }

    // ------------------------------------------------------------------------------- the world closes -------------
    done.clear();
    o = order_req(boar3, dummy);
    const uint32_t o11 = t.enemy_order(W(), &o);
    const uint32_t still = t.enemy_moving(W(), vukah);
    CHECK(still != 0 && o11 != 0);
    orders_world_stopping();
    CHECK(done.size() == 1 && done[0].world == nullptr && done[0].status == EMF_MOVE_CANCELLED);
    CHECK(t.enemy_moving(W(), vukah) == 0 && !t.enemy_order_cancel(o11) && !t.enemy_move_cancel(still));
    const size_t prefers_before = prefers.size(), cmds_before2 = cmds.size();
    orders_world_started();
    step();  // the new world: nothing of the old one is written
    CHECK(prefers.size() == prefers_before && cmds.size() == cmds_before2);

    CHECK(unsupported_calls.empty());
    std::printf("orders_test (EmfApi v39 enemy orders, v40 move orders on Kiln): %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

// SPDX-License-Identifier: MIT
// The EmfApi table, v39 enemy orders (enemy_order, enemy_order_cancel, enemy_target) and v40 move orders (enemy_move,
// enemy_move_cancel, enemy_moving), since 0.2.0-dev.31.13. The rules are orders_model.* (no game); this file gives it
// Kiln's services:
//  - kiln.creature_control: kiln-emf binds the creature (Enemy baseline) and holds a lease for each order (PetCombat,
//    AI on) and each move (EngineGuard, AI on: the game's guard walks it). The broker decides who may control a body:
//    another owner's pet lease makes a move Busy, a ride preempts an order or a move (it waits), and the broker ending
//    a lease ends the order. Leases go back on cancel, an end, death, despawn, a mod that stopped and the world's end;
//    the binding goes once no lease of kiln-emf is left on the body.
//  - kiln.creatures: inspect (AI, dead, faction, state, pick, guard), the target hold (target_prefer), the game's
//    AddThreatTargetEvent / StartGuardingEvent / StopGuardingEvent (command), the home (set, no layer), and the
//    template's guardSetup.targetDistance (ai_get).
//  - kiln.spawn: the hidden waypoint (EMF's own: the game's TestTransform template, not saved, not tracked).
//  - kiln.world: positions, PlayerState.
// Server-authoritative, as EMF: orders and moves are taken only inside a world callback on the server world (the host or
// a dedicated server). A joined client's world is answered as EMF answers it: "not a server world (host only)", 0.
#include <intrin.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

#include "emf_internal.h"
#include "orders_model.h"
#include "services/kiln_creature_control.h"
#include "services/kiln_creatures.h"
#include "v26_model.h"

namespace kemf {

namespace {

// EMF's waypoint: the game's TestTransform template 6d71f70d-602c-4d55-9de2-73e5b84133b5 (CurrentTransform,
// StaticTransform, DoNotSave), in the game's GUID layout (EMF src/core/move_orders.h kWaypointGuid).
constexpr uint8_t kWaypointGuid[16] = {0x0d, 0xf7, 0x71, 0x6d, 0x2c, 0x60, 0x55, 0x4d,
                                       0x9d, 0xe2, 0x73, 0xe5, 0xb8, 0x41, 0x33, 0xb5};

struct Svc {
    const KilnCreaturesApi* creatures = nullptr;
    const KilnCreatureControlApi* control = nullptr;
};
// Looked up at the first call (services are published before any mod loads).
const Svc& svc() {
    static const Svc s = [] {
        Svc r;
        auto* c = static_cast<const KilnCreaturesApi*>(g_api->service(kstr(KILN_CREATURES_SERVICE), 1));
        if (c && c->header.size >= offsetof(KilnCreaturesApi, target_prefer) + sizeof(void*) && c->target_prefer && c->inspect &&
            c->command && c->set && c->template_get)
            r.creatures = c;
        auto* k = static_cast<const KilnCreatureControlApi*>(g_api->service(kstr(KILN_CREATURE_CONTROL_SERVICE), 1));
        if (k && k->header.size >= sizeof(KilnCreatureControlApi) && k->bind_body && k->acquire_control && k->query_control &&
            k->can_drive && k->release_control && k->release_body && k->next_request)
            r.control = k;
        return r;
    }();
    return s;
}

EmfWorld* server_world() { return reinterpret_cast<EmfWorld*>(g_api->world(KILN_WORLD_SERVER)); }
bool is_server(EmfWorld* w) { return w && w == server_world(); }
// Inside a world callback on the server world, on its thread: what EMF asks of a call that acts at once.
bool in_server_callback() {
    return t_ctx.world_cb && is_server(t_ctx.world) && g_api->on_tick_thread();
}

// ------------------------------------------------------------------------------------------- the broker's leases --
std::mutex g_lease_mutex;
std::map<KilnHandle, KilnControlBodyKey> g_leases;  // kiln-emf's leases and their bodies
std::vector<KilnControlBodyKey> g_unbind;           // bodies to let go once no lease of ours is left on them

void unbind_now(const KilnControlBodyKey& key) {
    const KilnResult rc = svc().control->release_body(g_owner, key);
    if (rc == KILN_ERR_INVALID_ARGUMENT) g_unbind.push_back(key);  // a lease of ours is not over yet: next step
}

uint64_t take_lease(uint32_t entity, bool combat, std::string* why) {
    const auto* cc = svc().control;
    KilnControlBodyKey key{};
    const KilnResult rc = cc->bind_body(g_owner, entity, KILN_CONTROL_BASELINE_ENEMY, 1, &key);
    if (rc != KILN_OK) {
        *why = rc == KILN_ERR_NOT_FOUND         ? "creature control: no such creature in the server world"
               : rc == KILN_ERR_UNAVAILABLE     ? "creature control: it cannot be controlled (creature control is off, or "
                                                  "another mod or loader moves it)"
               : rc == KILN_ERR_BUFFER_TOO_SMALL ? "creature control: too many controlled creatures (256)"
               : rc == KILN_ERR_WRONG_THREAD    ? "creature control: not on the server world's thread"
                                                : "creature control: the creature could not be bound";
        return 0;
    }
    uint64_t request = 0;
    KilnControlRequest r{};
    r.size = sizeof r;
    r.role = combat ? KILN_CONTROL_ROLE_PET_COMBAT : KILN_CONTROL_ROLE_ENGINE_GUARD;
    r.body = key;
    KilnControlState st{};
    st.size = sizeof st;
    KilnResult arc = cc->next_request(g_owner, &request);
    r.request_id = request;
    if (arc == KILN_OK) arc = cc->acquire_control(g_owner, &r, &st);
    if (arc != KILN_OK || !st.lease || st.lease_state == KILN_CONTROL_FAILED || st.lease_state == KILN_CONTROL_RELEASED) {
        st.reason[sizeof st.reason - 1] = 0;
        *why = std::string("creature control: ") + (arc == KILN_OK && st.reason[0] ? st.reason : "the lease was refused");
        if (st.lease) cc->release_control(g_owner, st.lease);
        std::lock_guard l(g_lease_mutex);
        unbind_now(key);
        return 0;
    }
    std::lock_guard l(g_lease_mutex);
    g_leases[st.lease] = key;
    return st.lease;
}

orders::LeaseView view_lease(uint64_t lease) {
    orders::LeaseView v;
    KilnControlBodyKey key{};
    {
        std::lock_guard l(g_lease_mutex);
        auto it = g_leases.find(lease);
        if (it == g_leases.end()) return v;  // released (or the world ended)
        key = it->second;
    }
    const auto* cc = svc().control;
    KilnControlState st{};
    st.size = sizeof st;
    if (cc->query_control(g_owner, lease, &st) != KILN_OK) return v;
    st.reason[sizeof st.reason - 1] = 0;
    v.reason = st.reason;
    v.sequence = st.transition_sequence;
    switch (st.lease_state) {
        case KILN_CONTROL_PENDING: v.state = orders::LeaseState::Pending; break;
        case KILN_CONTROL_PREEMPTED: v.state = orders::LeaseState::Preempted; break;
        case KILN_CONTROL_ACTIVE: {
            uint32_t may = 0;  // the only authority, asked right before the writes it allows
            cc->can_drive(g_owner, lease, key, st.transition_sequence, &may);
            v.state = may ? orders::LeaseState::Active : orders::LeaseState::Pending;
            break;
        }
        default:
            v.state = orders::LeaseState::Ended;
            v.body_gone = st.result == KILN_CONTROL_RESULT_BODY_GONE || st.result == KILN_CONTROL_RESULT_RETIRED;
            v.world_ended = st.result == KILN_CONTROL_RESULT_WORLD_ENDED;
            break;
    }
    return v;
}

void give_back(uint64_t lease) {
    KilnControlBodyKey key{};
    {
        std::lock_guard l(g_lease_mutex);
        auto it = g_leases.find(lease);
        if (it == g_leases.end()) return;
        key = it->second;
        g_leases.erase(it);
    }
    svc().control->release_control(g_owner, lease);
    std::lock_guard l(g_lease_mutex);
    unbind_now(key);
}

void unbind_pending() {
    std::vector<KilnControlBodyKey> keys;
    {
        std::lock_guard l(g_lease_mutex);
        keys.swap(g_unbind);
    }
    if (keys.empty() || !svc().control) return;
    std::lock_guard l(g_lease_mutex);
    for (const auto& k : keys) unbind_now(k);
}

// ------------------------------------------------------------------------------------------- kiln.creatures --------
uint32_t g_player_state = 0;
bool is_player(uint32_t e) {
    if (e >= 1 && e <= 16) return true;
    if (!S.world) return false;
    if (!g_player_state && S.world->component_hash) g_player_state = S.world->component_hash(kstr("PlayerState"));
    uint32_t size = 0;
    return g_player_state && S.world->component(e, g_player_state, &size) != nullptr;
}

bool read_creature(uint32_t e, orders::Creature* out) {
    const auto* cr = svc().creatures;
    KilnCreatureInfo info{};
    info.size = sizeof info;
    if (!e || cr->inspect(e, &info) != KILN_OK) return false;
    out->ai = (info.flags & KILN_CREATURE_I_AI) != 0;
    out->dead = (info.flags & KILN_CREATURE_I_DEAD) != 0 || (info.health_max > 0 && info.health <= 0);
    out->faction = info.faction;
    out->state = info.state;
    out->target = info.target;
    out->guard = info.guard;
    out->boss = false;
    KilnCreatureTemplate t{};
    t.size = sizeof t;
    if (out->ai && cr->template_get(&info.template_guid, &t) == KILN_OK)
        out->boss = (t.flags & (KILN_CREATURE_T_BOSS | KILN_CREATURE_T_BOSS_BAR)) != 0;
    return true;
}

void command(uint32_t kind, uint32_t entity, uint32_t target, float value) {
    KilnCreatureCommand c{};
    c.size = sizeof c;
    c.kind = kind;
    c.entity = entity;
    c.target = target;
    c.value = value;
    const KilnResult rc = svc().creatures->command(&c);
    if (rc != KILN_OK) logf(KILN_LOG_WARN, "emf orders: kiln.creatures did not take a command (%d) for %u", rc, entity);
}

double guard_target_distance(uint32_t e) {
    const auto* cr = svc().creatures;
    if (!cr->ai_get) return -1;
    char buf[64] = "";
    size_t len = 0;
    uint32_t priv = 0;
    if (cr->ai_get(e, kstr("Enemy01BehaviorState"), kstr("guardSetup.targetDistance"), buf, sizeof buf, &len, &priv) != KILN_OK)
        return -1;
    buf[sizeof buf - 1] = 0;
    char* end = nullptr;
    const double v = std::strtod(buf, &end);
    return end != buf ? v : -1;
}

uint32_t spawn_waypoint(const orders::Vec& p, const void* tag) {
    if (!S.spawn) return 0;
    KilnSpawnRequest req{};
    req.size = sizeof req;
    std::memcpy(req.template_guid.b, kWaypointGuid, 16);
    req.position = KilnVec3{p[0], p[1], p[2]};
    req.flags = KILN_SPAWN_NO_TRACK | KILN_SPAWN_TRANSIENT;  // never saved, never in kiln.spawn's sidecar
    const std::string origin = "emf:" + std::string(mod_name(static_cast<const Mod*>(tag)));
    req.tag = kstr("emf");
    req.origin = kstr(origin);
    KilnEntity e = 0;
    return S.spawn->spawn(&req, &e) == KILN_OK ? e : 0;
}

bool mod_runs(const void* tag) {
    const auto* m = static_cast<const Mod*>(tag);
    return !m || (m->status != EMF_MOD_FAILED && m->status != EMF_MOD_DISABLED && m->status != EMF_MOD_REJECTED);
}

orders::Orders& model() {
    static orders::Orders o([] {
        orders::Deps d;
        d.now_ms = [] { return now_ms(); };
        d.creature = &read_creature;
        d.player = &is_player;
        d.position = [](uint32_t e, orders::Vec* out) {
            KilnVec3 p{};
            if (!e || !S.world || S.world->position(e, &p) != KILN_OK) return false;
            *out = {p.x, p.y, p.z};
            return true;
        };
        d.lease = &take_lease;
        d.lease_view = &view_lease;
        d.release = &give_back;
        d.prefer = [](uint32_t e, uint32_t target, double seconds) {
            svc().creatures->target_prefer(g_owner, e, target, static_cast<float>(seconds));
        };
        d.threat = [](uint32_t e, uint32_t target) { command(KILN_CREATURE_CMD_ATTACK, e, target, orders::kOrderThreat); };
        d.guard = [](uint32_t e, uint32_t guarded) { command(KILN_CREATURE_CMD_GUARD, e, guarded, 0); };
        d.stop_guard = [](uint32_t e, uint32_t guarded) { command(KILN_CREATURE_CMD_STOP_GUARD, e, guarded, 0); };
        d.set_home = [](uint32_t e, const orders::Vec& p) {
            KilnCreatureValue v{};
            v.vec = KilnVec3{p[0], p[1], p[2]};
            svc().creatures->set(0, e, KILN_CF_HOME, 0, &v);  // no layer: it lives there, as on EMF
        };
        d.guard_target_distance = &guard_target_distance;
        d.spawn_waypoint = &spawn_waypoint;
        d.destroy = [](uint32_t e) {
            if (S.spawn) S.spawn->destroy(e);
        };
        d.tag_alive = &mod_runs;
        return d;
    }());
    return o;
}

bool services_ready() { return svc().creatures && svc().control && S.world && S.spawn; }
constexpr const char* kOff = "kiln.creatures, kiln.creature_control, kiln.spawn or kiln.world is not available";

template <size_t N>
void say(char (&why)[N], const std::string& text) {
    std::snprintf(why, N, "%s", text.c_str());
}

// The checks EMF makes before an order or a move is read: a server world callback.
const char* world_refusal(EmfWorld* w) {
    if (!w || w != t_ctx.world || !t_ctx.world_cb || !g_api->on_tick_thread())
        return "call it from a world callback (on_tick, run_on_world, on_event) on the server world passed to it";
    if (!is_server(w)) return "not a server world (host only)";
    return nullptr;
}

// ------------------------------------------------------------------------------------------- v39 -------------------
uint32_t api_enemy_order(EmfWorld* w, EmfEnemyOrder* req) {
    Mod* m = caller_mod(_ReturnAddress());
    if (!req || req->size < v26::kEnemyOrderV39Size) {
        refuse(m, "enemy_order", "no order, or its size field is not sizeof(EmfEnemyOrder)", true);
        return 0;
    }
    req->why[0] = 0;
    auto no = [&](const std::string& why) {
        say(req->why, why);
        refuse(m, "enemy_order", req->why, true);
        return 0u;
    };
    if (const char* bad = world_refusal(w)) return no(bad);
    if (!services_ready()) {
        unsupported(m, "enemy_order", kOff);
        return no(std::string("enemy orders are off (") + kOff + ")");
    }
    orders::OrderRequest r;
    r.enemy = req->enemy;
    r.target = req->target;
    r.flags = req->flags;
    r.seconds = req->seconds;
    r.damage = req->damage;
    r.tag = m;
    std::string why;
    const uint32_t id = model().order(r, &why);
    return id ? id : no(why);
}

bool api_enemy_order_cancel(uint32_t id) {
    if (!id || !services_ready()) return false;
    return model().order_cancel(id, in_server_callback());  // else it ends at the next step
}

// What `enemy` goes for now (SelectedTargets slot 0, read by kiln.creatures): the server world, on its thread.
uint32_t api_enemy_target(EmfWorld* w, uint32_t enemy) {
    if (!is_server(w) || !svc().creatures || !g_api->on_tick_thread()) return 0;
    KilnCreatureInfo info{};
    info.size = sizeof info;
    return enemy && svc().creatures->inspect(enemy, &info) == KILN_OK ? info.target : 0;
}

// ------------------------------------------------------------------------------------------- v40 -------------------
uint32_t api_enemy_move(EmfWorld* w, EmfMoveOrder* req, EmfMoveCallback cb, void* user) {
    Mod* m = caller_mod(_ReturnAddress());
    if (!req || req->size < v26::kMoveOrderV40Size) {
        refuse(m, "enemy_move", "no order, or its size field is not sizeof(EmfMoveOrder)", true);
        return 0;
    }
    req->why[0] = 0;
    auto no = [&](const std::string& why) {
        say(req->why, why);
        refuse(m, "enemy_move", req->why, true);
        return 0u;
    };
    if (const char* bad = world_refusal(w)) return no(bad);
    if (!services_ready()) {
        unsupported(m, "enemy_move", kOff);
        return no(std::string("move orders are off (") + kOff + ")");
    }
    orders::MoveRequest r;
    r.enemy = req->enemy;
    r.flags = req->flags;
    r.arrive = req->arrive == 0.0f ? 2.0 : req->arrive;
    r.seconds = req->seconds;
    // v41 stuck_seconds: read only when the order's size covers it (a v40 mod's 128-byte order ends at why)
    if (req->size >= offsetof(EmfMoveOrder, stuck_seconds) + sizeof(float) && req->stuck_seconds != 0.0f)
        r.stuck_seconds = req->stuck_seconds;
    r.tag = m;
    // the flags first (as EMF checks them), then the points: copied under a guard, never read in place
    if (std::string bad = orders::Orders::check_flags(r.flags); !bad.empty()) return no(bad);
    if (!req->points || req->count == 0 || req->count > orders::kPointsMax) return no("1 to 16 points");
    EmfPathPoint pts[orders::kPointsMax];
    if (!seh_copy(pts, req->points, sizeof(EmfPathPoint) * req->count)) return no("its points cannot be read");
    for (uint32_t i = 0; i < req->count; ++i) r.points.push_back({pts[i].x, pts[i].y, pts[i].z});
    orders::MoveDone done;
    if (cb)
        done = [m, cb, user](uint32_t id, uint32_t status, bool closed) {
            struct Call {
                EmfMoveCallback cb;
                EmfWorld* world;
                uint32_t id, status;
                void* user;
            } c{cb, closed ? nullptr : server_world(), id, status, user};
            ContextScope scope(m, c.world, c.world != nullptr);
            guarded(m, "an enemy_move callback", [](void* raw) {
                auto& x = *static_cast<Call*>(raw);
                x.cb(x.world, x.id, x.status, x.user);
            }, &c);
        };
    std::string why;
    const uint32_t id = model().move(r, std::move(done), &why);
    return id ? id : no(why);
}

bool api_enemy_move_cancel(uint32_t id) {
    if (!id || !services_ready()) return false;
    return model().move_cancel(id, in_server_callback());  // else at the next step
}

uint32_t api_enemy_moving(EmfWorld* w, uint32_t enemy) {
    if (!is_server(w) || !enemy || !services_ready()) return 0;
    return model().moving(enemy);
}

}  // namespace

void fill_orders(EmfApi* t) {
    t->enemy_order = &api_enemy_order;
    t->enemy_order_cancel = &api_enemy_order_cancel;
    t->enemy_target = &api_enemy_target;
    t->enemy_move = &api_enemy_move;
    t->enemy_move_cancel = &api_enemy_move_cancel;
    t->enemy_moving = &api_enemy_moving;
}

void orders_after_step(EmfWorld* w, uint8_t kind) {
    if (kind != EMF_WORLD_SERVER || !is_server(w) || !services_ready()) return;
    unbind_pending();
    if (model().order_count() || model().move_count()) model().step();
}

void orders_world_stopping() {
    if (!services_ready()) return;
    model().world_reset(true);  // every move ends cancelled with world NULL; the broker ends the leases (WorldEnded)
    std::lock_guard l(g_lease_mutex);
    g_leases.clear();
    g_unbind.clear();
}

void orders_world_started() {}

void orders_shutdown() {
    if (!services_ready()) return;
    model().world_reset(false);  // no call into a mod at unload; the broker restores kiln-emf's bodies (owner retired)
    {
        std::lock_guard l(g_lease_mutex);
        g_leases.clear();
        g_unbind.clear();
    }
    if (svc().creatures->release_owner) svc().creatures->release_owner(g_owner);  // kiln-emf's target holds
}

}  // namespace kemf

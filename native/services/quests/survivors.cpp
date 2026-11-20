// SPDX-License-Identifier: MIT
// kiln.survivors (kiln-quests.dll): the game's roster survivors for mods. Recruit, summon tool, summon/move into a base,
// assign and unassign beds. Header and contract: native/core/abi/services/kiln_survivors.h; docs/kiln/services/survivors.md.
// Engine facts: dossier chapter 18 (the bed relation is NpcCommon.bedId <-> NpcBed.associatedNpc, written on both ends;
// the bed side is saved, the NPC side rebuilt at load; Reset = the game's NpcCommandEvent that moves an NPC's home).
//
// Composition, not new game calls: the unlock is a kiln.knowledge world grant, the tool a kiln.items give, the move a
// kiln.creatures npc_command Reset, a new NPC a kiln.spawn spawn, the bed relation one Kiln program with tracked
// other-entity writes (registered at on_ready). No child step is ever cancelled (a request drains instead), so nothing a
// request started can land after its terminal result (NoMutationAfterTerminal); the price is that a request whose child
// hangs stays DRAINING (no thread is killed).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "kiln_api.h"
#include "kiln_knowledge.h"
#include "services/kiln_build.h"
#include "services/kiln_caps.h"
#include "services/kiln_creatures.h"
#include "services/kiln_gameplay.h"
#include "services/kiln_items.h"
#include "services/kiln_players.h"
#include "services/kiln_spawn.h"
#include "services/kiln_survivors.h"
#include "services/kiln_world.h"
#include "survivors_core.h"

namespace kiln::survivors {
namespace {

const KilnApi* A = nullptr;
KilnOwner O = 0;
const KilnWorldApi* W = nullptr;
const KilnSpawnApi* S = nullptr;
const KilnKnowledgeApi* K = nullptr;
const KilnItemsApi* I = nullptr;
const KilnCreaturesApi* C = nullptr;
const KilnPlayersApi* P = nullptr;
const KilnGameplayApi* G = nullptr;
const KilnBuildApi* B = nullptr;

KilnStr ks(const char* s) { return KilnStr{s, std::strlen(s)}; }

void logf(int level, const char* fmt, ...) {
    if (!A) return;
    char b[700];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    A->log(O, level, KilnStr{b, std::min<size_t>(size_t(n), sizeof b - 1)});
}
#define SI(...) logf(KILN_LOG_INFO, "survivors: " __VA_ARGS__)
#define SW(...) logf(KILN_LOG_WARN, "survivors: " __VA_ARGS__)

uint64_t now_ms() { return GetTickCount64(); }
uint64_t step_now() {
    KilnTickInfo t{};
    t.size = sizeof t;
    return A && A->tick_info(&t) == KILN_OK ? t.step : 0;
}
uint32_t session_now() {
    KilnSessionInfo s{};
    s.size = sizeof s;
    if (!A || A->size < offsetof(KilnApi, session) + sizeof(void*) || A->session(&s) != KILN_OK) return 0;
    return s.worlds[KILN_WORLD_SERVER] ? s.sessions[KILN_WORLD_SERVER] : 0;
}

// ---- layout (sigdb: sigdb/symbols/server.d/dev31-18-survivors.json, feature survivors.assignment.layout)
struct Layout {
    int64_t common_bed = 0, common_size = 0, bed_npc = 0, bed_size = 0, saved_size = 0, saved_home = 0;
    int64_t base_size = 0, base_abandoned = 0, tpl_bed_radius = 0;
} L;
bool g_layout_ok = false;
std::string g_layout_why;
uint32_t h_common = 0, h_bed = 0, h_saved = 0, h_base = 0;

bool cst(const char* n, int64_t* v) { return A->constant(ks(n), v) == KILN_OK; }

void load_layout() {
    if (!A->feature(ks("survivors.assignment.layout"))) {
        g_layout_why = "feature survivors.assignment.layout is off on this build";
        return;
    }
    const bool ok = cst("survivors/npc_common.bed", &L.common_bed) && cst("survivors/npc_common.size", &L.common_size) &&
                    cst("survivors/npc_bed.npc", &L.bed_npc) && cst("survivors/npc_bed.size", &L.bed_size) &&
                    cst("survivors/npc_saved.size", &L.saved_size) && cst("survivors/npc_saved.home", &L.saved_home) &&
                    cst("survivors/base_id.size", &L.base_size) && cst("survivors/base_id.abandoned", &L.base_abandoned) &&
                    cst("survivors/npc_tpl.bed_radius", &L.tpl_bed_radius);
    if (!ok || L.common_bed + 4 > L.common_size || L.bed_npc + 4 > L.bed_size || L.saved_home + 12 > L.saved_size ||
        L.base_abandoned + 1 > L.base_size || L.base_size < 16) {
        g_layout_why = "a survivors/ layout constant did not resolve or does not fit";
        return;
    }
    g_layout_ok = true;
}

// ---- reads (tick)
template <class T>
bool comp_read(KilnEntity e, uint32_t hash, int64_t want_size, int64_t off, T* out) {
    uint32_t size = 0;
    const auto* p = static_cast<const uint8_t*>(W->component(e, hash, &size));
    if (!p || int64_t(size) != want_size || off + int64_t(sizeof(T)) > int64_t(size)) return false;
    std::memcpy(out, p + off, sizeof(T));
    return true;
}

struct RosterEntry {
    uint32_t index = 0;
    KilnKnowledgeNpc npc{};
    std::string name;
};

bool roster_at(uint32_t i, RosterEntry* r) {
    char name[KILN_SURVIVOR_NAME_MAX * 2] = {};
    size_t len = 0;
    r->npc = KilnKnowledgeNpc{};
    r->npc.size = sizeof r->npc;
    if (K->npc(i, &r->npc, name, sizeof name, &len) != KILN_OK) return false;
    r->index = i;
    r->name.assign(name, std::min(len, sizeof name - 1));
    return true;
}
uint32_t roster_count() { return K ? K->catalog_count(KILN_KNOW_CAT_NPCS) : 0; }
bool roster_find(uint32_t npc_id, RosterEntry* r) {
    const uint32_t n = roster_count();
    for (uint32_t i = 0; i < n; ++i)
        if (roster_at(i, r) && r->npc.npc_id == npc_id) return true;
    return false;
}

// NpcId -> live entities, rebuilt at most once per step (one pass over the NPCs with a saved NPC state).
uint64_t g_ent_step = ~0ull;
std::unordered_map<uint32_t, std::vector<KilnEntity>> g_by_npc;
std::unordered_map<KilnEntity, uint32_t> g_npc_of;
std::vector<KilnEntity> find_all(uint32_t hash) {
    std::vector<KilnEntity> v(256);
    uint32_t n = 0;
    KilnResult rc = W->find(hash, v.data(), uint32_t(v.size()), &n);
    if (rc == KILN_ERR_BUFFER_TOO_SMALL) {
        v.resize(n + 64);
        rc = W->find(hash, v.data(), uint32_t(v.size()), &n);
    }
    if (rc != KILN_OK) return {};
    v.resize(std::min<size_t>(n, v.size()));
    return v;
}
void refresh_entities(bool force = false) {
    const uint64_t st = step_now();
    if (!force && st == g_ent_step) return;
    g_ent_step = st;
    g_by_npc.clear();
    g_npc_of.clear();
    for (KilnEntity e : find_all(h_saved)) {
        KilnEntitySnapshot s{};
        s.size = sizeof s;
        if (S->entity_info(e, &s) != KILN_OK || !s.has_template) continue;
        const uint32_t id = npc_id_of(s.template_guid.b);
        g_by_npc[id].push_back(e);
        g_npc_of[e] = id;
    }
}
std::vector<KilnEntity> entities_of(uint32_t npc_id) {
    refresh_entities();
    auto it = g_by_npc.find(npc_id);
    return it == g_by_npc.end() ? std::vector<KilnEntity>{} : it->second;
}

uint32_t bed_of(KilnEntity npc) {
    uint32_t b = 0;
    return g_layout_ok && comp_read(npc, h_common, L.common_size, L.common_bed, &b) ? b : 0;
}
bool home_of(KilnEntity npc, KilnVec3* out) {
    float f[3];
    if (!g_layout_ok) return false;
    uint32_t size = 0;
    const auto* p = static_cast<const uint8_t*>(W->component(npc, h_saved, &size));
    if (!p || int64_t(size) != L.saved_size) return false;
    std::memcpy(f, p + L.saved_home, sizeof f);
    *out = KilnVec3{f[0], f[1], f[2]};
    return true;
}
// 1: a bed (associated NpcId in *npc); 0: not a bed
bool bed_npc(KilnEntity bed, uint32_t* npc) { return g_layout_ok && comp_read(bed, h_bed, L.bed_size, L.bed_npc, npc); }

struct Zone {
    KilnEntity altar;
    KilnVec3 center, half;
    bool abandoned;
    uint8_t guid[16];
    bool has_guid;
};
std::vector<Zone> zones() {
    std::vector<Zone> out;
    if (!B) return out;
    KilnBuildBase buf[64];
    for (auto& b : buf) {
        b = KilnBuildBase{};
        b.size = sizeof b;
    }
    size_t n = 0;
    if (B->bases(buf, 64, &n) != KILN_OK) return out;
    for (size_t i = 0; i < n && i < 64; ++i) {
        Zone z{buf[i].altar, buf[i].center, buf[i].half, buf[i].abandoned != 0, {}, false};
        uint32_t size = 0;
        const auto* p = static_cast<const uint8_t*>(W->component(z.altar, h_base, &size));
        if (p && g_layout_ok && int64_t(size) == L.base_size) {
            std::memcpy(z.guid, p, 16);
            z.has_guid = true;
            if (p[L.base_abandoned]) z.abandoned = true;
        }
        out.push_back(z);
    }
    return out;
}
const Zone* zone_at(const std::vector<Zone>& zs, const KilnVec3& p) {
    for (const Zone& z : zs)
        if (in_box(z.center, z.half, p)) return &z;
    return nullptr;
}

void fill_state(const RosterEntry& r, KilnSurvivorState* s, const std::vector<Zone>* zs = nullptr) {
    *s = KilnSurvivorState{};
    s->size = sizeof *s;
    s->npc_id = r.npc.npc_id;
    s->summon_item = r.npc.item;
    s->unlocked = r.npc.unlock.id == 0 ? 1u : r.npc.unlocked;
    s->index = r.index;
    set_text(s->name, sizeof s->name, r.name);
    const auto ents = entities_of(r.npc.npc_id);
    if (ents.empty()) return;
    s->entity = ents.front();
    s->flags |= KILN_SURV_STATE_IN_WORLD;
    s->bed = bed_of(s->entity);
    if (home_of(s->entity, &s->home)) {
        s->flags |= KILN_SURV_STATE_HAS_HOME;
        std::vector<Zone> local;
        if (!zs) {
            local = zones();
            zs = &local;
        }
        if (const Zone* z = zone_at(*zs, s->home); z && z->has_guid) {
            std::memcpy(s->base_id, z->guid, 16);
            s->flags |= KILN_SURV_STATE_HAS_BASE;
        }
    }
}
bool state_of(uint32_t npc_id, KilnSurvivorState* s) {
    RosterEntry r;
    if (!roster_find(npc_id, &r)) return false;
    fill_state(r, s);
    return true;
}

// ---- capabilities
bool g_read = false, g_recruit = false, g_summon = false, g_assign = false;
std::string g_why;
std::atomic<bool> g_program_active{false};

const KilnCapsApi* caps() {
    static const KilnCapsApi* c = static_cast<const KilnCapsApi*>(A->service(ks(KILN_CAPS_SERVICE), 1));
    return c && c->report ? c : nullptr;
}
void row(const char* name, bool on, uint32_t reason, const std::string& text) {
    if (const KilnCapsApi* c = caps())
        c->report(O, ks(name), on ? KILN_CAP_AVAILABLE : KILN_CAP_UNAVAILABLE, on ? KILN_CAP_REASON_NONE : reason,
                  KilnStr{text.data(), text.size()});
}
bool has_npc_command() {
    return C && C->header.size >= offsetof(KilnCreaturesApi, npc_command) + sizeof(void*) && C->npc_command;
}
void publish_caps() {
    KilnKnowledgeStatus ks_{};
    ks_.size = sizeof ks_;
    const bool know = K && K->status(&ks_) == KILN_OK && ks_.knowledge && ks_.npc;
    const bool changes = know && ks_.changes;
    g_read = know && W && S;
    g_recruit = g_read && changes && I;
    g_summon = g_recruit && B && has_npc_command();
    g_assign = g_read && g_layout_ok && g_program_active.load() && P && G && B;
    std::string why;
    if (!W || !S) why = "kiln.world or kiln.spawn is missing";
    else if (!know) why = "kiln.knowledge has no roster on this build (features quests.knowledge, quests.npc)";
    else if (!changes) why = "kiln.knowledge cannot apply world grants here (feature quests.changes)";
    g_why = why;
    const uint32_t miss = KILN_CAP_REASON_SERVICE_MISSING;
    row("survivors.read", g_read, miss, g_read ? "kiln.survivors v1, 0.2.0-dev.31" : why);
    row("survivors.recruit", g_recruit, g_read ? miss : miss, g_recruit ? "knowledge grant + registry confirmation + kiln.items tool" : (why.empty() ? "kiln.items is missing" : why));
    row("survivors.summon", g_summon, miss,
        g_summon ? "kiln.creatures Reset or kiln.spawn, base zones of kiln.build" : (!has_npc_command() ? "kiln.creatures has no npc_command" : (why.empty() ? "kiln.build is missing" : why)));
    std::string aw = !g_layout_ok ? g_layout_why
                   : !g_program_active.load() ? "the bed program (kiln_survivors_beds) is not registered in this world"
                   : (!P || !G || !B) ? "kiln.players, kiln.gameplay or kiln.build is missing" : why;
    row("survivors.assign", g_assign, !g_layout_ok ? KILN_CAP_REASON_SYMBOL_MISSING : KILN_CAP_REASON_SESSION,
        g_assign ? "two-sided tracked bed writes" : aw);
}

// ---- requests
struct Req {
    uint64_t id = 0;
    KilnOwner owner = 0;
    KilnSurvivorRequest in{};
    uint64_t accepted_ms = 0, accepted_step = 0;
    uint32_t session = 0;
    int stage = 0;           // 0 = not taken yet
    uint32_t state = KILN_SURV_QUEUED;
    uint32_t effects = 0;
    uint32_t in_flight = 0;
    std::string waiting;
    RosterEntry roster;
    KilnSurvivorState before{};
    bool then_summon = false;
    // recruit
    uint64_t know_req = 0, confirm_ms = 0, confirm_step = 0;
    uint32_t recipient_handle = 0;
    bool item_done = false;
    int32_t item_status = 0;
    uint32_t item_count = 0;
    std::string item_message;
    // summon
    KilnEntity spawned = 0, moved_entity = 0;
    uint64_t spawn_ms = 0, spawn_step = 0;
    bool reset_sent = false;
    // beds
    KilnEntity npc = 0, old_bed = 0, displaced = 0;
    uint32_t displaced_npc = 0, actor_handle = 0;
    uint64_t write_step = 0;
};

std::mutex g_mu;  // inbox, progress map, counters
std::vector<std::unique_ptr<Req>> g_inbox;
std::unordered_map<uint64_t, KilnSurvivorProgress> g_progress;
std::vector<std::unique_ptr<Req>> g_active;  // tick thread
ResultCache g_results;
EventRing g_events;
std::atomic<uint64_t> g_next{1}, g_submitted{0}, g_ended{0};

void publish_progress(const Req& r) {
    KilnSurvivorProgress p{};
    p.size = sizeof p;
    p.state = r.state;
    p.request = r.id;
    p.elapsed_ms = now_ms() - r.accepted_ms;
    p.committed_effects = r.effects;
    p.in_flight = r.in_flight;
    set_text(p.reason, sizeof p.reason, r.waiting);
    std::lock_guard lock(g_mu);
    g_progress[r.id] = p;
}

const char* status_name(uint32_t s) {
    static const char* n[] = {"DONE", "NO_CHANGE", "REFUSED", "TIMED_OUT", "PARTIAL", "WORLD_ENDED"};
    return s < 6 ? n[s] : "?";
}

void post_event(uint32_t kind, const Req* r, const KilnSurvivorState& before, const KilnSurvivorState& after) {
    KilnSurvivorEvent e{};
    e.kind = kind;
    e.request = r ? r->id : 0;
    e.step = step_now();
    e.before = before;
    e.after = after;
    g_events.push(e);
}

// The terminal result. after: a readback when the world is alive (after_known), else the last confirmed state.
void finish(Req& r, uint32_t status, const std::string& reason, bool world_alive = true) {
    KilnSurvivorResult res{};
    res.size = sizeof res;
    res.status = status;
    res.request = r.id;
    res.step = step_now();
    res.committed_effects = r.effects;
    res.op = r.in.op;
    res.steps = uint32_t(res.step - r.accepted_step);
    res.displaced_entity = r.displaced;
    res.displaced_npc = r.displaced_npc;
    res.before = r.before;
    if (world_alive && r.stage > 0 && !r.roster.name.empty()) {
        RosterEntry now;
        if (roster_at(r.roster.index, &now) && now.npc.npc_id == r.in.npc_id) {
            g_ent_step = ~0ull;  // a fresh look
            fill_state(now, &res.after);
            res.after_known = 1;
        }
    }
    if (!res.after_known) res.after = r.before;
    set_text(res.reason, sizeof res.reason, reason);
    r.state = KILN_SURV_TERMINAL;
    r.stage = -1;
    g_results.put(res, now_ms());
    {
        std::lock_guard lock(g_mu);
        g_progress.erase(r.id);
    }
    ++g_ended;
    SI("request %llu op %u npc %08x: %s effects=0x%x%s%s", (unsigned long long)r.id, r.in.op, r.in.npc_id,
       status_name(status), r.effects, reason.empty() ? "" : " ", reason.c_str());
}
void fail(Req& r, uint32_t no_write_status, const std::string& reason) { finish(r, failed_status(r.effects, no_write_status), reason); }

Req* find_active(uint64_t id) {
    for (auto& p : g_active)
        if (p->id == id) return p.get();
    return nullptr;
}

void KILN_CALL item_done(const KilnItemsResult* res, void* user) {
    Req* r = find_active(uint64_t(reinterpret_cast<uintptr_t>(user)));
    if (!r || !res) return;
    r->item_done = true;
    r->item_status = res->status;
    r->item_count = res->done;
    r->item_message = res->message;
    r->in_flight = 0;
}

bool player_view(KilnEntity e, ActorView* v, KilnVec3* pos = nullptr) {
    *v = ActorView{};
    if (!P || !e) return false;
    KilnPlayer p{};
    p.size = sizeof p;
    if (P->get(e, &p) != KILN_OK) return false;
    v->found = true;
    v->in_game = p.state == KILN_PLAYER_IN_GAME;
    v->handle = p.handle;
    v->permissions_known = (p.flags & KILN_PLAYER_HAS_PERMISSIONS) != 0;
    v->can_edit_base = (p.permissions & KILN_PLAYER_CAN_EDIT_BASE) != 0;
    if (pos) *pos = p.position;
    return true;
}

bool past(const Req& r, uint64_t now) { return now >= r.accepted_ms + KILN_SURVIVOR_DEADLINE_MS; }

// ---- RECRUIT (and SUMMON's recruitment prerequisite)
void start_recruit(Req& r) {
    const KilnKnowledgeNpc& n = r.roster.npc;
    if (!unlock_supported(n.unlock.id, n.unlock.type, n.unlock.compare, n.unlock.compare_value, n.unlock.player_only))
        return fail(r, KILN_SURV_REFUSED, "UnsupportedUnlockRequirement: this survivor's unlock is a compound query");
    const bool unlocked = n.unlock.id == 0 || n.unlocked == 1;
    if (unlocked) {
        r.stage = 3;
        return;
    }
    KilnKnowledgeStatus st{};
    st.size = sizeof st;
    if (K->status(&st) != KILN_OK || !st.applying)
        return fail(r, KILN_SURV_REFUSED, "KnowledgeNotApplying: the world's knowledge update is not running yet");
    const KilnResult rc = K->grant(O, 0, n.unlock.id, 1, &r.know_req);
    if (rc != KILN_OK) return fail(r, KILN_SURV_REFUSED, "KnowledgeRefused: kiln.knowledge refused the grant (" + std::to_string(rc) + ")");
    r.in_flight = 1;
    r.waiting = "the knowledge grant";
    r.state = KILN_SURV_APPLYING;
    r.stage = 1;
}

void step_recruit(Req& r, uint64_t now, uint64_t step) {
    if (r.stage == 1) {
        KilnKnowledgeResult kr{};
        kr.size = sizeof kr;
        if (K->result(r.know_req, &kr) != KILN_OK) {
            if (past(r, now)) r.state = KILN_SURV_DRAINING;
            if (now > r.accepted_ms + KILN_SURVIVOR_DEADLINE_MS + 30000)  // the knowledge queue never answered
                return fail(r, KILN_SURV_TIMED_OUT, "KnowledgeNotApplied: no answer from kiln.knowledge");
            return;
        }
        r.in_flight = 0;
        if (kr.status != KILN_OK) return fail(r, KILN_SURV_REFUSED, std::string("KnowledgeRefused: ") + kr.reason);
        if (kr.changed || kr.after != kr.before) r.effects |= KILN_SURV_KNOWLEDGE_WRITTEN;
        r.confirm_ms = now;
        r.confirm_step = step;
        r.state = KILN_SURV_VERIFYING;
        r.waiting = "the NPC registry's unlock";
        r.stage = 2;
    }
    if (r.stage == 2) {
        RosterEntry cur;
        if (roster_at(r.roster.index, &cur) && cur.npc.npc_id == r.in.npc_id && cur.npc.unlocked == 1) {
            r.effects |= KILN_SURV_REGISTRY_CONFIRMED;
            r.roster = cur;
            KilnSurvivorState after;
            fill_state(cur, &after);
            post_event(KILN_SURV_EV_RECRUITED, &r, r.before, after);
            r.stage = 3;
        } else if (past(r, now)) {
            return fail(r, KILN_SURV_TIMED_OUT, "Deadline: the registry did not confirm the unlock in time");
        } else if (now - r.confirm_ms > 10000 || step - r.confirm_step > 180) {
            return fail(r, KILN_SURV_REFUSED, "RegistryConfirmationTimeout: the NPC registry did not follow the unlock");
        } else {
            return;
        }
    }
    if (r.stage == 3) {
        if (r.then_summon) {
            r.stage = 10;
            return;
        }
        if (!(r.in.flags & KILN_SURV_GIVE_TOOL))
            return finish(r, (r.effects & kWriteEffects) ? KILN_SURV_DONE : KILN_SURV_NO_CHANGE, "");
        if (past(r, now)) return fail(r, KILN_SURV_TIMED_OUT, "Deadline: no time left for the tool");
        ActorView v;
        if (!player_view(r.in.recipient, &v) || !v.in_game) return fail(r, KILN_SURV_REFUSED, "RecipientLeft: the recipient is not in game");
        if (v.handle != r.recipient_handle)
            return fail(r, KILN_SURV_REFUSED, "RecipientSessionChanged: the recipient's slot belongs to another connection now");
        KilnItemsRequest give{};
        give.size = sizeof give;
        give.entity = r.in.recipient;
        give.item = r.roster.npc.item;
        give.count = 1;
        uint64_t ticket = 0;
        const KilnResult rc = I->give(O, &give, &item_done, reinterpret_cast<void*>(uintptr_t(r.id)), &ticket);
        if (rc != KILN_OK) return fail(r, KILN_SURV_REFUSED, "ToolRefused: kiln.items refused the give (" + std::to_string(rc) + ")");
        r.in_flight = 1;
        r.waiting = "the summon tool's inventory transaction";
        r.state = KILN_SURV_APPLYING;
        r.stage = 4;
        return;
    }
    if (r.stage == 4) {
        if (!r.item_done) {
            if (past(r, now)) r.state = KILN_SURV_DRAINING;
            return;
        }
        if (r.item_status == KILN_ITEMS_DONE && r.item_count == 1) {
            r.effects |= KILN_SURV_TOOL_GIVEN;
            KilnSurvivorState s;
            fill_state(r.roster, &s);
            post_event(KILN_SURV_EV_TOOL_GIVEN, &r, r.before, s);
            if (past(r, now)) return fail(r, KILN_SURV_TIMED_OUT, "Deadline: the tool arrived after the budget");
            return finish(r, KILN_SURV_DONE, "");
        }
        return fail(r, KILN_SURV_REFUSED, "ToolNotGiven: " + r.item_message);
    }
}

// ---- SUMMON
void step_summon(Req& r, uint64_t now, uint64_t step) {
    if (r.stage >= 1 && r.stage <= 4) {
        step_recruit(r, now, step);
        if (r.stage != 10) return;
    }
    const KilnVec3 want = r.in.position;
    if (r.stage == 10) {
        if (past(r, now)) return fail(r, KILN_SURV_TIMED_OUT, "Deadline: no time left to place it");
        g_ent_step = ~0ull;
        const auto ents = entities_of(r.in.npc_id);
        if (ents.size() > 1) return fail(r, KILN_SURV_REFUSED, "NotUnique: the world holds more than one of this NPC");
        if (ents.size() == 1) {
            KilnVec3 home{}, pos{};
            if (home_of(ents[0], &home) && distance(home, want) <= 0.05 && W->position(ents[0], &pos) == KILN_OK &&
                distance(pos, want) <= 1.0) {
                r.moved_entity = ents[0];
                r.stage = 12;
                return;
            }
            KilnNpcCommand c{};
            c.size = sizeof c;
            c.type = KILN_NPC_CMD_RESET;
            c.npc_id = r.in.npc_id;
            c.position = want;
            const float half = r.in.yaw_degrees * 3.14159265f / 360.0f;
            c.rotation[1] = std::sin(half);
            c.rotation[3] = std::cos(half);
            const KilnResult rc = C->npc_command(&c);
            if (rc != KILN_OK) return fail(r, KILN_SURV_REFUSED, "MoveRefused: kiln.creatures refused the Reset (" + std::to_string(rc) + ")");
            r.reset_sent = true;
            r.moved_entity = ents[0];
            r.state = KILN_SURV_VERIFYING;
            r.waiting = "the move (home and position)";
            r.stage = 12;
            return;
        }
        KilnSpawnRequest q{};
        q.size = sizeof q;
        q.flags = KILN_SPAWN_NO_TRACK;
        q.template_name = KilnStr{r.roster.name.data(), r.roster.name.size()};
        q.position = want;
        q.yaw_degrees = r.in.yaw_degrees;
        q.origin = ks("kiln.survivors");
        KilnEntity e = 0;
        const KilnResult rc = S->spawn(&q, &e);
        if (rc != KILN_OK || !e) return fail(r, KILN_SURV_REFUSED, "SpawnFailed: kiln.spawn could not create " + r.roster.name);
        r.effects |= KILN_SURV_ENTITY_CREATED;
        r.spawned = e;
        r.spawn_ms = now;
        r.spawn_step = step;
        r.state = KILN_SURV_VERIFYING;
        r.waiting = "the new NPC to settle";
        r.stage = 11;
        return;
    }
    if (r.stage == 11) {
        if (step < r.spawn_step + 2) return;
        KilnVec3 home{};
        if (!home_of(r.spawned, &home) || distance(home, want) > 0.05) {
            KilnNpcCommand c{};
            c.size = sizeof c;
            c.type = KILN_NPC_CMD_RESET;
            c.npc_id = r.in.npc_id;
            c.position = want;
            const float half = r.in.yaw_degrees * 3.14159265f / 360.0f;
            c.rotation[1] = std::sin(half);
            c.rotation[3] = std::cos(half);
            if (C->npc_command(&c) == KILN_OK) r.reset_sent = true;
        }
        r.stage = 12;
        return;
    }
    if (r.stage == 12) {
        g_ent_step = ~0ull;
        const auto ents = entities_of(r.in.npc_id);
        const KilnEntity e = r.spawned ? r.spawned : r.moved_entity;
        KilnVec3 home{}, pos{};
        const bool alive = W->exists(e) && ents.size() == 1 && ents[0] == e;
        const bool home_ok = alive && home_of(e, &home) && distance(home, want) <= 0.05;
        const bool pos_ok = alive && W->position(e, &pos) == KILN_OK && distance(pos, want) <= 1.0;
        const bool settled = !r.spawned || (now - r.spawn_ms >= 3000 && step >= r.spawn_step + 2);
        RosterEntry cur;
        const bool unlocked = roster_at(r.roster.index, &cur) && (cur.npc.unlock.id == 0 || cur.npc.unlocked == 1);
        if (alive && home_ok && pos_ok && settled && unlocked) {
            if (r.reset_sent) r.effects |= KILN_SURV_HOME_MOVED;
            KilnSurvivorState s;
            fill_state(cur, &s);
            if (r.effects & kWriteEffects) post_event(KILN_SURV_EV_SUMMONED, &r, r.before, s);
            return finish(r, (r.effects & kWriteEffects) ? KILN_SURV_DONE : KILN_SURV_NO_CHANGE, "");
        }
        if (r.spawned && !W->exists(r.spawned)) return fail(r, KILN_SURV_REFUSED, "Despawned: the new NPC did not stay in the world");
        if (past(r, now)) {
            char b[160];
            std::snprintf(b, sizeof b, "NotConfirmed: alive=%d unique=%zu home=%d position=%d unlocked=%d", alive, ents.size(), home_ok,
                          pos_ok, unlocked);
            if (r.reset_sent && home_ok) r.effects |= KILN_SURV_HOME_MOVED;
            return fail(r, KILN_SURV_TIMED_OUT, b);
        }
    }
}

// ---- beds: one write at a time, done by the program in the step after Pre took it
struct BedOp {
    uint64_t req = 0;
    uint32_t op = 0, npc_id = 0;
    KilnEntity npc = 0, bed = 0;
    std::vector<KilnEntity> clear_beds;     // beds naming this NPC (old bed, stale links), not the target
    std::vector<KilnEntity> clear_commons;  // other NPCs naming the target bed (displaced)
    double distance = -1;                   // bed to home, -1 unknown
    bool ran = false, written = false;
    float radius = -1;
    std::string why;
};
std::mutex g_op_mu;
std::unique_ptr<BedOp> g_op;  // under g_op_mu
KilnHandle g_prog = 0;
int g_slot_common = -1, g_slot_bed = -1, g_slot_tpl = -1;
std::atomic<bool> g_tpl_warned{false};

void KILN_CALL bed_program(KilnProgramCtx* ctx, uint64_t* it, KilnEntity, void*) {
    g_program_active.store(true);
    std::lock_guard lock(g_op_mu);
    if (!g_op || g_op->ran) return;
    BedOp& op = *g_op;
    op.ran = true;
    if (g_slot_common < 0) {
        g_slot_common = W->program_slot(g_prog, KILN_SLOT_OTHER_WRITE, ks("NpcCommon"));
        g_slot_bed = W->program_slot(g_prog, KILN_SLOT_OTHER_WRITE, ks("NpcBed"));
        g_slot_tpl = W->program_slot(g_prog, KILN_SLOT_OTHER_TPL, ks("ActiveNpcState"));
    }
    if (g_slot_common < 0 || g_slot_bed < 0) {
        op.why = "Unavailable: the bed program has no write slots";
        return;
    }
    if (op.op == KILN_SURV_OP_ASSIGN_BED && op.distance >= 0 && g_slot_tpl >= 0) {
        uint32_t size = 0;
        const auto* t = static_cast<const uint8_t*>(W->read_other(ctx, it[g_slot_tpl], op.npc, &size));
        if (t && L.tpl_bed_radius + 4 <= int64_t(size)) {
            std::memcpy(&op.radius, t + L.tpl_bed_radius, 4);
            if (std::isfinite(op.radius) && op.radius > 0 && op.distance > op.radius) {
                char b[160];
                std::snprintf(b, sizeof b, "BedOutOfHomeRange: the bed is %.1f m from its home, its bed radius is %.1f m", op.distance,
                              op.radius);
                op.why = b;
                return;
            }
        } else if (!g_tpl_warned.exchange(true)) {
            logf(KILN_LOG_WARN, "survivors: the NPC's bed radius (ActiveNpcState template) could not be read; the home-range check is skipped");
        }
    }
    // every pointer first: nothing is written unless all ends can be
    uint32_t size = 0;
    auto* common = static_cast<uint8_t*>(W->write_other(ctx, it[g_slot_common], op.npc, &size));
    if (!common || int64_t(size) != L.common_size) {
        op.why = "NotInWorld: the NPC's state cannot be written";
        return;
    }
    uint8_t* bed = nullptr;
    if (op.op == KILN_SURV_OP_ASSIGN_BED) {
        bed = static_cast<uint8_t*>(W->write_other(ctx, it[g_slot_bed], op.bed, &size));
        if (!bed || int64_t(size) != L.bed_size) {
            op.why = "NotABed: the bed cannot be written";
            return;
        }
    }
    std::vector<uint8_t*> beds, commons;
    for (KilnEntity b : op.clear_beds) {
        auto* p = static_cast<uint8_t*>(W->write_other(ctx, it[g_slot_bed], b, &size));
        if (p && int64_t(size) == L.bed_size) beds.push_back(p);
    }
    for (KilnEntity c : op.clear_commons) {
        auto* p = static_cast<uint8_t*>(W->write_other(ctx, it[g_slot_common], c, &size));
        if (p && int64_t(size) == L.common_size) commons.push_back(p);
    }
    const uint32_t zero = 0;
    for (uint8_t* p : beds) {
        uint32_t v;
        std::memcpy(&v, p + L.bed_npc, 4);
        if (v == op.npc_id) std::memcpy(p + L.bed_npc, &zero, 4);
    }
    for (uint8_t* p : commons) {
        uint32_t v;
        std::memcpy(&v, p + L.common_bed, 4);
        if (v == op.bed) std::memcpy(p + L.common_bed, &zero, 4);
    }
    if (op.op == KILN_SURV_OP_ASSIGN_BED) {
        std::memcpy(common + L.common_bed, &op.bed, 4);
        std::memcpy(bed + L.bed_npc, &op.npc_id, 4);
    } else {
        std::memcpy(common + L.common_bed, &zero, 4);
    }
    op.written = true;
}

void register_program() {
    if (!W || !g_layout_ok) return;
    static const KilnStr writes[] = {ks("NpcBed"), ks("NpcCommon")};
    static const KilnStr tpl[] = {ks("ActiveNpcState")};
    KilnProgramDesc d{};
    d.size = sizeof d;
    d.flags = KILN_PROGRAM_SYSTEM | KILN_PROGRAM_HOST_ONLY | KILN_PROGRAM_BARRIER;
    d.name = ks("kiln_survivors_beds");
    d.each = &bed_program;
    d.other_write = KilnNames{writes, 2, 0};
    d.other_tpl = KilnNames{tpl, 1, 0};
    const KilnResult rc = W->register_program(O, &d, &g_prog);
    if (rc != KILN_OK) {
        // without the template read the program still works (no home-range check)
        d.other_tpl = KilnNames{};
        if (W->register_program(O, &d, &g_prog) != KILN_OK) {
            SW("the bed program was refused (%d): survivors.assign is off", rc);
            g_prog = 0;
            return;
        }
        SW("the bed program runs without the ActiveNpcState template read: no home-range check");
    }
}

void take_bed(Req& r) {
    {
        std::lock_guard lock(g_op_mu);
        if (g_op) return;  // one bed write at a time: wait (the queue limit applies)
    }
    if (!roster_find(r.in.npc_id, &r.roster)) return finish(r, KILN_SURV_REFUSED, "NotARosterNpc: no roster entry has this NpcId");
    g_ent_step = ~0ull;
    const auto zs = zones();
    fill_state(r.roster, &r.before, &zs);
    const auto ents = entities_of(r.in.npc_id);
    if (ents.empty()) return finish(r, KILN_SURV_REFUSED, "NotInWorld: the survivor is not in the world");
    if (ents.size() > 1) return finish(r, KILN_SURV_REFUSED, "NotUnique: the world holds more than one of this NPC");
    if (!(r.roster.npc.unlock.id == 0 || r.roster.npc.unlocked == 1)) return finish(r, KILN_SURV_REFUSED, "Locked: the survivor is not recruited");
    r.npc = ents[0];
    r.old_bed = bed_of(r.npc);
    auto op = std::make_unique<BedOp>();
    op->req = r.id;
    op->op = r.in.op;
    op->npc_id = r.in.npc_id;
    op->npc = r.npc;
    op->bed = r.in.bed;
    std::vector<KilnEntity> touched;
    if (r.in.op == KILN_SURV_OP_ASSIGN_BED) {
        uint32_t occupant = 0;
        if (!bed_npc(r.in.bed, &occupant)) return finish(r, KILN_SURV_REFUSED, "NotABed: the entity is not a bed (a double bed's frame is not one: use a sleeping place)");
        touched.push_back(r.in.bed);
        KilnVec3 home{}, bpos{};
        if (home_of(r.npc, &home) && W->position(r.in.bed, &bpos) == KILN_OK) op->distance = distance(home, bpos);
        if (occupant && occupant != r.in.npc_id) {
            r.displaced_npc = occupant;
            const auto d = entities_of(occupant);
            if (!d.empty()) r.displaced = d.front();
        }
    }
    for (KilnEntity b : find_all(h_bed)) {
        uint32_t v = 0;
        if (b != r.in.bed && bed_npc(b, &v) && v == r.in.npc_id) {
            op->clear_beds.push_back(b);
            touched.push_back(b);
        }
    }
    if (r.old_bed && r.old_bed != r.in.bed && std::find(touched.begin(), touched.end(), r.old_bed) == touched.end()) {
        uint32_t v = 0;
        if (bed_npc(r.old_bed, &v)) touched.push_back(r.old_bed);
    }
    if (r.in.op == KILN_SURV_OP_ASSIGN_BED) {
        for (KilnEntity c : find_all(h_common)) {
            if (c == r.npc) continue;
            if (bed_of(c) == r.in.bed) {
                op->clear_commons.push_back(c);
                if (!r.displaced) {
                    r.displaced = c;
                    auto it = g_npc_of.find(c);
                    r.displaced_npc = it == g_npc_of.end() ? 0 : it->second;
                }
            }
        }
    }
    // authorization (D5), before the no-change answer
    ActorView a;
    player_view(r.in.actor, &a);
    std::vector<BedView> views;
    for (KilnEntity b : touched) {
        BedView v;
        KilnVec3 p{};
        if (W->position(b, &p) == KILN_OK) {
            if (const Zone* z = zone_at(zs, p)) {
                v.in_base = true;
                v.abandoned = z->abandoned;
            }
            uint32_t zone = 0;
            v.denied = r.in.authorization == KILN_SURV_AUTH_PLAYER_PERMISSIONS && G && G->zone_denies(r.in.actor, p, KILN_GAMEPLAY_DENY_BUILD, &zone);
        }
        views.push_back(v);
    }
    if (const char* why = authorize(r.in.authorization, a, 0, views)) return finish(r, KILN_SURV_REFUSED, why);
    r.actor_handle = a.handle;
    if (r.in.op == KILN_SURV_OP_ASSIGN_BED) {
        uint32_t occ = 0;
        bed_npc(r.in.bed, &occ);
        if (r.old_bed == r.in.bed && occ == r.in.npc_id && op->clear_beds.empty() && op->clear_commons.empty()) {
            r.displaced = 0;
            r.displaced_npc = 0;
            return finish(r, KILN_SURV_NO_CHANGE, "");
        }
    } else if (r.old_bed == 0 && op->clear_beds.empty()) {
        return finish(r, KILN_SURV_NO_CHANGE, "");
    }
    {
        std::lock_guard lock(g_op_mu);
        g_op = std::move(op);
    }
    r.state = KILN_SURV_APPLYING;
    r.waiting = "the bed program's write";
    r.in_flight = 1;
    r.stage = 20;
}

// Post: the program ran in this step (20); the step after, read both ends back (21).
std::unordered_map<uint32_t, KilnSurvivorState> g_seen;  // the observer's last view, by NpcId
void step_bed_post(Req& r, uint64_t now, uint64_t step) {
    if (r.stage == 20) {
        std::unique_ptr<BedOp> op;
        {
            std::lock_guard lock(g_op_mu);
            if (!g_op || g_op->req != r.id) return;
            if (!g_op->ran) {
                if (now - r.accepted_ms > 5000 && !past(r, now)) r.waiting = "the bed program has not run yet";
                if (past(r, now)) {
                    g_op.reset();
                    r.in_flight = 0;
                    return fail(r, KILN_SURV_TIMED_OUT, "Deadline: the bed program did not run (survivors.assign)");
                }
                return;
            }
            op = std::move(g_op);
        }
        r.in_flight = 0;
        if (!op->written) return fail(r, KILN_SURV_REFUSED, op->why.empty() ? "Refused: the write did not happen" : op->why);
        r.effects |= KILN_SURV_BED_CHANGED;
        r.write_step = step;
        r.state = KILN_SURV_VERIFYING;
        r.waiting = "the readback one update after the write";
        r.stage = 21;
        return;
    }
    if (r.stage == 21 && step >= r.write_step + 1) {
        const uint32_t now_bed = bed_of(r.npc);
        bool ok;
        if (r.in.op == KILN_SURV_OP_ASSIGN_BED) {
            uint32_t occ = 0;
            ok = now_bed == r.in.bed && bed_npc(r.in.bed, &occ) && occ == r.in.npc_id;
        } else {
            ok = now_bed == 0;
            for (KilnEntity b : find_all(h_bed)) {
                uint32_t v = 0;
                if (bed_npc(b, &v) && v == r.in.npc_id) ok = false;
            }
        }
        RosterEntry cur;
        KilnSurvivorState after = r.before;
        if (roster_at(r.roster.index, &cur)) {
            g_ent_step = ~0ull;
            fill_state(cur, &after);
        }
        post_event(KILN_SURV_EV_ASSIGNMENT_CHANGED, &r, r.before, after);
        g_seen[r.in.npc_id] = after;
        if (r.displaced_npc) {
            KilnSurvivorState ds;
            if (state_of(r.displaced_npc, &ds)) g_seen[r.displaced_npc] = ds;
        }
        if (ok) return finish(r, KILN_SURV_DONE, "");
        return fail(r, KILN_SURV_REFUSED, "ChangedByGame: the readback after the write does not show the assignment");
    }
}

// ---- the pump
void take(Req& r, uint64_t now) {
    r.accepted_step = r.accepted_step ? r.accepted_step : step_now();
    if (r.session == 0 || r.session != session_now()) return finish(r, KILN_SURV_REFUSED, "WorldChanged: accepted for another world session", false);
    if (now > r.accepted_ms + KILN_SURVIVOR_QUEUE_MS) return finish(r, KILN_SURV_TIMED_OUT, "QueueTimeout: not taken within 5 s", false);
    switch (r.in.op) {
    case KILN_SURV_OP_ASSIGN_BED:
    case KILN_SURV_OP_UNASSIGN_BED:
        return take_bed(r);
    default:
        break;
    }
    if (!roster_find(r.in.npc_id, &r.roster)) return finish(r, KILN_SURV_REFUSED, "NotARosterNpc: no roster entry has this NpcId");
    const auto zs = zones();
    fill_state(r.roster, &r.before, &zs);
    r.stage = 1;  // provisional: start_recruit sets the real stage
    if (r.in.op == KILN_SURV_OP_RECRUIT) {
        if (r.in.flags & KILN_SURV_GIVE_TOOL) {
            if (!r.roster.npc.item) return finish(r, KILN_SURV_REFUSED, "NoSummonTool: this survivor has no summon tool");
            ActorView v;
            if (!player_view(r.in.recipient, &v) || !v.in_game) return finish(r, KILN_SURV_REFUSED, "RecipientNotInGame: the recipient is not a player in game");
            r.recipient_handle = v.handle;
        }
        return start_recruit(r);
    }
    // SUMMON: the base and the point
    const Zone* base = nullptr;
    for (const Zone& z : zs)
        if (z.has_guid && std::memcmp(z.guid, r.in.base_id, 16) == 0 && (!base || in_box(z.center, z.half, r.in.position))) base = &z;
    if (!base) return finish(r, KILN_SURV_REFUSED, "BaseNotFound: no loaded base has this id");
    if (base->abandoned) return finish(r, KILN_SURV_REFUSED, "BaseAbandoned: the base is abandoned");
    bool inside = false;
    for (const Zone& z : zs)
        if (z.has_guid && std::memcmp(z.guid, r.in.base_id, 16) == 0 && in_box(z.center, z.half, r.in.position)) inside = true;
    if (!inside) return finish(r, KILN_SURV_REFUSED, "PositionOutsideBase: the point is not inside the base's build zone");
    r.then_summon = true;
    start_recruit(r);
}

void advance(Req& r, uint64_t now, uint64_t step) {
    if (r.stage == 0) return take(r, now);
    if (r.stage < 0) return;
    if (r.in.op == KILN_SURV_OP_RECRUIT) step_recruit(r, now, step);
    else if (r.in.op == KILN_SURV_OP_SUMMON) step_summon(r, now, step);
}

void KILN_CALL on_pre(const KilnTickInfo* t, void*) {
    const uint64_t now = now_ms();
    {
        std::lock_guard lock(g_mu);
        for (auto& r : g_inbox) g_active.push_back(std::move(r));
        g_inbox.clear();
    }
    for (size_t i = 0; i < g_active.size(); ++i) {
        Req& r = *g_active[i];
        if (r.stage >= 20) continue;  // beds go on in Post
        advance(r, now, t->step);
        if (r.stage >= 0) publish_progress(r);
    }
    g_active.erase(std::remove_if(g_active.begin(), g_active.end(), [](const auto& p) { return p->stage < 0; }), g_active.end());
}

uint64_t g_next_observe = 0;
bool g_observed_once = false;
void observe(uint64_t now) {
    if (!g_layout_ok || now < g_next_observe) return;
    g_next_observe = now + 1000;
    {
        std::lock_guard lock(g_op_mu);
        if (g_op) return;  // our own write is in flight
    }
    for (const auto& r : g_active)
        if (r->stage >= 20) return;
    g_ent_step = ~0ull;
    const auto zs = zones();
    const uint32_t n = roster_count();
    for (uint32_t i = 0; i < n; ++i) {
        RosterEntry e;
        if (!roster_at(i, &e)) continue;
        KilnSurvivorState s;
        fill_state(e, &s, &zs);
        auto it = g_seen.find(e.npc.npc_id);
        if (it == g_seen.end()) {
            g_seen[e.npc.npc_id] = s;
            continue;
        }
        // a game-origin change of the relation: the bed moved while the NPC stayed the same entity
        if (g_observed_once && it->second.entity == s.entity && s.entity && it->second.bed != s.bed)
            post_event(KILN_SURV_EV_ASSIGNMENT_CHANGED, nullptr, it->second, s);
        it->second = s;
    }
    g_observed_once = true;
}

void KILN_CALL on_post(const KilnTickInfo* t, void*) {
    const uint64_t now = now_ms();
    for (auto& p : g_active) {
        if (p->stage >= 20) {
            step_bed_post(*p, now, t->step);
            if (p->stage >= 0) publish_progress(*p);
        }
    }
    g_active.erase(std::remove_if(g_active.begin(), g_active.end(), [](const auto& p) { return p->stage < 0; }), g_active.end());
    observe(now);
}

void KILN_CALL on_world(const KilnWorldEvent* e, void*) {
    if (!e || e->kind != KILN_WORLD_SERVER) return;
    if (e->event == KILN_WORLD_STARTED) {
        publish_caps();
        return;
    }
    if (e->event != KILN_WORLD_STOPPING) return;
    {
        std::lock_guard lock(g_op_mu);
        g_op.reset();
    }
    for (auto& p : g_active)
        if (p->stage >= 0) finish(*p, KILN_SURV_WORLD_ENDED, "WorldEnded: the world stopped", false);
    g_active.clear();
    g_seen.clear();
    g_observed_once = false;
    g_ent_step = ~0ull;
}

// ---- the table
bool tick() { return A && A->on_tick_thread(); }

KilnResult KILN_CALL t_status(KilnSurvivorsStatus* out) {
    if (!out || out->size < sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    *out = KilnSurvivorsStatus{};
    out->size = sizeof *out;
    out->read = g_read;
    out->recruit = g_recruit;
    out->summon = g_summon;
    out->assign = g_assign;
    {
        std::lock_guard lock(g_mu);
        out->pending = uint32_t(g_submitted.load() - g_ended.load());
    }
    out->submitted = g_submitted.load();
    out->ended = g_ended.load();
    out->events = g_events.last();
    std::string d = g_why;
    if (!g_layout_ok) d += (d.empty() ? "" : "; ") + g_layout_why;
    set_text(out->detail, sizeof out->detail, d);
    return KILN_OK;
}
uint32_t KILN_CALL t_count() { return tick() && K ? roster_count() : 0; }
KilnResult KILN_CALL t_at(uint32_t index, KilnSurvivorState* out) {
    if (!out || out->size < sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    if (!tick()) return KILN_ERR_WRONG_THREAD;
    if (!g_read) return KILN_ERR_UNAVAILABLE;
    RosterEntry r;
    if (!roster_at(index, &r)) return KILN_ERR_NOT_FOUND;
    fill_state(r, out);
    return KILN_OK;
}
KilnResult KILN_CALL t_get(uint32_t npc_id, KilnSurvivorState* out) {
    if (!out || out->size < sizeof *out || !npc_id) return KILN_ERR_INVALID_ARGUMENT;
    if (!tick()) return KILN_ERR_WRONG_THREAD;
    if (!g_read) return KILN_ERR_UNAVAILABLE;
    return state_of(npc_id, out) ? KILN_OK : KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL t_of_entity(KilnEntity e, uint32_t* npc_id) {
    if (!npc_id || !e) return KILN_ERR_INVALID_ARGUMENT;
    if (!tick()) return KILN_ERR_WRONG_THREAD;
    if (!g_read) return KILN_ERR_UNAVAILABLE;
    refresh_entities();
    auto it = g_npc_of.find(e);
    if (it == g_npc_of.end()) return KILN_ERR_NOT_FOUND;
    RosterEntry r;
    if (!roster_find(it->second, &r)) return KILN_ERR_NOT_FOUND;
    *npc_id = it->second;
    return KILN_OK;
}
KilnResult KILN_CALL t_bed_occupant(KilnEntity bed, uint32_t* npc_id) {
    if (!npc_id || !bed) return KILN_ERR_INVALID_ARGUMENT;
    if (!tick()) return KILN_ERR_WRONG_THREAD;
    if (!g_layout_ok) return KILN_ERR_UNAVAILABLE;
    return bed_npc(bed, npc_id) ? KILN_OK : KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL t_base_at(KilnVec3 p, uint8_t base_id[16], KilnEntity* altar, uint32_t* abandoned) {
    if (!base_id) return KILN_ERR_INVALID_ARGUMENT;
    if (!tick()) return KILN_ERR_WRONG_THREAD;
    if (!g_read || !B || !g_layout_ok) return KILN_ERR_UNAVAILABLE;
    const auto zs = zones();
    for (const Zone& z : zs) {
        if (!z.has_guid || !in_box(z.center, z.half, p)) continue;
        std::memcpy(base_id, z.guid, 16);
        if (altar) *altar = z.altar;
        if (abandoned) *abandoned = z.abandoned ? 1u : 0u;
        return KILN_OK;
    }
    return KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL t_submit(KilnOwner owner, const KilnSurvivorRequest* in, uint64_t* out) {
    if (!out || check_request(in)) return KILN_ERR_INVALID_ARGUMENT;
    const bool on = in->op == KILN_SURV_OP_RECRUIT ? g_recruit : in->op == KILN_SURV_OP_SUMMON ? g_summon : g_assign;
    if (!on) return KILN_ERR_UNAVAILABLE;
    auto r = std::make_unique<Req>();
    r->id = g_next.fetch_add(1);
    r->owner = owner;
    r->in = *in;
    r->accepted_ms = now_ms();
    r->session = session_now();
    *out = r->id;
    ++g_submitted;
    KilnSurvivorProgress p{};
    p.size = sizeof p;
    p.request = r->id;
    p.state = KILN_SURV_QUEUED;
    set_text(p.reason, sizeof p.reason, "queued for the next world step");
    std::lock_guard lock(g_mu);
    g_progress[r->id] = p;
    g_inbox.push_back(std::move(r));
    return KILN_OK;
}
KilnResult KILN_CALL t_result(uint64_t id, KilnSurvivorResult* out) {
    if (!out || out->size < sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    return g_results.get(id, now_ms(), out) ? KILN_OK : KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL t_progress(uint64_t id, KilnSurvivorProgress* out) {
    if (!out || out->size < sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    {
        std::lock_guard lock(g_mu);
        auto it = g_progress.find(id);
        if (it != g_progress.end()) {
            *out = it->second;
            out->elapsed_ms = out->elapsed_ms;  // as of the last step
            return KILN_OK;
        }
    }
    KilnSurvivorResult r{};
    if (!g_results.get(id, now_ms(), &r)) return KILN_ERR_NOT_FOUND;
    *out = KilnSurvivorProgress{};
    out->size = sizeof *out;
    out->state = KILN_SURV_TERMINAL;
    out->request = id;
    out->committed_effects = r.committed_effects;
    return KILN_OK;
}
KilnResult KILN_CALL t_events(uint64_t after, KilnSurvivorEvent* out, uint32_t cap, uint32_t* n) {
    if (!n || (cap && !out)) return KILN_ERR_INVALID_ARGUMENT;
    *n = g_events.read(after, out, cap);
    return KILN_OK;
}

const KilnSurvivorsApi kTable = {
    {sizeof(KilnSurvivorsApi), KILN_SURVIVORS_VERSION},
    &t_status, &t_count, &t_at, &t_get, &t_of_entity, &t_bed_occupant, &t_base_at, &t_submit, &t_result, &t_progress, &t_events,
};

template <class T>
const T* svc(const char* name, uint32_t v) {
    return static_cast<const T*>(A->service(ks(name), v));
}

}  // namespace

// kiln_plugin_main of kiln-quests.dll (after kiln.knowledge registered): the table, the layout, the ticks.
void survivors_main(const KilnApi* api, KilnOwner owner) {
    A = api;
    O = owner;
    load_layout();
    KilnHandle h = 0;
    api->subscribe_world(owner, &on_world, nullptr, &h);
    if (api->subscribe_tick(owner, KILN_TICK_PRE, &on_pre, nullptr, &h) != KILN_OK ||
        api->subscribe_tick(owner, KILN_TICK_POST, &on_post, nullptr, &h) != KILN_OK) {
        SW("no tick on this build: kiln.survivors unavailable");
        return;
    }
    if (api->register_service(owner, ks(KILN_SURVIVORS_SERVICE), &kTable) != KILN_OK) SW("the service name is taken");
}

// on_ready: the other services and the bed program (before the server world registers its programs).
void survivors_ready() {
    if (!A) return;
    W = svc<KilnWorldApi>(KILN_WORLD_SERVICE, 1);
    S = svc<KilnSpawnApi>(KILN_SPAWN_SERVICE, 1);
    K = svc<KilnKnowledgeApi>(KILN_KNOWLEDGE_SERVICE, 1);
    I = svc<KilnItemsApi>(KILN_ITEMS_SERVICE, 1);
    C = svc<KilnCreaturesApi>(KILN_CREATURES_SERVICE, 1);
    P = svc<KilnPlayersApi>(KILN_PLAYERS_SERVICE, 1);
    G = svc<KilnGameplayApi>(KILN_GAMEPLAY_SERVICE, 1);
    B = svc<KilnBuildApi>(KILN_BUILD_SERVICE, 1);
    if (W) {
        h_common = W->component_hash(ks("NpcCommon"));
        h_bed = W->component_hash(ks("NpcBed"));
        h_saved = W->component_hash(ks("ActiveNpcStateSaved"));
        h_base = W->component_hash(ks("BaseIdComponent"));
    }
    register_program();  // the server world: a dedicated server, or a game while it hosts
    if (g_prog) {
        KilnWorldProgramInfo info{};
        info.size = sizeof info;
        if (W->program_info(g_prog, &info) == KILN_OK && info.state <= KILN_PROGRAM_ACTIVE) g_program_active.store(true);
    }
    publish_caps();
    SI("layout %s, bed program %s; read %s, recruit %s, summon %s, assign %s", g_layout_ok ? "on" : g_layout_why.c_str(),
       g_prog ? "registered" : "off", g_read ? "on" : "off", g_recruit ? "on" : "off", g_summon ? "on" : "off", g_assign ? "on" : "off");
}

}  // namespace kiln::survivors

// SPDX-License-Identifier: MIT
// kiln.survivors' request handling (survivors.cpp) on the host, with fake services: kiln.knowledge, kiln.items, kiln.world
// (entities, components, the bed program's writes), kiln.spawn, kiln.players, kiln.gameplay and kiln.build, a stepped clock
// and the world's Pre / bed program / Post per step. Review findings S1 (a recruit never reports "nothing written" while its
// grant can still land), S2 (one bed request at a time until its readback) and S3 (the bed program checks the relation at
// the write).
#include "../survivors.cpp"

#include <cstdio>
#include <map>

using namespace kiln::survivors;

namespace {
int g_fail = 0, g_pass = 0;
void check(bool ok, const std::string& what) {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
}

// ---- the fake world
uint64_t t_ms = 1000, t_step = 1;
struct Npc { uint8_t guid[16]; std::vector<uint8_t> common = std::vector<uint8_t>(1688), saved = std::vector<uint8_t>(48); KilnVec3 pos; };
std::map<KilnEntity, Npc> npcs;
std::map<KilnEntity, uint32_t> beds;  // bed entity -> associatedNpc
std::map<KilnEntity, KilnVec3> where;
struct Entry { KilnKnowledgeNpc npc; std::string name; };
std::vector<Entry> roster;
// knowledge
uint64_t next_know = 1;
std::map<uint64_t, KilnKnowledgeResult> know_results;  // what result() answers (absent: NOT_FOUND)
std::vector<uint64_t> granted;
const uint32_t kCommon = 11, kBed = 12, kSaved = 13, kBase = 14, kTpl = 15;

uint32_t npc_of_guid(const uint8_t g[16]) { return npc_id_of(g); }
uint32_t& bed_field(KilnEntity npc) { return *reinterpret_cast<uint32_t*>(npcs[npc].common.data() + 0x48); }

KilnResult KILN_CALL k_status(KilnKnowledgeStatus* s) {
    *s = KilnKnowledgeStatus{};
    s->size = sizeof *s;
    s->knowledge = s->npc = s->changes = 1;
    s->applying = 1;
    return KILN_OK;
}
uint32_t KILN_CALL k_count(uint32_t) { return static_cast<uint32_t>(roster.size()); }
KilnResult KILN_CALL k_npc(uint32_t i, KilnKnowledgeNpc* out, char* name, size_t cap, size_t* len) {
    if (i >= roster.size()) return KILN_ERR_NOT_FOUND;
    *out = roster[i].npc;
    const size_t n = std::min(cap - 1, roster[i].name.size());
    std::memcpy(name, roster[i].name.data(), n);
    name[n] = 0;
    *len = n;
    return KILN_OK;
}
KilnResult KILN_CALL k_grant(KilnOwner, KilnEntity, uint32_t, uint32_t, uint64_t* req) {
    *req = next_know++;
    granted.push_back(*req);
    return KILN_OK;
}
KilnResult KILN_CALL k_result(uint64_t id, KilnKnowledgeResult* out) {
    auto it = know_results.find(id);
    if (it == know_results.end()) return KILN_ERR_NOT_FOUND;
    *out = it->second;
    return KILN_OK;
}
KilnKnowledgeApi K_{};

// world
uint32_t KILN_CALL w_hash(KilnStr n) {
    const std::string s(n.ptr, n.len);
    return s == "NpcCommon" ? kCommon : s == "NpcBed" ? kBed : s == "ActiveNpcStateSaved" ? kSaved : s == "BaseIdComponent" ? kBase : kTpl;
}
const void* KILN_CALL w_component(KilnEntity e, uint32_t h, uint32_t* size) {
    if (h == kCommon && npcs.count(e)) return *size = 1688, npcs[e].common.data();
    if (h == kSaved && npcs.count(e)) return *size = 48, npcs[e].saved.data();
    if (h == kBed && beds.count(e)) return *size = 4, &beds[e];
    return nullptr;
}
KilnResult KILN_CALL w_find(uint32_t h, KilnEntity* out, uint32_t cap, uint32_t* n) {
    std::vector<KilnEntity> v;
    if (h == kSaved || h == kCommon) for (auto& [e, x] : npcs) v.push_back(e);
    if (h == kBed) for (auto& [e, x] : beds) v.push_back(e);
    *n = static_cast<uint32_t>(v.size());
    for (uint32_t i = 0; i < v.size() && i < cap; ++i) out[i] = v[i];
    return v.size() > cap ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}
bool KILN_CALL w_exists(KilnEntity e) { return npcs.count(e) || beds.count(e); }
KilnResult KILN_CALL w_position(KilnEntity e, KilnVec3* p) {
    if (npcs.count(e)) return *p = npcs[e].pos, KILN_OK;
    if (where.count(e)) return *p = where[e], KILN_OK;
    return KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL w_register(KilnOwner, const KilnProgramDesc*, KilnHandle* h) { return *h = 77, KILN_OK; }
int32_t KILN_CALL w_slot(KilnHandle, int32_t, KilnStr n) { return std::string(n.ptr, n.len) == "NpcCommon" ? 0 : 1; }
uint32_t prog_state = KILN_PROGRAM_ACTIVE;
KilnResult KILN_CALL w_info(KilnHandle, KilnWorldProgramInfo* i) { i->state = prog_state; return KILN_OK; }
void* KILN_CALL w_write_other(KilnProgramCtx*, uint64_t acc, KilnEntity e, uint32_t* size) {
    if (acc == 0 && npcs.count(e)) return *size = 1688, npcs[e].common.data();
    if (acc == 1 && beds.count(e)) return *size = 4, &beds[e];
    return nullptr;
}
KilnWorldApi W_{};
// spawn
KilnResult KILN_CALL s_info(KilnEntity e, KilnEntitySnapshot* s) {
    if (!npcs.count(e)) return KILN_ERR_NOT_FOUND;
    s->has_template = 1;
    std::memcpy(s->template_guid.b, npcs[e].guid, 16);
    return KILN_OK;
}
KilnSpawnApi S_{};
KilnItemsApi I_{};
KilnCreaturesApi C_{};
KilnResult KILN_CALL p_get(KilnEntity, KilnPlayer*) { return KILN_ERR_NOT_FOUND; }
KilnPlayersApi P_{};
bool KILN_CALL g_denies(KilnEntity, KilnVec3, uint32_t, uint32_t*) { return false; }
KilnGameplayApi G_{};
KilnResult KILN_CALL b_bases(KilnBuildBase*, size_t, size_t* n) { return *n = 0, KILN_OK; }
KilnBuildApi B_{};

// the core
void KILN_CALL a_log(KilnOwner, int32_t, KilnStr) {}
KilnResult KILN_CALL a_tick(KilnTickInfo* t) { t->step = t_step; return KILN_OK; }
KilnResult KILN_CALL a_session(KilnSessionInfo* s) { s->worlds[KILN_WORLD_SERVER] = 0x1000; s->sessions[KILN_WORLD_SERVER] = 1; return KILN_OK; }
bool KILN_CALL a_thread() { return true; }
bool KILN_CALL a_feature(KilnStr) { return true; }
KilnResult KILN_CALL a_constant(KilnStr n, int64_t* v) {
    static const std::map<std::string, int64_t> c = {
        {"survivors/npc_common.bed", 0x48}, {"survivors/npc_common.size", 1688}, {"survivors/npc_bed.npc", 0}, {"survivors/npc_bed.size", 4},
        {"survivors/npc_saved.size", 48}, {"survivors/npc_saved.home", 0}, {"survivors/base_id.size", 48}, {"survivors/base_id.abandoned", 40},
        {"survivors/npc_tpl.bed_radius", 188}};
    auto it = c.find(std::string(n.ptr, n.len));
    if (it == c.end()) return KILN_ERR_NOT_FOUND;
    *v = it->second;
    return KILN_OK;
}
const void* KILN_CALL a_service(KilnStr n, uint32_t) {
    const std::string s(n.ptr, n.len);
    if (s == KILN_WORLD_SERVICE) return &W_;
    if (s == KILN_SPAWN_SERVICE) return &S_;
    if (s == KILN_KNOWLEDGE_SERVICE) return &K_;
    if (s == KILN_ITEMS_SERVICE) return &I_;
    if (s == KILN_CREATURES_SERVICE) return &C_;
    if (s == KILN_PLAYERS_SERVICE) return &P_;
    if (s == KILN_GAMEPLAY_SERVICE) return &G_;
    if (s == KILN_BUILD_SERVICE) return &B_;
    return nullptr;
}
KilnResult KILN_CALL a_register(KilnOwner, KilnStr, const void*) { return KILN_OK; }
KilnResult KILN_CALL a_sub_world(KilnOwner, KilnWorldEventFn, void*, KilnHandle*) { return KILN_OK; }
KilnResult KILN_CALL a_sub_tick(KilnOwner, int32_t, KilnTickFn, void*, KilnHandle*) { return KILN_OK; }
KilnResult KILN_CALL a_symbol(KilnStr, uintptr_t* out) { *out = 0; return KILN_ERR_NOT_FOUND; }
uintptr_t KILN_CALL a_world() { return 0; }
KilnApi A_{};

// one world step: Pre, the bed program, Post; the clock moves 41 ms (a dedicated server's ~24 Hz)
void step() {
    KilnTickInfo t{};
    t.size = sizeof t;
    t.step = t_step;
    on_pre(&t, nullptr);
    uint64_t iter[2] = {0, 1};
    bed_program(nullptr, iter, 0, nullptr);
    on_post(&t, nullptr);
    ++t_step;
    t_ms += 41;
}
bool done(uint64_t id, KilnSurvivorResult* r) {
    r->size = sizeof *r;
    return t_result(id, r) == KILN_OK;
}

void setup() {
    K_.header.size = sizeof K_;
    K_.status = &k_status, K_.catalog_count = &k_count, K_.npc = &k_npc, K_.grant = &k_grant, K_.result = &k_result;
    W_.header.size = sizeof W_;
    W_.component_hash = &w_hash, W_.component = &w_component, W_.find = &w_find, W_.exists = &w_exists, W_.position = &w_position;
    W_.register_program = &w_register, W_.program_slot = &w_slot, W_.program_info = &w_info, W_.write_other = &w_write_other;
    S_.header.size = sizeof S_;
    S_.entity_info = &s_info;
    I_.header.size = sizeof I_;
    C_.header.size = sizeof C_;
    P_.header.size = sizeof P_;
    P_.get = &p_get;
    G_.header.size = sizeof G_;
    G_.zone_denies = &g_denies;
    B_.header.size = sizeof B_;
    B_.bases = &b_bases;
    A_.size = sizeof A_;
    A_.log = &a_log, A_.tick_info = &a_tick, A_.session = &a_session, A_.on_tick_thread = &a_thread, A_.feature = &a_feature;
    A_.constant = &a_constant, A_.service = &a_service, A_.register_service = &a_register, A_.subscribe_world = &a_sub_world;
    A_.subscribe_tick = &a_sub_tick, A_.symbol = &a_symbol, A_.server_world = &a_world;
    g_clock = [] { return t_ms; };
    survivors_main(&A_, 5);
    survivors_ready();
    // two survivors: A (locked, a plain world Bool) and B (unlocked), both in the world near two beds
    for (int k = 0; k < 2; ++k) {
        Npc n;
        std::memset(n.guid, 0x40 + k, 16);
        n.pos = {100.0 + k, 50, 100};
        float home[3] = {100.0f + k, 50, 100};
        std::memcpy(n.saved.data(), home, 12);
        npcs[2000 + k] = n;
        Entry e{};
        e.npc.size = sizeof e.npc;
        e.npc.npc_id = npc_of_guid(n.guid);
        e.npc.item = 0x1234;
        e.npc.unlock.id = k == 0 ? 0x1c68d6ae : 0;
        e.npc.unlock.compare = 2, e.npc.unlock.type = 1, e.npc.unlock.compare_value = 1;
        e.npc.unlocked = k == 0 ? 0 : 1;
        e.name = k == 0 ? "NPC_A" : "NPC_B";
        roster.push_back(e);
    }
    beds[3000] = 0, beds[3001] = 0;
    where[3000] = {103, 50, 100}, where[3001] = {104, 50, 100};
}
KilnSurvivorRequest req(uint32_t op, int who) {
    KilnSurvivorRequest r{};
    r.size = sizeof r;
    r.op = op;
    r.npc_id = roster[who].npc.npc_id;
    if (op == KILN_SURV_OP_ASSIGN_BED || op == KILN_SURV_OP_UNASSIGN_BED) r.authorization = KILN_SURV_AUTH_TRUSTED_SERVER;
    return r;
}

// S1: the grant is handed to kiln.knowledge and its result never comes (pending, or forgotten after 256 newer ones).
void recruit_drains() {
    roster[0].npc.unlocked = 0;
    know_results.clear();
    uint64_t id = 0;
    const KilnSurvivorRequest r = req(KILN_SURV_OP_RECRUIT, 0);
    check(t_submit(1, &r, &id) == KILN_OK, "S1: recruit accepted");
    KilnSurvivorResult res{};
    bool ended = false;
    for (int i = 0; i < 24 * 120 && !ended; ++i) {  // two minutes of steps: past the 30 s budget and its 30 s backstop
        step();
        ended = done(id, &res);
    }
    check(!(ended && res.status == KILN_SURV_TIMED_OUT && res.committed_effects == 0),
          "S1: a recruit whose grant may still land never ends TIMED_OUT with nothing written");
    // the unlock lands late (the result itself was forgotten): the request reports what happened
    roster[0].npc.unlocked = 1;
    for (int i = 0; i < 48 && !ended; ++i) {
        step();
        ended = done(id, &res);
    }
    check(ended && (res.committed_effects & KILN_SURV_KNOWLEDGE_WRITTEN) && (res.committed_effects & KILN_SURV_REGISTRY_CONFIRMED) &&
              res.status == KILN_SURV_DONE,
          "S1: the late unlock is reported as written and confirmed (status " + std::to_string(res.status) + ", effects " +
              std::to_string(res.committed_effects) + ")");
}

// S1: a grant that never answers and never lands ends INDETERMINATE (it may still land), never TIMED_OUT
void recruit_indeterminate() {
    roster[0].npc.unlocked = 0;
    uint64_t id = 0;
    const KilnSurvivorRequest r = req(KILN_SURV_OP_RECRUIT, 0);
    check(t_submit(1, &r, &id) == KILN_OK, "S1: second recruit accepted");
    KilnSurvivorResult res{};
    step();
    t_ms += 31000 + 600000;  // the budget and ten minutes of draining
    for (int i = 0; i < 5 && !done(id, &res); ++i) step();
    check(done(id, &res) && res.status == KILN_SURV_INDETERMINATE && std::string(res.reason).rfind("KnowledgeUnknown", 0) == 0,
          "S1: unanswered for the budget and ten minutes more: INDETERMINATE, may still land (status " + std::to_string(res.status) + ")");
}

// S2: two bed requests back to back on the same bed: each reads back its own write
void beds_one_at_a_time() {
    roster[0].npc.unlocked = 1;
    bed_field(2000) = 0, bed_field(2001) = 0, beds[3000] = 0, beds[3001] = 0;
    KilnSurvivorRequest a = req(KILN_SURV_OP_ASSIGN_BED, 0), b = req(KILN_SURV_OP_ASSIGN_BED, 1);
    a.bed = 3000, b.bed = 3000;
    uint64_t ia = 0, ib = 0;
    check(t_submit(1, &a, &ia) == KILN_OK && t_submit(1, &b, &ib) == KILN_OK, "S2: both accepted");
    KilnSurvivorResult ra{}, rb{};
    for (int i = 0; i < 40 && !(done(ia, &ra) && done(ib, &rb)); ++i) step();
    check(done(ia, &ra) && ra.status == KILN_SURV_DONE, "S2: the first assignment reads back its own write (status " + std::to_string(ra.status) + ": " + ra.reason + ")");
    check(done(ib, &rb) && rb.status == KILN_SURV_DONE && rb.displaced_entity == 2000, "S2: the second displaces the first after it (status " +
          std::to_string(rb.status) + ": " + rb.reason + ")");
    check(beds[3000] == roster[1].npc.npc_id && bed_field(2001) == 3000 && bed_field(2000) == 0, "S2: both ends as the second left them");
}

// S3: in the step of the write, the game itself gives the bed to another NPC before Kiln's barrier program runs
void bed_changed_in_step() {
    bed_field(2000) = 0, bed_field(2001) = 0, beds[3000] = 0, beds[3001] = 0;
    KilnSurvivorRequest a = req(KILN_SURV_OP_ASSIGN_BED, 0);
    a.bed = 3001;
    uint64_t ia = 0;
    check(t_submit(1, &a, &ia) == KILN_OK, "S3: accepted");
    // Pre takes the request; then the game's own assignment program gives bed 3001 to B; then Kiln's program runs
    KilnTickInfo t{};
    t.size = sizeof t;
    t.step = t_step;
    on_pre(&t, nullptr);
    beds[3001] = roster[1].npc.npc_id, bed_field(2001) = 3001;
    uint64_t iter[2] = {0, 1};
    bed_program(nullptr, iter, 0, nullptr);
    on_post(&t, nullptr);
    ++t_step, t_ms += 41;
    KilnSurvivorResult ra{};
    for (int i = 0; i < 10 && !done(ia, &ra); ++i) step();
    const bool one_sided = beds[3001] == roster[0].npc.npc_id && bed_field(2001) == 3001;
    check(!one_sided, "S3: no one-sided relation is left (B still names the bed Kiln gave A)");
    check(done(ia, &ra) && (ra.status != KILN_SURV_DONE || (bed_field(2001) == 0 && beds[3001] == roster[0].npc.npc_id)),
          "S3: either refused (ChangedByGame) or B was displaced at the write (status " + std::to_string(ra.status) + ": " + ra.reason + ")");
}
}  // namespace

int main() {
    setup();
    recruit_drains();
    recruit_indeterminate();
    beds_one_at_a_time();
    bed_changed_in_step();
    // N4: the world refused the bed program after on_ready: survivors.assign is no longer advertised
    prog_state = KILN_PROGRAM_REFUSED;
    publish_caps();
    KilnSurvivorsStatus st{};
    st.size = sizeof st;
    check(t_status(&st) == KILN_OK && st.assign == 0, "N4: a refused bed program takes survivors.assign down");
    prog_state = KILN_PROGRAM_ACTIVE;
    std::printf("survivors_flow_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

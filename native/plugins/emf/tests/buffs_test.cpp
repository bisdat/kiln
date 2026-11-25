// SPDX-License-Identifier: MIT
// Host test of kiln-emf's v32 buffs (api_buffs.cpp) and v35 player_view (api_view.cpp) with fake Kiln services
// (kiln.playerstate, kiln.abilities, kiln.spawn, kiln.world, kiln.players, kiln.client, kiln.content, kiln.render),
// and of their game-free model (buffs_model.cpp). The two Impact programs go through kiln.abilities' own verifier.
// No game, no Wine.
//   buffs_test            model, server-side glue (a dedicated server or a host's server world)
//   buffs_test client     a joined client's process: buff reads answer 0, giving and removing are refused, texts and
//                         player_view come from the client world
//   buffs_test server     a dedicated server: buff_text and player_view answer false
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "buffs_model.h"
#include "emf_internal.h"
#include "services/kiln_abilities.h"
#include "services/kiln_playerstate.h"
#include "services/kiln_render.h"
#include "verify.h"

constexpr double kPi = 3.14159265358979323846;
static int failures = 0, checks = 0;
#define CHECK(x)                                                \
    do {                                                        \
        ++checks;                                               \
        if (!(x)) {                                             \
            ++failures;                                         \
            std::printf("FAIL %d: %s\n", __LINE__, #x);         \
        }                                                       \
    } while (0)

// ---------------------------------------------------------------------------------------------- kemf fakes ------
namespace kemf {
const KilnApi* g_api;
KilnOwner g_owner = 1;
uint32_t g_side = KILN_SIDE_CLIENT;
Services S;
Paths P;
thread_local Context t_ctx;
static Mod mod_a, mod_b;
static Mod* active_mod = &mod_a;
static Mod* fault_mod = nullptr;
Mod* caller_mod(const void*) { return active_mod; }
Mod* mod_of_address(const void*) { return nullptr; }
std::vector<std::string> kiln_lines;
void logf(int, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kiln_lines.emplace_back(buf);
    if (std::getenv("KEMF_TEST_VERBOSE")) std::printf("  kiln %s\n", buf);
}
std::map<const Mod*, std::vector<std::string>> mod_lines;
void mod_logf(const Mod* m, int, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    mod_lines[m].emplace_back(buf);
    if (std::getenv("KEMF_TEST_VERBOSE")) std::printf("  mod %s\n", buf);
}
int unsupported_calls = 0, refusals = 0;
std::set<std::string> unsupported_fns;
std::vector<std::string> refusal_lines;
void unsupported(const Mod*, const char* fn, const char* why) {
    ++unsupported_calls;
    if (fn) unsupported_fns.insert(fn);
    if (std::getenv("KEMF_TEST_VERBOSE")) std::printf("  unsupported %s: %s\n", fn, why ? why : "");
}
void refuse(const Mod*, const char* fn, const char* why, bool) {
    ++refusals;
    refusal_lines.push_back(std::string(fn) + ": " + (why ? why : ""));
    if (std::getenv("KEMF_TEST_VERBOSE")) std::printf("  refuse %s: %s\n", fn, why ? why : "");
}
void note_call(const char*) {}
void mark_status_dirty() {}
void account(Mod*, uint64_t) {}
uint64_t qpc_ns() { return 0; }
static uint64_t fake_ms = 100000;
uint64_t now_ms() { return fake_ms; }
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
uint32_t fnv1a(std::string_view s) {
    uint32_t h = 2166136261u;
    for (auto c : s) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return h;
}
uint32_t type_size(uint32_t) { return 0; }
}  // namespace kemf
using namespace kemf;

// ---------------------------------------------------------------------------------------------- worlds ----------
static uintptr_t worlds[3] = {0x1000, 0, 0x2000};  // server, prediction, client (KILN_WORLD_*)
static thread_local int t_thread = 0;              // 0 = the server's tick thread, 1 = the client's main thread, 2 = another
static EmfWorld* server() { return reinterpret_cast<EmfWorld*>(worlds[0]); }
static EmfWorld* client() { return reinterpret_cast<EmfWorld*>(worlds[2]); }
namespace kemf {
uint8_t world_kind_of(EmfWorld* w) {
    for (uint8_t k = 0; k < 3; ++k)
        if (w && worlds[k] && worlds[k] == reinterpret_cast<uintptr_t>(w)) return k;
    return 0xFF;
}
bool on_thread_of(EmfWorld* w) {
    const uint8_t k = world_kind_of(w);
    return (k == 0 && t_thread == 0) || (k == 2 && t_thread == 1);
}
// CurrentTransform of the local player in each world: position 32.32 fixed, then the orientation
struct Transform {
    int64_t pos[3];
    float q[4];
    float scale[3];
    uint32_t pad;
};
static Transform server_tf{}, client_tf{};
static uint8_t faction_of_7 = 5;  // Wildlife
const void* component_of(EmfWorld* w, uint32_t hash, uint32_t entity, uint64_t* size) {
    if (size) *size = 0;
    if (!on_thread_of(w)) return nullptr;
    if (hash == fnv1a("keen::ecs::CurrentTransform") && entity == 1) {
        if (size) *size = sizeof(Transform);
        return w == server() ? &server_tf : w == client() ? &client_tf : nullptr;
    }
    if (hash == fnv1a("keen::ecs::Faction") && entity == 7 && w == server()) {
        if (size) *size = 1;
        return &faction_of_7;
    }
    return nullptr;
}
static int64_t ecs_now = 50'000'000'000LL;  // 50 s
static EmfApi fake_table{};
const EmfApi* api_table() {
    fake_table.ecs_time_ns = [](EmfWorld*) -> int64_t { return ecs_now; };
    return &fake_table;
}
}  // namespace kemf

// ---------------------------------------------------------------------------------------------- buff types ------
// The BuffRegistry singleton -> +0x20 the collection resource -> +0x18 BlobArray {rel, count} -> 184-byte records.
alignas(8) static uint8_t registry[64];
alignas(8) static uint8_t collection[0x20 + 4 * buffs::kTypeRecordSize];
static const uint8_t kNameGuid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint8_t kDescGuid[16] = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
static const uint8_t kFollowGuid[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
constexpr uint32_t kBleed = 0xb1eed000u, kWet = 0x0000e700u, kPotion = 0x00710700u, kRested = 0x00055700u;
static void make_record(uint8_t* r, uint32_t id, uint8_t slot, uint8_t apply, int64_t ns, uint16_t faction, bool saved,
                        bool follow) {
    std::memset(r, 0, buffs::kTypeRecordSize);
    std::memcpy(r, &id, 4);
    r[4] = slot;
    r[5] = apply;
    std::memcpy(r + 8, &ns, 8);
    std::memcpy(r + 0x10, kNameGuid, 16);
    std::memcpy(r + 0x20, kDescGuid, 16);
    r[0x30] = slot == EMF_BUFF_FOOD;  // isFoodPoison
    r[0x31] = id == kRested;         // hideInUi
    r[0x32] = slot == EMF_BUFF_DEBUFF;  // showAsTag
    r[0x49] = slot == EMF_BUFF_DEBUFF;  // despawnOnDeath
    r[0x4A] = saved;
    std::memcpy(r + 0x5C, &faction, 2);
    std::memcpy(r + 0x88, kFollowGuid, 16);
    r[0x98] = follow;  // onTimeoutIsSet
}
static void make_collection(uint32_t count) {
    std::memset(registry, 0, sizeof registry);
    std::memset(collection, 0, sizeof collection);
    const uintptr_t c = reinterpret_cast<uintptr_t>(collection);
    std::memcpy(registry + 0x20, &c, 8);
    const int32_t rel = 0x20 - 0x18;  // records right after the header, relative to the BlobArray field
    std::memcpy(collection + 0x18, &rel, 4);
    std::memcpy(collection + 0x1C, &count, 4);
    uint8_t* r = collection + 0x20;
    make_record(r + 0 * buffs::kTypeRecordSize, kBleed, EMF_BUFF_DEBUFF, 0, 6'000'000'000LL, 1u << 7, false, false);
    make_record(r + 1 * buffs::kTypeRecordSize, kWet, EMF_BUFF_DEBUFF, 1, 60'000'000'000LL, 0, true, true);
    make_record(r + 2 * buffs::kTypeRecordSize, kPotion, EMF_BUFF_BUFF, 2, 300'000'000'000LL, 0, true, false);
    make_record(r + 3 * buffs::kTypeRecordSize, kRested, EMF_BUFF_RESTED, 1, 900'000'000'000LL, 0, false, false);
}
static int64_t collection_constant = 0x20;  // sigdb gamedata/buffreg.collection; -1 = not resolved on this "build"

// ---------------------------------------------------------------------------------------------- playerstate -----
static std::map<uint32_t, std::vector<KilnPlayerBuff>> buffs_on;  // entity -> its BuffContainer as playerstate reads it
static int buffs_calls = 0;
static KilnPlayerBuff pb(uint32_t id, uint32_t entity, uint32_t slot, uint32_t index, int64_t life, int64_t end, uint32_t cleansed = 0) {
    KilnPlayerBuff b{};
    b.id = id;
    b.entity = entity;
    b.slot = slot;
    b.index = index;
    b.life_ns = life;
    b.end_ns = end;
    b.cleansed = cleansed;
    b.level = 3;
    return b;
}
static KilnPlayerStateApi playerstate{};
static KilnResult KILN_CALL ps_buffs(KilnEntity e, KilnPlayerBuff* out, uint32_t cap, uint32_t* count) {
    ++buffs_calls;
    *count = 0;
    if (t_thread != 0) return t_thread == 1 ? KILN_ERR_NOT_FOUND : KILN_ERR_WRONG_THREAD;  // BuffContainer is server only
    auto it = buffs_on.find(e);
    if (it == buffs_on.end()) return KILN_ERR_NOT_FOUND;
    const uint32_t n = static_cast<uint32_t>(it->second.size());
    for (uint32_t i = 0; i < n && i < cap; ++i) out[i] = it->second[i];
    *count = n;
    return out && n > cap ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}

// ---------------------------------------------------------------------------------------------- abilities -------
static KilnAbilitiesApi abilities{};
static std::map<uint32_t, std::vector<uint8_t>> registered;  // program id -> blob
static bool registry_loaded = true, abilities_ready = true;
static std::vector<std::string> verify_errors;
static std::map<KilnEntity, std::vector<KilnConfigOverride>> overrides;
// impact instances: impact id -> its variables (index -> bytes)
static std::map<uint32_t, std::map<uint32_t, std::vector<uint8_t>>> instances;
static KilnResult KILN_CALL ab_status(KilnAbilitiesStatus* s) {
    s->natives = s->programs = s->instances = abilities_ready;
    return KILN_OK;
}
static KilnResult KILN_CALL ab_program_info(uint32_t id, KilnImpactProgramInfo* out) {
    if (t_thread != 0) return KILN_ERR_WRONG_THREAD;
    if (!registered.count(id)) return KILN_ERR_NOT_FOUND;
    out->id = id;
    out->kiln = 1;
    return KILN_OK;
}
static KilnResult KILN_CALL ab_program_register(KilnOwner, const void* blob, size_t len, uint32_t* id, char* err, size_t cap) {
    if (t_thread != 0) return KILN_ERR_WRONG_THREAD;
    if (!registry_loaded) return KILN_ERR_UNAVAILABLE;
    const auto* b = static_cast<const uint8_t*>(blob);
    const kab::VerifyResult v = kab::verify_program(b, len, [](uint32_t h) -> int {
        return h == buffs::kApplyBuffNative ? 5 : h == buffs::kRemoveBuffNative ? 3 : -1;
    });
    if (!v.ok) {
        verify_errors.push_back(v.error);
        std::snprintf(err, cap, "%s", v.error.c_str());
        return KILN_ERR_INVALID_ARGUMENT;
    }
    *id = v.id;
    registered[v.id].assign(b, b + len);
    return KILN_OK;
}
static KilnResult KILN_CALL ab_overrides(KilnEntity e, const KilnConfigOverride* list, uint32_t n, uint32_t) {
    if (t_thread != 0) return KILN_ERR_WRONG_THREAD;
    overrides[e].assign(list, list + n);
    return KILN_OK;
}
static KilnResult KILN_CALL ab_instance_read(uint32_t impact, uint32_t index, void* dst, uint32_t cap, uint32_t* len) {
    if (t_thread != 0) return KILN_ERR_WRONG_THREAD;
    auto it = instances.find(impact);
    if (it == instances.end() || !it->second.count(index)) return KILN_ERR_NOT_FOUND;
    const auto& v = it->second[index];
    std::memcpy(dst, v.data(), std::min<size_t>(cap, v.size()));
    *len = static_cast<uint32_t>(v.size());
    return KILN_OK;
}

// ---------------------------------------------------------------------------------------------- spawn / world ---
struct Spawned {
    KilnEntity entity;
    uint8_t guid[16];
    KilnVec3 at;
    uint32_t flags;
    std::map<uint32_t, std::vector<uint8_t>> comps;
};
static std::vector<Spawned> spawned;
static std::set<KilnEntity> destroyed, live_impacts;
static KilnEntity next_entity = 500;
static bool spawn_refuses = false;
static KilnSpawnApi spawn{};
static KilnResult KILN_CALL sp_spawn(const KilnSpawnRequest* r, KilnEntity* out) {
    if (t_thread != 0) return KILN_ERR_WRONG_THREAD;
    if (spawn_refuses) return KILN_ERR_INTERNAL;
    Spawned s{};
    s.entity = next_entity++;
    std::memcpy(s.guid, r->template_guid.b, 16);
    s.at = r->position;
    s.flags = r->flags;
    for (uint32_t i = 0; i < r->component_count; ++i) {
        const auto* d = static_cast<const uint8_t*>(r->components[i].data);
        s.comps[r->components[i].hash].assign(d, d + r->components[i].size);
    }
    spawned.push_back(s);
    live_impacts.insert(s.entity);
    *out = s.entity;
    return KILN_OK;
}
static KilnResult KILN_CALL sp_destroy(KilnEntity e) {
    if (t_thread != 0) return KILN_ERR_WRONG_THREAD;
    destroyed.insert(e);
    live_impacts.erase(e);
    return KILN_OK;
}
static KilnResult KILN_CALL sp_entity_info(KilnEntity e, KilnEntitySnapshot* out) {
    if (t_thread != 0) return KILN_ERR_WRONG_THREAD;
    if (e != 7 && e != 1 && e != 8) return KILN_ERR_NOT_FOUND;
    out->position = KilnVec3{10.0 * e, 20, 30};
    return KILN_OK;
}
static uint32_t KILN_CALL sp_caps() { return KILN_SPAWN_CAP_COMPONENTS; }
static uint32_t KILN_CALL sp_world_state() { return KILN_SPAWN_WORLD_RUNNING; }
// The game runs a spawned impact's program in the step: its Impact component gets an impact id and the variables.
static std::map<KilnEntity, std::array<uint8_t, 24>> impact_comps;
static uint32_t next_impact = 9000;
static void game_creates_instance(KilnEntity e, bool done, bool answer) {
    std::array<uint8_t, 24> c{};
    const auto& s = *std::find_if(spawned.begin(), spawned.end(), [&](const Spawned& x) { return x.entity == e; });
    std::memcpy(c.data(), s.comps.at(fnv1a("keen::ecs::Impact")).data(), 24);
    const uint32_t id = next_impact++;
    std::memcpy(c.data() + 20, &id, 4);
    impact_comps[e] = c;
    const uint32_t prog = 0;
    (void)prog;
    const bool apply = std::memcmp(c.data(), buffs::kApplyProgram, 16) == 0;
    auto& v = instances[id];
    v[apply ? uint32_t{buffs::kApplyVarDone} : uint32_t{buffs::kRemoveVarDone}] = {static_cast<uint8_t>(done)};
    v[apply ? uint32_t{buffs::kApplyVarApplied} : uint32_t{buffs::kRemoveVarWas}] = {static_cast<uint8_t>(answer)};
}
static KilnWorldApi world_api{};
static const void* KILN_CALL wd_component(KilnEntity e, uint32_t hash, uint32_t* size) {
    if (t_thread != 0) return nullptr;
    if (hash == fnv1a("keen::ecs::Impact") && live_impacts.count(e)) {
        if (!impact_comps.count(e)) {  // spawned, the game has not created its instance yet: impact id 0
            for (const auto& s : spawned)
                if (s.entity == e) std::memcpy(impact_comps[e].data(), s.comps.at(fnv1a("keen::ecs::Impact")).data(), 24);
        }
        if (size) *size = 24;
        return impact_comps[e].data();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------- players ---------
static KilnPlayersApi players{};
static std::vector<KilnEntity> in_game = {1};
static KilnResult KILN_CALL pl_list(uint32_t, KilnPlayer* out, uint32_t cap, uint32_t* n) {
    *n = static_cast<uint32_t>(in_game.size());
    for (uint32_t i = 0; i < *n && i < cap; ++i) {
        out[i] = KilnPlayer{};
        out[i].size = sizeof(KilnPlayer);
        out[i].entity = in_game[i];
        out[i].state = KILN_PLAYER_IN_GAME;
    }
    return *n > cap ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}

// ---------------------------------------------------------------------------------------------- client ----------
static KilnClientApi client_api{};
static uint32_t session = KILN_SESSION_HOST, local = 1, generation = 3;
static KilnResult KILN_CALL cl_state(KilnClientState* s) {
    s->side = kemf::g_side;
    s->session = session;
    s->local_player = local;
    s->generation = generation;
    return KILN_OK;
}
static bool KILN_CALL cl_on_client_thread() { return t_thread == 1; }

// ---------------------------------------------------------------------------------------------- content ---------
static KilnContentApi content{};
static std::map<uint32_t, std::string> texts;
static uint32_t language = 1;
static int text_calls = 0;
static KilnResult KILN_CALL ct_text(uint32_t id, char* buf, size_t cap, size_t* len) {
    ++text_calls;
    if (t_thread != 1) return KILN_ERR_WRONG_THREAD;
    auto it = texts.find(id);
    if (it == texts.end()) return KILN_ERR_NOT_FOUND;
    std::snprintf(buf, cap, "%s", it->second.c_str());
    *len = it->second.size();
    return it->second.size() + 1 > cap ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}
static KilnResult KILN_CALL ct_text_info(KilnContentTextInfo* out) {
    out->state = 1;
    out->language = language;
    return KILN_OK;
}

// ---------------------------------------------------------------------------------------------- render ----------
static KilnRenderApi render{};
static bool camera_valid = true;
static float camera_q[4] = {0, 0, 0, 1};
static KilnResult KILN_CALL rd_camera(KilnRenderCamera* c) {
    c->available = 1;
    c->valid = camera_valid;
    std::memcpy(c->rotation, camera_q, sizeof camera_q);
    return KILN_OK;
}

// ---------------------------------------------------------------------------------------------- core ------------
static KilnApi api{};
static void setup() {
    api.service = [](KilnStr n, uint32_t) -> const void* {
        const std::string s(n.ptr, n.len);
        if (s == KILN_PLAYERSTATE_SERVICE) return &playerstate;
        if (s == KILN_ABILITIES_SERVICE) return &abilities;
        if (s == KILN_RENDER_SERVICE) return kemf::g_side == KILN_SIDE_SERVER ? nullptr : &render;
        return nullptr;
    };
    api.world = [](uint32_t k) -> uintptr_t { return k < 3 ? worlds[k] : 0; };
    api.on_world_thread = [](uint32_t k) -> bool { return (k == 0 && t_thread == 0) || (k == 2 && t_thread == 1); };
    api.on_tick_thread = []() -> bool { return t_thread == 0; };
    api.constant = [](KilnStr n, int64_t* v) -> KilnResult {
        if (std::string(n.ptr, n.len) == "gamedata/buffreg.collection" && collection_constant >= 0) {
            *v = collection_constant;
            return KILN_OK;
        }
        return KILN_ERR_NOT_FOUND;
    };
    api.singleton = [](uintptr_t w, KilnStr n) -> uintptr_t {
        return (w == worlds[0] || w == worlds[2]) && std::string(n.ptr, n.len) == "BuffRegistry" ? reinterpret_cast<uintptr_t>(registry) : 0;
    };
    kemf::g_api = &api;
    playerstate.header.size = sizeof playerstate;
    playerstate.header.version = KILN_PLAYERSTATE_VERSION;
    playerstate.buffs = &ps_buffs;
    abilities.header.size = sizeof abilities;
    abilities.header.version = KILN_ABILITIES_VERSION;
    abilities.status = &ab_status;
    abilities.program_info = &ab_program_info;
    abilities.program_register = &ab_program_register;
    abilities.entity_overrides = &ab_overrides;
    abilities.instance_read = &ab_instance_read;
    spawn.header.size = sizeof spawn;
    spawn.spawn = &sp_spawn;
    spawn.destroy = &sp_destroy;
    spawn.entity_info = &sp_entity_info;
    spawn.capabilities = &sp_caps;
    spawn.world_state = &sp_world_state;
    S.spawn = &spawn;
    world_api.header.size = sizeof world_api;
    world_api.component = &wd_component;
    S.world = &world_api;
    players.header.size = sizeof players;
    players.list = &pl_list;
    S.players = &players;
    client_api.header.size = sizeof client_api;
    client_api.state = &cl_state;
    client_api.on_client_thread = &cl_on_client_thread;
    S.client = &client_api;
    content.header.size = sizeof content;
    content.text = &ct_text;
    content.text_info = &ct_text_info;
    S.content = &content;
    render.header.size = sizeof render;
    render.header.version = KILN_RENDER_VERSION;
    render.camera = &rd_camera;
    make_collection(4);
}

// the plugin's ticks, as plugin.cpp / api_core.cpp call them
static void server_pre() { t_thread = 0; buffs_server_pre(); }
static void server_post() { t_thread = 0; buffs_after_step(server(), EMF_WORLD_SERVER); }
static void client_post() {
    const int was = t_thread;
    t_thread = 1;
    buffs_after_step(client(), EMF_WORLD_CLIENT);
    t_thread = was;
}

struct Got {
    std::vector<EmfBuffEvent> ev;
    std::vector<std::string> why;
    std::vector<const Mod*> mods;
    std::vector<bool> world_cb;
};
static Got got;
static void done_cb(EmfWorld* w, const EmfBuffEvent* e, void* user) {
    got.ev.push_back(*e);
    got.why.push_back(e->why ? e->why : "");
    got.mods.push_back(t_ctx.mod);
    got.world_cb.push_back(t_ctx.world_cb && t_ctx.world == w && w == server() && user == &got);
}
static Got watched;
static void watch_cb(EmfWorld*, const EmfBuffEvent* e, void*) {
    watched.ev.push_back(*e);
    watched.mods.push_back(t_ctx.mod);
}

// ---------------------------------------------------------------------------------------------- model ------------
static void test_model() {
    // a record at EMF's offsets
    EmfBuffType t{};
    CHECK(buffs::type_of(collection + 0x20, &t));
    CHECK(t.size == sizeof t && t.id == kBleed && t.slot == EMF_BUFF_DEBUFF && t.apply_type == 0 && t.default_ns == 6'000'000'000LL);
    CHECK(t.name_tag == buffs::guid_id(kNameGuid) && t.desc_tag == buffs::guid_id(kDescGuid) && t.name_tag != 0);
    CHECK(t.faction_exclude == (1u << 7) && t.follow_up == 0);
    CHECK(t.flags == (EMF_BUFF_SHOW_AS_TAG | EMF_BUFF_DESPAWN_ON_DEATH));
    CHECK(buffs::type_of(collection + 0x20 + buffs::kTypeRecordSize, &t) && t.id == kWet && t.apply_type == 1 &&
          t.flags == (EMF_BUFF_SAVED | EMF_BUFF_SHOW_AS_TAG | EMF_BUFF_DESPAWN_ON_DEATH) && t.follow_up == buffs::guid_id(kFollowGuid));
    CHECK(buffs::type_of(collection + 0x20 + 3 * buffs::kTypeRecordSize, &t) && t.flags == EMF_BUFF_HIDDEN && t.slot == EMF_BUFF_RESTED);
    uint8_t zero[16] = {};
    CHECK(buffs::guid_id(zero) == 0 && !buffs::type_of(nullptr, &t));

    // rows -> EmfBuff: the time base is the world's ECS time; end 0 = not known
    const buffs::Row rows[] = {
        {kBleed, 600, EMF_BUFF_DEBUFF, 4, 0, 6'000'000'000LL, 52'000'000'000LL, 2},
        {kWet, 601, EMF_BUFF_DEBUFF, 5, 1, 60'000'000'000LL, 0, 0},
        {kPotion, 602, EMF_BUFF_BUFF, 0, 0, 300'000'000'000LL, 40'000'000'000LL, 0},
    };
    EmfBuff out[3]{};
    CHECK(buffs::fill_buffs(rows, 3, 50'000'000'000LL, out, 3) == 3);
    CHECK(out[0].size == sizeof(EmfBuff) && out[0].type == kBleed && out[0].entity == 600 && out[0].slot == EMF_BUFF_DEBUFF &&
          out[0].index == 4 && out[0].cleansed == 0 && out[0].level == 2 && out[0].total_ns == 6'000'000'000LL &&
          out[0].remaining_ns == 2'000'000'000LL);
    CHECK(out[1].cleansed == 1 && out[1].total_ns == -1 && out[1].remaining_ns == -1 && out[1].level == 0);
    CHECK(out[2].remaining_ns == 0 && out[2].total_ns == 300'000'000'000LL);
    EmfBuff one[1]{};
    CHECK(buffs::fill_buffs(rows, 3, 0, one, 1) == 3 && one[0].type == kBleed && buffs::fill_buffs(rows, 3, 0, nullptr, 0) == 3);
    CHECK(std::string(buffs::slot_name(EMF_BUFF_DEBUFF)) == "debuff" && std::string(buffs::slot_name(9)) == "?");

    // events: first sight says nothing; then applied, refreshed (end later by more than 1 s), removed, replaced
    buffs::Diff d;
    std::vector<buffs::Row> r1 = {rows[0]};
    CHECK(d.step(1, r1.data(), 1, 50'000'000'000LL).empty() && d.size() == 1);
    std::vector<buffs::Row> r2 = {rows[0], rows[2]};
    auto ev = d.step(1, r2.data(), 2, 50'000'000'000LL);
    CHECK(ev.size() == 1 && ev[0].kind == EMF_BUFF_APPLIED && ev[0].target == 1 && ev[0].type == kPotion && ev[0].buff_entity == 602 &&
          ev[0].slot == EMF_BUFF_BUFF && ev[0].size == sizeof(EmfBuffEvent) && ev[0].why == nullptr);
    r2[0].end_ns += 500'000'000LL;  // rain re-applies Wet every step: its end creeps on, no event
    CHECK(d.step(1, r2.data(), 2, 50'000'000'000LL).empty());
    r2[0].end_ns += 2'000'000'000LL;
    ev = d.step(1, r2.data(), 2, 50'000'000'000LL);
    CHECK(ev.size() == 1 && ev[0].kind == EMF_BUFF_REFRESHED && ev[0].remaining_ns == 4'500'000'000LL &&
          ev[0].total_ns == 6'000'000'000LL);
    r2[0].cleansed = 1;
    CHECK(d.step(1, r2.data(), 2, 50'000'000'000LL).empty());  // being removed: no event yet
    std::vector<buffs::Row> r3 = {rows[2]};
    ev = d.step(1, r3.data(), 1, 50'000'000'000LL);
    CHECK(ev.size() == 1 && ev[0].kind == EMF_BUFF_REMOVED && ev[0].cleansed == 1 && ev[0].replaced == 0 &&
          ev[0].remaining_ns == 0 && ev[0].total_ns == -1);
    std::vector<buffs::Row> r4 = {rows[2]};
    r4[0].entity = 777;  // a replace type given again: the game put a new buff entity in its place
    ev = d.step(1, r4.data(), 1, 50'000'000'000LL);
    CHECK(ev.size() == 2 && ev[0].kind == EMF_BUFF_REMOVED && ev[0].replaced == 1 && ev[0].cleansed == 0 &&
          ev[1].kind == EMF_BUFF_APPLIED && ev[1].buff_entity == 777);
    d.retain({2});
    CHECK(d.size() == 0);

    // a GUID for an id: the game hashes it back to the id
    for (uint32_t id : {kBleed, kWet, 0x5adb2644u, 1u, 0xFFFFFFFFu}) {
        const auto g = buffs::guid_with_id(id);
        CHECK(buffs::fnv1a(g.data(), 16) == id);
        CHECK(buffs::guid_with_id(id) == g);  // stable
    }

    // the programs: kiln.abilities' verifier takes them; the natives, their arguments and the answers' places
    const auto argc = [](uint32_t h) -> int { return h == buffs::kApplyBuffNative ? 5 : h == buffs::kRemoveBuffNative ? 3 : -1; };
    for (const bool apply : {true, false}) {
        const std::vector<uint8_t> b = apply ? buffs::apply_program() : buffs::remove_program();
        const kab::VerifyResult v = kab::verify_program(b.data(), b.size(), argc);
        CHECK(v.ok);
        if (!v.ok) std::printf("  verifier: %s\n", v.error.c_str());
        const uint8_t* g = apply ? buffs::kApplyProgram : buffs::kRemoveProgram;
        CHECK(v.id == buffs::fnv1a(g, 16) && std::memcmp(v.guid, g, 16) == 0 && v.unknown_natives.empty());
        int32_t rel = 0;
        uint32_t words = 0, vars = 0;
        std::memcpy(&rel, b.data() + 0x18, 4);
        std::memcpy(&words, b.data() + 0x1C, 4);
        std::vector<uint32_t> code(words);
        std::memcpy(code.data(), b.data() + 0x18 + rel, words * 4);
        std::memcpy(&vars, b.data() + 0x2C, 4);
        CHECK(vars == (apply ? 5u : 4u));
        if (apply) {
            // args[4] Duration, [3] 0, [2] 0 (no damage), [1] Target, [0] Buff; the native's answer into Applied
            CHECK((std::vector<uint32_t>(code.begin(), code.begin() + 8) ==
                   std::vector<uint32_t>{11, 0xFFFF0002u, 12, 12, 11, 0xFFFF0001u, 11, 0xFFFF0000u}));
            CHECK(code[8] == 19 && code[10] == buffs::kApplyBuffNative && code[11] == 24 && code[12] == 0xFFFF0003u &&
                  code[13] == 13 && code[14] == 24 && code[15] == 0xFFFF0004u);
        } else {
            CHECK((std::vector<uint32_t>(code.begin(), code.begin() + 6) ==
                   std::vector<uint32_t>{11, 0xFFFF0001u, 11, 0xFFFF0000u, 11, 0xFFFF0002u}));
            CHECK(code[6] == 19 && code[8] == buffs::kRemoveBuffNative && code[9] == 27 && code[10] == 13 && code[11] == 24 &&
                  code[12] == 0xFFFF0003u);
        }
        CHECK(code[code.size() - 2] == 29 && code.back() == 30);  // DSELF, HALT: nobody came
        size_t waits = 0;
        for (uint32_t w : code) waits += w == 28;
        CHECK(waits == buffs::kWaitSteps);
    }
    CHECK(std::memcmp(buffs::kApplyProgram, buffs::kRemoveProgram, 16) != 0);

    // judging a running request
    CHECK(buffs::judge(true, true, -1, 1) == buffs::Wait::Waiting && buffs::judge(true, true, 0, 1) == buffs::Wait::Waiting);
    CHECK(buffs::judge(true, true, 1, 2) == buffs::Wait::Answered && buffs::judge(true, false, -1, 2) == buffs::Wait::Gone);
    CHECK(buffs::judge(true, true, 0, buffs::kTimeoutSteps + 1) == buffs::Wait::TimedOut &&
          buffs::judge(true, true, 0, buffs::kTimeoutSteps) == buffs::Wait::Waiting);
    CHECK(buffs::judge(false, true, 1, 2) == buffs::Wait::Closed);
    CHECK(std::string(buffs::wait_why(buffs::Wait::TimedOut)).find("120 steps") != std::string::npos);

    // after a give
    uint32_t be = 0;
    CHECK(buffs::apply_check(true, rows, 3, kBleed, EMF_BUFF_DEBUFF, 0, 5, &be).empty() && be == 600);
    CHECK(buffs::apply_check(true, rows, 3, kWet, EMF_BUFF_DEBUFF, 0, 5, &be) == "the game did not give it");  // only a cleansed copy
    CHECK(buffs::apply_check(true, rows, 3, kRested, EMF_BUFF_RESTED, 1u << 7, 7, &be) ==
          "the type is never given to the target's faction (7)");
    CHECK(buffs::apply_check(false, rows, 3, kBleed, EMF_BUFF_DEBUFF, 0, 5, &be) == "the game's applyBuff refused it");
    CHECK(buffs::apply_check(true, rows, 3, kRested, EMF_BUFF_RESTED, 0, -1, &be) ==
          "the game did not give it (the target's faction could not be read)");
    CHECK(buffs::removal_target(rows, 3, kBleed) == 600 && buffs::removal_target(rows, 3, kWet) == 0 &&
          buffs::removal_target(rows, 3, kRested) == 0);

    // the view
    CHECK(std::fabs(view::heading_from_quat(0, 0, 0, 1)) < 1e-4f);
    const float s = std::sin(static_cast<float>(kPi) / 4), c = std::cos(static_cast<float>(kPi) / 4);  // 90 degrees about +y
    CHECK(std::fabs(view::heading_from_quat(0, s, 0, c) - 90.f) < 1e-3f);
    CHECK(std::fabs(view::heading_from_quat(0, -s, 0, c) - 270.f) < 1e-3f);
    CHECK(view::heading_from_quat(0, 0, 0, 0) == 0.f);
    CHECK(view::metres(static_cast<int64_t>(12.5 * 4294967296.0)) == 12.5);
    EmfPlayerView v{};
    v.size = sizeof v;
    v.flags = EMF_VIEW_CAMERA;
    v.x = 1;
    v.world_size = 10240;
    EmfPlayerView o{};
    o.size = 0;
    view::copy_view(v, &o);
    CHECK(o.size == sizeof o && o.x == 1 && o.world_size == 10240);
    EmfPlayerView small{};
    small.size = 16;  // an older struct: size, flags and x
    small.world_size = -5;
    view::copy_view(v, &small);
    CHECK(small.size == 16 && small.x == 1 && small.world_size == -5);
}

// ---------------------------------------------------------------------------------------------- the table --------
static void deliver_steps(int n) {
    for (int i = 0; i < n; ++i) {
        server_pre();
        server_post();
    }
}

static void test_server() {
    EmfApi t{};
    fill_v40(&t);
    fill_buffs(&t);
    fill_view(&t);
    const size_t first = offsetof(EmfApi, on_player) / sizeof(void*), n = sizeof(EmfApi) / sizeof(void*);
    const auto* slots = reinterpret_cast<void* const*>(&t);
    int empty = 0;
    for (size_t i = first; i < n; ++i)
        if (!slots[i] && i != offsetof(EmfApi, chat_tell_ex) / sizeof(void*) &&
            i != offsetof(EmfApi, chat_broadcast_ex) / sizeof(void*) && i != offsetof(EmfApi, on_hit) / sizeof(void*) &&
            i != offsetof(EmfApi, spawn_facing) / sizeof(void*))
            ++empty;
    CHECK(empty == 0);
    t_thread = 0;

    // ---- types: any world, the BuffRegistry's collection ----
    std::vector<EmfBuffType> types(t.buff_types(server(), nullptr, 0));
    CHECK(types.size() == 4);
    CHECK(t.buff_types(server(), types.data(), 4) == 4 && types[0].id == kBleed && types[3].id == kRested);
    EmfBuffType two[2]{};
    CHECK(t.buff_types(client(), two, 2) == 4 && two[1].id == kWet);  // the client world reads the same collection
    EmfBuffType bt{};
    CHECK(t.buff_type(server(), kPotion, &bt) && bt.slot == EMF_BUFF_BUFF && bt.default_ns == 300'000'000'000LL);
    CHECK(!t.buff_type(server(), 0x12345u, &bt) && !t.buff_type(server(), kPotion, nullptr) && !t.buff_type(nullptr, kPotion, &bt));
    CHECK(t.buff_types(reinterpret_cast<EmfWorld*>(0x9999), &bt, 1) == 0);
    collection_constant = 0x28;  // a game build whose BuffRegistry keeps the collection elsewhere: not read at +0x20
    CHECK(t.buff_types(server(), nullptr, 0) == 0 && !t.buff_type(server(), kPotion, &bt));
    collection_constant = -1;  // not resolved: EMF's +0x20
    CHECK(t.buff_types(server(), nullptr, 0) == 4);
    collection_constant = 0x20;
    make_collection(0);  // an empty or unreadable collection
    CHECK(t.buff_types(server(), nullptr, 0) == 0);
    make_collection(4);
    CHECK(unsupported_calls == 0);

    // ---- an entity's buffs: kiln.playerstate on the world's own thread ----
    buffs_on[7] = {pb(kBleed, 600, EMF_BUFF_DEBUFF, 2, 6'000'000'000LL, 52'000'000'000LL),
                   pb(kWet, 601, EMF_BUFF_DEBUFF, 5, 60'000'000'000LL, 0, 1)};
    EmfBuff b[4]{};
    CHECK(t.buffs_of(server(), 7, b, 4) == 2 && b[0].type == kBleed && b[0].index == 2 && b[0].remaining_ns == 2'000'000'000LL &&
          b[0].level == 3 && b[1].cleansed == 1 && b[1].remaining_ns == -1);
    CHECK(t.buffs_of(server(), 7, nullptr, 0) == 2 && t.buffs_of(server(), 99, b, 4) == 0 && t.buffs_of(server(), 0, b, 4) == 0);
    CHECK(t.buffs_of(server(), 7, b, 1) == 2);  // the number there is, the first written
    t_thread = 2;
    const int calls = buffs_calls;
    CHECK(t.buffs_of(server(), 7, b, 4) == 0 && buffs_calls == calls);  // not on the world's thread: nothing read
    t_thread = 0;

    // ---- removing: the game's removeBuff in kiln-emf's program, the answer after the step ----
    const int before_refusals = refusals;
    CHECK(!t.buff_remove(client(), 7, kBleed, &done_cb, &got));  // the server world only
    CHECK(!t.buff_remove(server(), 0, kBleed, &done_cb, &got) && !t.buff_remove(server(), 7, 0, &done_cb, &got));
    CHECK(!t.buff_remove(server(), 7, 0x12345u, &done_cb, &got));  // a type the collection does not have
    CHECK(refusals == before_refusals + 4 && got.ev.empty());
    active_mod = &mod_b;
    CHECK(t.buff_remove(server(), 7, kBleed, &done_cb, &got));
    active_mod = &mod_a;
    CHECK(spawned.empty() && got.ev.empty());
    server_pre();  // started: kiln-emf's remove program registered, the impact spawned at the target with its configs
    CHECK(spawned.size() == 1 && registered.count(buffs::fnv1a(buffs::kRemoveProgram, 16)) && verify_errors.empty());
    const Spawned& s = spawned[0];
    CHECK(std::memcmp(s.guid, buffs::kGenericImpact, 16) == 0 && s.at.x == 70 && s.at.z == 30);
    CHECK(s.flags == (KILN_SPAWN_TRANSIENT | KILN_SPAWN_ALLOW_UNKNOWN | KILN_SPAWN_NO_TRACK));
    const auto& impact = s.comps.at(fnv1a("keen::ecs::Impact"));
    uint32_t prog = 0;
    std::memcpy(&prog, impact.data() + 16, 4);
    CHECK(impact.size() == 24 && std::memcmp(impact.data(), buffs::kRemoveProgram, 16) == 0 && prog == buffs::fnv1a(buffs::kRemoveProgram, 16));
    const auto& hier = s.comps.at(fnv1a("keen::ecs::ImpactHierarchy"));
    uint32_t h[4];
    std::memcpy(h, hier.data(), 16);
    CHECK(hier.size() == 16 && h[0] == 7 && h[1] == 7 && h[2] == 0 && h[3] == 7);
    CHECK(s.comps.count(fnv1a("keen::ecs::Faction")) && s.comps.at(fnv1a("keen::ecs::Faction"))[0] == 5);
    const auto& ov = overrides[s.entity];
    CHECK(ov.size() == 2 && ov[0].match == KILN_OVERRIDE_BY_INDEX && ov[0].key == buffs::kRemoveVarBuff && ov[0].size == 16 &&
          buffs::fnv1a(ov[0].data, 16) == kBleed && ov[1].key == buffs::kRemoveVarTarget && ov[1].size == 4 && ov[1].data[0] == 7);
    server_post();  // the instance is not there yet: waiting
    CHECK(got.ev.empty() && destroyed.empty());
    game_creates_instance(s.entity, false, false);
    deliver_steps(1);
    CHECK(got.ev.empty());
    instances[next_impact - 1][buffs::kRemoveVarDone] = {1};
    instances[next_impact - 1][buffs::kRemoveVarWas] = {1};
    server_post();
    CHECK(got.ev.size() == 1 && got.ev[0].kind == EMF_BUFF_REMOVED && got.ev[0].target == 7 && got.ev[0].type == kBleed &&
          got.ev[0].buff_entity == 600 && got.ev[0].cleansed == 1 && got.ev[0].slot == EMF_BUFF_DEBUFF && got.ev[0].why == nullptr);
    CHECK(got.mods[0] == &mod_b && got.world_cb[0] && destroyed.count(s.entity));
    deliver_steps(3);
    CHECK(got.ev.size() == 1);  // once
    // not there: the game's answer
    CHECK(t.buff_remove(server(), 7, kPotion, &done_cb, &got));
    server_pre();
    game_creates_instance(spawned.back().entity, true, false);
    server_post();
    CHECK(got.ev.size() == 2 && got.ev[1].kind == EMF_BUFF_REFUSED && got.why[1] == "the target does not have it" &&
          got.ev[1].buff_entity == 0);
    // the impact goes away before its answer
    CHECK(t.buff_remove(server(), 7, kBleed, nullptr, nullptr));  // no callback is fine
    CHECK(t.buff_remove(server(), 7, kBleed, &done_cb, &got));
    server_pre();
    live_impacts.erase(spawned.back().entity);
    server_post();
    CHECK(got.ev.size() == 3 && got.ev[2].kind == EMF_BUFF_REFUSED && got.why[2].find("went away") != std::string::npos);
    deliver_steps(static_cast<int>(buffs::kTimeoutSteps) + 2);  // the one without a callback times out quietly
    // the game refuses the impact entity
    spawn_refuses = true;
    CHECK(t.buff_remove(server(), 7, kBleed, &done_cb, &got));
    deliver_steps(1);
    spawn_refuses = false;
    CHECK(got.ev.size() == 4 && got.ev[3].kind == EMF_BUFF_REFUSED && got.why[3].find("impact") != std::string::npos);
    // a target that is not in the world
    CHECK(t.buff_remove(server(), 99, kBleed, &done_cb, &got));
    deliver_steps(1);
    CHECK(got.ev.size() == 5 && got.ev[4].kind == EMF_BUFF_REFUSED && got.why[4] == "the target is not in the world");

    // ---- giving: applyBuff's answer and the target's buffs read back ----
    CHECK(!t.buff_apply(server(), 7, kPotion, -1, 0, 0, &done_cb, &got));  // a negative duration
    CHECK(!t.buff_apply(server(), 7, kPotion, 0, 0, 0x777, &done_cb, &got));  // an item's stats: not on Kiln
    CHECK(!t.buff_apply(client(), 7, kPotion, 0, 0, 0, &done_cb, &got) && !t.buff_apply(server(), 7, 0x4444u, 0, 0, 0, &done_cb, &got));
    CHECK(t.buff_apply(server(), 7, kPotion, 5'000'000'000LL, 8, 0, &done_cb, &got));
    server_pre();
    const Spawned& a = spawned.back();
    CHECK(registered.count(buffs::fnv1a(buffs::kApplyProgram, 16)) && verify_errors.empty());
    const auto& aimp = a.comps.at(fnv1a("keen::ecs::Impact"));
    CHECK(std::memcmp(aimp.data(), buffs::kApplyProgram, 16) == 0);
    std::memcpy(h, a.comps.at(fnv1a("keen::ecs::ImpactHierarchy")).data(), 16);
    CHECK(h[0] == 8 && h[1] == 8 && h[3] == 8);  // the source is the impact's root
    CHECK(!a.comps.count(fnv1a("keen::ecs::Faction")));  // entity 8's faction is not known here: none given
    const auto& aov = overrides[a.entity];
    int64_t dur = 0;
    CHECK(aov.size() == 3 && aov[2].key == buffs::kApplyVarDuration && aov[2].size == 8);
    if (aov.size() == 3) std::memcpy(&dur, aov[2].data, 8);
    CHECK(dur == 5'000'000'000LL && buffs::fnv1a(aov[0].data, 16) == kPotion);
    game_creates_instance(a.entity, true, true);
    buffs_on[7].push_back(pb(kPotion, 610, EMF_BUFF_BUFF, 0, 5'000'000'000LL, 55'000'000'000LL));
    server_post();
    CHECK(got.ev.size() == 6 && got.ev[5].kind == EMF_BUFF_APPLIED && got.ev[5].buff_entity == 610 &&
          got.ev[5].total_ns == 5'000'000'000LL && got.ev[5].remaining_ns == 5'000'000'000LL && got.ev[5].slot == EMF_BUFF_BUFF);
    // the type's default time, and a faction the type is never given to
    CHECK(t.buff_apply(server(), 7, kBleed, 0, 0, 0, &done_cb, &got));
    server_pre();
    std::memcpy(&dur, overrides[spawned.back().entity][2].data, 8);
    CHECK(dur == 0);
    faction_of_7 = 7;  // a player: bleed excludes bit 7
    game_creates_instance(spawned.back().entity, true, true);
    buffs_on[7][0].cleansed = 1;
    server_post();
    CHECK(got.ev.size() == 7 && got.ev[6].kind == EMF_BUFF_REFUSED && got.why[6] == "the type is never given to the target's faction (7)");
    faction_of_7 = 5;
    buffs_on[7][0].cleansed = 0;

    // ---- limits, closing ----
    int accepted = 0;
    for (int i = 0; i < 70; ++i) accepted += t.buff_remove(server(), 7, kBleed, &done_cb, &got);
    CHECK(accepted == static_cast<int>(buffs::kMaxPending));
    server_pre();  // 16 start a step
    const size_t started = spawned.size();
    buffs_world_stopping();
    CHECK(got.ev.size() == 7);  // not on the destroying thread: at the next server Post
    server_post();
    CHECK(got.ev.size() == 7 + buffs::kMaxPending);
    bool all_closed = true;
    for (size_t i = 7; i < got.ev.size(); ++i) all_closed &= got.ev[i].kind == EMF_BUFF_REFUSED && got.why[i].find("closed") != std::string::npos;
    CHECK(all_closed && spawned.size() == started);
    deliver_steps(2);
    CHECK(got.ev.size() == 7 + buffs::kMaxPending && spawned.size() == started);

    // the registry is not loaded yet: the request waits, then is refused
    registered.clear();
    registry_loaded = false;
    CHECK(t.buff_remove(server(), 7, kBleed, &done_cb, &got));
    deliver_steps(3);
    CHECK(got.ev.size() == 7 + buffs::kMaxPending);
    registry_loaded = true;
    deliver_steps(1);
    CHECK(registered.size() == 1 && got.ev.size() == 7 + buffs::kMaxPending);
    game_creates_instance(spawned.back().entity, true, true);
    server_post();
    CHECK(got.ev.size() == 8 + buffs::kMaxPending && got.ev.back().kind == EMF_BUFF_REMOVED);
    registered.clear();
    registry_loaded = false;
    CHECK(t.buff_remove(server(), 7, kBleed, &done_cb, &got));
    deliver_steps(static_cast<int>(buffs::kRegistryWaitSteps) + 2);
    registry_loaded = true;
    CHECK(got.ev.size() == 9 + buffs::kMaxPending && got.ev.back().kind == EMF_BUFF_REFUSED &&
          got.why.back().find("ImpactRegistry") != std::string::npos);
    // kiln.abilities without its instance hooks on this build: refused at once
    abilities_ready = false;
    CHECK(!t.buff_remove(server(), 7, kBleed, &done_cb, &got) && !t.buff_apply(server(), 7, kBleed, 0, 0, 0, &done_cb, &got));
    abilities_ready = true;
    // a call from another thread is queued, and runs in the server's next Pre
    t_thread = 2;
    CHECK(t.buff_remove(server(), 7, kBleed, &done_cb, &got));
    t_thread = 0;
    server_pre();
    game_creates_instance(spawned.back().entity, true, true);
    server_post();
    CHECK(got.ev.back().kind == EMF_BUFF_REMOVED);

    // ---- on_buff: every player's buffs against the step before ----
    CHECK(!t.on_buff(nullptr, nullptr));
    CHECK(t.on_buff(&watch_cb, nullptr));
    buffs_on[1] = {pb(kWet, 700, EMF_BUFF_DEBUFF, 0, 60'000'000'000LL, 70'000'000'000LL)};
    deliver_steps(1);
    CHECK(watched.ev.empty());  // the first sight says nothing
    buffs_on[1].push_back(pb(kPotion, 701, EMF_BUFF_BUFF, 0, 300'000'000'000LL, 330'000'000'000LL));
    deliver_steps(1);
    CHECK(watched.ev.size() == 1 && watched.ev[0].kind == EMF_BUFF_APPLIED && watched.ev[0].target == 1 && watched.ev[0].buff_entity == 701);
    buffs_on[1].erase(buffs_on[1].begin());
    deliver_steps(1);
    CHECK(watched.ev.size() == 2 && watched.ev[1].kind == EMF_BUFF_REMOVED && watched.ev[1].type == kWet);
    buffs_on[7].clear();  // a creature is not a player: no events for it
    deliver_steps(1);
    CHECK(watched.ev.size() == 2);
    int more = 1;
    for (int i = 0; i < 20; ++i) more += t.on_buff(&watch_cb, reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1)));
    CHECK(more == static_cast<int>(buffs::kMaxWatchers));
}

// ---------------------------------------------------------------------------------------------- texts / view -----
static void test_text_and_view(bool joined) {
    EmfApi t{};
    fill_buffs(&t);
    fill_view(&t);
    const uint32_t tag = buffs::guid_id(kNameGuid), desc = buffs::guid_id(kDescGuid);
    texts[tag] = "Bleeding";
    texts[desc] = "You lose health over time, a long description";
    char buf[64];
    // on the game's main thread: the game's text now
    t_thread = 1;
    CHECK(t.buff_text(kBleed, 0, buf, sizeof buf) && std::string(buf) == "Bleeding");
    char tiny[5];
    CHECK(t.buff_text(kBleed, 0, tiny, sizeof tiny) && std::string(tiny) == "Blee");  // cut to fit
    CHECK(!t.buff_text(kBleed, 2, buf, sizeof buf) && buf[0] == 0 && !t.buff_text(0x12345u, 0, buf, sizeof buf));
    // another thread (the host's server tick): the copy the main thread looked up; a new tag is looked up there first
    t_thread = joined ? 2 : 0;
    CHECK(t.buff_text(kBleed, 0, buf, sizeof buf) && std::string(buf) == "Bleeding");
    std::memset(buf, 'x', sizeof buf);
    CHECK(!t.buff_text(kBleed, 1, buf, sizeof buf) && buf[0] == 0);
    client_post();
    CHECK(t.buff_text(kBleed, 1, buf, sizeof buf) && std::string(buf).rfind("You lose", 0) == 0);
    language = 3;  // the player switched the game's language: the copies are looked up again at the next client step
    texts[tag] = "Blutung";
    CHECK(t.buff_text(kBleed, 0, buf, sizeof buf) && std::string(buf) == "Bleeding");  // until that step
    client_post();
    CHECK(t.buff_text(kBleed, 0, buf, sizeof buf) && std::string(buf) == "Blutung");
    texts.erase(desc);  // a tag the game has no text for: false, and not asked again and again
    language = 4;
    client_post();
    const int asked = text_calls;
    CHECK(!t.buff_text(kBleed, 1, buf, sizeof buf) && buf[0] == 0);
    client_post();
    CHECK(text_calls == asked);

    // player_view: the local player's transform and the camera
    const double m = 4294967296.0;
    client_tf = Transform{{static_cast<int64_t>(100.5 * m), static_cast<int64_t>(20 * m), static_cast<int64_t>(-30 * m)},
                          {0, std::sin(static_cast<float>(kPi) / 4), 0, std::cos(static_cast<float>(kPi) / 4)}, {1, 1, 1}, 0};
    server_tf = client_tf;
    server_tf.pos[0] = static_cast<int64_t>(101 * m);
    const float s = std::sin(static_cast<float>(kPi) / 2), c = std::cos(static_cast<float>(kPi) / 2);  // 180 degrees
    camera_q[0] = 0;
    camera_q[1] = s;
    camera_q[2] = 0;
    camera_q[3] = c;
    EmfPlayerView v{};
    v.size = sizeof v;
    t_thread = 1;
    CHECK(t.player_view(&v) && v.size == sizeof v && v.x == 100.5 && v.y == 20 && v.z == -30 && std::fabs(v.heading_deg - 90) < 1e-3f &&
          (v.flags & EMF_VIEW_CAMERA) && std::fabs(v.camera_yaw_deg - 180) < 1e-3f && v.world_size == view::kDefaultWorldSize);
    camera_valid = false;
    v = {};
    v.size = sizeof v;
    CHECK(t.player_view(&v) && !(v.flags & EMF_VIEW_CAMERA) && v.camera_yaw_deg == 0);
    camera_valid = true;
    CHECK(!t.player_view(nullptr));
    // the server's tick thread of a host: the server world's own copy of the local player
    t_thread = 0;
    v = {};
    v.size = sizeof v;
    if (!joined) CHECK(t.player_view(&v) && v.x == 101);
    // any other thread: the copy of the latest client step (kept while asked for)
    t_thread = 2;
    client_post();
    v = {};
    v.size = sizeof v;
    CHECK(t.player_view(&v) && v.x == 100.5 && (v.flags & EMF_VIEW_CAMERA));
    generation = 4;  // a new world: the old copy does not count
    CHECK(!t.player_view(&v));
    client_post();
    CHECK(t.player_view(&v));
    local = 0;  // the main menu, or before the player is known
    t_thread = 1;
    CHECK(!t.player_view(&v));
    client_post();
    t_thread = 2;
    CHECK(!t.player_view(&v));
    local = 1;
    t_thread = 0;
}

static void test_dedicated() {
    EmfApi t{};
    fill_buffs(&t);
    fill_view(&t);
    char buf[16];
    std::memset(buf, 'x', sizeof buf);
    t_thread = 0;
    CHECK(!t.buff_text(kBleed, 0, buf, sizeof buf) && buf[0] == 0);
    EmfPlayerView v{};
    v.size = sizeof v;
    CHECK(!t.player_view(&v));
    CHECK(t.buff_types(server(), nullptr, 0) == 4);  // the types and buffs are there on a server
    float e = 4;
    EmfGameMarker gm{};
    EmfCanvas cv{};
    cv.size = sizeof cv;
    EmfCanvasCmd cmd{};
    const int before = unsupported_calls;
    CHECK(!t.fog_at(1, 2, &e) && e == 4 && t.map_markers(&gm, 1) == 0 && !t.canvas_set("c", &cv) && !t.canvas_draw("c", &cmd, 1));
    CHECK(unsupported_calls == before + 4 && unsupported_fns.count("fog_at") && unsupported_fns.count("canvas_draw"));
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    setup();
    if (mode == "server") {
        kemf::g_side = KILN_SIDE_SERVER;
        session = KILN_SESSION_DEDICATED;
        local = 0;
        test_dedicated();
    } else if (mode == "client") {
        session = KILN_SESSION_REMOTE;
        worlds[0] = 0;  // a joined client has no server world
        EmfApi t{};
        fill_buffs(&t);
        t_thread = 1;
        EmfBuff b[2]{};
        buffs_on[7] = {pb(kBleed, 600, EMF_BUFF_DEBUFF, 0, 1, 2)};
        CHECK(t.buffs_of(client(), 7, b, 2) == 0);  // BuffContainer is the server's: none on a client world
        CHECK(t.buff_types(client(), nullptr, 0) == 4);
        CHECK(!t.buff_remove(client(), 7, kBleed, &done_cb, &got) && !t.buff_apply(client(), 7, kBleed, 0, 0, 0, &done_cb, &got));
        CHECK(t.on_buff(&watch_cb, nullptr));  // EMF: true; no server step comes here, so nothing is called
        test_text_and_view(true);
        CHECK(got.ev.empty() && watched.ev.empty());
    } else {
        test_model();
        test_server();
        test_text_and_view(false);
    }
    std::printf("buffs_test%s%s: %d checks, %d failures\n", mode.empty() ? "" : " ", mode.c_str(), checks, failures);
    return failures ? 1 : 0;
}

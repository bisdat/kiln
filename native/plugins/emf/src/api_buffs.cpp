// SPDX-License-Identifier: MIT
// The EmfApi table, v32 buffs (EMF 0.6.3), since 0.2.0-dev.31.13: the game's buff types, an entity's buffs, giving and
// removing a buff the game's way, and the players' buff events, on Kiln's services.
//
// EMF's contract (sdk/include/emf/emf_api.h; EMF src/core/api_buffs.cpp, buffs.cpp, buff_service.cpp):
//  - buff_types / buff_type: the world's buff types (any world kind), from the BuffRegistry's collection.
//  - buff_text: a type's name or description in the player's language (the game, not the dedicated server).
//  - buffs_of: an entity's active buffs, in slot order, where no program runs.
//  - buff_apply / buff_remove: queued for the server world's next step, through the game's own buff code; `done` once,
//    after that step on the server world's thread: APPLIED / REMOVED, or REFUSED with `why`. False when refused at
//    once (logged): not the server world, an unknown type, 64 waiting, not available on this game build.
//  - on_buff: after each server step, every change of a player's buffs (a comparison with the step before; the first
//    sight says nothing). At most 16 callbacks.
//
// On Kiln:
//  - types: the world's BuffRegistry singleton (core singleton map) -> the collection at +0x20 (checked against sigdb
//    gamedata/buffreg.collection, kiln.gamedata's capture) -> the 184-byte records, read at EMF's offsets
//    (buffs_model.cpp). The collection is the game's static data, or kiln.gamedata's copy once a buff clone exists
//    (whose old headers are never freed), so it is read from any thread, as EMF does. Clones are listed with the game's
//    buffs.
//  - texts: kiln.content text() (the game's own text lookup, the client's main thread). On another thread the answer is
//    the copy kiln-emf keeps of what the main thread looked up; a tag not looked up yet is looked up at the next client
//    step (that first call answers false). A language change empties the copy.
//  - an entity's buffs: kiln.playerstate buffs() (BuffContainer and each buff entity's BuffNetworkData, any entity) on
//    the world's own thread; the time left against the world's ECS time (EMF's time base). BuffContainer is a server
//    component, so a client world answers 0 (EMF reads the same component there).
//  - giving and removing: the game's applyBuff and removeBuff natives (the calls the game's own Impact programs make:
//    the buff entity, its program, the HUD icon, BuffNetworkData for clients, saving; a removal takes the buff entity
//    and its effect away) in two small Impact programs kiln-emf registers with kiln.abilities, as Kiln.Abilities'
//    ApplyBuff and RemoveBuff do, on the game's generic impact entity spawned through kiln.spawn at the target, with
//    the source (or the target) as the impact's root and its faction. The program keeps the native's answer and a done
//    flag readable for 30 steps; Kiln reads them after the step and destroys the impact. A give is checked against the
//    target's buffs read back (EMF's check: the game says "ok" without giving for an excluded faction).
//    No engine call or write in kiln-emf itself: no new hook, no new signature.
//  - on_buff: kiln.players' in-game players, their kiln.playerstate buffs compared with the step before (EMF's diff).
// Differences (docs/kiln/emf-parity.md, v32): an `item` (a food buff's stats from the eaten item) is refused, since the
// applyBuff native takes no item; buff_text off the main thread answers from the copy.
#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <unordered_map>
#include <unordered_set>

#include "buffs_model.h"
#include "emf_internal.h"
#include "services/kiln_abilities.h"
#include "services/kiln_playerstate.h"

namespace kemf {

void view_client_step(EmfWorld* client);  // api_view.cpp: the local player's copy for player_view

namespace {

constexpr uint32_t kMaxRows = 40;  // BuffContainer has 38 slots (food 4, buffs 16, debuffs 16, rested, terrain)

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

// ---- services, looked up at the first call (all are published before any mod loads) ----
const KilnPlayerStateApi* playerstate() {
    static const KilnPlayerStateApi* const p = []() -> const KilnPlayerStateApi* {
        auto* s = static_cast<const KilnPlayerStateApi*>(g_api->service(kstr(KILN_PLAYERSTATE_SERVICE), 1));
        return s && s->header.size >= offsetof(KilnPlayerStateApi, buffs) + sizeof(void*) && s->buffs ? s : nullptr;
    }();
    return p;
}

const KilnAbilitiesApi* abilities() {
    static const KilnAbilitiesApi* const a = []() -> const KilnAbilitiesApi* {
        auto* s = static_cast<const KilnAbilitiesApi*>(g_api->service(kstr(KILN_ABILITIES_SERVICE), 1));
        const bool ok = s && s->header.size >= offsetof(KilnAbilitiesApi, program_refresh) && s->status && s->instance_read &&
                        s->entity_overrides && s->program_info && s->program_register;
        return ok ? s : nullptr;
    }();
    return a;
}

// Giving and removing need kiln.abilities' natives and programs on this build (the capability abilities.buffs) and
// kiln.spawn's spawn-time components. Empty = usable.
std::string giving_unavailable() {
    const KilnAbilitiesApi* a = abilities();
    if (!a) return "kiln.abilities is not available";
    KilnAbilitiesStatus st{};
    st.size = sizeof st;
    if (a->status(&st) != KILN_OK || !st.natives || !st.programs)
        return "kiln.abilities has no natives or programs on this game build (capability abilities.buffs)";
    if (!S.spawn || S.spawn->header.size < offsetof(KilnSpawnApi, capabilities) + sizeof(void*) || !S.spawn->capabilities ||
        !(S.spawn->capabilities() & KILN_SPAWN_CAP_COMPONENTS) || !S.spawn->entity_info || !S.spawn->destroy)
        return "kiln.spawn spawn-time components are unavailable";
    if (!S.world || !S.world->component) return "kiln.world is not available";
    return {};
}

// ---- the buff collection ----
struct Coll {
    uintptr_t records = 0;
    uint32_t count = 0;
};

bool collection(EmfWorld* w, Coll* out) {
    if (!w || world_kind_of(w) > EMF_WORLD_CLIENT) return false;
    int64_t at = 0;
    if (g_api->constant && g_api->constant(kstr("gamedata/buffreg.collection"), &at) == KILN_OK && at != buffs::kCollectionOffset) {
        static std::once_flag said;
        std::call_once(said, [at] {
            logf(KILN_LOG_WARN, "buffs: the BuffRegistry keeps its collection at +0x%llx on this game build, kiln-emf reads "
                 "+0x20: buff_types and buff_type answer 0 / false", static_cast<unsigned long long>(at));
        });
        return false;
    }
    const uintptr_t reg = g_api->singleton(reinterpret_cast<uintptr_t>(w), kstr("BuffRegistry"));
    uintptr_t blob = 0;
    if (!reg || !seh_copy(&blob, reinterpret_cast<const void*>(reg + buffs::kCollectionOffset), 8) || !blob) return false;
    int32_t rel = 0;
    uint32_t count = 0;
    if (!seh_copy(&rel, reinterpret_cast<const void*>(blob + buffs::kBuffsArray), 4) ||
        !seh_copy(&count, reinterpret_cast<const void*>(blob + buffs::kBuffsArray + 4), 4))
        return false;
    if (!count || count > buffs::kMaxTypes || !rel) return false;
    const uintptr_t records = blob + buffs::kBuffsArray + static_cast<intptr_t>(rel);
    uint8_t probe = 0;
    if (!seh_copy(&probe, reinterpret_cast<const void*>(records), 1) ||
        !seh_copy(&probe, reinterpret_cast<const void*>(records + static_cast<uintptr_t>(count) * buffs::kTypeRecordSize - 1), 1))
        return false;
    out->records = records;
    out->count = count;
    return true;
}

bool record(const Coll& c, uint32_t i, EmfBuffType* out) {
    uint8_t r[buffs::kTypeRecordSize];
    return i < c.count && seh_copy(r, reinterpret_cast<const void*>(c.records + static_cast<uintptr_t>(i) * buffs::kTypeRecordSize), sizeof r) &&
           buffs::type_of(r, out);
}

bool find_type(const Coll& c, uint32_t id, EmfBuffType* out) {
    if (!id) return false;
    for (uint32_t i = 0; i < c.count; ++i) {
        uint32_t rid = 0;
        if (!seh_copy(&rid, reinterpret_cast<const void*>(c.records + static_cast<uintptr_t>(i) * buffs::kTypeRecordSize), 4))
            return false;
        if (rid == id) return record(c, i, out);
    }
    return false;
}

// ---- an entity's buffs (kiln.playerstate, the calling world's thread) ----
// *found: the entity has a BuffContainer here (false: none, or not readable on this thread)
uint32_t read_rows(uint32_t entity, buffs::Row* rows, uint32_t cap, bool* found = nullptr) {
    if (found) *found = false;
    const KilnPlayerStateApi* ps = playerstate();
    if (!ps || !entity) return 0;
    KilnPlayerBuff raw[kMaxRows]{};
    uint32_t n = 0;
    const KilnResult rc = ps->buffs(entity, raw, kMaxRows, &n);
    if (rc != KILN_OK && rc != KILN_ERR_BUFFER_TOO_SMALL) return 0;
    if (found) *found = true;
    n = (std::min)({n, kMaxRows, cap});
    for (uint32_t i = 0; i < n; ++i) {
        rows[i].type = raw[i].id;
        rows[i].entity = raw[i].entity;
        rows[i].slot = static_cast<uint8_t>(raw[i].slot);
        rows[i].index = static_cast<uint8_t>(raw[i].index);
        rows[i].cleansed = raw[i].cleansed ? 1 : 0;
        rows[i].life_ns = raw[i].life_ns;
        rows[i].end_ns = raw[i].end_ns;
        rows[i].level = raw[i].level;
    }
    return n;
}

int faction_of(EmfWorld* w, uint32_t entity) {
    uint64_t size = 0;
    const void* f = component_of(w, fnv1a("keen::ecs::Faction"), entity, &size);
    uint8_t v = 0;
    return f && size >= 1 && seh_copy(&v, f, 1) ? v : -1;
}

// ---- texts ----
struct Texts {
    std::mutex m;
    std::unordered_map<uint32_t, std::string> copy;  // tag -> text ("" = the game has none)
    std::unordered_set<uint32_t> asked;              // tags to look up at the next client step
    uint32_t language = 0;
};
Texts& texts() {
    static Texts t;
    return t;
}

bool content_texts() {
    return S.content && S.content->header.size >= offsetof(KilnContentApi, text_info) + sizeof(void*) && S.content->text &&
           S.content->text_info;
}

uint32_t text_language() {
    KilnContentTextInfo info{};
    info.size = sizeof info;
    return S.content->text_info(&info) == KILN_OK ? info.language : 0;
}

// On the client's main thread: the game's text now (and into the copy). False: the game has none, or not here yet.
bool look_up(uint32_t tag, std::string* out) {
    std::vector<char> buf(1024);
    size_t len = 0;
    KilnResult rc = S.content->text(tag, buf.data(), buf.size(), &len);
    if (rc == KILN_ERR_BUFFER_TOO_SMALL && len < (1u << 20)) {
        buf.resize(len + 1);
        rc = S.content->text(tag, buf.data(), buf.size(), &len);
    }
    Texts& t = texts();
    if (rc == KILN_ERR_NOT_FOUND) {
        std::lock_guard lock(t.m);
        t.copy[tag].clear();
        t.asked.erase(tag);
        return false;
    }
    if (rc != KILN_OK) return false;  // not here or not yet (the English table loading): asked again later
    out->assign(buf.data(), (std::min)(len, buf.size() - 1));
    std::lock_guard lock(t.m);
    t.copy[tag] = *out;
    t.asked.erase(tag);
    return !out->empty();
}

void check_language() {
    Texts& t = texts();
    const uint32_t now = text_language();
    std::lock_guard lock(t.m);
    if (now == t.language) return;
    t.language = now;
    for (auto& [tag, text] : t.copy) t.asked.insert(tag);  // the same tags again, in the new language
    t.copy.clear();
}

bool text_of(uint32_t tag, std::string* out) {
    if (!content_texts()) return false;
    if (S.client && S.client->on_client_thread()) {
        check_language();
        return look_up(tag, out);
    }
    Texts& t = texts();
    std::lock_guard lock(t.m);
    const auto it = t.copy.find(tag);
    if (it != t.copy.end()) {
        *out = it->second;
        return !out->empty();
    }
    if (t.asked.size() < 1024) t.asked.insert(tag);
    return false;
}

void texts_client_step() {
    if (!content_texts()) return;
    check_language();
    std::vector<uint32_t> todo;
    {
        Texts& t = texts();
        std::lock_guard lock(t.m);
        for (uint32_t tag : t.asked) {
            if (todo.size() >= 64) break;
            todo.push_back(tag);
        }
    }
    std::string s;
    for (uint32_t tag : todo) look_up(tag, &s);
}

// ---- gives and removals ----
struct Job {
    bool remove = false;
    Mod* mod = nullptr;
    EmfWorld* world = nullptr;
    uint32_t target = 0, type = 0, source = 0;
    int64_t duration = 0;
    EmfBuffCallback done = nullptr;
    void* user = nullptr;
    std::array<uint8_t, 16> guid{};  // a BuffTypeReference whose hash is `type`
    uint64_t queued = 0, started = 0;
    EmfBuffType t{};
    KilnEntity impact = 0;
    uint32_t before = 0;  // remove: the instance it takes
    EmfBuffEvent ev{};
    std::string why;
};

struct Jobs {
    std::mutex m;
    std::deque<Job> waiting;
    std::vector<Job> running, finished;
    uint64_t step = 0;  // the server world's Pre count
};
Jobs& jobs() {
    static Jobs j;
    return j;
}

std::array<uint8_t, 16> guid_for(uint32_t id) {
    static std::mutex m;
    static std::unordered_map<uint32_t, std::array<uint8_t, 16>> cache;
    {
        std::lock_guard lock(m);
        if (const auto it = cache.find(id); it != cache.end()) return it->second;
    }
    const auto g = buffs::guid_with_id(id);
    std::lock_guard lock(m);
    if (cache.size() < 4096) cache[id] = g;
    return g;
}

void finish(Job& j, uint32_t kind, std::string why) {
    EmfBuffEvent& e = j.ev;
    e = EmfBuffEvent{};
    e.size = sizeof e;
    e.kind = kind;
    e.target = j.target;
    e.type = j.type;
    e.slot = j.t.slot;
    e.total_ns = e.remaining_ns = -1;
    j.why = std::move(why);
}

bool request(Mod* m, const char* fn, bool remove, EmfWorld* w, uint32_t target, uint32_t type, int64_t duration,
             uint32_t source, uint32_t item, EmfBuffCallback done, void* user) {
    note_call(fn);
    auto no = [&](const std::string& why) {
        refuse(m, fn, why.c_str(), true);
        return false;
    };
    if (!w || reinterpret_cast<uintptr_t>(w) != g_api->world(KILN_WORLD_SERVER))
        return no("buffs are given and removed on the server world only (the host's game or the dedicated server)");
    if (!target) return no("entity 0");
    if (!type) return no("buff type 0");
    if (!remove && duration < 0) return no("a negative duration");
    if (!remove && item)
        return no("giving a buff with an item's stats (item " + std::to_string(item) +
                  ") is not available on Kiln yet: the game's applyBuff takes no item (pass 0)");
    if (const std::string off = giving_unavailable(); !off.empty()) {
        unsupported(m, fn, off.c_str());
        return false;
    }
    Coll c;
    EmfBuffType t{};
    if (collection(w, &c) && !find_type(c, type, &t)) return no("unknown buff type " + hex(type));
    Job j;
    j.remove = remove;
    j.mod = m;
    j.world = w;
    j.target = target;
    j.type = type;
    j.duration = duration;
    j.source = source;
    j.done = done;
    j.user = user;
    j.guid = guid_for(type);
    Jobs& q = jobs();
    std::lock_guard lock(q.m);
    if (q.waiting.size() + q.running.size() >= buffs::kMaxPending) return no("64 buff requests are already waiting");
    j.queued = q.step;
    q.waiting.push_back(std::move(j));
    return true;
}

enum class Start { Running, Finished, Retry };

// The server world's Pre: the program in this world's ImpactRegistry, the impact entity at the target with its values.
Start start(Job& j, uint64_t step) {
    const KilnAbilitiesApi* a = abilities();
    if (reinterpret_cast<uintptr_t>(j.world) != g_api->world(KILN_WORLD_SERVER))
        return finish(j, EMF_BUFF_REFUSED, "the world it was asked for has ended"), Start::Finished;
    if (const std::string off = giving_unavailable(); !off.empty()) return finish(j, EMF_BUFF_REFUSED, off), Start::Finished;
    Coll c;
    if (!collection(j.world, &c)) return finish(j, EMF_BUFF_REFUSED, "the world's buff types cannot be read"), Start::Finished;
    if (!find_type(c, j.type, &j.t)) return finish(j, EMF_BUFF_REFUSED, "unknown buff type " + hex(j.type)), Start::Finished;
    // Kiln keeps registered programs across worlds and puts them back into each new world's registry; asked of the live
    // registry every time (a world change empties it)
    const uint8_t* guid = j.remove ? buffs::kRemoveProgram : buffs::kApplyProgram;
    const uint32_t pid = buffs::fnv1a(guid, 16);
    KilnImpactProgramInfo info{};
    info.size = sizeof info;
    if (a->program_info(pid, &info) != KILN_OK) {
        const std::vector<uint8_t> blob = j.remove ? buffs::remove_program() : buffs::apply_program();
        char err[256] = {};
        uint32_t id = 0;
        const KilnResult rc = a->program_register(g_owner, blob.data(), blob.size(), &id, err, sizeof err);
        if (rc == KILN_ERR_UNAVAILABLE) {
            if (step - j.queued <= buffs::kRegistryWaitSteps) return Start::Retry;
            return finish(j, EMF_BUFF_REFUSED, "the game's ImpactRegistry is not loaded yet"), Start::Finished;
        }
        if (rc != KILN_OK) {
            logf(KILN_LOG_WARN, "buffs: kiln.abilities refused kiln-emf's %s program (%d): %s", j.remove ? "remove" : "give", rc, err);
            return finish(j, EMF_BUFF_REFUSED, std::string("kiln.abilities refused kiln-emf's buff program: ") + err), Start::Finished;
        }
    }
    KilnEntitySnapshot snap{};
    snap.size = sizeof snap;
    if (S.spawn->entity_info(j.target, &snap) != KILN_OK)
        return finish(j, EMF_BUFF_REFUSED, "the target is not in the world"), Start::Finished;
    if (j.remove) {
        buffs::Row rows[kMaxRows];
        const uint32_t n = read_rows(j.target, rows, kMaxRows);
        j.before = buffs::removal_target(rows, n, j.type);
    }
    // the impact: Impact {program GUID, program id, impact id 0}, ImpactHierarchy {parent, root, weapon, permission}
    // = the source (else the target), and the root's Faction (Kiln.Abilities' RunProgram does the same)
    const uint32_t root = j.source ? j.source : j.target;
    uint8_t impact[24] = {};
    std::memcpy(impact, guid, 16);
    std::memcpy(impact + 16, &pid, 4);
    const uint32_t hier[4] = {root, root, 0, root};
    const int faction = faction_of(j.world, root);
    const uint8_t fbyte = static_cast<uint8_t>(faction < 0 ? 0 : faction);
    KilnSpawnComponent comps[3] = {{fnv1a("keen::ecs::Impact"), 0, impact, sizeof impact},
                                   {fnv1a("keen::ecs::ImpactHierarchy"), 0, hier, sizeof hier},
                                   {fnv1a("keen::ecs::Faction"), 0, &fbyte, 1}};
    KilnSpawnRequest req{};
    req.size = sizeof req;
    std::memcpy(req.template_guid.b, buffs::kGenericImpact, 16);
    req.position = snap.position;
    req.flags = KILN_SPAWN_TRANSIENT | KILN_SPAWN_ALLOW_UNKNOWN | KILN_SPAWN_NO_TRACK;
    req.tag = kstr("emf");
    req.origin = kstr("emf:buffs");
    req.components = comps;
    req.component_count = faction < 0 ? 2 : 3;
    KilnEntity e = 0;
    const KilnResult rc = S.spawn->spawn(&req, &e);
    if (rc != KILN_OK || !e)
        return finish(j, EMF_BUFF_REFUSED, "the game refused the request's impact entity (kiln.spawn " + std::to_string(rc) + ")"),
               Start::Finished;
    KilnConfigOverride ov[3]{};
    auto set = [&](KilnConfigOverride& o, uint32_t index, const void* data, uint32_t size) {
        o.match = KILN_OVERRIDE_BY_INDEX;
        o.key = index;
        o.size = size;
        std::memcpy(o.data, data, size);
    };
    set(ov[0], j.remove ? buffs::kRemoveVarBuff : buffs::kApplyVarBuff, j.guid.data(), 16);
    set(ov[1], j.remove ? buffs::kRemoveVarTarget : buffs::kApplyVarTarget, &j.target, 4);
    if (!j.remove) set(ov[2], buffs::kApplyVarDuration, &j.duration, 8);  // 0 = the type's default
    if (a->entity_overrides(e, ov, j.remove ? 2 : 3, 50) != KILN_OK) {
        S.spawn->destroy(e);
        return finish(j, EMF_BUFF_REFUSED, "kiln.abilities did not take the request's values"), Start::Finished;
    }
    j.impact = e;
    j.started = step;
    return Start::Running;
}

// After the step: what the program says now.
bool poll(Job& j, EmfWorld* w, uint64_t step) {
    const KilnAbilitiesApi* a = abilities();
    uint32_t size = 0;
    const void* c = S.world->component(j.impact, fnv1a("keen::ecs::Impact"), &size);
    uint32_t impact_id = 0;
    const bool exists = c && size >= 24 && seh_copy(&impact_id, static_cast<const uint8_t*>(c) + 20, 4);
    int done = -1;
    uint8_t answer = 0;
    if (exists && impact_id && a) {
        uint8_t d = 0;
        uint32_t len = 0;
        if (a->instance_read(impact_id, j.remove ? buffs::kRemoveVarDone : buffs::kApplyVarDone, &d, 1, &len) == KILN_OK) {
            done = d ? 1 : 0;
            // done only counts with the answer beside it: a read that fails is asked again at the next step
            if (d && a->instance_read(impact_id, j.remove ? buffs::kRemoveVarWas : buffs::kApplyVarApplied, &answer, 1, &len) != KILN_OK)
                done = -1;
        }
    }
    const buffs::Wait st = buffs::judge(true, exists, done, step - j.started);
    if (st == buffs::Wait::Waiting) return false;
    if (exists) S.spawn->destroy(j.impact);
    if (st != buffs::Wait::Answered) return finish(j, EMF_BUFF_REFUSED, buffs::wait_why(st)), true;
    if (j.remove) {
        if (!answer) return finish(j, EMF_BUFF_REFUSED, "the target does not have it"), true;
        finish(j, EMF_BUFF_REMOVED, {});
        j.ev.buff_entity = j.before;
        j.ev.cleansed = 1;
        return true;
    }
    buffs::Row rows[kMaxRows];
    const uint32_t n = read_rows(j.target, rows, kMaxRows);
    uint32_t be = 0;
    std::string why = buffs::apply_check(answer != 0, rows, n, j.type, j.t.slot, j.t.faction_exclude, faction_of(w, j.target), &be);
    if (!why.empty()) return finish(j, EMF_BUFF_REFUSED, std::move(why)), true;
    finish(j, EMF_BUFF_APPLIED, {});
    j.ev.buff_entity = be;
    j.ev.total_ns = j.ev.remaining_ns = j.duration ? j.duration : j.t.default_ns;
    return true;
}

struct DoneCall {
    EmfBuffCallback cb;
    EmfWorld* w;
    const EmfBuffEvent* ev;
    void* user;
};
void done_thunk(void* p) {
    auto* c = static_cast<DoneCall*>(p);
    c->cb(c->w, c->ev, c->user);
}

void deliver(EmfWorld* w, std::vector<Job>& done) {
    for (Job& j : done) {
        if (!j.done) continue;
        EmfBuffEvent ev = j.ev;
        ev.why = ev.kind == EMF_BUFF_REFUSED ? j.why.c_str() : nullptr;
        DoneCall c{j.done, w, &ev, j.user};
        ContextScope scope(j.mod, w, true);
        const uint64_t t0 = qpc_ns();
        guarded(j.mod, j.remove ? "a buff_remove callback" : "a buff_apply callback", &done_thunk, &c);
        account(j.mod, qpc_ns() - t0);
    }
    done.clear();
}

// ---- on_buff ----
struct Watcher {
    EmfBuffCallback cb;
    void* user;
    Mod* mod;
    bool on;
};
struct Watch {
    std::mutex m;
    std::vector<Watcher> list;
    buffs::Diff diff;  // the server world's thread only
    std::vector<uint32_t> players;
    uint64_t players_ms = 0;
    bool listed = false;
    std::atomic<bool> reset{false};  // the server world stopped: the next world's players are new
};
Watch& watch() {
    static Watch w;
    return w;
}

struct WatchCall {
    EmfBuffCallback cb;
    EmfWorld* w;
    const EmfBuffEvent* ev;
    void* user;
};
void watch_thunk(void* p) {
    auto* c = static_cast<WatchCall*>(p);
    c->cb(c->w, c->ev, c->user);
}

void watch_step(EmfWorld* w) {
    Watch& x = watch();
    std::vector<Watcher> on;
    {
        std::lock_guard lock(x.m);
        for (const Watcher& v : x.list)
            if (v.on) on.push_back(v);
    }
    if (x.reset.exchange(false)) {
        x.diff.reset();
        x.listed = false;
    }
    if (on.empty()) {  // nobody listens: a later callback starts from a first sight, not a stale copy
        if (x.diff.size()) x.diff.reset();
        x.listed = false;
        return;
    }
    const uint64_t now = now_ms();
    if (S.players && S.players->list && (!x.listed || now - x.players_ms >= 1000)) {
        x.listed = true;
        x.players_ms = now;
        KilnPlayer p[KILN_PLAYERS_MAX_SLOTS];
        uint32_t n = 0;
        const KilnResult rc = S.players->list(0, p, KILN_PLAYERS_MAX_SLOTS, &n);
        x.players.clear();
        if (rc == KILN_OK || rc == KILN_ERR_BUFFER_TOO_SMALL)
            for (uint32_t i = 0; i < n && i < KILN_PLAYERS_MAX_SLOTS; ++i)
                if (p[i].state == KILN_PLAYER_IN_GAME && p[i].entity) x.players.push_back(p[i].entity);
        x.diff.retain(x.players);
    }
    const int64_t ecs = api_table()->ecs_time_ns(w);
    std::vector<EmfBuffEvent> events;
    for (uint32_t p : x.players) {
        buffs::Row rows[kMaxRows];
        bool found = false;
        const uint32_t n = read_rows(p, rows, kMaxRows, &found);
        if (!found) {  // no BuffContainer (yet): a first sight later
            x.diff.forget(p);
            continue;
        }
        for (const EmfBuffEvent& e : x.diff.step(p, rows, n, ecs)) events.push_back(e);
    }
    for (const EmfBuffEvent& e : events)
        for (Watcher& v : on) {
            if (!v.on) continue;
            WatchCall c{v.cb, w, &e, v.user};
            ContextScope scope(v.mod, w, true);
            const uint64_t t0 = qpc_ns();
            const bool ok = guarded(v.mod, "an on_buff callback", &watch_thunk, &c);
            account(v.mod, qpc_ns() - t0);
            if (ok) continue;
            v.on = false;  // a faulting callback is switched off
            std::lock_guard lock(x.m);
            for (Watcher& y : x.list)
                if (y.cb == v.cb && y.user == v.user) y.on = false;
        }
}

// ---------------------------------------------------------------------------------------------- the API ---------
uint32_t api_buff_types(EmfWorld* w, EmfBuffType* out, uint32_t cap) {
    note_call("buff_types");
    Coll c;
    if (!collection(w, &c)) return 0;  // "0 when the world's buff registry cannot be read"
    uint32_t n = 0;
    for (uint32_t i = 0; i < c.count; ++i) {
        EmfBuffType t{};
        if (!record(c, i, &t)) break;
        if (out && n < cap) out[n] = t;
        ++n;
    }
    return n;
}

bool api_buff_type(EmfWorld* w, uint32_t id, EmfBuffType* out) {
    note_call("buff_type");
    Coll c;
    EmfBuffType t{};
    if (!out || !collection(w, &c) || !find_type(c, id, &t)) return false;
    *out = t;
    return true;
}

bool api_buff_text(uint32_t id, uint32_t which, char* buf, uint32_t cap) {
    note_call("buff_text");
    if (buf && cap) buf[0] = 0;
    if (!buf || cap < 2 || which > 1 || g_side == KILN_SIDE_SERVER) return false;  // the game only
    EmfWorld* w = reinterpret_cast<EmfWorld*>(g_api->world(KILN_WORLD_CLIENT));
    if (!w) w = reinterpret_cast<EmfWorld*>(g_api->world(KILN_WORLD_SERVER));
    Coll c;
    EmfBuffType t{};
    if (!collection(w, &c) || !find_type(c, id, &t)) return false;
    const uint32_t tag = which == 0 ? t.name_tag : t.desc_tag;
    std::string text;
    if (!tag || !text_of(tag, &text) || text.empty()) return false;
    size_t n = (std::min)(text.size(), static_cast<size_t>(cap - 1));
    while (n > 0 && n < text.size() && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;  // whole UTF-8 characters
    std::memcpy(buf, text.data(), n);
    buf[n] = 0;
    return true;
}

uint32_t api_buffs_of(EmfWorld* w, uint32_t entity, EmfBuff* out, uint32_t cap) {
    note_call("buffs_of");
    if (!w || !entity || !on_thread_of(w)) return 0;  // where no program runs: the world's own thread
    if (!playerstate()) {
        unsupported(caller_mod(_ReturnAddress()), "buffs_of", "kiln.playerstate is not available");
        return 0;
    }
    buffs::Row rows[kMaxRows];
    const uint32_t n = read_rows(entity, rows, kMaxRows);
    return buffs::fill_buffs(rows, n, api_table()->ecs_time_ns(w), out, cap);
}

bool api_buff_apply(EmfWorld* w, uint32_t target, uint32_t type, int64_t duration_ns, uint32_t source, uint32_t item,
                    EmfBuffCallback done, void* user) {
    return request(caller_mod(_ReturnAddress()), "buff_apply", false, w, target, type, duration_ns, source, item, done, user);
}

bool api_buff_remove(EmfWorld* w, uint32_t target, uint32_t type, EmfBuffCallback done, void* user) {
    return request(caller_mod(_ReturnAddress()), "buff_remove", true, w, target, type, 0, 0, 0, done, user);
}

bool api_on_buff(EmfBuffCallback cb, void* user) {
    Mod* m = cb ? mod_of_address(reinterpret_cast<const void*>(cb)) : nullptr;
    if (!m) m = caller_mod(_ReturnAddress());
    note_call("on_buff");
    if (!cb) return refuse(m, "on_buff", "no callback"), false;
    Watch& x = watch();
    std::lock_guard lock(x.m);
    if (x.list.size() >= buffs::kMaxWatchers) return refuse(m, "on_buff", "16 callbacks already", true), false;
    x.list.push_back({cb, user, m, true});
    return true;
}

}  // namespace

void buffs_server_pre() {
    Jobs& q = jobs();
    std::vector<Job> work;
    uint64_t step;
    {
        std::lock_guard lock(q.m);
        step = ++q.step;
        while (!q.waiting.empty() && work.size() < buffs::kStartsPerStep) {
            work.push_back(std::move(q.waiting.front()));
            q.waiting.pop_front();
        }
    }
    if (work.empty()) return;
    std::vector<Job> retry, running, finished;
    for (Job& j : work) {
        switch (start(j, step)) {
            case Start::Running: running.push_back(std::move(j)); break;
            case Start::Retry: retry.push_back(std::move(j)); break;
            case Start::Finished: finished.push_back(std::move(j)); break;
        }
    }
    std::lock_guard lock(q.m);
    for (auto it = retry.rbegin(); it != retry.rend(); ++it) q.waiting.push_front(std::move(*it));
    for (Job& j : running) q.running.push_back(std::move(j));
    for (Job& j : finished) q.finished.push_back(std::move(j));
}

void buffs_after_step(EmfWorld* w, uint8_t kind) {
    if (!w) return;
    if (kind == EMF_WORLD_CLIENT) {
        texts_client_step();
        view_client_step(w);
        return;
    }
    if (kind != EMF_WORLD_SERVER) return;
    Jobs& q = jobs();
    std::vector<Job> running, done;
    uint64_t step;
    {
        std::lock_guard lock(q.m);
        step = q.step;
        running.swap(q.running);
        done.swap(q.finished);
    }
    std::vector<Job> still;
    for (Job& j : running) {
        if (poll(j, w, step))
            done.push_back(std::move(j));
        else
            still.push_back(std::move(j));
    }
    {
        std::lock_guard lock(q.m);
        for (Job& j : still) q.running.push_back(std::move(j));
    }
    deliver(w, done);
    watch_step(w);
}

void buffs_world_stopping() {
    Jobs& q = jobs();
    std::lock_guard lock(q.m);
    // the world's entities go with it (the impacts too); the answers come at the next server Post, not on the
    // destroying thread
    for (Job& j : q.running) {
        finish(j, EMF_BUFF_REFUSED, buffs::wait_why(buffs::Wait::Closed));
        q.finished.push_back(std::move(j));
    }
    for (Job& j : q.waiting) {
        finish(j, EMF_BUFF_REFUSED, buffs::wait_why(buffs::Wait::Closed));
        q.finished.push_back(std::move(j));
    }
    q.running.clear();
    q.waiting.clear();
    watch().reset = true;
}

void fill_buffs(EmfApi* t) {
    t->buff_types = &api_buff_types;
    t->buff_type = &api_buff_type;
    t->buff_text = &api_buff_text;
    t->buffs_of = &api_buffs_of;
    t->buff_apply = &api_buff_apply;
    t->buff_remove = &api_buff_remove;
    t->on_buff = &api_on_buff;
}

}  // namespace kemf

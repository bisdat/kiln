// SPDX-License-Identifier: MIT
// kiln.routines v1 (kiln-routines.dll): the service. It resolves the villager worker, its chooser and the setters
// (sigdb feature routines.decisions), checks the layout the hooks rely on, publishes the table, and drives the
// assignments from the server world's Pre and Post: Pre commits queued changes and gives the hooks this step's list
// of villagers (NpcId and the address of each one's DynamicActiveNpcState, read through kiln.world while no job
// runs); Post completes the changes and sends the events. What runs inside the game's worker is hooks.cpp; the
// queue is state.cpp; the plan's reader is plan.cpp.
// Contract: native/core/abi/services/kiln_routines.h; docs/kiln/services/routines.md.
//
// The links go in with the first assignment (kiln.hooks adds links while the game runs) and stay: without a list of
// villagers each costs one load and one branch. Nothing is installed while routines.decisions is off (or a policy
// holds it); then assign, remove and resume answer KILN_ERR_UNAVAILABLE and the game chooses alone.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "kiln_api.h"
#include "kiln_hook.hpp"
#include "services/kiln_caps.h"
#include "services/kiln_hooks.h"
#include "services/kiln_routines.h"
#include "services/kiln_types.h"
#include "services/kiln_world.h"
#include "state.h"

namespace {

using namespace kroutines;

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
const KilnWorldApi* W = nullptr;
const KilnCapsApi* g_caps = nullptr;

Engine g_engine;
Book g_book(g_engine);

void logf(int level, const char* fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= static_cast<int>(sizeof buf)) n = sizeof buf - 1;
    std::string line = "routines: ";
    line.append(buf, static_cast<size_t>(n));
    g_api->log(g_owner, level, KilnStr{line.data(), line.size()});
}
#define RT_I(...) logf(KILN_LOG_INFO, __VA_ARGS__)
#define RT_W(...) logf(KILN_LOG_WARN, __VA_ARGS__)

KilnStr ks(const char* s) { return KilnStr{s, std::strlen(s)}; }
bool on_tick() { return g_api->on_tick_thread && g_api->on_tick_thread(); }
bool world_runs() { return g_api->server_world && g_api->server_world() != 0; }

// ------------------------------------------------------------------------------------------ what is resolved ----

enum { kWorker, kDecide, kSetState, kFinishState, kSetDecision, kSleepCheck, kCommands, kSymbols };
const char* const kNames[kSymbols] = {"routines/VillagerWorker", "routines/DecideTask",  "routines/SetState",      "routines/FinishState",
                                      "routines/SetDecision",    "routines/SleepCheck", "routines/ProcessCommands"};
uintptr_t g_at[kSymbols] = {};
kiln::RawHook g_link[kSymbols];  // every function but SetDecision
// MarkWritten, captured from the call in the game's SetDecision (no descriptor of its own: its arguments are the ones
// that call passes, chapter 16g §4.1)
constexpr const char* kMarkWritten = "routines/MarkWritten";
uintptr_t g_mark_written = 0;

// Why the service cannot serve, in words; empty: it can. Decided once at start, and again when a link or the layout
// fails later.
std::string g_off = "kiln.routines has not started";
bool g_linked = false;

void report_off(uint32_t reason) {
    if (!g_caps || !g_caps->report) return;
    g_caps->report(g_owner, ks(KILN_ROUTINES_FEATURE), KILN_CAP_UNAVAILABLE, reason, KilnStr{g_off.data(), g_off.size()});
}

// A fault found while running (a link that did not install, a record of another size): the hooks let go of every
// villager, the row says why, and the game chooses alone from here on.
void fail(const std::string& why) {
    if (!g_off.empty()) return;
    g_off = why;
    g_engine.clear();
    RT_W("%s: routines.decisions is off, the game chooses for every villager", why.c_str());
    report_off(KILN_CAP_REASON_FAULTED);
}

// ---- the game's functions as the engine calls them ----
void next_worker(void* ctx) { g_link[kWorker].original<void (*)(void*)>()(ctx); }
void next_decide(void* ctx, uint8_t phase) { g_link[kDecide].original<void (*)(void*, uint8_t)>()(ctx, phase); }
void next_set_state(void* ctx, uint8_t state) { g_link[kSetState].original<void (*)(void*, uint8_t)>()(ctx, state); }
bool next_finish_state(void* ctx, uint8_t condition) { return g_link[kFinishState].original<bool (*)(void*, uint8_t)>()(ctx, condition); }
void game_set_decision(void* ctx, uint8_t decision) { reinterpret_cast<void (*)(void*, uint8_t)>(g_at[kSetDecision])(ctx, decision); }
void game_set_state(void* ctx, uint8_t state) { reinterpret_cast<void (*)(void*, uint8_t)>(g_at[kSetState])(ctx, state); }
bool game_finish_state(void* ctx, uint8_t condition) { return reinterpret_cast<bool (*)(void*, uint8_t)>(g_at[kFinishState])(ctx, condition); }
bool game_sleep_check(void* ctx) { return reinterpret_cast<bool (*)(void*)>(g_at[kSleepCheck])(ctx); }
bool next_sleep_check(void* ctx) { return g_link[kSleepCheck].original<bool (*)(void*)>()(ctx); }
void next_commands(void* ctx) { g_link[kCommands].original<void (*)(void*)>()(ctx); }
void game_mark_written(void* tracker, uint64_t a, uint64_t b, uint64_t c, uint16_t d, uint8_t e) {
    reinterpret_cast<void (*)(void*, uint64_t, uint64_t, uint64_t, uint16_t, uint8_t)>(g_mark_written)(tracker, a, b, c, d, e);
}

// ---- the links ----
void hk_worker(void* ctx) { g_engine.on_worker(ctx); }
void hk_decide(void* ctx, uint8_t phase) { g_engine.on_decide(ctx, phase); }
void hk_set_state(void* ctx, uint8_t state) { g_engine.on_set_state(ctx, state); }
bool hk_finish_state(void* ctx, uint8_t condition) { return g_engine.on_finish_state(ctx, condition); }
bool hk_sleep_check(void* ctx) { return g_engine.on_sleep_check(ctx); }
void hk_commands(void* ctx) { g_engine.on_commands(ctx); }

// Tick thread. The links of the six hooked functions, once; a failure is final.
bool ensure_links() {
    if (g_linked) return true;
    void* fns[kSymbols] = {};
    fns[kWorker] = reinterpret_cast<void*>(&hk_worker);
    fns[kDecide] = reinterpret_cast<void*>(&hk_decide);
    fns[kSetState] = reinterpret_cast<void*>(&hk_set_state);
    fns[kFinishState] = reinterpret_cast<void*>(&hk_finish_state);
    fns[kSleepCheck] = reinterpret_cast<void*>(&hk_sleep_check);
    fns[kCommands] = reinterpret_cast<void*>(&hk_commands);
    // the worker last: until it is in, no context is authorized and the others only pass through
    for (int i : {kDecide, kSetState, kFinishState, kSleepCheck, kCommands, kWorker}) {
        if (g_link[i]) continue;
        if (!g_link[i].install(g_api, g_owner, g_at[i], fns[i], kNames[i])) {
            fail(std::string("the hook on ") + kNames[i] + " did not install (" + (g_link[i].error() ? g_link[i].error() : "?") + ")");
            return false;
        }
    }
    g_linked = true;
    RT_I("the links on the villager worker, its chooser, SetState, FinishState, SleepCheck and ProcessCommands are in");
    return true;
}

// ------------------------------------------------------------------------------------------------ the layout ----

bool constant(const char* name, uint32_t* out) {
    int64_t v = 0;
    if (g_api->constant(ks(name), &v) != KILN_OK || v < 0 || v > 0xffff) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

std::string_view sv(KilnStr s) { return s.ptr ? std::string_view(s.ptr, s.len) : std::string_view(); }

// The size of a type and the offset of one of its own fields, from the game's reflection. False: not there.
bool type_field(const KilnTypesApi* t, const char* type_name, const char* field, uint32_t* size, uint64_t* offset) {
    const KilnType* type = t->type_by_name(ks(type_name));
    KilnTypeInfo ti{};
    ti.size = sizeof ti;
    if (!type || t->type_info(type, &ti) != KILN_OK) return false;
    *size = ti.type_size;
    for (uint32_t i = 0; i < ti.field_count; ++i) {
        KilnTypeField f{};
        if (t->type_field(type, i, &f) == KILN_OK && sv(f.name) == field) return *offset = f.offset, true;
    }
    return false;
}

constexpr uint32_t kActiveSize = 456, kDynamicSize = 136, kCommonSize = 1688, kFailures = 0x68e;

// The constants the hooks read through (captured from the game's own SetDecision, or fixed in sigdb for this build),
// checked against each other and against the game's reflection. A word in `why` when something does not fit.
bool check_layout(std::string* why) {
    Layout& l = g_engine.layout;
    uint32_t failures = 0, states = 0, exit_phase = 0, override_bit = 0;
    const struct {
        const char* name;
        uint32_t* out;
    } wanted[] = {{"routines/ctx.dynamic", &l.ctx_dynamic},   {"routines/ctx.common", &l.ctx_common},     {"routines/ctx.template", &l.ctx_template},
                  {"routines/ctx.time", &l.ctx_time},         {"routines/time.now", &l.time_now},         {"routines/dynamic.state", &l.dyn_state},
                  {"routines/dynamic.previous", &l.dyn_previous}, {"routines/dynamic.decision", &l.dyn_decision},
                  {"routines/common.failures", &failures},    {"routines/state.count", &states},          {"routines/phase.exit", &exit_phase},
                  {"routines/ctx.dynamicTracker", &l.ctx_dynamic_tracker}, {"routines/ctx.commonTracker", &l.ctx_common_tracker},
                  {"routines/common.flags", &l.common_flags}, {"routines/common.sleep", &l.common_sleep}, {"routines/common.wake", &l.common_wake},
                  {"routines/common.sleepOverrideBit", &override_bit}};
    for (const auto& c : wanted)
        if (!constant(c.name, c.out)) return *why = std::string("constant ") + c.name + " is not resolved", false;
    // The keep-awake prefix's marks: each record's tracker object sits 0x28 after the record's pointer, with the
    // bookkeeping words MarkWritten takes between them; the hours are floats inside NpcCommon; the flag is one bit.
    if (l.ctx_dynamic_tracker != l.ctx_dynamic + 0x28 || l.ctx_common_tracker != l.ctx_common + 0x28)
        return *why = "a write record's tracker is not where MarkWritten's bookkeeping words put it", false;
    if (l.common_flags >= kCommonSize || l.common_sleep + 4 > kCommonSize || l.common_wake + 4 > kCommonSize || l.common_sleep == l.common_wake)
        return *why = "NpcCommon's flags, sleep and wake hours are not inside a 1688-byte record", false;
    if (!override_bit || override_bit > 0x80 || (override_bit & (override_bit - 1)))
        return *why = "the forced sleep window's flag is not one bit of a byte", false;
    l.sleep_override_bit = static_cast<uint8_t>(override_bit);
    if (g_api->symbol(ks(kMarkWritten), &g_mark_written) != KILN_OK || !g_mark_written)
        return *why = std::string("symbol ") + kMarkWritten + " is not resolved", false;
    if (l.dyn_state >= kDynamicSize || l.dyn_previous >= kDynamicSize || l.dyn_decision >= kDynamicSize || l.dyn_state == l.dyn_decision ||
        l.dyn_state == l.dyn_previous)
        return *why = "the state, previous state and decision of DynamicActiveNpcState are not where a 136-byte record has them", false;
    if (l.ctx_dynamic == l.ctx_common || l.ctx_dynamic == l.ctx_template || l.ctx_common == l.ctx_template || l.ctx_time == l.ctx_dynamic)
        return *why = "two records of the worker's context resolved to one offset", false;
    if (failures != kFailures) return *why = "NpcCommon.taskFailures was captured at another offset than this version was written for", false;
    if (states != native::StateCount || exit_phase != native::PhaseExit) return *why = "the villager's states or phases are not the ones this version was written for", false;

    const auto* types = static_cast<const KilnTypesApi*>(g_api->service(ks(KILN_TYPES_SERVICE), KILN_TYPES_VERSION));
    if (!types || !types->type_by_name || !types->type_info || !types->type_field) return *why = "kiln.types is not here to check the villager records against", false;
    uint32_t size = 0;
    uint64_t offset = 0;
    if (!type_field(types, "keen::ecs::ActiveNpcState", "npcId", &size, &offset) || size != kActiveSize || offset != 0)
        return *why = "keen::ecs::ActiveNpcState is not 456 bytes with npcId first", false;
    if (!type_field(types, "keen::ecs::NpcCommon", "taskFailures", &size, &offset) || size != kCommonSize || offset != kFailures)
        return *why = "keen::ecs::NpcCommon is not 1688 bytes with taskFailures at 0x68e", false;
    // DynamicActiveNpcState's size is checked on the records themselves, in Pre (kiln.world gives a component's size).
    // The state enum: checked when the game's reflection names it.
    if (const KilnType* state = types->type_by_name(ks("keen::active_npc::State")); state && types->type_enum) {
        KilnTypeInfo ti{};
        ti.size = sizeof ti;
        if (types->type_info(state, &ti) == KILN_OK && ti.field_count != native::StateCount)
            return *why = "keen::active_npc::State does not have 18 values", false;
    }
    return true;
}

// ------------------------------------------------------------------------------------- the world, read in Pre ----

uint32_t h_active = 0, h_dynamic = 0;
constexpr KilnEntity kTwice = 0xffffffffu;
std::unordered_map<uint32_t, KilnEntity> g_entity;  // NpcId -> its entity; kTwice: more than one has it
uint64_t g_scanned = 0;                               // the step of the last scan
bool g_missing = false;                               // an assigned villager had no entity in this step
std::vector<KilnEntity> g_scan;

// A guarded copy of game memory. Nothing with a destructor lives in this frame.
bool seh_read(const void* p, void* out, size_t n) {
    if (!p) return false;
    __try {
        std::memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t npc_of(KilnEntity e) {
    uint32_t size = 0, id = 0;
    const void* p = W->component(e, h_active, &size);
    return p && size >= 4 && seh_read(p, &id, 4) ? id : 0;
}

// Every villager of the server world, by NpcId. One pass over the entities that have an ActiveNpcState.
void scan(uint64_t step) {
    g_scanned = step;
    g_entity.clear();
    uint32_t count = 0;
    g_scan.resize(256);
    KilnResult rc = W->find(h_active, g_scan.data(), static_cast<uint32_t>(g_scan.size()), &count);
    if (rc == KILN_ERR_BUFFER_TOO_SMALL) {
        g_scan.resize(count + 64);
        rc = W->find(h_active, g_scan.data(), static_cast<uint32_t>(g_scan.size()), &count);
    }
    if (rc != KILN_OK) return;
    for (uint32_t i = 0; i < count && i < g_scan.size(); ++i) {
        const uint32_t npc = npc_of(g_scan[i]);
        if (!npc) continue;
        auto [it, fresh] = g_entity.emplace(npc, g_scan[i]);
        if (!fresh && it->second != g_scan[i]) it->second = kTwice;
    }
}

// Where the villager's DynamicActiveNpcState is now; 0: not in the world, or two entities share the NpcId.
uintptr_t where(uint32_t npc_id) {
    auto it = g_entity.find(npc_id);
    if (it == g_entity.end() || it->second == kTwice || npc_of(it->second) != npc_id) {
        if (it == g_entity.end() || it->second != kTwice) g_missing = true;
        return 0;
    }
    uint32_t size = 0;
    const void* dynamic = W->component(it->second, h_dynamic, &size);
    if (!dynamic) return 0;
    if (size != kDynamicSize) {
        fail("DynamicActiveNpcState is " + std::to_string(size) + " bytes in this world, not 136");
        return 0;
    }
    return reinterpret_cast<uintptr_t>(dynamic);
}

// ------------------------------------------------------------------------------------------------ the events ----

struct Subscriber {
    KilnHandle handle;
    KilnOwner owner;
    KilnRoutineEventFn fn;
    void* user;
};
std::mutex g_sub_mu;
std::vector<Subscriber> g_subs;
KilnHandle g_next_sub = 1;

// Tick thread: the completions of this step's changes, then the events. Callbacks run with no lock held.
void deliver() {
    for (const Completion& c : g_book.take_completions()) {
        KilnRoutineResult r{};
        r.size = sizeof r;
        r.ok = c.ok ? 1u : 0u;
        r.ticket = c.ticket;
        r.revision = c.revision;
        r.npc_id = c.npc_id;
        r.reason_code = c.code;
        r.reason = KilnStr{c.reason.data(), c.reason.size()};
        c.done(&r, c.user);
    }
    const std::vector<std::string> events = g_book.take_events();
    if (events.empty()) return;
    std::vector<Subscriber> subs;
    {
        std::lock_guard lock(g_sub_mu);
        subs = g_subs;
    }
    for (const std::string& e : events)
        for (const Subscriber& s : subs) s.fn(KilnStr{e.data(), e.size()}, s.user);
}

// ------------------------------------------------------------------------------------------------- the steps ----

void KILN_CALL on_pre(const KilnTickInfo* tick, void*) {
    if (!g_off.empty() || !W) return;
    if (!g_book.queued() && !g_book.assigned() && g_engine.empty()) return;  // nobody uses routines: nothing to do
    // Look for villagers again when one that has a plan was not found (at most once a second), and now and then anyway:
    // a second entity with the NpcId of an assigned villager shows only in a scan.
    if (!g_scanned || (g_missing && tick->step - g_scanned >= 25) || tick->step - g_scanned >= 250) scan(tick->step);
    g_missing = false;
    g_book.pre(tick->step, &where);
    if (!g_off.empty()) g_engine.clear();  // the layout failed while this step's list was made
}

void KILN_CALL on_post(const KilnTickInfo*, void*) {
    g_book.post();
    deliver();
    if (const uint64_t lost = g_book.dropped_task_events()) {
        static uint64_t told = 0;
        if (lost != told) RT_W("%llu task changes were not reported: a villager changed its task more than 16 times in one step",
                               static_cast<unsigned long long>(lost)), told = lost;
    }
}

void KILN_CALL on_world(const KilnWorldEvent* ev, void*) {
    if (ev->kind != KILN_WORLD_SERVER || ev->event != KILN_WORLD_STOPPING) return;
    g_book.stop();
    deliver();
    g_entity.clear();
    g_scanned = 0;
}

void KILN_CALL on_owner_retiring(KilnOwner retiring, void*) {
    g_book.revoke_owner(retiring);
    std::lock_guard lock(g_sub_mu);
    std::erase_if(g_subs, [&](const Subscriber& s) { return s.owner == retiring; });
}

// ------------------------------------------------------------------------------------------------- the table ----

// What a change needs before it is queued: the tick thread, the capability, a world, and the links.
KilnResult ready() {
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    if (!g_off.empty() || !world_runs()) return KILN_ERR_UNAVAILABLE;
    return ensure_links() ? KILN_OK : KILN_ERR_UNAVAILABLE;
}

KilnResult KILN_CALL assign(KilnOwner owner, uint32_t npc_id, KilnStr plan_json, KilnRoutineDone done, void* user, uint64_t* ticket) {
    if (ticket) *ticket = 0;
    if (KilnResult rc = ready(); rc != KILN_OK) return rc;
    if (!plan_json.ptr) return KILN_ERR_INVALID_ARGUMENT;
    std::string problem;
    const KilnResult rc = g_book.assign(owner, npc_id, std::string_view(plan_json.ptr, plan_json.len), done, user, ticket, &problem);
    if (rc == KILN_ERR_INVALID_ARGUMENT) RT_W("assign for villager 0x%08x refused: %s", npc_id, problem.c_str());
    return rc;
}

KilnResult KILN_CALL remove(KilnOwner owner, uint32_t npc_id, KilnRoutineDone done, void* user, uint64_t* ticket) {
    if (ticket) *ticket = 0;
    if (KilnResult rc = ready(); rc != KILN_OK) return rc;
    return g_book.remove(owner, npc_id, done, user, ticket);
}

KilnResult KILN_CALL resume(KilnOwner owner, uint32_t npc_id, KilnRoutineDone done, void* user, uint64_t* ticket) {
    if (ticket) *ticket = 0;
    if (KilnResult rc = ready(); rc != KILN_OK) return rc;
    return g_book.resume(owner, npc_id, done, user, ticket);
}

KilnResult KILN_CALL inspect(uint32_t npc_id, char* json, size_t cap, size_t* out_len) {
    if (!out_len || (cap && !json)) return KILN_ERR_INVALID_ARGUMENT;
    *out_len = 0;
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    std::string text;
    if (!g_book.inspect(npc_id, text)) return KILN_ERR_NOT_FOUND;
    *out_len = text.size();
    if (text.size() > cap) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(json, text.data(), text.size());
    return KILN_OK;
}

void KILN_CALL revoke_owner(KilnOwner owner) { on_owner_retiring(owner, nullptr); }

KilnResult KILN_CALL restore(KilnOwner owner, uint32_t npc_id, KilnStr plan_json, uint64_t revision, uint32_t flags, KilnStr reason,
                             KilnRoutineDone done, void* user, uint64_t* ticket) {
    if (ticket) *ticket = 0;
    if (KilnResult rc = ready(); rc != KILN_OK) return rc;
    if (!plan_json.ptr || (flags & ~KILN_ROUTINE_RESTORE_SUSPENDED)) return KILN_ERR_INVALID_ARGUMENT;
    std::string problem;
    const KilnResult rc = g_book.restore(owner, npc_id, std::string_view(plan_json.ptr, plan_json.len), revision,
                                         (flags & KILN_ROUTINE_RESTORE_SUSPENDED) != 0, reason.ptr ? std::string_view(reason.ptr, reason.len) : std::string_view(),
                                         done, user, ticket, &problem);
    if (rc == KILN_ERR_INVALID_ARGUMENT) RT_W("restore for villager 0x%08x refused: %s", npc_id, problem.c_str());
    return rc;
}

KilnResult KILN_CALL set_revision_floor(uint64_t floor) {
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    if (!g_off.empty() || !world_runs()) return KILN_ERR_UNAVAILABLE;
    g_book.set_revision_floor(floor);
    return KILN_OK;
}

KilnResult KILN_CALL subscribe_events(KilnOwner owner, KilnRoutineEventFn fn, void* user, KilnHandle* out) {
    if (!owner || !fn || !out) return KILN_ERR_INVALID_ARGUMENT;
    *out = 0;
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    std::lock_guard lock(g_sub_mu);
    if (g_subs.size() >= 8) return KILN_ERR_UNAVAILABLE;
    g_subs.push_back(Subscriber{*out = g_next_sub++, owner, fn, user});
    return KILN_OK;
}

KilnResult KILN_CALL unsubscribe_events(KilnHandle handle) {
    if (!on_tick()) return KILN_ERR_WRONG_THREAD;
    std::lock_guard lock(g_sub_mu);
    return std::erase_if(g_subs, [&](const Subscriber& s) { return s.handle == handle; }) ? KILN_OK : KILN_ERR_NOT_FOUND;
}

const KilnRoutinesApi kTable = {{sizeof(KilnRoutinesApi), KILN_ROUTINES_VERSION}, &assign, &remove, &resume, &inspect, &revoke_owner,
                                &subscribe_events, &unsubscribe_events, &restore, &set_revision_floor};

// --------------------------------------------------------------------------------------------------- start ----

// Everything the service needs on this build and side, or why not (with the row's reason code).
bool decide(std::string* why, uint32_t* reason) {
    const KilnStr feature = ks(KILN_ROUTINES_FEATURE);
    *reason = KILN_CAP_REASON_POLICY;
    if (kiln_caps_held(g_caps, feature)) return *why = "held until its game test", false;  // the gate's own row says which checks
    *reason = KILN_CAP_REASON_SYMBOL_MISSING;
    if (!g_api->feature || !g_api->feature(feature)) return *why = "the villager worker's functions are not found on this game build", false;
    for (int i = 0; i < kSymbols; ++i) {
        if (g_api->symbol(ks(kNames[i]), &g_at[i]) != KILN_OK || !g_at[i]) return *why = std::string("symbol ") + kNames[i] + " is not resolved", false;
        // the function's code still takes the arguments the hooks pass (06b: one row per described symbol)
        const std::string sig = std::string("sig/") + kNames[i];
        if (!g_api->feature(ks(sig.c_str()))) return *why = sig + " is off: the function's code no longer fits its descriptor", false;
    }
    *reason = KILN_CAP_REASON_SERVICE_MISSING;
    if (!kiln::hooks_v2(g_api)) return *why = "kiln.hooks v2 is not here", false;
    if (!W || !W->component || !W->find || !W->component_hash) return *why = "kiln.world is not here", false;
    if (!g_api->feature(ks("world.lifecycle"))) return *why = "world stops are not seen on this build, so a routine could outlive its world", false;
    *reason = KILN_CAP_REASON_FAULTED;
    return check_layout(why);
}

void KILN_CALL on_ready(void*) {
    W = static_cast<const KilnWorldApi*>(g_api->service(ks(KILN_WORLD_SERVICE), 1));
    g_caps = static_cast<const KilnCapsApi*>(g_api->service(ks(KILN_CAPS_SERVICE), KILN_CAPS_VERSION));
    std::string why;
    uint32_t reason = KILN_CAP_REASON_POLICY;
    if (!decide(&why, &reason)) {
        g_off = why;
        // A held or unresolved feature already has its row from the core; the service adds its own only for what the
        // core cannot know.
        if (reason == KILN_CAP_REASON_SERVICE_MISSING || reason == KILN_CAP_REASON_FAULTED) report_off(reason);
        RT_I("off: %s. The game chooses for every villager", why.c_str());
        return;
    }
    h_active = W->component_hash(ks("ActiveNpcState"));
    h_dynamic = W->component_hash(ks("DynamicActiveNpcState"));
    g_engine.game = Game{&next_worker,       &next_decide,     &next_set_state,   &next_finish_state, &game_set_decision,
                         &game_set_state,    &game_finish_state, &game_sleep_check, &next_sleep_check,  &game_mark_written, &next_commands};
    KilnHandle h = 0;
    if (g_api->subscribe_tick(g_owner, KILN_TICK_PRE, &on_pre, nullptr, &h) != KILN_OK ||
        g_api->subscribe_tick(g_owner, KILN_TICK_POST, &on_post, nullptr, &h) != KILN_OK ||
        g_api->subscribe_world(g_owner, &on_world, nullptr, &h) != KILN_OK) {
        g_off = "the tick or the world lifecycle could not be subscribed";
        report_off(KILN_CAP_REASON_FAULTED);
        RT_W("off: %s", g_off.c_str());
        return;
    }
    // a mod that unloads takes its routines with it, also when its managed side never says so
    if (const KilnHooksApi* hooks = kiln::hooks_v2(g_api);
        hooks && hooks->header.size >= offsetof(KilnHooksApi, subscribe_owner_retiring) + sizeof(void*) && hooks->subscribe_owner_retiring)
        hooks->subscribe_owner_retiring(g_owner, &on_owner_retiring, nullptr, &h);
    g_off.clear();
    RT_I("ready: the worker, the chooser and the setters resolved, the layout fits; the links go in with the first assignment");
}

}  // namespace

extern "C" KILN_EXPORT KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api, KilnOwner owner, KilnPluginInfo* info) {
    if (api->abi_version != KILN_ABI_VERSION || api->size < sizeof(KilnApi)) return KILN_ERR_UNAVAILABLE;
    g_api = api;
    g_owner = owner;
    info->id = ks(KILN_ROUTINES_SERVICE);
    info->version = ks("1");
    info->on_ready = &on_ready;
    // The table is always there: what it cannot do on this build it answers with KILN_ERR_UNAVAILABLE, and the row
    // routines.decisions says why.
    return api->register_service(owner, ks(KILN_ROUTINES_SERVICE), &kTable);
}

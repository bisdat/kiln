// SPDX-License-Identifier: MIT
// kiln.markers S3, following an entity (chapter 04 §2.6, §4.2, §5): the glue of the anchors. It resolves the explicit-world
// reader and the structural primitives, checks the reflected transform layouts through kiln.types, installs the
// observers (one raw link each on the game's immediate create and destroy, both worlds; the read-only network-create mid
// handler on a client), answers capture_anchor/release_anchor, does the follow reads at each world's completed Post and
// keeps the rows markers.anchors.server/.client. The watch table itself is anchors.h (pure, host-tested).
//
// The observers forward every argument and the return unchanged, read no game memory, allocate nothing, call no mod
// code and hold no lock across the continuation: they only end watched incarnations (Watches::structural). A guard that
// fails turns that side's anchors off and ends its watches; fixed markers keep working.
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "kiln_api.h"
#include "kiln_hook.hpp"
#include "services/kiln_caps.h"
#include "services/kiln_markers.h"
#include "services/kiln_markers_lab.h"
#include "services/kiln_spawn.h"
#include "services/kiln_types.h"

#include "anchors.h"
#include "glue.h"
#include "world_read.h"

namespace kmk {

namespace {

constexpr char kRowServer[] = "markers.anchors.server";
constexpr char kRowClient[] = "markers.anchors.client";
const char* row_of(uint32_t kind) { return kind == KILN_WORLD_CLIENT ? kRowClient : kRowServer; }
const char* world_name(uint32_t kind) { return kind == KILN_WORLD_CLIENT ? "client" : "server"; }

// Never destroyed: an observer may still run while the process exits (the game's own teardown), after this DLL's static
// destructors would have run. Allocated once at load.
Watches& g_w = *new Watches;

struct Resolved {
    GetComponentFn get_component = nullptr;
    uintptr_t create = 0, destroy = 0, net_create = 0;
    int64_t storage = -1, net_entity = -1, net_parent = -1;
} R;

bool g_feature[KILN_WORLD_KINDS] = {};             // sigdb resolved what that side needs
std::atomic<bool> g_on[KILN_WORLD_KINDS] = {};     // captures and follow reads on that world now
const KilnTypesApi* g_types = nullptr;
kiln::RawHook g_create_hook, g_destroy_hook;
kiln::MidHook2 g_net_hook;
std::atomic<bool> g_net_faulted{false}, g_net_fault_told{false};
bool g_lab = false;  // kiln.markers.lab is published (a dedicated lab server); set before any observer is installed

// Observer counters (the lab table and the world-stop line) and, on a lab server only, the threads they ran on.
struct Observed {
    std::atomic<uint64_t> creates{0}, destroys{0}, nets{0}, captures{0};
    std::atomic<uint32_t> threads[16] = {};
    std::atomic<uint32_t> world_thread[KILN_WORLD_KINDS] = {};
    std::atomic<bool> worker_told{false};
} g_obs;

void note(std::atomic<uint64_t>& counter) {
    counter.fetch_add(1, std::memory_order_relaxed);
    if (!g_lab) return;
    const uint32_t tid = GetCurrentThreadId();
    for (auto& t : g_obs.threads) {
        uint32_t v = t.load(std::memory_order_relaxed);
        if (v == tid) return;
        if (v == 0 && (t.compare_exchange_strong(v, tid) || v == tid)) return;
    }
}

// ---- the observers (any thread) -----------------------------------------------------------------------------------------

// u32 CreateEntity(storage, requestedId, const SpawnRequest*, const void* mask): 0 refused, else the created id.
uint32_t hk_create(uintptr_t storage, uint32_t requested, const void* request, const void* mask) {
    note(g_obs.creates);
    if (requested) g_w.structural(storage, requested);  // an explicit id: before the game reuses it
    const uint32_t id = g_create_hook.call<uint32_t>(storage, requested, request, mask);
    if (id) g_w.structural(storage, id);
    return id;
}

// u8 DestroyEntity(storage, id, mode, skipReplication): before it runs (a failed attempt ends the watch too).
uint8_t hk_destroy(uintptr_t storage, uint32_t id, uint8_t mode, uint8_t skip) {
    note(g_obs.destroys);
    g_w.structural(storage, id);
    return g_destroy_hook.call<uint8_t>(storage, id, mode, skip);
}

// A guarded stack read: inside this thread's stack and under an exception guard (no object with a destructor here).
bool stack_u32(uintptr_t at, uint32_t* out) {
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const uintptr_t lo = reinterpret_cast<uintptr_t>(tib->StackLimit), hi = reinterpret_cast<uintptr_t>(tib->StackBase);
    if (at < lo || hi < sizeof(uint32_t) || at > hi - sizeof(uint32_t)) return false;
    __try {
        *out = *reinterpret_cast<const volatile uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The client's network create path (net/CreateTemplateCall), read-only, ahead of kiln.net's guard (priority 100 over its
// 0), on every create attempt: the entity id the message creates (and its parent) from the frame. Registers and rip are
// never changed. A failed read turns client anchors off and ends every client watch (told at the next client Post).
void hk_net_create(KilnRegisters& ctx) {
    note(g_obs.nets);
    uint32_t entity = 0, parent = 0;
    if (!stack_u32(ctx.rbp + static_cast<uintptr_t>(R.net_entity), &entity) ||
        !stack_u32(ctx.rbp + static_cast<uintptr_t>(R.net_parent), &parent)) {
        g_on[KILN_WORLD_CLIENT].store(false);
        if (g.sw) g.sw->anchors_client.store(false);
        g_net_faulted.store(true);
        g_w.end_all(KILN_WORLD_CLIENT);
        return;
    }
    (void)parent;  // read as the contract fixes it; the id alone decides
    g_w.client_create(entity);
}

// ---- helpers ------------------------------------------------------------------------------------------------------------

bool in_program_callback() {
    return g_types && g_types->header.size >= offsetof(KilnTypesApi, in_system_callback) + sizeof(g_types->in_system_callback) &&
           g_types->in_system_callback && g_types->in_system_callback() != 0;
}

bool on_world_thread(uint32_t kind) { return g.api && g.api->on_world_thread(kind) && !in_program_callback(); }

// A guard failed at run time: that side's captures stop and its watches end; fixed markers are untouched.
void lower(uint32_t kind, const char* why) {
    if (!g_on[kind].exchange(false)) return;
    if (kind == KILN_WORLD_CLIENT && g.sw) g.sw->anchors_client.store(false);
    g_w.end_all(kind);
    report(row_of(kind), KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED, why);
    logf(KILN_LOG_WARN, "markers: %s is off: %s; followed markers there show AnchorEnded, fixed markers keep working", row_of(kind), why);
}

bool control_file(const char* leaf) {
    char dir[1024];
    size_t len = 0;
    if (g.api->path(KILN_PATH_CONTROL, dir, sizeof dir - 1, &len) != KILN_OK || len + std::strlen(leaf) + 2 > sizeof dir) return false;
    std::string path(dir, len);
    path += '\\';
    path += leaf;
    wchar_t wide[1100];
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide, 1100);
    return n > 0 && GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
}

struct ReadNote {
    ReadKind kind = ReadKind::Absent;
};

TransformRead capture_read(uint32_t kind, uint32_t entity, void* ctx) {
    const TransformRead r = read_transform(R.get_component, g.api->world(kind), kind, entity);
    static_cast<ReadNote*>(ctx)->kind = r.kind;
    return r;
}

// Store callbacks (Store::set_follow): the local store's records follow client-world entities.
uint32_t store_acquire(KilnOwner owner, uint32_t kind, KilnMarkerAnchor lease, uint64_t* serial, char* why, size_t cap, void*) {
    return g_w.retain(owner, kind, lease, serial, why, cap);
}
void store_release(uint64_t serial, void*) { g_w.drop(serial); }

uint32_t count_threads(uint32_t* other) {
    uint32_t n = 0, o = 0;
    for (const auto& t : g_obs.threads) {
        const uint32_t tid = t.load(std::memory_order_relaxed);
        if (!tid) continue;
        ++n;
        bool world = false;
        for (const auto& w : g_obs.world_thread) world = world || w.load(std::memory_order_relaxed) == tid;
        if (!world) ++o;
    }
    if (other) *other = o;
    return n;
}

// ---- kiln.markers.lab (lab servers only) --------------------------------------------------------------------------------

KilnResult KILN_CALL lab_follow_now(KilnOwner owner, KilnMarkerAnchor anchor, KilnMarkersLabFollow* out) {
    if (!out || out->size < sizeof(KilnMarkersLabFollow)) return KILN_ERR_INVALID_ARGUMENT;
    Watches::ToRead t;
    uint32_t kind = 0;
    uint64_t epoch = 0;
    if (!g_w.lease(owner, anchor, &t, &kind, &epoch)) return KILN_ERR_NOT_FOUND;
    if (!on_world_thread(kind)) return KILN_ERR_WRONG_THREAD;
    if (g_on[kind].load() && t.serial) g_w.read_done(t, read_transform(R.get_component, g.api->world(kind), kind, t.entity));
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    out->reason = KILN_MARKER_ANCHOR_ENDED;
    if (!g_w.follow(t.serial, &out->position, &out->reason)) out->reason = KILN_MARKER_ANCHOR_ENDED;
    out->incarnation = t.serial;
    out->world_epoch = epoch;
    out->world_kind = kind;
    out->entity = t.entity;
    return KILN_OK;
}

KilnResult KILN_CALL lab_stats(KilnMarkersLabStats* out) {
    if (!out || out->size < sizeof(KilnMarkersLabStats)) return KILN_ERR_INVALID_ARGUMENT;
    const Watches::Counts c = g_w.counts();
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    out->threads = count_threads(&out->other_threads);
    out->watched = c.live;
    out->leases = c.leases;
    out->references = c.references;
    out->creates = g_obs.creates.load();
    out->destroys = g_obs.destroys.load();
    out->network_creates = g_obs.nets.load();
    out->ended = c.ended;
    out->serials = c.serials;
    return KILN_OK;
}

// The engine's SpawnRequest and transform as the research probe built them (chapter 04 §2.6 lab: a template and a
// CurrentTransform, nothing else).
struct LabTransform {
    int64_t position[3];
    float orientation[4];
    float scale[3];
    uint32_t pad;
};
struct LabRequest {
    uint32_t parent;
    uint8_t guid[16];
    uint32_t pad;
    const KilnSpawnComponent* components;
    uint64_t count;
    uint16_t pad2, player_mask;
    uint8_t kind, pad3[3];
};
static_assert(sizeof(LabTransform) == 56 && sizeof(LabRequest) == 0x30 && offsetof(LabRequest, components) == 0x18, "lab request layout");

using CreateFn = uint32_t (*)(uintptr_t storage, uint32_t requested, const void* request, const void* mask);

bool seh_create(uintptr_t fn, uintptr_t storage, uint32_t requested, const LabRequest* req, uint32_t* out) {
    __try {
        *out = reinterpret_cast<CreateFn>(fn)(storage, requested, req, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

KilnResult KILN_CALL lab_create_raw(uint32_t requested, const uint8_t guid[16], KilnVec3 p, KilnEntity* out) {
    if (!guid || !out || !position_ok(p)) return KILN_ERR_INVALID_ARGUMENT;
    *out = 0;
    if (!R.create || R.storage <= 0) return KILN_ERR_UNAVAILABLE;
    if (!on_world_thread(KILN_WORLD_SERVER)) return KILN_ERR_WRONG_THREAD;
    const uintptr_t world = g.api->world(KILN_WORLD_SERVER);
    if (!world) return KILN_ERR_UNAVAILABLE;
    LabTransform t{};
    t.position[0] = static_cast<int64_t>(p.x * 4294967296.0);
    t.position[1] = static_cast<int64_t>(p.y * 4294967296.0);
    t.position[2] = static_cast<int64_t>(p.z * 4294967296.0);
    t.orientation[3] = 1;
    t.scale[0] = t.scale[1] = t.scale[2] = 1;
    const KilnSpawnComponent comp{kServerTransform.hash, 0, &t, sizeof t};
    LabRequest req{};
    std::memcpy(req.guid, guid, 16);
    req.components = &comp;
    req.count = 1;
    uint32_t id = 0;
    if (!seh_create(R.create, world + static_cast<uintptr_t>(R.storage), requested, &req, &id)) return KILN_ERR_INTERNAL;
    if (!id) return KILN_ERR_NOT_FOUND;
    *out = id;
    return KILN_OK;
}

const KilnMarkersLabApi kLabTable = {{sizeof(KilnMarkersLabApi), KILN_MARKERS_LAB_VERSION}, &lab_follow_now, &lab_stats, &lab_create_raw};

}  // namespace

// ---- glue.h ---------------------------------------------------------------------------------------------------------------

void anchors_start() {
    const KilnApi* api = g.api;
    auto sym = [api](const char* n, uintptr_t* out) { return api->symbol(KilnStr{n, std::strlen(n)}, out) == KILN_OK && *out; };
    auto cst = [api](const char* n, int64_t* out) { return api->constant(KilnStr{n, std::strlen(n)}, out) == KILN_OK; };
    auto feat = [api](const char* n) { return api->feature && api->feature(KilnStr{n, std::strlen(n)}); };
    uintptr_t component = 0;
    const bool common = sym("GetComponent", &component) && sym("spawn/CreateEntity", &R.create) && sym("spawn/DestroyEntity", &R.destroy) &&
                        cst("world.storage", &R.storage) && R.storage > 0;
    if (common) R.get_component = reinterpret_cast<GetComponentFn>(component);
    g_feature[KILN_WORLD_SERVER] = common && feat(kRowServer);
    g_feature[KILN_WORLD_CLIENT] = g.side == KILN_SIDE_CLIENT && common && feat(kRowClient) && sym("net/CreateTemplateCall", &R.net_create) &&
                                   cst("net/create.entity", &R.net_entity) && cst("net/create.parent", &R.net_parent) && R.net_entity > 0 &&
                                   R.net_parent > 0;
    // a lab server's own table (kiln_markers_lab.h): only with kiln/control/markers.lab there at start
    if (g.side == KILN_SIDE_SERVER && control_file("markers.lab")) {
        g_lab = api->register_service(g.owner, KilnStr{KILN_MARKERS_LAB_SERVICE, sizeof(KILN_MARKERS_LAB_SERVICE) - 1}, &kLabTable) == KILN_OK;
        if (g_lab) logf(KILN_LOG_WARN, "markers: kiln.markers.lab is published (kiln/control/markers.lab): a lab only, never a player's server");
    }
}

void anchors_ready() {
    g_types = static_cast<const KilnTypesApi*>(g.api->service(KilnStr{KILN_TYPES_SERVICE, sizeof(KILN_TYPES_SERVICE) - 1}, KILN_TYPES_VERSION));
    bool ok[KILN_WORLD_KINDS] = {};
    uint32_t reason[KILN_WORLD_KINDS] = {KILN_CAP_REASON_SYMBOL_MISSING, KILN_CAP_REASON_SYMBOL_MISSING, KILN_CAP_REASON_SYMBOL_MISSING};
    std::string why[KILN_WORLD_KINDS];
    // the server world: a dedicated server's, or the world a client hosts
    if (!g_feature[KILN_WORLD_SERVER])
        why[KILN_WORLD_SERVER] = "needs the sigdb feature markers.anchors.server (the explicit-world reader, the create and destroy functions, "
                                 "world.storage)";
    else if (!g.lifecycle)
        why[KILN_WORLD_SERVER] = "needs world.lifecycle (an anchor ends with its world)";
    else if (!g.server_post)
        reason[KILN_WORLD_SERVER] = KILN_CAP_REASON_FAULTED, why[KILN_WORLD_SERVER] = "the server world's step could not be subscribed";
    else if (!g_types)
        reason[KILN_WORLD_SERVER] = KILN_CAP_REASON_SERVICE_MISSING, why[KILN_WORLD_SERVER] = "needs kiln.types (the game's reflection) for the layout check";
    else if (std::string w; !layout_check(g_types, KILN_WORLD_SERVER, &w))
        why[KILN_WORLD_SERVER] = "the game's transform layout is not the checked one: " + w;
    else
        ok[KILN_WORLD_SERVER] = true;
    // the client's own world
    if (g.side != KILN_SIDE_CLIENT)
        reason[KILN_WORLD_CLIENT] = KILN_CAP_REASON_WRONG_SIDE, why[KILN_WORLD_CLIENT] = "the client's world exists in a game client only";
    else if (!g_feature[KILN_WORLD_CLIENT])
        why[KILN_WORLD_CLIENT] = "needs the sigdb feature markers.anchors.client (the explicit-world reader, the create and destroy functions, the "
                                 "network create site and its fields)";
    else if (!g.sw || !g.sw->local.load())
        why[KILN_WORLD_CLIENT] = "needs markers.local (world.lifecycle and the client world's step)";
    else if (!g_types)
        reason[KILN_WORLD_CLIENT] = KILN_CAP_REASON_SERVICE_MISSING, why[KILN_WORLD_CLIENT] = "needs kiln.types (the game's reflection) for the layout check";
    else if (std::string w; !layout_check(g_types, KILN_WORLD_CLIENT, &w))
        why[KILN_WORLD_CLIENT] = "the game's render transform layout is not the checked one: " + w;
    else
        ok[KILN_WORLD_CLIENT] = true;
    // the observers: one create link and one destroy link serve both worlds; the network create site the client's
    if (ok[KILN_WORLD_SERVER] || ok[KILN_WORLD_CLIENT]) {
        const bool create = g_create_hook.install(g.api, g.owner, R.create, reinterpret_cast<void*>(&hk_create), "spawn/CreateEntity");
        const bool destroy = create && g_destroy_hook.install(g.api, g.owner, R.destroy, reinterpret_cast<void*>(&hk_destroy), "spawn/DestroyEntity");
        if (!create || !destroy) {
            const std::string e = std::string("the create/destroy observers could not be installed (") +
                                  (create ? g_destroy_hook.error() : g_create_hook.error()) + ")";
            g_create_hook.revoke();
            g_destroy_hook.revoke();
            for (uint32_t k : {static_cast<uint32_t>(KILN_WORLD_SERVER), static_cast<uint32_t>(KILN_WORLD_CLIENT)})
                if (ok[k]) ok[k] = false, reason[k] = KILN_CAP_REASON_FAULTED, why[k] = e;
        }
    }
    if (ok[KILN_WORLD_CLIENT] && !g_net_hook.install(g.api, g.owner, R.net_create, &hk_net_create, "net/CreateTemplateCall", 100)) {
        ok[KILN_WORLD_CLIENT] = false;
        reason[KILN_WORLD_CLIENT] = KILN_CAP_REASON_FAULTED;
        why[KILN_WORLD_CLIENT] = std::string("the network create observer could not be installed (") + g_net_hook.error() + ")";
    }
    g_on[KILN_WORLD_SERVER].store(ok[KILN_WORLD_SERVER]);
    g_on[KILN_WORLD_CLIENT].store(ok[KILN_WORLD_CLIENT]);
    if (g.sw) g.sw->anchors_client.store(ok[KILN_WORLD_CLIENT]);
    if (g.store) g.store->set_follow(&store_acquire, &store_release, nullptr);
    for (uint32_t k : {static_cast<uint32_t>(KILN_WORLD_SERVER), static_cast<uint32_t>(KILN_WORLD_CLIENT)}) {
        if (ok[k])
            report(row_of(k), KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, "");
        else
            report(row_of(k), KILN_CAP_UNAVAILABLE, reason[k], why[k].c_str());
    }
    logf(KILN_LOG_INFO, "markers: anchors on the server world %s; on the client world %s",
         ok[KILN_WORLD_SERVER] ? "on (CurrentTransform checked, create/destroy observed)" : ("off: " + why[KILN_WORLD_SERVER]).c_str(),
         ok[KILN_WORLD_CLIENT] ? "on (RenderTransform checked, create/destroy and network create observed)" : ("off: " + why[KILN_WORLD_CLIENT]).c_str());
}

void anchors_world(const KilnWorldEvent& e) {
    if (e.kind != KILN_WORLD_SERVER && e.kind != KILN_WORLD_CLIENT) return;
    if (e.event == KILN_WORLD_STARTED) {
        uint64_t epoch = 0;
        if (g.store) g.store->world_epoch(e.kind, &epoch);
        g_w.world_started(e.kind, R.storage > 0 && e.world ? e.world + static_cast<uintptr_t>(R.storage) : 0, epoch);
    } else if (e.event == KILN_WORLD_STOPPING) {
        const Watches::Counts c = g_w.counts();
        if (g_on[e.kind].load() || c.leases) {
            uint32_t other = 0;
            const uint32_t threads = g_lab ? count_threads(&other) : 0;
            char extra[96] = "";
            if (g_lab) std::snprintf(extra, sizeof extra, "; observers on %u thread(s), %u of them not a world's own", threads, other);
            logf(KILN_LOG_INFO,
                 "markers: anchors at the %s world's stop: %llu captures, %u leases, %u watched; observers saw %llu creates, %llu destroys, %llu "
                 "network creates; %llu incarnations ended%s",
                 world_name(e.kind), static_cast<unsigned long long>(g_obs.captures.load()), c.leases, c.live,
                 static_cast<unsigned long long>(g_obs.creates.load()), static_cast<unsigned long long>(g_obs.destroys.load()),
                 static_cast<unsigned long long>(g_obs.nets.load()), static_cast<unsigned long long>(c.ended), extra);
        }
        g_w.world_stopping(e.kind);  // before its storage can be reused; the records' references go with the store's clear
    }
}

void anchors_post(uint32_t kind) {
    if (kind != KILN_WORLD_SERVER && kind != KILN_WORLD_CLIENT) return;
    g_obs.world_thread[kind].store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (kind == KILN_WORLD_CLIENT && g_net_faulted.load() && !g_net_fault_told.exchange(true)) {
        const char* why = "the network create observer could not read the created entity from the game's frame";
        report(kRowClient, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED, why);
        logf(KILN_LOG_WARN, "markers: markers.anchors.client is off: %s; followed markers show AnchorEnded, fixed markers keep working", why);
    }
    if (g_lab && !g_obs.worker_told.load()) {
        uint32_t other = 0;
        count_threads(&other);
        if (other && !g_obs.worker_told.exchange(true))
            logf(KILN_LOG_INFO, "markers: anchors (lab): the create/destroy observers ran on a thread that is not a world's own (the game's workers)");
    }
    if (!g_on[kind].load()) return;
    static Watches::ToRead buf[2][kWatchSlots];  // one per world kind: each kind's Post runs on one thread
    Watches::ToRead* b = buf[kind == KILN_WORLD_CLIENT];
    const uint32_t n = g_w.to_read(kind, b, kWatchSlots);
    if (!n) return;
    const uintptr_t world = g.api->world(kind);
    for (uint32_t i = 0; i < n; ++i) {
        const TransformRead r = read_transform(R.get_component, world, kind, b[i].entity);
        if (r.kind == ReadKind::Fault || r.kind == ReadKind::WrongSize) {
            lower(kind, r.kind == ReadKind::Fault ? "a follow read faulted" : "a transform had another size than the checked layout");
            return;
        }
        g_w.read_done(b[i], r);
    }
}

KilnResult anchors_capture(KilnOwner owner, uint32_t kind, uint64_t expected_epoch, KilnEntity entity, KilnMarkerAnchorResult* out) {
    if (!out || out->size < sizeof(KilnMarkerAnchorResult)) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof(KilnMarkerAnchorResult));
    out->size = size;
    out->world_kind = kind;
    out->entity = entity;
    auto bad = [out](const char* why) {
        std::snprintf(out->reason, sizeof out->reason, "%s", why);
        return KILN_ERR_INVALID_ARGUMENT;
    };
    if (!owner) return bad("owner 0 cannot hold a lease");
    if (kind != KILN_WORLD_SERVER && kind != KILN_WORLD_CLIENT) return bad("world_kind must be KILN_WORLD_SERVER (0) or KILN_WORLD_CLIENT (2)");
    if (!entity) return bad("entity 0 is no entity");
    if (!g_on[kind].load()) {
        std::snprintf(out->reason, sizeof out->reason, "%s", row_of(kind));
        return KILN_ERR_UNAVAILABLE;
    }
    if (!on_world_thread(kind)) {
        std::snprintf(out->reason, sizeof out->reason, "capture_anchor runs on the %s world's thread between its steps", world_name(kind));
        return KILN_ERR_WRONG_THREAD;
    }
    uint64_t current = 0;
    if (g.store) g.store->world_epoch(kind, &current);
    ReadNote seen;
    capture_into(g_w, owner, kind, expected_epoch, current, entity, &capture_read, &seen, out);
    if (out->status == KILN_MARKER_ACCEPTED) g_obs.captures.fetch_add(1, std::memory_order_relaxed);
    if (seen.kind == ReadKind::Fault) lower(kind, "an explicit-world read faulted");
    else if (seen.kind == ReadKind::WrongSize) lower(kind, "a transform had another size than the checked layout");
    return KILN_OK;
}

KilnResult anchors_release(KilnOwner owner, KilnMarkerAnchor anchor) {
    if (!owner || !anchor) return KILN_ERR_INVALID_ARGUMENT;
    return g_w.release(owner, anchor);
}

void anchors_release_owner(KilnOwner owner) {
    if (owner) g_w.release_owner(owner);
}

bool anchors_follow(uint64_t serial, KilnVec3* position, uint32_t* reason) { return g_w.follow(serial, position, reason); }

uint32_t anchors_retain(KilnOwner owner, uint32_t world_kind, KilnMarkerAnchor lease, uint64_t* serial, char* why, size_t cap) {
    if (world_kind != KILN_WORLD_SERVER && world_kind != KILN_WORLD_CLIENT) {
        if (serial) *serial = 0;
        if (why && cap) std::snprintf(why, cap, "InvalidAnchor: no world of kind %u", world_kind);
        return KILN_MARKER_INVALID_ANCHOR;
    }
    return g_w.retain(owner, world_kind, lease, serial, why, cap);
}

void anchors_drop(uint64_t serial) { g_w.drop(serial); }

}  // namespace kmk

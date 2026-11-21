// SPDX-License-Identifier: MIT
// kiln.abilities: the impact pool watch (since 0.2.0-dev.29.19; inbox 116). Read-only: it never writes game memory,
// hooks nothing of its own and calls no game function but the getters kiln.abilities already uses (GetComponent,
// getProgram). Pure parts and their reasons: impact_watch.h. Layout and evidence: docs/re/impact.md "The impact memory
// pool". What players and support read: docs/kiln/services/abilities.md "Impact memory watch".
//
// Threads: on_created runs on the game's job threads inside createImpactInstance (kiln.abilities' hk_create): two
// relaxed loads, one GetTickCount64 and one lock-free table write; no lock, no allocation. Everything else runs in a
// world's Pre on that world's thread (the server world: the tick thread; a client's own world: its main thread), when
// none of that world's jobs run. Per-world state is touched only by its own world's thread.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "engine.h"
#include "impact_watch.h"
#include "kiln_api.h"
#include "services/kiln_spawn.h"

namespace kab::iw {
namespace {

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
VanillaFn g_is_vanilla = nullptr;
const KilnSpawnApi* g_spawn = nullptr;
bool g_spawn_looked = false;

// sigdb (sigdb/symbols/server.d/impact-pool.json, feature impact.pool)
int64_t c_used = -1, c_cap = -1, c_pool = -1, c_blocks = -1, c_block_count = -1, c_pool_blocks = -1, c_live = -1,
        c_peak = -1, c_block_slots = -1, c_block_cap = -1, c_block_live = -1, c_slot_program = -1;
int64_t c_world_storage = -1, c_storage_clock = -1, c_storage_period = -1;  // the ECS clock (LifeTime's end)
constexpr uint64_t kBlockStride = 0x40, kSlotSize = 0x200;  // the reporter's shl 6 / shl 9 (in the patterns)
constexpr uint32_t kLifeTime = 0xf22c253cu;                  // keen::ecs::LifeTime {lifeTime, endOfLife, enforce, prevent, triggered}
constexpr uint64_t kMaxBlocks = 256, kMaxSlotsPerBlock = 1u << 16, kMaxVisits = 1u << 18;

std::atomic<bool> g_on{false};       // the hot path records creation times
std::atomic<bool> g_faulted{false};  // a read faulted: off for the process
std::atomic<bool> g_ctl_off{false};  // kiln/control/impacts says "off"
std::atomic<uintptr_t> g_sys[2] = {};  // the server world's and the client world's ImpactSystem (table index 1, 2)
std::atomic<uint32_t> g_dump_gen{0};
std::atomic<uint64_t> g_next_control{0};
std::wstring g_control_file;

AgeTable<16384, 64> g_ages;  // 256 KiB, constant-initialised

struct World {
    uintptr_t world = 0, sys = 0;
    uint32_t session = 0;  // KilnWorldEvent.session of the world this state is for (0: no start seen)
    bool announced = false, layout_bad = false, oldest_done = false;
    Thresholds th;
    uint64_t next_ms = 0;
    uint32_t dump_seen = 0;
};
World g_world[KILN_WORLD_KINDS];
std::atomic<uint32_t> g_session[KILN_WORLD_KINDS] = {};  // each kind's last KilnWorldEvent.session at its start

ProgramCounts<2048> g_counts;  // tick-thread work areas (one dump at a time: see g_dump_busy)
OldestK<20> g_oldest;
std::atomic<bool> g_dump_busy{false};

void logv(int level, const char* fmt, va_list ap) {
    char buf[1400];
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return;
    g_api->log(g_owner, level, KilnStr{buf, static_cast<size_t>(std::min(n, static_cast<int>(sizeof buf) - 1))});
}
void logi(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logv(KILN_LOG_INFO, fmt, ap);
    va_end(ap);
}
void logw(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logv(KILN_LOG_WARN, fmt, ap);
    va_end(ap);
}

const char* kind_name(uint32_t kind) { return kind == KILN_WORLD_SERVER ? "server" : kind == KILN_WORLD_CLIENT ? "client" : "prediction"; }
uint32_t index_of(uint32_t kind) { return kind == KILN_WORLD_SERVER ? 1 : 2; }
uint32_t now_ms() { return static_cast<uint32_t>(GetTickCount64()); }

void fault(const char* what) {
    if (g_faulted.exchange(true)) return;
    g_on.store(false);
    logw("kiln.abilities: impact pool watch: reading %s faulted; the watch is off until the game restarts (the game is "
         "not affected)", what);
}

template <class T>
bool rd(uintptr_t p, T* out) {
    return p && read_bytes(reinterpret_cast<const void*>(p), out, sizeof(T));
}

bool read_pool(uintptr_t sys, Reading* r) {
    uintptr_t alloc = 0;
    uint64_t up[2] = {};
    if (!rd(sys, &alloc) || !alloc || !read_bytes(reinterpret_cast<const void*>(alloc + c_used), up, sizeof up) ||
        !rd(alloc + c_cap, &r->capacity) || !rd(sys + c_pool + c_live, &r->live) || !rd(sys + c_pool + c_peak, &r->live_peak))
        return false;
    r->used = up[0];
    r->peak = up[1];  // 8 bytes after "used": the reporter's stats copy (16-byte moves) and its Peak
    return true;
}

// Calls fn(slot) for every live slot of the instance pool, as the game's own report walks it. False on a fault.
template <class Fn>
bool walk(uintptr_t sys, Fn&& fn, uint64_t* visited) {
    uintptr_t blocks = 0;
    uint64_t count = 0;
    *visited = 0;
    if (!rd(sys + c_blocks, &blocks) || !rd(sys + c_block_count, &count)) return false;
    if (!blocks) return true;
    count = std::min<uint64_t>(count, kMaxBlocks);
    for (uint64_t b = 0; b < count; ++b) {
        const uintptr_t block = blocks + b * kBlockStride;
        uint64_t live = 0, cap = 0;
        uintptr_t base = 0;
        if (!rd(block + c_block_live, &live) || !rd(block + c_block_cap, &cap) || !rd(block + c_block_slots, &base)) return false;
        if (!live || !base) continue;
        cap = std::min<uint64_t>(cap, kMaxSlotsPerBlock);
        for (uint64_t s = 0; s < cap; ++s) {
            const uintptr_t slot = base + s * kSlotSize;
            uint32_t hdr = 0;
            if (!rd(slot, &hdr)) return false;
            if (hdr >> 31) continue;  // free
            fn(slot);
            if (++*visited >= kMaxVisits) return true;
        }
    }
    return true;
}

std::string guid_of(uintptr_t sys, uint32_t program) {
    uintptr_t reg = 0;
    uint8_t b[16];
    const uint8_t* p = rd(sys + E.system_registry, &reg) ? get_program(reg, program) : nullptr;
    char s[48];
    if (!p || !read_bytes(p + 4, b, 16)) {
        std::snprintf(s, sizeof s, "program 0x%08x", program);
        return s;
    }
    std::snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[3], b[2], b[1],
                  b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

std::string label_of(const std::string& guid, uint32_t program) {
    if (guid == "0c3a7176-9336-47a3-ae98-d9433ac5ffbb") return " (the game's generic melee hit: weapons, tools, most enemies)";
    bool known = false;
    const bool vanilla = g_is_vanilla && g_is_vanilla(program, &known);
    return known && !vanilla ? " (not in the game's list: a mod's program)" : "";
}

int64_t ecs_now(uintptr_t world) {
    if (!world || c_world_storage < 0 || c_storage_clock < 0 || c_storage_period < 0) return 0;
    const uintptr_t s = world + static_cast<uintptr_t>(c_world_storage);
    int64_t period = 0;
    uint32_t tick = 0;
    if (!rd(s + c_storage_period, &period) || !rd(s + c_storage_clock, &tick)) return 0;
    return period * static_cast<int64_t>(tick);
}

const KilnSpawnApi* spawn() {
    if (!g_spawn_looked) {
        g_spawn_looked = true;
        g_spawn = static_cast<const KilnSpawnApi*>(g_api->service(KilnStr{KILN_SPAWN_SERVICE, sizeof KILN_SPAWN_SERVICE - 1}, 1));
    }
    return g_spawn;
}

// "50123 (exists, Enemy_Vukah_Brawler)" on the server world; client world entities: the id.
std::string entity_text(uint32_t kind, uint32_t entity) {
    char b[200];
    if (!entity) return "none";
    const KilnSpawnApi* sp = kind == KILN_WORLD_SERVER ? spawn() : nullptr;
    if (!sp) {
        std::snprintf(b, sizeof b, "%u", entity);
        return b;
    }
    KilnEntitySnapshot snap{};
    snap.size = sizeof snap;
    if (sp->entity_info(entity, &snap) != KILN_OK) {
        std::snprintf(b, sizeof b, "%u (gone)", entity);
        return b;
    }
    KilnTemplateInfo t{};
    t.size = sizeof t;
    const bool named = snap.has_template && sp->template_get(&snap.template_guid, &t) == KILN_OK && t.name[0];
    std::snprintf(b, sizeof b, "%u (exists%s%s)", entity, named ? ", " : ", no template", named ? t.name : "");
    return b;
}

void log_instance(uint32_t kind, uintptr_t world, size_t n, uintptr_t slot, int64_t t, uint32_t now) {
    const uintptr_t inst = slot + static_cast<uintptr_t>(c_slot_program - E.inst_program);
    uint32_t id = 0, entity = 0;
    if (!rd(inst, &id) || !rd(inst + E.inst_entity, &entity)) return fault("an impact instance");
    char age[48] = "age unknown";
    if (t != kUnknownAge) std::snprintf(age, sizeof age, "age %.1f s", static_cast<double>(-t) / 1000.0);
    // LifeTime of the hit entity (the countdown that should have ended it) and ImpactHierarchy's root (the attacker)
    char life[160] = "no LifeTime";
    uint64_t size = 0;
    if (const auto* lt = static_cast<const uint8_t*>(component(world, kLifeTime, entity, &size)); lt && size >= 19) {
        int64_t v[2] = {};
        uint8_t f[3] = {};
        if (read_bytes(lt, v, sizeof v) && read_bytes(lt + 16, f, 3)) {
            const int64_t now_t = ecs_now(world);
            if (now_t)
                std::snprintf(life, sizeof life, "LifeTime %.2f s, ends in %.2f s (enforce %u, prevent %u, triggered %u)",
                              static_cast<double>(v[0]) / 1e9, static_cast<double>(v[1] - now_t) / 1e9, f[0], f[1], f[2]);
            else
                std::snprintf(life, sizeof life, "LifeTime %.2f s (enforce %u, prevent %u, triggered %u)", static_cast<double>(v[0]) / 1e9,
                              f[0], f[1], f[2]);
        }
    }
    uint32_t root = 0;
    if (const auto* h = static_cast<const uint8_t*>(component(world, kImpactHierarchy, entity, &size)); h && size >= 8)
        read_bytes(h + 4, &root, 4);
    logi("kiln.abilities: impact pool (%s world): [%zu] impact 0x%08x, %s; hit entity %s, %s; attacker/owner %s", kind_name(kind),
         n, id, age, entity_text(kind, entity).c_str(), life, entity_text(kind, root).c_str());
    (void)now;
}

// The top programs by live instances (and, with oldest, up to 20 of the top program's oldest instances).
bool dump(uint32_t kind, World& w, const Reading& r, const char* why, bool oldest) {
    if (g_dump_busy.exchange(true)) return false;  // the other world's thread is dumping: next reading
    struct Release {
        ~Release() { g_dump_busy.store(false); }
    } release;
    g_counts.clear();
    uint64_t visited = 0;
    if (!walk(w.sys, [&](uintptr_t slot) {
            uint32_t program = 0;
            if (rd(slot + c_slot_program, &program)) g_counts.add(program);
        }, &visited)) {
        fault("the instance pool");
        return true;
    }
    Top top[5];
    const size_t n = g_counts.top(top, 5);
    std::string list;
    for (size_t i = 0; i < n; ++i) {
        const std::string g = guid_of(w.sys, top[i].id);
        char b[64];
        std::snprintf(b, sizeof b, "%s%u x ", i ? "; " : "", top[i].count);
        list += b + g + label_of(g, top[i].id);
    }
    logi("kiln.abilities: impact pool (%s world, %s): %.1f%% of %.0f MiB used, %llu live instances; top programs by live "
         "instances: %s (walked %llu instances, %zu programs%s)", kind_name(kind), why, used_pct(r),
         static_cast<double>(r.capacity) / (1024.0 * 1024.0), static_cast<unsigned long long>(r.live),
         n ? list.c_str() : "none", static_cast<unsigned long long>(visited), g_counts.distinct(),
         g_counts.overflow() ? ", more than the list holds" : "");
    if (!oldest || !n) return true;
    g_oldest.clear();
    const uint32_t now = now_ms(), want = top[0].id, idx = index_of(kind);
    if (!walk(w.sys, [&](uintptr_t slot) {
            uint32_t program = 0, id = 0;
            if (!rd(slot + c_slot_program, &program) || program != want) return;
            int64_t t = kUnknownAge;
            if (rd(slot + static_cast<uintptr_t>(c_slot_program - E.inst_program), &id))
                if (const auto* e = g_ages.find(key_of(idx, id))) {
                    const uint64_t s = e->stamp.load(std::memory_order_acquire);
                    if (stamp_id(s) == id) t = -static_cast<int64_t>(age_ms(stamp_ms(s), now));
                }
            g_oldest.offer(slot, t);
        }, &visited)) {
        fault("the instance pool");
        return true;
    }
    logi("kiln.abilities: impact pool (%s world): the %zu oldest live instances of %s (%u live; creation times tracked for "
         "%zu instances, %llu not tracked because the table was full):", kind_name(kind), g_oldest.size(),
         guid_of(w.sys, want).c_str(), top[0].count, g_ages.used(), static_cast<unsigned long long>(g_ages.full()));
    for (size_t i = 0; i < g_oldest.size(); ++i) log_instance(kind, w.world, i + 1, static_cast<uintptr_t>(g_oldest.at(i).key), g_oldest.at(i).t, now);
    return true;
}

void check_control() {
    const uint64_t t = GetTickCount64();
    uint64_t due = g_next_control.load();
    if (t < due || !g_next_control.compare_exchange_strong(due, t + 2000)) return;  // one thread, every 2 s
    if (g_control_file.empty()) return;
    char buf[64] = {};
    DWORD got = 0;
    const HANDLE h = CreateFileW(g_control_file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Control c = Control::None;
    if (h != INVALID_HANDLE_VALUE) {
        if (ReadFile(h, buf, sizeof buf - 1, &got, nullptr)) c = parse_control(std::string_view(buf, got));
        CloseHandle(h);
    }
    if (c == Control::Dump) {
        DeleteFileW(g_control_file.c_str());
        g_dump_gen.fetch_add(1);
        logi("kiln.abilities: impact pool watch: dump requested (kiln/control/impacts)");
    }
    const bool off = c == Control::Off;
    if (off != g_ctl_off.load()) {
        g_ctl_off.store(off);
        logi(off ? "kiln.abilities: impact pool watch: off (kiln/control/impacts says off)"
                 : "kiln.abilities: impact pool watch: on again (kiln/control/impacts no longer says off)");
    }
    g_on.store(!off && !g_faulted.load());
}

void step(uint32_t kind) {
    if (g_faulted.load(std::memory_order_relaxed)) return;
    check_control();
    if (g_ctl_off.load(std::memory_order_relaxed)) return;
    World& w = g_world[kind];
    const uintptr_t world = g_api->world(kind);
    if (!world) return;
    // a new session of this world kind: by its session number, as a new world can sit at the old one's address (#108)
    const uint32_t session = g_session[kind].load(std::memory_order_relaxed);
    if (world != w.world || session != w.session) {
        const uint32_t idx = index_of(kind);
        g_sys[idx - 1].store(0);
        g_ages.clear_system(idx);
        w = World{};
        w.world = world;
        w.session = session;
        w.dump_seen = g_dump_gen.load();
    }
    if (w.layout_bad) return;
    const uint32_t now = now_ms();
    const bool dump_asked = w.dump_seen != g_dump_gen.load();
    if (!dump_asked && static_cast<int32_t>(now - static_cast<uint32_t>(w.next_ms)) < 0) return;
    w.next_ms = now + 500;
    if (!w.sys) {
        const char* n = "impact::ImpactSystem";
        w.sys = g_api->singleton(world, KilnStr{n, std::strlen(n)});
        if (!w.sys) return;
    }
    Reading r{};
    if (!read_pool(w.sys, &r)) return fault("the impact pool's counters");
    if (!w.announced) {
        w.announced = true;
        const Layout l = check_layout(r);
        if (l != Layout::Ok) {
            w.layout_bad = true;
            logw("kiln.abilities: impact pool watch off for the %s world: the pool's layout check failed (%s: used %llu, peak "
                 "%llu, capacity %llu, live %llu, peak live %llu); this game build may differ", kind_name(kind), layout_text(l),
                 static_cast<unsigned long long>(r.used), static_cast<unsigned long long>(r.peak),
                 static_cast<unsigned long long>(r.capacity), static_cast<unsigned long long>(r.live),
                 static_cast<unsigned long long>(r.live_peak));
            return;
        }
        g_sys[index_of(kind) - 1].store(w.sys);
        logi("kiln.abilities: impact pool watch on for the %s world: %.0f MiB, %.1f%% used (peak %.1f%%), %llu live impact "
             "instances (peak %llu); kiln.log warns at 50/75/90%%", kind_name(kind), static_cast<double>(r.capacity) / (1024.0 * 1024.0),
             used_pct(r), peak_pct(r), static_cast<unsigned long long>(r.live), static_cast<unsigned long long>(r.live_peak));
    }
    const double pct = used_pct(r);
    const uint32_t ev = w.th.step(pct);
    if (ev & (kWarn50 | kWarn75 | kWarn90)) {
        logw("kiln.abilities: the %s world's impact memory is %d%% full (%.1f%%: %.1f of %.0f MiB, peak %.1f%%; %llu live impact "
             "instances, peak %llu). When it is full the game drops new hits and attacks (\"could not allocate impact\" in its "
             "log), the world slows down and the game can crash", kind_name(kind), highest(ev), pct,
             static_cast<double>(r.used) / (1024.0 * 1024.0), static_cast<double>(r.capacity) / (1024.0 * 1024.0), peak_pct(r),
             static_cast<unsigned long long>(r.live), static_cast<unsigned long long>(r.live_peak));
        const bool oldest = (ev & (kWarn75 | kWarn90)) && !w.oldest_done;
        char why[32];
        std::snprintf(why, sizeof why, "%d%% warning", highest(ev));
        if (dump(kind, w, r, why, oldest) && oldest) w.oldest_done = true;
    }
    if (ev & kRecovered)
        logi("kiln.abilities: the %s world's impact memory is back under 40%% (%.1f%%; %llu live impact instances)", kind_name(kind),
             pct, static_cast<unsigned long long>(r.live));
    if (dump_asked && !g_faulted.load()) {
        const uint32_t gen = g_dump_gen.load();
        if (dump(kind, w, r, "requested", true)) w.dump_seen = gen;
    }
}

void KILN_CALL on_pre(const KilnTickInfo* tick, void*) { step(tick && tick->size >= 8 ? tick->world_kind : KILN_WORLD_SERVER); }

int64_t cst(const char* name) {
    int64_t v = -1;
    return g_api->constant(KilnStr{name, std::strlen(name)}, &v) == KILN_OK ? v : -1;
}

// World starts (on that world's thread, before its first Pre): the session step() keys its per-world state by.
void KILN_CALL on_world(const KilnWorldEvent* ev, void*) {
    if (ev && ev->event == KILN_WORLD_STARTED && ev->kind < KILN_WORLD_KINDS)
        g_session[ev->kind].store(ev->session, std::memory_order_relaxed);
}

}  // namespace

void init(const KilnApi* api, KilnOwner owner, VanillaFn is_vanilla) {
    g_api = api;
    g_owner = owner;
    g_is_vanilla = is_vanilla;
    if (!api->feature(KilnStr{"impact.pool", 11}) || !E.get_component) {
        logi("kiln.abilities: impact pool watch unavailable on this build (sigdb feature impact.pool)");
        return;
    }
    c_used = cst("impact/alloc.used");
    c_cap = cst("impact/alloc.capacity");
    c_pool = cst("impact/sys.pool");
    c_blocks = cst("impact/sys.blocks");
    c_block_count = cst("impact/sys.block_count");
    c_pool_blocks = cst("impact/pool.blocks");
    c_live = cst("impact/pool.live");
    c_peak = cst("impact/pool.peak");
    c_block_slots = cst("impact/block.slots");
    c_block_cap = cst("impact/block.capacity");
    c_block_live = cst("impact/block.live");
    c_slot_program = cst("impact/slot.program");
    c_world_storage = cst("world.storage");
    c_storage_clock = cst("storage.clock");
    c_storage_period = cst("storage.clock_period");
    // the walk's two sources must agree: the report's blocks/count (ImpactSystem-relative) and PoolAlloc's (pool-relative)
    if (c_used < 0 || c_cap < 0 || c_pool <= 0 || c_live < 0 || c_peak < 0 || c_blocks != c_pool + c_pool_blocks ||
        c_block_count != c_blocks + 8 || c_block_slots < 0 || c_block_cap < 0 || c_block_live < 0 ||
        c_slot_program != E.inst_program + 8) {
        logw("kiln.abilities: impact pool watch off: its sigdb offsets disagree (pool 0x%llx, blocks 0x%llx/0x%llx, slot "
             "program 0x%llx)", static_cast<unsigned long long>(c_pool), static_cast<unsigned long long>(c_blocks),
             static_cast<unsigned long long>(c_pool_blocks), static_cast<unsigned long long>(c_slot_program));
        return;
    }
    char path[1024];
    size_t n = 0;
    if (api->path(KILN_PATH_CONTROL, path, sizeof path, &n) == KILN_OK && n < sizeof path)
        g_control_file = (std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path), n)) / L"impacts").wstring();
    KilnHandle h = 0;
    api->subscribe_world(owner, &on_world, nullptr, &h);
    api->subscribe_tick(owner, KILN_TICK_PRE, &on_pre, nullptr, &h);
    if (api->size >= offsetof(KilnApi, subscribe_world_tick) + sizeof(void*) && api->subscribe_world_tick)
        api->subscribe_world_tick(owner, KILN_WORLD_CLIENT, KILN_TICK_PRE, &on_pre, nullptr, &h);
    g_on.store(true);
}

void on_created(uintptr_t system, uint32_t id) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    const uint32_t idx = system == g_sys[0].load(std::memory_order_relaxed) ? 1 : system == g_sys[1].load(std::memory_order_relaxed) ? 2 : 0;
    if (idx) g_ages.insert_id(key_of(idx, id), id, now_ms());
}

}  // namespace kab::iw

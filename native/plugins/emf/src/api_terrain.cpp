// SPDX-License-Identifier: MIT
// EMF v44/v45/v47 block calls translated onto kiln.build. No engine functions or memory layouts live here.
// Queue limits, validation and sphere spacing follow EMF 0.6.19 by matenm and the EMF contributors (MIT;
// licenses/EMF-LICENSE.txt): src/core/block_rules.cpp, block_service.cpp, block_glue.cpp and api.cpp.
// Since 0.2.0-dev.30.6. Engine availability and protection decisions belong to kiln.build, not this adapter.
#include "emf_internal.h"
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace kemf {
namespace {
constexpr const char* kUnavailable = "not available on this game version";
constexpr const char* kTerrainUnavailable = "breaking terrain is not available on this game version";
constexpr const char* kFaulted = "off (a block write faulted or could not be verified); breaks are refused until a restart";
struct Sphere { float x, y, z, radius; };
struct Request {
    uint32_t id = 0;
    uint64_t session = 0;
    Mod* mod = nullptr;
    EmfBreakCallback cb = nullptr;
    void* user = nullptr;
    KilnBuildBreak spec{}; // owns its material bitset; no caller buffers kept
    std::vector<Sphere> spheres;
};
struct Done { Request request; EmfBreakResult result; };
std::mutex g_mutex;
std::vector<Request> g_waiting;
std::vector<Done> g_done;
uint32_t g_next_id = 1;
bool g_stopping = false, g_faulted = false;
uint64_t g_last_run = 0;
uint64_t g_accepted = 0, g_completed = 0, g_cancelled = 0, g_failed = 0, g_partial = 0;

bool has_build(size_t end) { return S.build && S.build->header.size >= end; }
uint64_t session() {
    return S.world && S.world->header.size >= offsetof(KilnWorldApi, game_world_session) + sizeof(void*) &&
           S.world->game_world_session ? S.world->game_world_session(KILN_WORLD_SERVER) : 0;
}
bool server_world(EmfWorld* w) { return w && g_api && g_api->world && reinterpret_cast<uintptr_t>(w) == g_api->world(KILN_WORLD_SERVER); }
bool world_callback(EmfWorld* w) {
    return server_world(w) && t_ctx.world_cb && t_ctx.world == w && g_api->on_tick_thread && g_api->on_tick_thread();
}
bool cap(const char* name) {
    KilnCapRow r{}; r.size = sizeof r;
    return S.caps && S.caps->header.size >= offsetof(KilnCapsApi, find) + sizeof(void*) && S.caps->find &&
           S.caps->find(kstr(name), &r) == KILN_OK && r.state == KILN_CAP_AVAILABLE;
}
const char* mode_unavailable(uint32_t flags) {
    if (!has_build(offsetof(KilnBuildApi, break_box) + sizeof(void*)) || !S.build->break_box || !S.build->grid ||
        !cap("build.break_building") || !cap("build.block_metadata")) return kUnavailable;
    if (!(flags & EMF_BREAK_WORLD) && !cap("build.block_provenance")) return kUnavailable;
    if (!(flags & EMF_BREAK_INSTANT) && !cap("build.break_damage")) return kUnavailable;
    if ((flags & EMF_BREAK_DROPS) && !cap("build.break_drops")) return kUnavailable;
    if ((flags & EMF_BREAK_TERRAIN) && (!cap("build.break_terrain") || !cap("build.block_provenance"))) return kTerrainUnavailable;
    return nullptr;
}
const char* query_why(uint32_t reason) {
    switch (reason) {
    case KILN_BUILD_BLOCK_NOT_LOADED: return "that place is not loaded";
    case KILN_BUILD_BLOCK_NO_SESSION: return "the world has no voxel editor yet";
    case KILN_BUILD_BLOCK_FAULTED: return "block reads are off (kiln.build faulted); restart required";
    default: return kUnavailable;
    }
}
void put_info(EmfBlockInfo* out, const KilnBuildBlockInfo& b) {
    EmfBlockInfo e{};
    e.size = out->size;
    e.kind = b.kind;
    e.material = b.kind == KILN_BUILD_BLOCK_AIR ? 0 : b.kind == KILN_BUILD_BLOCK_BUILDING ? b.material - 128 : b.material;
    e.fill = b.fill;
    e.hardness = b.kind == KILN_BUILD_BLOCK_AIR || b.hardness < 0 ? 0 : static_cast<uint32_t>(b.hardness);
    e.health = b.kind == KILN_BUILD_BLOCK_AIR ? 0 : std::max(0, b.health); // raw material health, NOT terrain's damage-mode x250 scale
    e.damage = b.kind == KILN_BUILD_BLOCK_BUILDING ? b.damage : -1;
    e.x = static_cast<float>(b.center.x); e.y = static_cast<float>(b.center.y); e.z = static_cast<float>(b.center.z);
    e.world = b.kind == KILN_BUILD_BLOCK_AIR ? 0 : b.provenance;
    std::memcpy(out, &e, std::min<size_t>(out->size, sizeof e));
}
bool api_block_at(EmfWorld* w, float x, float y, float z, EmfBlockInfo* out) {
    Mod* m = caller_mod(_ReturnAddress());
    note_call("block_at");
    if (!out || out->size < 40) return false;
    if (!has_build(offsetof(KilnBuildApi, block_at) + sizeof(void*)) || !S.build->block_at) {
        unsupported(m, "block_at", kUnavailable); return false;
    }
    if (!world_callback(w)) {
        refuse(m, "block_at", "server world only, inside a world callback on the world passed to it", true); return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        refuse(m, "block_at", "the point is not a number", true); return false;
    }
    KilnBuildBlockInfo b{}; b.size = sizeof b;
    if (S.build->block_at({x,y,z}, &b) != KILN_OK || b.refusal != KILN_BUILD_BLOCK_NONE) {
        refuse(m, "block_at", query_why(b.refusal), true); return false;
    }
    put_info(out, b); return true;
}
bool find(Mod* m, const char* fn, EmfWorld* w, const float* from, const float* to, uint32_t flags, EmfBlockInfo* out) {
    note_call(fn);
    if (!out || out->size < 40 || !from || !to) return false;
    if (!has_build(offsetof(KilnBuildApi, find_wall) + sizeof(void*)) || !S.build->find_wall) {
        unsupported(m, fn, kUnavailable); return false;
    }
    if (!world_callback(w)) {
        refuse(m, fn, "server world only, inside a world callback on the world passed to it", true); return false;
    }
    if (flags & ~EMF_FIND_TERRAIN) { refuse(m, fn, "unknown flags", true); return false; }
    for (int i = 0; i < 3; ++i) if (!std::isfinite(from[i]) || !std::isfinite(to[i])) {
        refuse(m, fn, "a point is not a number", true); return false;
    }
    KilnBuildBlockInfo b{}; b.size = sizeof b;
    auto rc = S.build->find_wall({from[0],from[1],from[2]}, {to[0],to[1],to[2]},
                                flags & EMF_FIND_TERRAIN ? KILN_BUILD_FIND_PLAYER_TERRAIN : 0, &b);
    if (rc != KILN_OK || b.refusal != KILN_BUILD_BLOCK_NONE) {
        if (rc != KILN_ERR_NOT_FOUND || b.refusal != KILN_BUILD_BLOCK_NONE) refuse(m, fn, query_why(b.refusal), true);
        return false;
    }
    put_info(out, b); return true;
}
bool api_find_wall(EmfWorld* w, const float* from, const float* to, EmfBlockInfo* out) {
    return find(caller_mod(_ReturnAddress()), "find_wall", w, from, to, 0, out);
}
bool api_find_wall_ex(EmfWorld* w, const float* from, const float* to, uint32_t flags, EmfBlockInfo* out) {
    return find(caller_mod(_ReturnAddress()), "find_wall_ex", w, from, to, flags, out);
}
std::string check(const EmfBreakBlocks& r) {
    if (r.flags & ~(EMF_BREAK_INSTANT | EMF_BREAK_DROPS | EMF_BREAK_WORLD | EMF_BREAK_TERRAIN)) return "unknown flags";
    if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.z)) return "the point is not a number";
    if (!std::isfinite(r.radius) || !std::isfinite(r.height)) return "radius or height is not a number";
    const float radius = r.radius == 0 ? 1 : r.radius;
    char text[120];
    if (radius < .25f || radius > 3.5f) {
        std::snprintf(text, sizeof text, "radius %.2f m: it must be 0.25 to 3.5 m (0 = 1 m)", radius); return text;
    }
    if (r.height < 0 || r.height > 4) {
        std::snprintf(text, sizeof text, "height %.2f m: it must be 0 to 4 m", r.height); return text;
    }
    if (r.height > 0 && 1 + std::ceil(r.height / radius - 1e-4f) > 8) return "at most 8 spheres: height / radius at most 7";
    for (auto d : r.damage) if (d < 0 || d > 100000) return "damage must be 0 to 100000";
    if (r.material_count > 128) return "at most 128 materials";
    if (r.material_count && !r.materials) return "material_count without a materials list";
    for (uint32_t i = 0; i < r.material_count; ++i) if (r.materials[i] > 127) {
        std::snprintf(text, sizeof text, "material %u: building materials are 0 to 127", r.materials[i]); return text;
    }
    return {};
}
uint32_t api_break_blocks(EmfWorld* w, EmfBreakBlocks* r, EmfBreakCallback cb, void* user) {
    Mod* m = caller_mod(_ReturnAddress());
    note_call("break_blocks");
    auto reject = [&](const char* why, bool missing = false) -> uint32_t {
        if (r && r->size >= 160) {
            const size_t n = std::min(std::strlen(why), sizeof r->why - 1);
            std::memcpy(r->why, why, n); r->why[n] = 0;
        }
        if (missing) unsupported(m, "break_blocks", why); else refuse(m, "break_blocks", why, true);
        return 0;
    };
    if (!r) return reject("no request");
    if (r->size < 160) return reject("the request's size is too small (set size = sizeof(EmfBreakBlocks))");
    if (!server_world(w)) return reject("not a server world (a joined game cannot break blocks: do it on the host or the dedicated server)");
    if (const char* why = mode_unavailable(r->flags)) return reject(why, true);
    if (auto why = check(*r); !why.empty()) return reject(why.c_str());
    Request q; q.mod = m; q.cb = cb; q.user = user; q.session = session();
    q.spec.size = sizeof q.spec; q.spec.flags = r->flags;
    q.spec.terrain_health_scale = 250; q.spec.terrain_heal_seconds = 20;
    q.spec.build_flags = KILN_BUILD_F_IN_BASES; // EMF raids intentionally break player bases; no opt-in zone checks
    const int32_t defaults[5] = EMF_BREAK_DEFAULT_DAMAGE;
    const bool any = std::any_of(std::begin(r->damage), std::end(r->damage), [](int32_t n) { return n != 0; });
    std::copy_n(any ? r->damage : defaults, 5, q.spec.damage);
    if (r->material_count) {
        if (r->flags & EMF_BREAK_TERRAIN) std::fill_n(q.spec.materials, 16, uint8_t{255}); // list filters buildings only
        for (uint32_t i = 0; i < r->material_count; ++i) {
            const unsigned raw = 128u + r->materials[i]; q.spec.materials[raw/8] |= uint8_t(1u << (raw%8));
        }
    }
    const float radius = r->radius == 0 ? 1 : r->radius;
    const int n = r->height <= 0 ? 1 : 1 + static_cast<int>(std::ceil(r->height / radius - 1e-4f));
    for (int i = 0; i < n; ++i) q.spheres.push_back({r->x, r->y + (n == 1 ? 0 : r->height * i / (n-1)), r->z, radius});
    const char* why = nullptr;
    uint32_t id = 0;
    {
        std::lock_guard lock(g_mutex);
        if (g_faulted) why = kFaulted;
        else if (g_stopping || !q.session || q.session != session() || !server_world(w)) why = "the server world is stopping or has no session";
        else if (g_waiting.size() >= 64) why = "64 break requests are already waiting";
        else {
            if (!g_last_run) g_last_run = now_ms();
            id = q.id = g_next_id++;
            if (!g_next_id) g_next_id = 1;
            g_waiting.push_back(std::move(q)); ++g_accepted;
        }
    }
    if (why) return reject(why);
    r->why[0] = 0; mark_status_dirty(); return id;
}
Done answer(Request q, uint32_t status) {
    EmfBreakResult r{}; r.size = sizeof r; r.status = status; return {std::move(q), r};
}
void cancel_waiting() { // g_mutex held; callbacks are always deferred to server Post, as EMF's dispatcher does
    for (auto& q : g_waiting) { g_done.push_back(answer(std::move(q), EMF_BREAK_CANCELLED)); ++g_cancelled; }
    g_waiting.clear();
}
bool raster(const Sphere& s, const KilnBuildGrid& grid, KilnBuildBreak& r, std::vector<uint8_t>& mask) {
    if (!std::isfinite(grid.cell_size) || grid.cell_size <= 0) return false;
    const double center[3] = {s.x,s.y,s.z}, origin[3] = {grid.origin.x,grid.origin.y,grid.origin.z};
    size_t cells = 1;
    for (int a = 0; a < 3; ++a) {
        const double lo = std::floor((center[a] - s.radius - origin[a]) / grid.cell_size);
        const double hi = std::floor((center[a] + s.radius - origin[a]) / grid.cell_size);
        if (!std::isfinite(lo) || !std::isfinite(hi) || lo < INT32_MIN || hi > INT32_MAX || hi < lo || hi-lo > 4095) return false;
        r.origin[a] = static_cast<int32_t>(lo); r.dims[a] = static_cast<uint32_t>(hi-lo+1);
        cells *= r.dims[a]; if (cells > 4096) return false;
    }
    mask.resize(cells);
    size_t i = 0;
    for (uint32_t z = 0; z < r.dims[2]; ++z) for (uint32_t y = 0; y < r.dims[1]; ++y) for (uint32_t x = 0; x < r.dims[0]; ++x) {
        const double dx = origin[0] + (double(r.origin[0]) + x + .5)*grid.cell_size - s.x;
        const double dy = origin[1] + (double(r.origin[1]) + y + .5)*grid.cell_size - s.y;
        const double dz = origin[2] + (double(r.origin[2]) + z + .5)*grid.cell_size - s.z;
        mask[i++] = dx*dx + dy*dy + dz*dz <= double(s.radius)*s.radius;
    }
    r.selection = mask.data(); return true;
}
}  // namespace

void fill_terrain(EmfApi* t) {
    t->break_blocks = &api_break_blocks; t->block_at = &api_block_at;
    t->find_wall = &api_find_wall; t->find_wall_ex = &api_find_wall_ex;
}
void terrain_server_pre() {
    if (!g_api || !g_api->on_tick_thread || !g_api->on_tick_thread()) return;
    std::vector<Request> run;
    bool failed = false;
    {
        std::lock_guard lock(g_mutex); g_last_run = now_ms(); failed = g_faulted;
        size_t n = 0, spheres = 0;
        while (n < g_waiting.size() && spheres + g_waiting[n].spheres.size() <= 8) spheres += g_waiting[n++].spheres.size();
        run.assign(std::make_move_iterator(g_waiting.begin()), std::make_move_iterator(g_waiting.begin()+n));
        g_waiting.erase(g_waiting.begin(), g_waiting.begin()+n);
    }
    std::vector<Done> done;
    for (auto& q : run) {
        const auto expected = q.session;
        Done d = answer(std::move(q), failed ? EMF_BREAK_FAILED : EMF_BREAK_DONE);
        if (expected != session()) d.result.status = EMF_BREAK_CANCELLED;
        if (d.result.status == EMF_BREAK_DONE) {
            KilnBuildGrid grid{}; grid.size = sizeof grid;
            const char* unavailable = mode_unavailable(d.request.spec.flags);
            bool ok = !unavailable && S.build->grid(&grid) == KILN_OK;
            std::string mod = "emf:" + (d.request.mod ? d.request.mod->id : std::string("unknown"));
            for (const auto& s : d.request.spheres) {
                if (!ok) break;
                auto r = d.request.spec; r.origin_mod = kstr(mod);
                r.flags |= KILN_BUILD_BREAK_PROTECTED_SPHERE;
                r.sphere_center = {s.x,s.y,s.z}; r.sphere_radius = s.radius;
                r.sphere_margin = .01; r.sphere_minimum = .25;
                std::vector<uint8_t> mask;
                if (!raster(s, grid, r, mask)) { ok = false; break; }
                KilnBuildBreakResult result{}; result.size = sizeof result;
                const auto rc = S.build->break_box(&r, &result);
                d.result.damaged += static_cast<uint32_t>(result.damaged);
                d.result.removed += static_cast<uint32_t>(result.removed);
                d.result.dropped += static_cast<uint32_t>(result.dropped);
                ok = rc == KILN_OK && result.refusal == KILN_BUILD_BLOCK_NONE;
                const bool partial = (result.flags & KILN_BUILD_BREAK_R_MAY_HAVE_CHANGED) &&
                                     (!ok || !(result.flags & KILN_BUILD_BREAK_R_VERIFIED));
                if (partial || result.refusal == KILN_BUILD_BLOCK_FAULTED || result.refusal == KILN_BUILD_BLOCK_UNVERIFIED) {
                    { std::lock_guard lock(g_mutex); g_faulted = true; if (partial) ++g_partial; }
                    refuse(d.request.mod, "break_blocks", "a write may have partially committed; reported removals are a lower bound; not retried", true);
                    ok = false;
                }
            }
            if (!ok) {
                d.result.status = EMF_BREAK_FAILED; failed = true;
                refuse(d.request.mod, "break_blocks", unavailable ? unavailable : "kiln.build refused the queued break; not retried", true);
            }
        }
        done.push_back(std::move(d));
    }
    if (!done.empty()) {
        std::lock_guard lock(g_mutex);
        for (auto& d : done) {
            if (d.result.status == EMF_BREAK_CANCELLED) ++g_cancelled;
            else if (d.result.status == EMF_BREAK_FAILED) ++g_failed;
            else ++g_completed;
            g_done.push_back(std::move(d));
        }
        mark_status_dirty();
    }
}
void terrain_after_step(EmfWorld* w, uint8_t kind) {
    if (kind != EMF_WORLD_SERVER || !w) return;
    std::vector<Done> done;
    {
        std::lock_guard lock(g_mutex);
        if (!g_waiting.empty() && now_ms() - g_last_run >= 3000) cancel_waiting();
        done.swap(g_done);
    }
    for (auto& d : done) if (d.request.cb) {
        struct Call { Done* done; EmfWorld* world; } c{&d,w};
        ContextScope scope(d.request.mod, w, true);
        guarded(d.request.mod, "a break_blocks result callback", [](void* raw) {
            auto& c = *static_cast<Call*>(raw); auto& d = *c.done;
            d.request.cb(c.world, d.request.id, &d.result, d.request.user);
        }, &c);
    }
    if (!done.empty()) mark_status_dirty();
}
void terrain_world_stopping() {
    { std::lock_guard lock(g_mutex); g_stopping = true; cancel_waiting(); }
    mark_status_dirty();
}
void terrain_world_started() { std::lock_guard lock(g_mutex); g_stopping = false; g_last_run = now_ms(); }
std::string terrain_status_json() {
    std::lock_guard lock(g_mutex);
    return "{\"accepted\":" + std::to_string(g_accepted) + ",\"waiting\":" + std::to_string(g_waiting.size()) +
        ",\"callbacksPending\":" + std::to_string(g_done.size()) + ",\"completed\":" + std::to_string(g_completed) +
        ",\"cancelled\":" + std::to_string(g_cancelled) + ",\"failed\":" + std::to_string(g_failed) +
        ",\"partial\":" + std::to_string(g_partial) + ",\"faulted\":" + (g_faulted ? "true}" : "false}");
}
}  // namespace kemf

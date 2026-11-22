// SPDX-License-Identifier: MIT
// kiln.render v2's outlines (native/core/abi/services/kiln_render.h "Outlines"): an entity's model drawn with the game's
// own highlight. The logic is outline_core.h (host-tested); this file is the glue to the game:
//  - sigdb client.d/outline.json (feature ui.world_outline): the renderer's SetGroups and Instance, the RenderScene ->
//    renderer offset, the instance's group byte, the four palette conversion calls of Group1 and Group2;
//  - on the first request (main thread, the client world's Pre): checked mid hooks on those four calls, one per step,
//    which hand the renderer Kiln's colour for its groups; then an After stage on the client world's
//    dynamic_render_model (kiln.world game_program_wrap), removed again when nothing is left to show or give back;
//  - each client Pre: ends (world, owner, expiry, despawn), the entities' model handles (kiln.world's verified ECS reads
//    of keen::ecs::ModelComponent, kept between steps by outline_core's ModelCache), the plan and the palette words;
//  - the stage (the world's thread, inside dynamic_render_model, which holds the RenderScene): outline_core's apply with
//    the game's Instance and SetGroups, once per client step (and, with mods first, again in each slice that holds an
//    outlined entity).
// RE: docs/re/client-render.md "Highlight groups (outlines)". The approach follows GreeneyeDS' Quick Deposit and Find
// (MIT, CREDITS.md); Kiln adds nothing at a fixed offset: every address and offset comes from sigdb for this build.
#include <windows.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "kiln_hook.hpp"
#include "outline_core.h"
#include "safetyhook.hpp"  // safetyhook::Context: the palette hooks' register view
#include "services/kiln_caps.h"
#include "services/kiln_render.h"
#include "services/kiln_world.h"
#include "ui.h"

namespace {
using namespace kui;

// ---- what sigdb found for this build (plugin main) ----
struct Span {
    const void* data;
    uint64_t count;
};
using SetGroupsFn = void(__fastcall*)(void* renderer, const Span* handles, const Span* groups);
using InstanceFn = uint8_t*(__fastcall*)(void* renderer, uint32_t handle, bool checked);
SetGroupsFn g_set_groups = nullptr;
InstanceFn g_instance = nullptr;
int64_t g_group_off = 0, g_renderer_off = 0;
uintptr_t g_sites[4] = {};
const char* const kSiteNames[4] = {"outline/Palette0.Group1", "outline/Palette0.Group2", "outline/Palette1.Group1",
                                   "outline/Palette1.Group2"};
bool g_resolved = false;

// ---- state ----
enum Phase : int { kIdle = 0, kHooking = 1, kReady = 2, kFailed = 3 };
std::atomic<int> g_phase{kIdle};
std::mutex g_detail_mu;
std::string g_detail;  // why off or failed
kout::Book g_book;
std::atomic<int> g_setting{kout::kGameFirst};
std::atomic<uint64_t> g_slots[2] = {0, 0};  // the palette words of groups 4 and 5 (outline_core slot_word)
kiln::MidHook2 g_hooks[4];
int g_hooks_in = 0, g_hook_tries = 0;  // main thread
std::once_flag g_start_once;
std::atomic<bool> g_started{false};
const KilnWorldApi* g_world = nullptr;
uint32_t g_model_hash = 0;
// the stage (main thread decides; the world's thread runs it)
KilnHandle g_wrap = 0;
std::atomic<bool> g_wrapped{false};
std::atomic<int32_t> g_scene_slot{-1};
uint32_t g_wrap_tries = 0;
std::mutex g_plan_mu;
std::shared_ptr<const kout::Plan> g_plan;
std::mutex g_res_mu;
uint64_t g_res_gen = 0;
std::vector<uint8_t> g_res;
bool g_res_new = false;
std::atomic<uint64_t> g_sets{0}, g_steps{0}, g_last_step{~uint64_t{0}};
std::atomic<uint32_t> g_applied_count{0};
std::atomic<bool> g_stage_fault{false};
kout::Applied g_applied;  // the world's thread only
std::atomic<uint64_t> g_off_thread{0};  // slices the stage left because they ran off the world's thread
// the client world's entities (main thread)
struct Seen {
    KilnObjectRef ref{};
    bool open = false;
};
std::unordered_map<KilnEntity, Seen> g_seen;
kout::ModelCache g_models;
uint64_t g_session = 0, g_pre = 0;
// kiln.world's client-world reads this Pre: one answered UNAVAILABLE / one worked. UNAVAILABLE is also what a world
// that is not there yet (or no longer) answers, so outlines fail only after every read answered it for kReadsOffMs.
bool g_read_unavailable = false, g_read_ok = false;
uint64_t g_unavailable_since = 0;
constexpr uint64_t kReadsOffMs = 10000;
constexpr uint64_t kEpoch = 1;
bool g_logged_off_thread = false;

void set_detail(const std::string& s) {
    std::lock_guard lk(g_detail_mu);
    g_detail = s;
}

bool sym(const char* n, uintptr_t* out) { return g_api->symbol(KilnStr{n, std::strlen(n)}, out) == KILN_OK && *out; }
bool cst(const char* n, int64_t* out) { return g_api->constant(KilnStr{n, std::strlen(n)}, out) == KILN_OK; }
bool feat(const char* n) { return g_api->feature(KilnStr{n, std::strlen(n)}); }
KilnStr ks(const char* s) { return KilnStr{s, std::strlen(s)}; }

// The mechanism stopped: every request ends (FAILED), the capability row says why, nothing is applied any more. What
// Kiln set stays until the game makes those models again (the stage cannot run to give it back).
void fail(const std::string& why) {
    if (g_phase.exchange(kFailed) == kFailed) return;
    set_detail(why);
    logf(KILN_LOG_ERROR, "render: outlines are off: %s", why.c_str());
    g_slots[0].store(0);
    g_slots[1].store(0);
    g_book.end_all(KILN_RENDER_OUTLINE_FAILED);
    if (g_wrap && g_world) {  // fail() runs on the main thread or in a caller's thread, never inside the stage
        g_world->game_program_unwrap(g_wrap);
        g_wrap = 0;
        g_wrapped.store(false);
    }
    if (const auto* caps = static_cast<const KilnCapsApi*>(g_api->service(ks(KILN_CAPS_SERVICE), 1)))
        caps->report(g_owner, ks("ui.world_outline"), KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED, KilnStr{why.data(), why.size()});
}

// ---- the game's calls, each under a structured-exception guard (nothing with a destructor in these frames) ----
bool seh_instance(uintptr_t renderer, uint32_t handle, uint8_t** out) {
    __try {
        *out = g_instance(reinterpret_cast<void*>(renderer), handle, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool seh_byte(const uint8_t* p, uint8_t* out) {
    __try {
        *out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool seh_ptr(uintptr_t p, uintptr_t* out) {
    __try {
        *out = *reinterpret_cast<const uintptr_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool seh_set(uintptr_t renderer, const Span* h, const Span* g) {
    __try {
        g_set_groups(reinterpret_cast<void*>(renderer), h, g);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct GameIo final : kout::RendererIo {
    uintptr_t renderer;
    bool faulted = false;
    explicit GameIo(uintptr_t r) : renderer(r) {}
    int group(uint32_t model) override {
        if (!model || faulted) return -1;
        uint8_t* inst = nullptr;
        uint8_t g = 0;
        if (!seh_instance(renderer, model, &inst)) return faulted = true, -1;
        if (!inst) return -1;  // destroyed (hidden, streamed out, remade): the handle's generation no longer matches
        if (!seh_byte(inst + g_group_off, &g)) return faulted = true, -1;
        return g;
    }
    void set(const uint32_t* models, const uint8_t* groups, size_t n) override {
        if (faulted) return;
        const Span h{models, n}, g{groups, n};
        if (!seh_set(renderer, &h, &g)) faulted = true;
    }
};

// ---- the palette hooks (render thread): edx holds the group's colour as the renderer is about to convert it ----
template <int Slot>
void hk_palette(safetyhook::Context& c) {
    const uint64_t w = g_slots[Slot].load(std::memory_order_relaxed);
    if (!(w & kout::kSlotOn)) return;  // not Kiln's now: the game's own entry (transparent) goes through
    c.rdx = kout::palette_value(w, static_cast<uint32_t>(c.rdx), GetTickCount64());
}

// ---- the stage (the world's thread) ----
struct Grab {
    KilnGameSlice* slice;
    void* scene;
};
uint32_t KILN_CALL grab_scene(uint64_t* iter, uint32_t, KilnEntity, void* user) {
    auto* g = static_cast<Grab*>(user);
    uint32_t size = 0;
    uint8_t writable = 0;
    g->scene = g_world->game_slice_slot(g->slice, iter, KILN_GAME_LIST_SYS_WRITE, g_scene_slot.load(), &size, &writable);
    return 1;  // the first entity is enough: the RenderScene is the program's singleton
}

void KILN_CALL stage(KilnGameSlice* slice, void*) {
    if (!slice || slice->world_kind != KILN_WORLD_CLIENT || !slice->count || g_phase.load() != kReady) return;
    // g_applied is the world thread's alone. dynamic_render_model is PINNED (descriptor flag 0x400: its slices run one
    // after another on the client world's thread, docs/re/world.md), so this holds on 26430625; a build that ran its
    // slices on the workers gets no outlines (Pre logs it) instead of two slices in g_applied at once.
    if (!slice->thread_is_world) {
        g_off_thread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::shared_ptr<const kout::Plan> plan;
    {
        std::lock_guard lk(g_plan_mu);
        plan = g_plan;
    }
    if (!plan || (plan->targets.empty() && g_applied_count.load() == 0)) return;
    // the plan's models are the client world's of its session: never applied in another world's renderer (one at the
    // old address included) should a step ever run before its Pre published a new plan
    if (plan->session != g_world->game_world_session(KILN_WORLD_CLIENT)) return;
    const bool first = g_last_step.load(std::memory_order_relaxed) != slice->step;
    if (!first && plan->setting != kout::kModsFirst) return;
    std::vector<size_t> which;
    if (!first) {  // mods first: the outlined entities of this slice, after the game set their groups in it
        kout::slice_targets(*plan, slice->entity_ids, slice->count, slice->entity_stride, which);
        if (which.empty()) return;
    }
    Grab g{slice, nullptr};
    if (g_world->game_slice_iterate(slice, &grab_scene, &g) != KILN_OK || !g.scene) return;  // the next slice tries
    uintptr_t renderer = 0;
    if (!seh_ptr(reinterpret_cast<uintptr_t>(g.scene) + g_renderer_off, &renderer)) {
        g_stage_fault.store(true);
        return;
    }
    if (!renderer) return;
    GameIo io(renderer);
    std::vector<uint8_t> res;
    const uint64_t sets = kout::apply(io, renderer, *plan, g_applied, res, first ? nullptr : &which, first);
    if (io.faulted) {
        g_stage_fault.store(true);
        return;
    }
    g_applied_count.store(static_cast<uint32_t>(g_applied.groups.size()));
    g_sets.fetch_add(sets, std::memory_order_relaxed);
    if (first) {
        g_last_step.store(slice->step, std::memory_order_relaxed);
        g_steps.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lk(g_res_mu);
        g_res_gen = plan->generation;
        g_res = std::move(res);
        g_res_new = true;
    }
}

// ---- the client world's Pre (main thread) ----
void KILN_CALL noop(void*) {}

// Hooks one palette call per step (each a checked install: every other thread stopped and checked), then the stage.
void install_step() {
    if (g_hooks_in < 4) {
        const int i = g_hooks_in;
        const bool ok = (i % 2 == 0) ? g_hooks[i].install_checked(g_api, g_owner, g_sites[i], &hk_palette<0>, kSiteNames[i])
                                     : g_hooks[i].install_checked(g_api, g_owner, g_sites[i], &hk_palette<1>, kSiteNames[i]);
        if (ok) {
            ++g_hooks_in;
            g_hook_tries = 0;
            if (g_hooks_in == 4) logf(KILN_LOG_INFO, "render: outlines: the palette of Kiln's highlight groups is hooked (4 checked mid hooks)");
        } else if (++g_hook_tries >= 20) {  // a thread inside the 5 bytes (the render thread converting) refuses one try
            fail(std::string("the palette hook ") + kSiteNames[i] + " could not be installed: " +
                 (g_hooks[i].error() ? g_hooks[i].error() : "unknown"));
        }
        return;
    }
    g_phase.store(kReady);
}

bool wrap_stage() {
    const int32_t idx = g_world->game_program_find(KILN_WORLD_CLIENT, ks("dynamic_render_model"));
    if (idx < 0) return false;  // the client world's directory is not read yet
    KilnStr names[16];
    int32_t scene = -1;
    const int32_t n = g_world->game_program_list(KILN_WORLD_CLIENT, idx, KILN_GAME_LIST_SYS_WRITE, names, 16);
    for (int32_t i = 0; i < n && i < 16; ++i)
        if (names[i].len == 11 && std::memcmp(names[i].ptr, "RenderScene", 11) == 0) scene = i;
    bool model = false;
    const int32_t m = g_world->game_program_list(KILN_WORLD_CLIENT, idx, KILN_GAME_LIST_WRITE, names, 16);
    for (int32_t i = 0; i < m && i < 16; ++i)
        if (names[i].len == 14 && std::memcmp(names[i].ptr, "ModelComponent", 14) == 0) model = true;
    if (scene < 0 || !model) {
        fail("the client world's dynamic_render_model no longer writes RenderScene and ModelComponent on this build");
        return false;
    }
    g_scene_slot.store(scene);
    KilnHandle h = 0;
    const KilnResult rc = g_world->game_program_wrap(g_owner, KILN_WORLD_CLIENT, idx, KILN_WRAP_AFTER, 0, &stage, nullptr, &h);
    if (rc == KILN_ERR_UNAVAILABLE) {
        fail("kiln.world cannot wrap the client world's programs here (systems.wraps.client off, or native SET is loaded)");
        return false;
    }
    if (rc != KILN_OK) {
        if (++g_wrap_tries >= 5) fail("kiln.world refused the stage on dynamic_render_model (" + std::to_string(rc) + ")");
        return false;
    }
    g_wrap = h;
    g_wrapped.store(true);
    g_wrap_tries = 0;
    return true;
}

void unwrap_stage() {
    if (!g_wrap) return;
    g_world->game_program_unwrap(g_wrap);
    g_wrap = 0;
    g_wrapped.store(false);
}

void check_stage() {
    if (!g_wrap) return;
    KilnGameWrapInfo w{};
    w.size = sizeof w;
    if (g_world->game_wrap_info(g_wrap, &w) != KILN_OK) return;
    if (w.state == KILN_WRAP_REFUSED || w.state == KILN_WRAP_REMOVED || w.disabled != KILN_WRAP_OK) {
        g_wrap = 0;
        g_wrapped.store(false);
        fail(std::string("the stage on dynamic_render_model stopped: ") + (w.detail[0] ? w.detail : "refused or disabled"));
    }
}

// The entity's model handle now, or 0. Ends its requests when it left the client world.
uint32_t model_of(KilnEntity e) {
    Seen& s = g_seen[e];
    if (s.open) {
        KilnObjectInfo info{};
        info.size = sizeof info;
        const KilnResult v = g_world->entity_validate(g_owner, &s.ref, &info);
        if (v == KILN_OK) g_read_ok = true;
        else if (v == KILN_ERR_UNAVAILABLE) g_read_unavailable = true;
        if (v != KILN_OK) {
            const bool alive = (info.flags & KILN_OBJECT_ALIVE) != 0;
            if (info.status == KILN_ECS_ABSENT || (info.status == KILN_ECS_STALE && !alive && info.session == s.ref.session)) {
                g_book.end_entity(e, KILN_RENDER_OUTLINE_DESPAWNED);  // gone, or its id names a new entity now
                g_seen.erase(e);
                return 0;
            }
            s.open = false;  // a layout change (a component added or removed) or a read that failed: open it again
        }
    }
    if (!s.open) {
        KilnObjectRef ref{};
        ref.size = sizeof ref;
        const KilnResult rc = g_world->entity_open(g_owner, kEpoch, KILN_WORLD_CLIENT, 0, e, &ref);
        if (rc == KILN_ERR_UNAVAILABLE) g_read_unavailable = true;
        if (rc != KILN_OK) return 0;  // not here (yet): not streamed to this client, or no client world
        g_read_ok = true;
        s.ref = ref;
        s.open = true;
    }
    uint32_t model = 0, need = 0;
    const KilnResult rc = g_world->ecs_snapshot(g_owner, &s.ref, g_model_hash, &model, sizeof model, &need);
    if (rc == KILN_ERR_UNAVAILABLE) g_read_unavailable = true;
    else if (rc == KILN_OK || rc == KILN_ERR_NOT_FOUND) g_read_ok = true;
    return rc == KILN_OK && need == 4 ? model : 0;  // NOT_FOUND: no ModelComponent (a voxel model, an effect)
}

// 0.2.0-dev.31: the positions of the entities of through-walls requests, each client step: the first WorldTransform of
// keen::ecs::RenderTransform (176 bytes; three 32.32 fixed-point metres first), as kiln.markers' client anchors read it
// (native/services/markers/world_read.h). Another size: not read (the layout moved; no silhouette rather than a wrong one).
constexpr uint32_t kRenderTransformBytes = 176;
uint32_t g_transform_hash = 0;
void read_positions() {
    const std::vector<KilnEntity> wall = g_book.wall_entities();
    g_book.keep_positions(wall);
    for (KilnEntity e : wall) {
        double p[3] = {};
        bool ok = false;
        auto it = g_seen.find(e);
        if (it == g_seen.end() || !it->second.open) {
            model_of(e);  // opens it (and ends its requests when it left the client world)
            it = g_seen.find(e);
        }
        if (g_transform_hash && it != g_seen.end() && it->second.open) {
            uint8_t buf[kRenderTransformBytes];
            uint32_t need = 0;
            if (g_world->ecs_snapshot(g_owner, &it->second.ref, g_transform_hash, buf, sizeof buf, &need) == KILN_OK &&
                need == kRenderTransformBytes) {
                int64_t fixed[3];
                std::memcpy(fixed, buf, sizeof fixed);
                for (int k = 0; k < 3; ++k) p[k] = static_cast<double>(fixed[k]) * (1.0 / 4294967296.0);
                ok = true;
            }
        }
        g_book.set_position(e, ok, p, static_cast<uint32_t>(g_pre));
    }
}

void KILN_CALL on_client_pre(const KilnTickInfo*, void*) {
    if (g_phase.load() == kFailed) return;
    ++g_pre;
    const uint64_t session = g_world->game_world_session(KILN_WORLD_CLIENT);
    if (session != g_session) {
        if (g_session) g_book.end_all(KILN_RENDER_OUTLINE_WORLD);
        g_seen.clear();
        g_models.clear();
        g_unavailable_since = 0;
        g_session = session;
    }
    if (!g_logged_off_thread && g_off_thread.load(std::memory_order_relaxed)) {
        g_logged_off_thread = true;
        logf(KILN_LOG_WARN, "render: outlines: dynamic_render_model ran off the client world's thread on this build; "
                            "the stage leaves those slices (no outlines there)");
    }
    g_book.expire(GetTickCount64());
    if (g_pre % 60 == 0)  // a mod's owner that was revoked (the mod unloaded without clearing)
        for (KilnOwner o : g_book.owners())
            if (g_api->post_world(o, KILN_WORLD_CLIENT, &noop, nullptr) == KILN_ERR_REVOKED) g_book.end_owner(o, KILN_RENDER_OUTLINE_OWNER);
    if (g_stage_fault.exchange(false)) {
        fail("the game's renderer faulted under the stage (SetGroups, Instance or the RenderScene)");
        return;
    }
    {
        std::lock_guard lk(g_res_mu);
        if (g_res_new) {
            g_book.results(g_res_gen, g_res);
            // a planned handle that named no instance (the model was destroyed or remade): read it again first
            std::shared_ptr<const kout::Plan> last;
            {
                std::lock_guard pk(g_plan_mu);
                last = g_plan;
            }
            if (last && last->generation == g_res_gen)
                for (size_t i = 0; i < g_res.size() && i < last->targets.size(); ++i)
                    if (g_res[i] == kout::kNoModel) g_models.stale(last->targets[i].entity, last->targets[i].model);
            g_res_new = false;
        }
    }
    const bool live = g_book.live() > 0;
    if (live && g_phase.load() == kHooking) install_step();
    if (g_phase.load() == kFailed) return;
    std::unordered_map<KilnEntity, uint32_t> models;
    g_read_unavailable = g_read_ok = false;
    if (live) {
        g_models.update(g_book.entities(), g_pre, &model_of, models);
        read_positions();
    } else {
        g_models.clear();
        g_seen.clear();
    }
    if (g_read_ok) {
        g_unavailable_since = 0;
    } else if (g_read_unavailable) {  // every read this step answered UNAVAILABLE
        const uint64_t now = GetTickCount64();
        if (!g_unavailable_since) g_unavailable_since = now;
        else if (now - g_unavailable_since >= kReadsOffMs) {
            fail("kiln.world cannot read the client world's entities on this build (no exact engine profile for it)");
            return;
        }
    }
    kout::Book::Slot slots[2];
    auto plan = std::make_shared<kout::Plan>(
        g_book.plan(models, static_cast<kout::Setting>(g_setting.load()), slots, session));
    for (int s = 0; s < 2; ++s) g_slots[s].store(slots[s].used ? kout::slot_word(slots[s].rgba, slots[s].pulse) : 0);
    {
        std::lock_guard lk(g_plan_mu);
        g_plan = std::move(plan);
    }
    if (g_phase.load() != kReady) return;
    const bool needed = live || g_applied_count.load() > 0;
    if (needed && !g_wrap) wrap_stage();
    else if (!needed && g_wrap) unwrap_stage();
    if (g_wrap && g_pre % 30 == 0) check_stage();
}

// <game>/kiln/kiln.json's text ("" when it cannot be read): the player's "ui": {"outlines": ...}.
std::string kiln_json_text() {
    char b[2048];
    size_t n = 0;
    if (g_api->path(KILN_PATH_KILN, b, sizeof b, &n) != KILN_OK || n >= sizeof b) return {};
    try {
        const std::string dir(b, n);
        std::ifstream in(std::filesystem::path(std::u8string(dir.begin(), dir.end())) / "kiln.json", std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    } catch (...) {
        return {};
    }
}

// The first request: kiln.world, the client world's Pre, the player's switch. Any thread.
void start() {
    std::call_once(g_start_once, [] {
        const auto* w = static_cast<const KilnWorldApi*>(g_api->service(ks(KILN_WORLD_SERVICE), 4));
        if (!w || w->header.size < offsetof(KilnWorldApi, ecs_snapshot) + sizeof(void*)) {
            fail("kiln.world is missing or older than 0.2.0-dev.29 (game programs, wraps and ECS reads)");
            return;
        }
        if (!(w->capabilities() & KILN_WORLD_CAP_ECS_READ)) {
            fail("kiln.world cannot read the client world's entities on this build (ECS reads off)");
            return;
        }
        g_world = w;
        g_model_hash = w->component_hash(ks("ModelComponent"));
        g_transform_hash = w->component_hash(ks("RenderTransform"));
        g_setting.store(kout::parse_setting(kiln_json_text()));
        static const char* const kSettings[] = {"game-first", "mods-first", "off"};
        KilnHandle h = 0;
        if (g_api->subscribe_world_tick(g_owner, KILN_WORLD_CLIENT, KILN_TICK_PRE, &on_client_pre, nullptr, &h) != KILN_OK) {
            fail("the client world's tick could not be subscribed");
            return;
        }
        g_phase.store(kHooking);
        g_started.store(true);
        logf(KILN_LOG_INFO, "render: outlines: first request; hooks go in at the next client steps (kiln.json ui.outlines: %s)",
             kSettings[g_setting.load()]);
    });
}

}  // namespace

namespace kui {

// Plugin main: what sigdb found. Nothing is hooked here.
bool outline_init() {
    uintptr_t set = 0, inst = 0;
    bool ok = feat("ui.world_outline") && sym("outline/SetGroups", &set) && sym("outline/Instance", &inst) &&
              cst("outline/inst.group", &g_group_off) && cst("outline/scene.renderer", &g_renderer_off);
    for (int i = 0; ok && i < 4; ++i) ok = sym(kSiteNames[i], &g_sites[i]);
    if (!ok) {
        set_detail("the game's highlight is not found on this build (feature ui.world_outline off)");
        logf(KILN_LOG_INFO, "render: outlines are off on this build (feature ui.world_outline)");
        return false;
    }
    g_set_groups = reinterpret_cast<SetGroupsFn>(set);
    g_instance = reinterpret_cast<InstanceFn>(inst);
    g_resolved = true;
    return true;
}

KilnResult KILN_CALL outline_add(KilnOwner owner, KilnEntity entity, const KilnRenderOutline* o, KilnHandle* out) {
    if (out) *out = 0;
    if (!owner || !entity || !o || o->size < sizeof(KilnRenderOutline) || !out || !(o->rgba & 0xFF) ||
        (o->flags & ~uint32_t{KILN_RENDER_OUTLINE_PULSE | KILN_RENDER_OUTLINE_THROUGH_WALLS}) || o->reserved)
        return KILN_ERR_INVALID_ARGUMENT;
    if (!g_resolved || g_api->side() != KILN_SIDE_CLIENT) return KILN_ERR_UNAVAILABLE;
    start();
    if (g_phase.load() == kFailed) return KILN_ERR_UNAVAILABLE;
    const KilnHandle h = next_handle();
    const KilnResult rc = g_book.add(owner, entity, *o, GetTickCount64(), h);
    if (rc != KILN_OK) return rc;
    if (g_phase.load() == kFailed) {  // the mechanism failed while this was added: it would wait for nothing
        g_book.end_all(KILN_RENDER_OUTLINE_FAILED);
        return KILN_ERR_UNAVAILABLE;
    }
    *out = h;
    return KILN_OK;
}

KilnResult KILN_CALL outline_info(KilnHandle h, KilnRenderOutlineInfo* out) {
    // a caller built before 0.2.0-dev.31 passes the 24-byte record (through merged): answered without the position
    constexpr uint32_t kOld = offsetof(KilnRenderOutlineInfo, position_ok);
    if (!out || out->size < kOld) return KILN_ERR_INVALID_ARGUMENT;
    KilnRenderOutlineInfo full{};
    full.size = sizeof full;
    const KilnResult rc = g_book.info(h, &full);
    const uint32_t n = out->size >= sizeof full ? static_cast<uint32_t>(sizeof full) : kOld;
    std::memcpy(out, &full, n);
    out->size = n;
    return rc;
}

KilnResult KILN_CALL outline_remove(KilnHandle h) { return g_book.remove(h); }

KilnResult KILN_CALL outline_clear(KilnOwner owner) {
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    g_book.clear(owner);
    return KILN_OK;
}

KilnResult KILN_CALL outline_status(KilnRenderOutlineStatus* out) {
    if (!out || out->size < sizeof(KilnRenderOutlineStatus)) return KILN_ERR_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    out->available = g_resolved && g_phase.load() != kFailed ? 1 : 0;
    out->installed = g_phase.load() == kReady ? 1 : 0;
    out->staged = g_wrapped.load() ? 1 : 0;
    out->setting = static_cast<uint8_t>(g_setting.load());
    const auto c = g_book.counts();
    out->requests = c.requests;
    out->entities = c.entities;
    out->shown = c.shown;
    out->game = c.game;
    out->no_model = c.no_model;
    out->sets = g_sets.load();
    out->steps = g_steps.load();
    std::lock_guard lk(g_detail_mu);
    std::strncpy(out->detail, g_detail.c_str(), sizeof out->detail - 1);
    return KILN_OK;
}

}  // namespace kui

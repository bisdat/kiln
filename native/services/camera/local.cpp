// SPDX-License-Identifier: MIT
// kiln.camera on a game client: local.h says what it is. This file is the glue to the game (sigdb, kiln.hooks, kiln.client,
// kiln.types, GetComponent, the client world's lifecycle, guarded memory); the logic it drives is in capture.cpp,
// sampler.cpp and render_tx.cpp.
#include "local.h"

#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

#include "render_tx.h"
#include "sampler.h"
#include "services/kiln_client.h"
#include "services/kiln_hooks.h"
#include "services/kiln_types.h"
#include "snapshot.h"
#include "state.h"

namespace kcam {

namespace {

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
Core* g_core = nullptr;
bool g_snapshot = false;  // camera.rendered_snapshot is served

KilnStr str(const char* s) { return KilnStr{s, std::strlen(s)}; }

void logf(int32_t level, const char* fmt, ...) {
    char text[512];
    va_list v;
    va_start(v, fmt);
    const int n = std::vsnprintf(text, sizeof text, fmt, v);
    va_end(v);
    if (n > 0) g_api->log(g_owner, level, KilnStr{text, static_cast<size_t>(n < static_cast<int>(sizeof text) ? n : static_cast<int>(sizeof text) - 1)});
}

// MSVC's steady_clock is QueryPerformanceCounter: the counter managed Kiln.Camera's Stopwatch reads too.
struct SteadyClock : Clock {
    int64_t now_ns() const override {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
};

// No frame here holds an object with a destructor, as __try requires.
bool guarded_copy(void* dst, const void* src, size_t n) {
#ifdef _WIN32
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(dst, src, n);
    return true;
#endif
}

struct GameMem : RendererMem {
    bool copy_in(uintptr_t a, void* dst, size_t n) override { return a && guarded_copy(dst, reinterpret_cast<const void*>(a), n); }
    bool copy_out(uintptr_t a, const void* src, size_t n) override { return a && guarded_copy(reinterpret_cast<void*>(a), src, n); }
};

SteadyClock g_clock;
GameMem g_mem;
Frames g_frames(&g_clock);

// ---- the client sampler's source: the game's GetComponent with the client world -------------------------------------------

constexpr uint32_t kRenderTransform = 0x3b8476dd;  // keen::ecs::RenderTransform (FNV-1a-32 of the qualified name)
constexpr uint32_t kRenderTransformSize = 176;

struct ComponentSlice {
    const void* data;
    uint64_t size;
};
using GetComponentFn = void (*)(ComponentSlice* out, uintptr_t world, uint32_t hash, uint32_t entity);

// 1: copied, 0: the entity has none, -1: a fault or another size than the checked one. No object with a destructor here.
int guarded_component(GetComponentFn fn, uintptr_t world, uint32_t entity, void* out, uint32_t want) {
#ifdef _WIN32
    __try {
        ComponentSlice s{nullptr, 0};
        fn(&s, world, kRenderTransform, entity);
        if (!s.data) return 0;
        if (s.size != kRenderTransformSize) return -1;
        std::memcpy(out, s.data, want);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
#else
    ComponentSlice s{nullptr, 0};
    fn(&s, world, kRenderTransform, entity);
    if (!s.data) return 0;
    if (s.size != kRenderTransformSize) return -1;
    std::memcpy(out, s.data, want);
    return 1;
#endif
}

struct GameSource : SampleSource {
    GetComponentFn fn = nullptr;
    // The client world's thread, in its Pre (no world job runs then): the world pointer is read here, never kept.
    Read read(uint32_t entity, EnginePose* out) override {
        const uintptr_t world = g_api->world(KILN_WORLD_CLIENT);
        if (!fn || !world) return Read::Absent;
        uint8_t raw[kPoseBytes] = {};
        const int rc = guarded_component(fn, world, entity, raw, kPoseValueBytes);  // the current WorldTransform at +0
        if (rc < 0) return Read::Fault;
        if (rc == 0) return Read::Absent;
        std::memcpy(out, raw, sizeof *out);
        return Read::Ok;
    }
};

const KilnClientApi* g_client = nullptr;

struct ClientWorld : LocalWorld {
    uint32_t local_player() override {
        KilnClientState st{};
        st.size = sizeof st;
        if (!g_client || g_client->state(&st) != KILN_OK || !(st.flags & KILN_CLIENT_LOCAL_PLAYER)) return 0;
        return st.local_player;
    }
};

GameSource g_source;
SampleBoard g_board;
Sampler g_sampler(&g_source, &g_board, &g_clock);
ClientWorld g_world;
std::unique_ptr<LocalBackend> g_backend;
bool g_sampling = false;    // GetComponent, kiln.client and RenderTransform's layout are there
uint64_t g_serving = 0;     // the local bits the hooks and the sampler allow in a healthy world

// keen::ecs::RenderTransform as kiln.types reflects it: 176 bytes, a 56-byte WorldTransform `transform` at +0 with the
// position (three s64) at +0, the orientation at +24 and the scale at +40. The sampler copies those 52 bytes only.
std::string_view sv(KilnStr s) { return s.ptr ? std::string_view(s.ptr, s.len) : std::string_view(); }
bool type_info(const KilnTypesApi* t, const KilnType* type, KilnTypeInfo* out) {
    *out = KilnTypeInfo{};
    out->size = sizeof *out;
    return type && t->type_info(type, out) == KILN_OK;
}
const KilnType* unwrap(const KilnTypesApi* t, const KilnType* type) {
    for (int i = 0; type && i < 8; ++i) {
        KilnTypeInfo ti{};
        if (!type_info(t, type, &ti) || ti.kind != KILN_TYPE_TYPEDEF || !ti.inner) return type;
        type = ti.inner;
    }
    return type;
}
bool field_at(const KilnTypesApi* t, const KilnType* type, std::string_view name, uint64_t offset, uint32_t size, const KilnType** ftype) {
    for (int depth = 0; type && depth < 16; ++depth) {
        type = unwrap(t, type);
        KilnTypeInfo ti{};
        if (!type_info(t, type, &ti)) return false;
        for (uint32_t i = 0; i < ti.field_count; ++i) {
            KilnTypeField f{};
            if (t->type_field(type, i, &f) != KILN_OK || sv(f.name) != name) continue;
            KilnTypeInfo fi{};
            if (f.offset != offset || !type_info(t, unwrap(t, f.type), &fi) || fi.type_size != size) return false;
            if (ftype) *ftype = f.type;
            return true;
        }
        if (ti.kind != KILN_TYPE_STRUCT) return false;
        type = ti.inner;
    }
    return false;
}
bool render_transform_layout(std::string* why) {
    const auto* t = static_cast<const KilnTypesApi*>(g_api->service(str(KILN_TYPES_SERVICE), KILN_TYPES_VERSION));
    if (!t || t->header.size < offsetof(KilnTypesApi, type_field) + sizeof(void*) || !t->type_by_hash || !t->type_info || !t->type_field)
        return *why = "kiln.types (the game's reflection) is not loaded", false;
    const KilnType* rt = t->type_by_hash(kRenderTransform);
    KilnTypeInfo ti{};
    if (!type_info(t, rt, &ti) || sv(ti.qualified_name) != "keen::ecs::RenderTransform" || ti.type_size != kRenderTransformSize)
        return *why = "keen::ecs::RenderTransform is not the reflected 176-byte type", false;
    const KilnType* wt = nullptr;
    if (!field_at(t, rt, "transform", 0, 56, &wt)) return *why = "RenderTransform has no 56-byte WorldTransform at +0", false;
    if (!field_at(t, wt, "position", 0, 24, nullptr) || !field_at(t, wt, "orientation", 24, 16, nullptr) || !field_at(t, wt, "scale", 40, 12, nullptr))
        return *why = "WorldTransform's position, orientation and scale moved", false;
    return true;
}

// ---- the hooks (render path: no game call, no author code, no waiting) ----------------------------------------------------

// camera/RenderFovSelected: r13 = renderer, xmm6's low float = the selected field of view (radians), stored by the
// instruction after this hook. Only that float of xmm6 may change; its upper lanes and every other register stay.
void KILN_CALL mid_select(KilnRegisters* regs, void*) {
    const Renderer r(g_mem, g_frames.layout(), regs->r13);
    const uint64_t selection = g_frames.select(r);
    if (!selection || !g_backend) return;
    float fov = 0;
    std::memcpy(&fov, regs->xmm6.bytes, sizeof fov);
    const float natural = fov;
    g_backend->on_select(r, selection, &fov);
    if (std::memcmp(&fov, &natural, sizeof fov) != 0) std::memcpy(regs->xmm6.bytes, &fov, sizeof fov);
}

// camera/VisualFrameReady: rsi = renderer.
void KILN_CALL mid_capture(KilnRegisters* regs, void*) {
    const Renderer r(g_mem, g_frames.layout(), regs->rsi);
    g_frames.capture(r);
}

// ---- the client world -----------------------------------------------------------------------------------------------------

// STARTED on the world's thread before its first Pre; STOPPING on the thread that destroys it.
void KILN_CALL on_world(const KilnWorldEvent* e, void*) {
    if (!e || e->kind != KILN_WORLD_CLIENT) return;
    if (e->event == KILN_WORLD_STARTED) {
        g_sampler.world_started();
        if (g_backend) g_backend->set_serving(g_serving);  // a new world starts healthy
        g_frames.world_started(e->session, static_cast<uint64_t>(g_api->world(KILN_WORLD_CLIENT)));
    } else if (e->event == KILN_WORLD_STOPPING) {
        g_frames.world_stopping();
    }
}

// A mid hook through the table's mid2 slot, asked for by size (an older core's table may end earlier than this
// build's; the full-size helper would refuse it).
bool install(const KilnHooksApi* hooks, const char* symbol, KilnMidFn fn, const char* label) {
    uintptr_t at = 0;
    if (g_api->symbol(str(symbol), &at) != KILN_OK || !at) {
        logf(KILN_LOG_WARN, "camera: %s is not resolved on this game build", symbol);
        return false;
    }
    KilnHookOrder order{};
    order.size = sizeof order;
    order.priority = -100;  // after other links on the same place: the camera reads (and writes) what they left
    order.label = str(label);
    KilnHandle h = 0;
    const KilnResult rc = hooks->mid2(g_owner, at, &order, fn, nullptr, &h);
    if (rc != KILN_OK) logf(KILN_LOG_WARN, "camera: kiln.hooks refused the hook at %s (%d)", symbol, static_cast<int>(rc));
    return rc == KILN_OK;
}

}  // namespace

void local_start(const KilnApi* api, KilnOwner owner, Core* core, bool client_side) {
    g_api = api;
    g_owner = owner;
    g_core = core;
    if (!client_side) return;

    RenderLayout layout;
    std::string why;
    const bool laid_out = RenderLayout::make([](const char* n, int64_t* out) { return g_api->constant(str(n), out) == KILN_OK; }, &layout, &why);
    if (!laid_out) logf(KILN_LOG_WARN, "camera: the renderer's layout does not check out (%s): the client camera is off", why.c_str());
    g_frames.set_layout(laid_out ? layout : RenderLayout{});

    const auto* hooks = static_cast<const KilnHooksApi*>(api->service(str(KILN_HOOKS_SERVICE), 2));
    constexpr size_t kMid2End = offsetof(KilnHooksApi, mid2) + sizeof(void*);
    const bool mid2 = hooks && hooks->header.size >= kMid2End && hooks->mid2;
    if (!mid2) logf(KILN_LOG_WARN, "camera: kiln.hooks has no ordered mid hooks: the client camera is off");

    // the local player and the sampler's GetComponent (the core's resolved symbol; no hook of its own)
    g_client = static_cast<const KilnClientApi*>(api->service(str(KILN_CLIENT_SERVICE), KILN_CLIENT_VERSION));
    uintptr_t get_component = 0;
    std::string layout_why;
    g_sampling = g_client && g_client->header.size >= offsetof(KilnClientApi, state) + sizeof(void*) && g_client->state &&
                 api->symbol(str("GetComponent"), &get_component) == KILN_OK && get_component && render_transform_layout(&layout_why);
    if (g_sampling) g_source.fn = reinterpret_cast<GetComponentFn>(get_component);
    else logf(KILN_LOG_WARN, "camera: the client sampler is off (%s): no pose, path, follow or shake", layout_why.empty() ? "no GetComponent or kiln.client" : layout_why.c_str());

    KilnHandle h = 0;
    const bool lifecycle = api->subscribe_world(owner, &on_world, nullptr, &h) == KILN_OK;
    const bool snapshot_row = api->feature(str("camera.rendered_snapshot"));
    const bool fov_row = api->feature(str("camera.local_fov")), pose_row = api->feature(str("camera.local_pose"));
    bool selection = false, captured = false;
    if (laid_out && mid2 && lifecycle && (snapshot_row || fov_row || pose_row)) {
        // the selection: a selection id per frame, and the one place a visual request is written
        selection = install(hooks, "camera/RenderFovSelected", &mid_select, "camera/RenderFovSelected");
        g_frames.set_selection_hooked(selection);
        // the capture: the snapshot, and the frame that confirms what the selection wrote
        captured = install(hooks, "camera/VisualFrameReady", &mid_capture, "camera/VisualFrameReady");
    }
    g_snapshot = captured && snapshot_row;
    if (g_snapshot) core->set_local_read_caps(KILN_CAMERA_CAP_RENDERED_SNAPSHOT);
    // a write needs both hooks (Applied only after the capture saw it); the pose parts also the local player's samples
    if (selection && captured) {
        if (fov_row) g_serving |= KILN_CAMERA_CAP_LOCAL_FOV;
        if (pose_row && g_sampling) g_serving |= KILN_CAMERA_CAP_LOCAL_POSE;
    }
    g_backend = std::make_unique<LocalBackend>(core, &g_frames, &g_board, &g_clock, &g_world);
    g_backend->set_serving(g_serving);
    core->set_backend(KILN_CAMERA_LOCAL_VISUAL, g_backend.get());
    logf(KILN_LOG_INFO, "camera: client side %s (rendered snapshot %s, local bits 0x%llx, sampler %s)", selection && captured ? "hooked" : "not hooked",
         g_snapshot ? "on" : "off", static_cast<unsigned long long>(g_serving), g_sampling ? "on" : "off");
}

bool local_pre(const KilnTickInfo* tick) {
    if (!g_backend) return false;
    const uint64_t step = tick ? tick->step : 0;
    if (g_sampling) {
        uint32_t targets[kSampleSlots];
        const uint32_t n = g_backend->targets(targets, kSampleSlots - 1);
        g_sampler.sample(g_frames.epoch(), step, g_world.local_player(), targets, n);
        if (g_sampler.faulted()) g_backend->set_serving(g_serving & KILN_CAMERA_CAP_LOCAL_FOV);  // until the next world
    }
    g_frames.set_sample_step(step);
    return g_backend->step();
}

KilnResult local_get_visual(KilnCameraVisual* out, uint32_t* out_reason) {
    if (!out || !out_reason || out->size < sizeof(KilnCameraVisual)) return KILN_ERR_INVALID_ARGUMENT;
    if (!g_snapshot) return KILN_ERR_UNAVAILABLE;
    *out_reason = g_frames.get_visual(out);
    return KILN_OK;
}

}  // namespace kcam

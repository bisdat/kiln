// SPDX-License-Identifier: MIT
// kiln.camera on a game client: local.h says what it is. This file is the glue to the game (sigdb, kiln.hooks, the
// client world's lifecycle, guarded memory); the logic it drives is in capture.cpp.
#include "local.h"

#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "services/kiln_hooks.h"
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

// ---- the hooks (render path: no game call, no author code, no waiting) ----------------------------------------------------

// camera/RenderFovSelected: r13 = renderer, xmm6's low float = the selected field of view (radians).
void KILN_CALL mid_select(KilnRegisters* regs, void*) {
    const Renderer r(g_mem, g_frames.layout(), regs->r13);
    g_frames.select(r);
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
    if (e->event == KILN_WORLD_STARTED) g_frames.world_started(e->session, static_cast<uint64_t>(g_api->world(KILN_WORLD_CLIENT)));
    else if (e->event == KILN_WORLD_STOPPING) g_frames.world_stopping();
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

    KilnHandle h = 0;
    const bool lifecycle = api->subscribe_world(owner, &on_world, nullptr, &h) == KILN_OK;
    const bool snapshot_row = api->feature(str("camera.rendered_snapshot"));
    if (laid_out && mid2 && lifecycle && snapshot_row) {
        // the selection only reads here (a selection id per frame); the capture takes the snapshot
        g_frames.set_selection_hooked(install(hooks, "camera/RenderFovSelected", &mid_select, "camera/RenderFovSelected"));
        g_snapshot = install(hooks, "camera/VisualFrameReady", &mid_capture, "camera/VisualFrameReady");
    }
    if (g_snapshot) core->set_local_read_caps(KILN_CAMERA_CAP_RENDERED_SNAPSHOT);
    logf(KILN_LOG_INFO, "camera: client side %s (rendered snapshot %s)", g_snapshot ? "hooked" : "not hooked", g_snapshot ? "on" : "off");
}

bool local_pre(const KilnTickInfo* tick) {
    if (tick) g_frames.set_sample_step(tick->step);
    return false;
}

KilnResult local_get_visual(KilnCameraVisual* out, uint32_t* out_reason) {
    if (!out || !out_reason || out->size < sizeof(KilnCameraVisual)) return KILN_ERR_INVALID_ARGUMENT;
    if (!g_snapshot) return KILN_ERR_UNAVAILABLE;
    *out_reason = g_frames.get_visual(out);
    return KILN_OK;
}

}  // namespace kcam

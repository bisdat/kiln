// SPDX-License-Identifier: MIT
// kiln.ui: Kiln's upscaler swap behind the game's FSR 3 option (0.2.0-dev.29.18 phase B; docs/kiln/graphics.md
// "Upscaler behind the game's FSR 3"; proven on the real client by spike B0, the wave's out/upscale-b0/REPORT.md).
//
// How the game upscales with FSR 3 (sigdb client.d/upscale.json, feature upscale.swap):
//  - PassRecord(ctx, state, in) builds the upscaler pass into the frame's render-command stream (state[0] 1 = FSR 3).
//    Its FSR branch declares how the six textures are used (pending barriers) and calls Fsr3Dispatch(ctx, fsr, desc),
//    which writes the FSR passes as ordinary render commands. Nothing touches Vulkan there.
//  - The game's main thread then runs the stream, one command at a time, through CmdExecute(state, vk functions,
//    VkCommandBuffer, command). Command types above upscale/client.cmd.last are skipped by the game.
//  - Fsr3Create makes the game's FSR 3 context at each FSR set-up (world load, FSR chosen, quality, resolution, 3D render
//    scale).
// What Kiln does, only when an upscaler other than the game's own was chosen at the game's start:
//  - Fsr3Create: the game's runs as always; Kiln latches the choice (it applies at an FSR set-up, like the DLSS
//    settings), and drops the backend's history.
//  - Fsr3Dispatch: Kiln reads the description (sigdb-captured offsets) and the six textures, checks them against the
//    input struct PassRecord saw, makes the game flush its pending barriers (FlushBarriers, as its DLSS branch does), and
//    writes one render command of its own (type 0xF0, KCmd) in place of the FSR passes. The game's FSR 3 recording is
//    skipped for that frame. Anything that does not fit: the game's own call, unchanged.
//  - CmdExecute: Kiln's command runs the backend into the frame's own VkCommandBuffer, at the FSR passes' place. A backend
//    that fails: a plain blit of the colour to the output for that frame, then the game's FSR 3 for the session.
//  - vk.cpp: the backend's instance and device needs are merged into the game's create infos (device_merge.h), only for
//    the game's own device (the first one with a swapchain extension); its objects go before the device does.
// Everything Kiln does inside the game's calls runs under fault guards. Kiln's hooks pass straight through while the
// player's choice is "game" (or anything failed); with "game" chosen at the start, no hook here is installed at all.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "device_merge.h"
#include "graphics_aside.h"
#include "graphics_settings.h"
#include "kiln_hook.hpp"
#include "ui.h"
#include "upscale.h"
#include "upscale_backend.h"
#include "upscale_logic.h"
#include "vendor_file.h"

namespace kui {
namespace {

using namespace upscale;

// ---------------------------------------------------------------------------------------------------------------
// The game's side (sigdb)

struct Layout {
    int64_t writer = 0, chunk = 0, view = 0, image = 0, w = 0, h = 0, fmt = 0, table = 0, last = 0, create_render = 0, create_output = 0;
    DescLayout desc;
} L;
using FlushFn = void (*)(void* ctx, uint64_t mask);
using AllocFn = uint8_t* (*)(void* pool);
FlushFn g_flush = nullptr;
AllocFn g_alloc = nullptr;
kiln::RawHook h_record, h_fsr, h_exec, h_create;

uint32_t g_fault_code = 0;
int fault_filter(EXCEPTION_POINTERS* ep) {
    g_fault_code = ep->ExceptionRecord->ExceptionCode;
    // a stack overflow is not recovered from here (its guard page would be gone): the game's own handling
    if (g_fault_code == EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_EXECUTE_HANDLER;
}
bool copy_guarded(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// State. The swap's per-frame work is the game's main thread's (PassRecord, Fsr3Dispatch, CmdExecute and Fsr3Create
// all run there, B0); the status is read from any thread (atomics, and g_mu for the strings).

std::mutex g_mu;                 // g_detail, g_backend's creation, g_files
std::string g_detail;            // the reason in words (status upscale_detail)
std::atomic<uint32_t> g_choice{0}, g_pin{0};  // the player's setting now (Choice, AmdPin)
std::atomic<uint32_t> g_latched{0}, g_latched_pin{0};  // as of the game's last FSR 3 set-up
std::atomic<bool> g_latched_any{false};
Choice g_start = Choice::game;   // the choice at the game's start: whose needs went into the device
std::atomic<bool> g_hooked{false};    // the pass hooks are in
std::atomic<bool> g_signatures{true};
uint32_t g_aside = 0;            // gfx::Aside keeping the swap off (fixed at start)
std::atomic<uint32_t> g_aside_run{0};
std::atomic<bool> g_started{false};  // upscale_start ran

std::unique_ptr<Backend> g_backend;   // the start choice's backend (null: game, or none in this build)
vendor::WinFs g_fs;
vendor::Verified g_files;             // kept for the session: the handles deny writes until the game ends
std::atomic<bool> g_load_tried{false}, g_loaded{false};
std::atomic<uint32_t> g_why{0};       // the backend's own Why (files, device, load, create...)
std::atomic<bool> g_failed{false};    // fell back to the game's FSR 3 for the session
std::atomic<bool> g_faulted{false};   // a fault in the backend: Kiln never calls into it again (it may hold a lock)
std::atomic<uint32_t> g_running{0}, g_version{0};
std::atomic<bool> g_amd_pinned{false};

// the game's device, as the backend got it
VkContext g_vk;                       // main thread (device hooks and frames)
std::atomic<VkDevice> g_device{VK_NULL_HANDLE};  // the merged device (the game's own)
std::atomic<bool> g_device_ready{false};
struct InstanceInfo {
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t api = 0;                  // apiVersion as created
    std::vector<std::string> extensions;  // enabled as created
};
std::vector<InstanceInfo> g_instances;  // live instances (g_mu)

// frames
std::atomic<uint64_t> g_last_fsr_ms{0}, g_last_kiln_ms{0}, g_fsr_frames{0}, g_kiln_frames{0};
std::atomic<uint32_t> g_rw{0}, g_rh{0}, g_ow{0}, g_oh{0};
std::atomic<bool> g_setup_pending{true};   // Fsr3Create ran since the last context check
bool g_ctx = false;                        // the backend has a context (main thread)
CreateInfo g_ci;                           // the context's sizes
HistoryGuard g_hist;
uint64_t g_world = 0;                      // bumped when FSR frames stop for over half a second (a load, a menu)
int64_t g_qpc_last = 0, g_qpc_freq = 0;
thread_local const uint8_t* t_in = nullptr;  // PassRecord's input struct, for the Fsr3Dispatch inside it

// One slot per frame in flight for Kiln's command (record -> execute; the same thread, but kept apart by seq)
struct Slot {
    std::atomic<uint32_t> seq{0};
    Frame frame;
};
constexpr uint32_t kSlots = 8;
Slot g_slots[kSlots];
std::atomic<uint32_t> g_seq{0};

// the blit fallback's functions (the game's device)
PFN_vkCmdPipelineBarrier p_barrier = nullptr;
PFN_vkCmdBlitImage p_blit = nullptr;
PFN_vkDeviceWaitIdle p_wait_idle = nullptr;
PFN_vkGetPhysicalDeviceFormatProperties p_format_props = nullptr;
VkPhysicalDevice g_phys = VK_NULL_HANDLE;

void set_detail(const std::string& d) {
    std::lock_guard lk(g_mu);
    g_detail = d;
}
// The backend's own reason (status), and one kiln.log line per change of it.
void say_why(Why w, const std::string& detail, int level = KILN_LOG_WARN) {
    static Why logged_why = Why::none;
    static std::string logged;
    bool fresh = false;
    {
        std::lock_guard lk(g_mu);
        g_why.store(static_cast<uint32_t>(w));
        g_detail = detail;
        fresh = w != logged_why || detail != logged;
        if (fresh) logged_why = w, logged = detail;
    }
    if (fresh && w != Why::none) logf(level, "graphics: upscaler: %s%s%s", why_text(w), detail.empty() ? "" : ": ", detail.c_str());
}
// The session falls back to the game's FSR 3.
void fall_back(Why w, const std::string& detail) {
    if (g_failed.exchange(true)) return;
    say_why(w, detail, KILN_LOG_ERROR);
    logf(KILN_LOG_ERROR, "graphics: upscaler: %s failed (%s): the game's own FSR 3 runs until the game restarts",
         g_backend ? g_backend->name().c_str() : "the backend", detail.c_str());
}

Choice choice_now() { return static_cast<Choice>(g_choice.load(std::memory_order_relaxed)); }
// The swap runs a frame: hooked, the backend chosen at start is the latched choice, it loaded, the device has it,
// nothing failed.
bool swap_on() {
    return g_hooked.load(std::memory_order_relaxed) && g_backend && !g_failed.load(std::memory_order_relaxed) &&
           g_latched_any.load(std::memory_order_relaxed) && static_cast<Choice>(g_latched.load(std::memory_order_relaxed)) == g_start &&
           g_loaded.load(std::memory_order_relaxed) && g_device_ready.load(std::memory_order_relaxed);
}

int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// ---------------------------------------------------------------------------------------------------------------
// The backend's files and load (the first vkCreateInstance while the swap is armed)

std::wstring api_wpath(int kind) {
    char b[2048];
    size_t n = 0;
    if (!g_api || g_api->path(kind, b, sizeof b, &n) != KILN_OK || !n || n >= sizeof b) return {};
    const int w = MultiByteToWideChar(CP_UTF8, 0, b, static_cast<int>(n), nullptr, 0);
    std::wstring s(static_cast<size_t>(w), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, b, static_cast<int>(n), s.data(), w);
    return s;
}

template <class F>
bool guarded(F&& f) {
    __try {
        f();
        return true;
    } __except (fault_filter(GetExceptionInformation())) {
        return false;
    }
}
// A call into the backend under a fault guard (a std::function-free trampoline: SEH and C++ objects do not mix in one
// frame under clang-cl, so the lambda runs in guarded() and its result comes back through the capture).
#define KUI_GUARD(what, ...)                                                                                       \
    ([&]() {                                                                                                       \
        if (guarded([&]() { __VA_ARGS__; })) return true;                                                          \
        char b_[96];                                                                                               \
        std::snprintf(b_, sizeof b_, "a fault (0x%08x) in %s", g_fault_code, what);                                \
        g_faulted = true;                                                                                          \
        fall_back(Why::fault, b_);                                                                                 \
        return false;                                                                                              \
    }())

void load_backend() {
    if (g_load_tried.exchange(true) || !g_backend) return;
    vendor::Verified v;
    try {
        v = vendor::verify_component(g_fs, api_wpath(KILN_PATH_KILN), g_backend->component());
    } catch (...) {
        v.check = vendor::Check::unreadable;
        v.detail = "checking the files failed";
    }
    if (!v.ok()) {
        say_why(static_cast<Why>(vendor::check_why(v.check)), v.detail);
        return;
    }
    Files files{v.dir, v.paths, v.version};
    {
        std::lock_guard lk(g_mu);
        g_files = std::move(v);
    }
    Why why = Why::load_failed;
    std::string detail;
    bool ok = false;
    if (!KUI_GUARD("the backend's load", ok = g_backend->load(files, &why, &detail))) return;
    if (!ok) {
        say_why(why, detail);
        return;
    }
    g_loaded = true;
    g_why = static_cast<uint32_t>(Why::none);
    logf(KILN_LOG_INFO, "graphics: upscaler: %s files %s checked against Kiln's pins%s%s", g_backend->component(),
         vendor::narrow(files.dir).c_str(), detail.empty() ? "" : "; ", detail.c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// The device

struct InstanceScope {
    device_merge::InstanceMerge merge;
    device_merge::Report report;
    uint32_t api = 0;
};
struct DeviceScope {
    device_merge::DeviceMerge merge;
    device_merge::Report report;
    PhysicalDevice gpu;
    const VkDeviceCreateInfo* kiln_ci = nullptr;  // the merger's answer (the game's own when nothing had to change)
    InstanceInfo inst;                             // the instance the physical device came from
    bool merged = false;  // the backend's needs were asked for and could all be met (Report::ok)
};

// The live instance `phys` came from: each instance hands out its own physical-device handles, so the one whose list
// holds `phys` is it. Not found (Kiln saw no instance made): the lowest api of the live ones, no extensions.
InstanceInfo instance_of(VkPhysicalDevice phys, PFN_vkGetInstanceProcAddr gipa) {
    std::vector<InstanceInfo> live;
    {
        std::lock_guard lk(g_mu);
        live = g_instances;
    }
    for (const auto& i : live) {
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(i.instance, "vkEnumeratePhysicalDevices"));
        uint32_t n = 0;
        if (!enumerate || enumerate(i.instance, &n, nullptr) != VK_SUCCESS || !n || n > 64) continue;
        std::vector<VkPhysicalDevice> p(n);
        if (enumerate(i.instance, &n, p.data()) != VK_SUCCESS && n == 0) continue;
        for (uint32_t k = 0; k < n; ++k)
            if (p[k] == phys) return i;
    }
    InstanceInfo none;
    for (const auto& i : live) none.api = none.api ? std::min(none.api, i.api) : i.api;
    if (!none.api) none.api = VK_API_VERSION_1_2;
    return none;
}

bool has_swapchain_ext(const VkDeviceCreateInfo* ci) {
    for (uint32_t i = 0; ci && i < ci->enabledExtensionCount; ++i)
        if (ci->ppEnabledExtensionNames[i] && std::strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) return true;
    return false;
}

void log_report(const char* what, const device_merge::Report& r) {
    std::string added;
    for (const auto& e : r.added_extensions) added += (added.empty() ? "" : ", ") + e;
    for (const auto& f : r.added_features) added += (added.empty() ? "" : ", ") + f;
    std::string skipped;
    for (const auto& s : r.skipped_optional) skipped += (skipped.empty() ? "" : ", ") + s;
    std::string missing;
    for (const auto& m : r.missing_required) missing += (missing.empty() ? "" : ", ") + m;
    logf(r.ok() ? KILN_LOG_INFO : KILN_LOG_WARN, "graphics: upscaler: %s: %s%s%s%s%s", what,
         added.empty() ? "nothing added" : ("added " + added).c_str(), skipped.empty() ? "" : "; the device lacks (optional) ",
         skipped.c_str(), missing.empty() ? "" : "; MISSING (required) ", missing.c_str());
    for (const auto& n : r.notes) logf(KILN_LOG_INFO, "graphics: upscaler: %s", n.c_str());
}

}  // namespace

const VkInstanceCreateInfo* upscale_instance_begin(const VkInstanceCreateInfo* ci, PFN_vkGetInstanceProcAddr gipa, void** scope) {
    *scope = nullptr;
    if (!g_hooked.load() || !g_backend || !ci) return ci;
    load_backend();
    auto* s = new InstanceScope();
    s->api = ci->pApplicationInfo && ci->pApplicationInfo->apiVersion ? ci->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    *scope = s;
    if (!g_loaded.load() || !gipa) return ci;
    InstanceNeeds needs;
    if (!KUI_GUARD("the backend's instance needs", needs = g_backend->instance_needs())) return ci;
    if (needs.extensions.empty() && (!needs.min_api_version || needs.min_api_version <= s->api)) return ci;
    const VkInstanceCreateInfo* use = ci;
    try {
        use = s->merge.merge(ci, needs, gipa, &s->report);
    } catch (...) {
        use = ci;
    }
    log_report("the game's Vulkan instance", s->report);
    if (!s->report.ok()) say_why(Why::device, "the Vulkan instance lacks what the upscaler needs");
    if (s->report.api_version) s->api = s->report.api_version;
    return use;
}

void upscale_instance_end(void* scope, const VkInstanceCreateInfo* used, VkResult r, VkInstance instance) {
    auto* s = static_cast<InstanceScope*>(scope);
    if (!s) return;
    if (r == VK_SUCCESS && instance) {
        InstanceInfo info;
        info.instance = instance;
        info.api = used && used->pApplicationInfo && used->pApplicationInfo->apiVersion ? used->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
        for (uint32_t i = 0; used && i < used->enabledExtensionCount; ++i) info.extensions.emplace_back(used->ppEnabledExtensionNames[i]);
        std::lock_guard lk(g_mu);
        g_instances.push_back(std::move(info));
    }
    delete s;
}

void upscale_instance_destroyed(VkInstance instance) {
    if (!g_hooked.load()) return;
    std::lock_guard lk(g_mu);
    g_instances.erase(std::remove_if(g_instances.begin(), g_instances.end(), [&](const InstanceInfo& p) { return p.instance == instance; }),
                      g_instances.end());
}

const VkDeviceCreateInfo* upscale_device_begin(VkInstance instance, VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                               PFN_vkGetInstanceProcAddr gipa, void** scope) {
    *scope = nullptr;
    if (!g_hooked.load() || !g_backend || !ci || !gipa) return ci;
    // the game's own device: the first one with a swapchain while Kiln has none (the client also makes a second device
    // with 79 extensions next to its own, B0; it is left alone)
    if (g_device.load() || !has_swapchain_ext(ci)) return ci;
    load_backend();
    auto* s = new DeviceScope();
    *scope = s;
    s->gpu.physical = phys;
    s->gpu.gipa = gipa;
    s->inst = instance_of(phys, gipa);
    s->gpu.instance = s->inst.instance ? s->inst.instance : instance;  // the instance phys came from (vk.cpp gives the latest)
    s->gpu.instance_api = s->inst.api;
    if (auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"))) props(phys, &s->gpu.props);
    if (!g_loaded.load()) return ci;
    DeviceNeeds needs;
    if (!KUI_GUARD("the backend's device needs", needs = g_backend->device_needs(s->gpu))) return ci;
    const VkDeviceCreateInfo* use = ci;
    bool threw = false;
    try {
        use = s->merge.merge(ci, needs, s->gpu, &s->report);
    } catch (...) {
        use = ci;
        threw = true;
    }
    s->merged = !threw && s->report.ok();
    s->kiln_ci = threw ? nullptr : use;
    log_report("the game's Vulkan device", s->report);
    if (!s->report.ok()) {
        std::string missing;
        for (const auto& m : s->report.missing_required) missing += (missing.empty() ? "" : ", ") + m;
        say_why(Why::device, "the graphics card or driver lacks " + missing);
    }
    return use;
}

void upscale_device_end(void* scope, const VkDeviceCreateInfo* used, VkResult r, VkDevice device, PFN_vkGetDeviceProcAddr gdpa,
                        uint32_t graphics_family) {
    std::unique_ptr<DeviceScope> s(static_cast<DeviceScope*>(scope));
    if (!s || r != VK_SUCCESS || !device || !used) return;
    VkDevice none = VK_NULL_HANDLE;
    if (!g_device.compare_exchange_strong(none, device)) return;
    VkContext vk;
    vk.instance = s->gpu.instance;
    vk.physical = s->gpu.physical;
    vk.device = device;
    vk.gipa = s->gpu.gipa;
    vk.gdpa = gdpa;
    vk.instance_api = s->gpu.instance_api;
    vk.instance_extensions = s->inst.extensions;
    vk.graphics_family = graphics_family;
    vk.props = s->gpu.props;
    // what is on the device: the merger's account when its answer was used (vk.cpp tries the game's own again when
    // Kiln's copy fails), else what the create info used holds
    const bool kiln_used = s->kiln_ci && used == s->kiln_ci;
    if (kiln_used && s->report.changed) {
        vk.extensions = s->report.extensions;
        vk.features = s->report.features;
    } else {
        for (uint32_t i = 0; i < used->enabledExtensionCount; ++i) vk.extensions.emplace_back(used->ppEnabledExtensionNames[i]);
        vk.features = device_merge::enabled_features(*used);
    }
    vk.needs_merged = s->merged && kiln_used;
    const bool refused = s->kiln_ci && s->report.changed && !kiln_used;  // vk.cpp tried Kiln's copy, the driver refused it
    if (gdpa) {
        p_barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(gdpa(device, "vkCmdPipelineBarrier"));
        p_blit = reinterpret_cast<PFN_vkCmdBlitImage>(gdpa(device, "vkCmdBlitImage"));
        p_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(gdpa(device, "vkDeviceWaitIdle"));
    }
    p_format_props = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(vk.gipa(vk.instance, "vkGetPhysicalDeviceFormatProperties"));
    g_phys = vk.physical;
    g_vk = vk;
    logf(KILN_LOG_INFO, "graphics: upscaler: the game's device %p on %s (Vulkan %u.%u, instance %p %u.%u with %zu extensions): %zu extensions",
         reinterpret_cast<void*>(device), vk.props.deviceName, VK_API_VERSION_MAJOR(vk.props.apiVersion), VK_API_VERSION_MINOR(vk.props.apiVersion),
         reinterpret_cast<void*>(s->inst.instance), VK_API_VERSION_MAJOR(vk.instance_api), VK_API_VERSION_MINOR(vk.instance_api),
         vk.instance_extensions.size(), vk.extensions.size());
    // the game unloads vulkan-1.dll and maps it again early in its start (B0): whether that happened before its device,
    // with a vendor DLL already mapped (one that imports vulkan-1.dll would keep it from unloading)
    const int maps = vk_install_count();
    logf(KILN_LOG_INFO, "graphics: upscaler: vulkan-1.dll was mapped %d time%s before the game's device: %s", maps, maps == 1 ? "" : "s",
         maps > 1 ? "the game's unload and reload happened" : "no unload and reload seen (a DLL mapped at the first vkCreateInstance may keep it mapped)");
    if (!g_loaded.load() || !g_backend) return;
    if (refused) {
        say_why(Why::device, "the graphics driver refused the Vulkan extensions and features the upscaler needs (the game's own device was made)");
        return;
    }
    Why why = Why::device;
    std::string detail;
    bool ok = false;
    if (!KUI_GUARD("the backend's device set-up", ok = g_backend->device_ready(g_vk, &why, &detail))) return;
    if (!ok) {
        say_why(why, detail);
        return;
    }
    g_device_ready = true;
    g_why = static_cast<uint32_t>(Why::none);
    set_detail(detail);
    logf(KILN_LOG_INFO, "graphics: upscaler: %s is ready on the game's device%s%s", g_backend->name().c_str(), detail.empty() ? "" : ": ",
         detail.c_str());
}

void upscale_device_destroy(VkDevice device) {
    if (!device || g_device.load() != device) return;
    g_device_ready = false;
    if (p_wait_idle) guarded([&]() { p_wait_idle(device); });
    if (g_backend && !g_faulted.load()) {
        if (g_ctx) KUI_GUARD("the backend's destroy", g_backend->destroy());
        KUI_GUARD("the backend's device end", g_backend->device_lost(device));
    }
    g_ctx = false;
    g_running = static_cast<uint32_t>(Running::off);
    g_device.store(VK_NULL_HANDLE);
    logf(KILN_LOG_INFO, "graphics: upscaler: the game's device %p is destroyed: the upscaler's Vulkan objects went first",
         reinterpret_cast<void*>(device));
}

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Reading the game's frame (record time)

struct TexRaw {
    uint64_t image = 0, view = 0;
    uint32_t w = 0, h = 0;
    uint8_t fmt = 0;
    bool ok = false;
};
// handle (16 bytes: owner pointer, u16 index at +8) -> texture object -> its fields. Guarded: a stale handle faults
// inside, never in the game.
bool resolve(const uint8_t* handle, TexRaw* t) {
    __try {
        const uintptr_t owner = *reinterpret_cast<const uintptr_t*>(handle);
        const uint16_t index = *reinterpret_cast<const uint16_t*>(handle + 8);
        if (!owner) return false;
        const uintptr_t table = *reinterpret_cast<const uintptr_t*>(owner + L.table);
        if (!table) return false;
        const uintptr_t obj = *reinterpret_cast<const uintptr_t*>(table + static_cast<uintptr_t>(index) * 8);
        if (!obj) return false;
        t->image = *reinterpret_cast<const uint64_t*>(obj + L.image);
        t->view = *reinterpret_cast<const uint64_t*>(obj + L.view);
        t->w = *reinterpret_cast<const uint32_t*>(obj + L.w);
        t->h = *reinterpret_cast<const uint32_t*>(obj + L.h);
        t->fmt = *reinterpret_cast<const uint8_t*>(obj + L.fmt);
        t->ok = t->image && t->view && t->w && t->h;
        return t->ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

Texture texture_of(const TexRaw& r, bool output) {
    Texture t;
    if (!r.ok) return t;
    t.image = reinterpret_cast<VkImage>(r.image);
    t.view = reinterpret_cast<VkImageView>(r.view);
    t.format = static_cast<VkFormat>(r.fmt);  // the engine's format byte is the VkFormat (B0)
    t.width = r.w;
    t.height = r.h;
    t.layout = output ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return t;
}

// Builds this frame from the description and the input struct. False: not usable this frame (`why`, `detail`).
bool build_frame(const uint8_t* desc_p, const uint8_t* in_p, Frame* f, Why* why, std::string* detail) {
    uint8_t desc[DescLayout::kDescBytes];
    uint8_t in[kInBytes];
    if (!copy_guarded(desc, desc_p, sizeof desc)) return *why = Why::inputs, *detail = "the FSR description could not be read", false;
    const bool have_in = in_p && copy_guarded(in, in_p, sizeof in);
    if (const char* d = desc_matches_input(have_in ? in : nullptr, desc, L.desc)) {
        *why = Why::inputs;
        *detail = std::string("the FSR description's ") + d + " is not the pass's (a game build Kiln does not know?)";
        return false;
    }
    TexRaw raw[kRoles];
    for (int r = 0; r < kRoles; ++r) resolve(desc + kDescRes[r] + kDescHandle, &raw[r]);
    f->color = texture_of(raw[kColor], false);
    f->depth = texture_of(raw[kDepth], false);
    f->motion = texture_of(raw[kMotion], false);
    f->output = texture_of(raw[kOutput], true);
    f->reactive = texture_of(raw[kReactive], false);
    f->transparency = texture_of(raw[kTransparency], false);
    const DescValues v = read_desc(desc, L.desc);
    f->jitter_x = v.jitter_x;
    f->jitter_y = v.jitter_y;
    f->mv_scale_x = v.mv_x;
    f->mv_scale_y = v.mv_y;
    f->render_width = v.render_w;
    f->render_height = v.render_h;
    f->output_width = v.upscale_w;
    f->output_height = v.upscale_h;
    planes(v.plane_a, v.plane_b, &f->camera_near, &f->camera_far);
    f->fov_y = sane_fov(v.fov);
    f->pre_exposure = sane_exposure(v.exposure);
    f->sharpness = std::isfinite(v.sharpness) ? std::clamp(v.sharpness, 0.0f, 1.0f) : 0.0f;
    f->reset = v.reset;
    // the frame time: the game's, else Kiln's own time since the last FSR frame
    const int64_t now = qpc();
    if (!g_qpc_freq) {
        LARGE_INTEGER fr;
        QueryPerformanceFrequency(&fr);
        g_qpc_freq = fr.QuadPart > 0 ? fr.QuadPart : 10'000'000;
    }
    const double measured = g_qpc_last ? static_cast<double>(now - g_qpc_last) * 1000.0 / static_cast<double>(g_qpc_freq) : 0.0;
    g_qpc_last = now;
    f->frame_ms = pick_frame_ms(v.frame_ns, measured);
    if (const char* bad = check_frame(*f, detail)) {
        (void)bad;
        *why = Why::inputs;
        return false;
    }
    return true;
}

// Kiln's command into the game's stream: FlushBarriers first (the six textures' declared transitions are recorded
// before Kiln's command, so they are in SHADER_READ_ONLY / GENERAL when it runs), then the command, in the writer's
// current chunk (a new chunk from the game's own allocator when it is full, linked as the game's DLSS branch does).
bool flush_guarded(void* ctx) {
    __try {
        g_flush(ctx, 0x7ffffffffULL);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool write_cmd(void* ctx, const KCmd& c) {
    __try {
        uint8_t* writer = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(ctx) + L.writer);
        uint8_t* chunk = *reinterpret_cast<uint8_t**>(writer + L.chunk);
        uint8_t* wp = *reinterpret_cast<uint8_t**>(chunk + 8);
        uint8_t* end = *reinterpret_cast<uint8_t**>(chunk + 0x10);
        if (!chunk_has_room(reinterpret_cast<uintptr_t>(wp), reinterpret_cast<uintptr_t>(end))) {
            uint8_t* fresh = g_alloc(*reinterpret_cast<void**>(writer));
            if (!fresh) return false;
            *reinterpret_cast<uint8_t**>(chunk) = fresh;  // the old chunk's next
            *reinterpret_cast<uint8_t**>(writer + L.chunk) = fresh;
            chunk = fresh;
            wp = *reinterpret_cast<uint8_t**>(chunk + 8);
            end = *reinterpret_cast<uint8_t**>(chunk + 0x10);
            if (!chunk_has_room(reinterpret_cast<uintptr_t>(wp), reinterpret_cast<uintptr_t>(end))) return false;
        }
        std::memcpy(wp, &c, sizeof c);
        *reinterpret_cast<uint8_t**>(chunk + 8) = wp + sizeof(KCmd);
        ++*reinterpret_cast<uint64_t*>(chunk + 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The context for this frame's sizes: made at the first frame after a set-up, and again when the textures outgrow it.
bool ensure_context(const Frame& f) {
    const CreateInfo ci = context_for(f, static_cast<AmdPin>(g_latched_pin.load()));  // hdr, inverted depth, dynamic resolution
    const bool pending = g_setup_pending.exchange(false);
    if (g_ctx && same_context(g_ci, ci) && frame_fits(g_ci, f)) {
        if (pending) {
            KUI_GUARD("the backend's reset", g_backend->reset());
            g_hist.drop();
        }
        return true;
    }
    if (g_ctx) {
        if (!KUI_GUARD("the backend's destroy", g_backend->destroy())) return false;
        g_ctx = false;
    }
    std::string detail;
    bool ok = false;
    if (!KUI_GUARD("the backend's create", ok = g_backend->create(ci, &detail))) return false;
    if (!ok) {
        fall_back(Why::create_failed, detail.empty() ? "the context could not be made" : detail);
        return false;
    }
    g_ctx = true;
    g_ci = ci;
    g_hist.drop();
    g_running = static_cast<uint32_t>(g_backend->running());
    g_version = g_backend->version();
    g_amd_pinned = ci.amd == AmdPin::fsr3 && g_backend->choice() == Choice::amd;
    logf(KILN_LOG_INFO, "graphics: upscaler: %s set up for %ux%u -> %ux%u (output texture %ux%u)%s%s", g_backend->name().c_str(),
         ci.max_render_width, ci.max_render_height, ci.output_width, ci.output_height, ci.output_texture_width, ci.output_texture_height,
         detail.empty() ? "" : ": ", detail.c_str());
    set_detail(detail);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The hooks

void hk_record(void* ctx, uint8_t* state, uint8_t* in) {
    uint8_t mode = 0;
    copy_guarded(&mode, state, 1);
    if (mode == 1) g_last_fsr_ms.store(now_ms(), std::memory_order_relaxed);
    const uint8_t* was = t_in;
    t_in = mode == 1 ? in : nullptr;
    h_record.call<void>(ctx, state, in);
    t_in = was;
}

void* hk_create(void* a, void* b, void* c, const uint8_t* p) {
    void* r = h_create.call<void*>(a, b, c, p);
    // the choice applies at an FSR set-up, as the DLSS settings do
    const uint32_t ch = g_choice.load(), pin = g_pin.load();
    const bool changed = !g_latched_any.load() || g_latched.load() != ch || g_latched_pin.load() != pin;
    g_latched = ch;
    g_latched_pin = pin;
    g_latched_any = true;
    g_setup_pending = true;
    uint32_t v[4] = {};
    copy_guarded(v, p + L.create_render, 8);
    copy_guarded(v + 2, p + L.create_output, 8);
    static uint32_t said = 0;
    if (changed || said < 3) {
        ++said;
        logf(KILN_LOG_INFO, "graphics: upscaler: the game set FSR 3 up for %ux%u -> %ux%u; behind it: %s", v[0], v[1], v[2], v[3],
             static_cast<Choice>(ch) == Choice::game ? "the game's own FSR 3"
             : static_cast<Choice>(ch) != g_start  ? "the game's own FSR 3 (the choice changed since the game's start: it applies from the next start)"
             : g_failed.load()                      ? "the game's own FSR 3 (the upscaler failed in this session)"
             : !swap_on()                           ? "the game's own FSR 3 (the upscaler is not ready)"
                                                    : g_backend->name().c_str());
    }
    return r;
}

// The game's own FSR 3 for this frame. Its first frame after Kiln's has the game's history from before them: the
// description's reset byte asks the game's FSR 3 to drop it (the game supports it; it never sets it itself).
bool g_kiln_recorded = false;  // the last FSR frame was Kiln's (main thread)
void game_fsr(void* ctx, uint8_t* fsr, uint8_t* desc) {
    if (g_kiln_recorded) {
        g_kiln_recorded = false;
        const uint8_t one = 1;
        copy_guarded(desc + L.desc.reset, &one, 1);
    }
    h_fsr.call<void>(ctx, fsr, desc);
}

void hk_fsr(void* ctx, uint8_t* fsr, uint8_t* desc) {
    const uint64_t t = now_ms();
    g_last_fsr_ms.store(t, std::memory_order_relaxed);
    const uint64_t n = g_fsr_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        // the pass's sizes for the status, whoever upscales
        uint32_t sz[4] = {};
        if (copy_guarded(sz, desc + L.desc.render, 8) && copy_guarded(sz + 2, desc + L.desc.upscale, 8)) {
            g_rw.store(sz[0], std::memory_order_relaxed);
            g_rh.store(sz[1], std::memory_order_relaxed);
            g_ow.store(sz[2], std::memory_order_relaxed);
            g_oh.store(sz[3], std::memory_order_relaxed);
        }
    }
    // what the backend kept for the GPU goes once the GPU is done with it, whether or not it records this frame
    if (g_backend && g_loaded.load(std::memory_order_relaxed) && !g_faulted.load(std::memory_order_relaxed))
        KUI_GUARD("the backend's tick", g_backend->tick(n));
    if (!swap_on()) {
        // the context goes once nothing uses it (the choice changed, a failure): the GPU memory back to the game
        if (g_ctx && g_backend) {
            if (!g_faulted.load()) KUI_GUARD("the backend's destroy", g_backend->destroy());
            g_ctx = false;
            g_running = static_cast<uint32_t>(Running::off);
        }
        game_fsr(ctx, fsr, desc);
        return;
    }
    Frame f;
    Why why = Why::none;
    std::string detail;
    // a gap of over half a second between FSR frames (loading, a menu without the world): a cut, history goes
    static uint64_t prev_ms = 0;
    if (prev_ms && t - prev_ms > 500) ++g_world;
    prev_ms = t;
    static uint64_t bad = 0, bad_logged = 0;  // unusable frames in a row (a load or a size change may give a few)
    if (!build_frame(desc, t_in, &f, &why, &detail)) {
        ++bad;
        if (bad_logged++ < 3 || bad_logged % 600 == 0)
            logf(KILN_LOG_WARN, "graphics: upscaler: frame %llu: %s: the game's FSR 3 runs this frame", static_cast<unsigned long long>(n), detail.c_str());
        if (bad >= 120) fall_back(why, detail);  // every frame for about two seconds: for the session
        say_why(why, detail);
        game_fsr(ctx, fsr, desc);
        return;
    }
    if (bad) {
        bad = 0;
        if (static_cast<Why>(g_why.load()) == Why::inputs) say_why(Why::none, {}, KILN_LOG_INFO);
    }
    if (VkDevice sd = vk_swapchain_device(); sd && sd != g_device.load()) {
        fall_back(Why::no_vulkan, "the game draws with another Vulkan device than the one Kiln set the upscaler up on");
        game_fsr(ctx, fsr, desc);
        return;
    }
    if (!ensure_context(f)) {
        game_fsr(ctx, fsr, desc);
        return;
    }
    f.frame = n;
    f.reset = g_hist.frame(f, g_world, f.reset);
    if (f.reset) {
        // what the backend gets, once per set-up, a load or a size change (kiln.log, for the real-client checks)
        static uint32_t said = 0;
        if (said++ < 12)
            logf(KILN_LOG_INFO, "graphics: upscaler: frame %llu (history reset): colour %ux%u fmt %d, depth fmt %d, motion fmt %d, masks fmt %d/%d, "
                 "output %ux%u fmt %d; render %ux%u -> %ux%u, jitter %.4f %.4f, MV scale %.0f %.0f, planes %.3g..%.6g, FOV %.4f, exposure %.4f, "
                 "frame %.2f ms", static_cast<unsigned long long>(n), f.color.width, f.color.height, f.color.format, f.depth.format, f.motion.format,
                 f.reactive.format, f.transparency.format, f.output.width, f.output.height, f.output.format, f.render_width, f.render_height,
                 f.output_width, f.output_height, f.jitter_x, f.jitter_y, f.mv_scale_x, f.mv_scale_y, f.camera_near, f.camera_far, f.fov_y,
                 f.pre_exposure, f.frame_ms);
    }
    const uint32_t seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    Slot& s = g_slots[seq % kSlots];
    s.seq.store(0, std::memory_order_relaxed);
    s.frame = f;
    s.seq.store(seq, std::memory_order_release);
    if (!flush_guarded(ctx)) {
        // a fault in the game's own flush: its barrier list may be half written, so Kiln writes nothing more there
        s.seq.store(0, std::memory_order_relaxed);
        fall_back(Why::fault, "a fault in the game's barrier flush before Kiln's command");
        game_fsr(ctx, fsr, desc);
        return;
    }
    if (!write_cmd(ctx, make_cmd(kUpscale, seq, seq % kSlots))) {
        s.seq.store(0, std::memory_order_relaxed);
        static uint32_t said = 0;
        if (said++ < 3) logf(KILN_LOG_WARN, "graphics: upscaler: writing Kiln's render command failed: the game's FSR 3 runs this frame");
        game_fsr(ctx, fsr, desc);
        return;
    }
    g_kiln_recorded = true;
    // the game's FSR 3 passes are not recorded for this frame
}

// A plain linear blit of the colour (render size) to the output: a frame whose backend failed is never left empty.
// Both textures have the transfer usage the game gave them (B0: colour TRANSFER_SRC, output TRANSFER_DST).
bool blit_formats_ok(VkFormat src, VkFormat dst) {
    static VkFormat last_src = VK_FORMAT_UNDEFINED, last_dst = VK_FORMAT_UNDEFINED;
    static bool last_ok = false;
    if (src == last_src && dst == last_dst) return last_ok;
    VkFormatProperties s{}, d{};
    if (p_format_props && g_phys) p_format_props(g_phys, src, &s), p_format_props(g_phys, dst, &d);
    last_src = src, last_dst = dst;
    last_ok = (s.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (s.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) &&
              (d.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
    return last_ok;
}
void blit_fallback(VkCommandBuffer cb, const Frame& f) {
    if (!p_barrier || !p_blit || !blit_formats_ok(f.color.format, f.output.format)) return;
    VkImageMemoryBarrier b[2]{};
    for (auto& x : b) {
        x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        x.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    b[0].image = f.color.image;
    b[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[1].image = f.output.image;
    b[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    p_barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    VkImageBlit r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.srcOffsets[1] = {static_cast<int32_t>(f.render_width), static_cast<int32_t>(f.render_height), 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstOffsets[1] = {static_cast<int32_t>(f.output_width), static_cast<int32_t>(f.output_height), 1};
    p_blit(cb, f.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_LINEAR);
    std::swap(b[0].oldLayout, b[0].newLayout);
    std::swap(b[1].oldLayout, b[1].newLayout);
    b[0].srcAccessMask = 0;
    b[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
}

void run_cmd(VkCommandBuffer cb, const KCmd& c) {
    Slot& s = g_slots[c.slot % kSlots];
    if (s.seq.load(std::memory_order_acquire) != c.seq) {
        static uint32_t said = 0;
        if (said++ < 5) logf(KILN_LOG_WARN, "graphics: upscaler: Kiln's command %u found its frame gone: skipped", c.seq);
        return;
    }
    Frame f = s.frame;
    f.cb = cb;
    std::string detail;
    bool ok = false;
    const bool live = g_ctx && g_backend && !g_failed.load() && !g_faulted.load();
    if (live && KUI_GUARD("the backend's upscale", ok = g_backend->record(f, &detail)) && ok) {
        const uint64_t k = g_kiln_frames.fetch_add(1, std::memory_order_relaxed) + 1;
        g_last_kiln_ms.store(now_ms(), std::memory_order_relaxed);
        if (k == 1 || k == 600 || k % 36000 == 0)
            logf(KILN_LOG_INFO, "graphics: upscaler: %s upscaled %llu frames in the game's command buffer", g_backend->name().c_str(),
                 static_cast<unsigned long long>(k));
        return;
    }
    if (live && !ok && !g_failed.load()) fall_back(Why::runtime_failed, detail.empty() ? "the upscale failed" : detail);
    guarded([&]() { blit_fallback(cb, f); });
}

void hk_exec(void* st, void* fns, VkCommandBuffer cb, const uint8_t* cmd) {
    if (cmd && cmd[2] == kCmdType) {
        KCmd c;
        if (copy_guarded(&c, cmd, sizeof c) && is_kiln_cmd(reinterpret_cast<const uint8_t*>(&c), sizeof c, &c)) {
            run_cmd(cb, c);
            return;
        }
    }
    h_exec.call<void>(st, fns, cb, cmd);
}

bool sym(const char* n, uintptr_t* out) { return g_api->symbol(KilnStr{n, std::strlen(n)}, out) == KILN_OK && *out; }
bool cst(const char* n, int64_t* out) { return g_api->constant(KilnStr{n, std::strlen(n)}, out) == KILN_OK; }
bool feat(const char* n) { return g_api->feature(KilnStr{n, std::strlen(n)}); }

std::unique_ptr<Backend> make_backend(Choice c) {
    if (c == Choice::amd) return make_amd_backend();
    if (c == Choice::xess) return make_xess_backend();
    return nullptr;
}

}  // namespace

void upscale_start(const gfx::Settings& s, uint32_t aside, const std::string& aside_files) {
    g_choice = static_cast<uint32_t>(s.upscaler);
    g_pin = static_cast<uint32_t>(s.amd_pin);
    g_start = static_cast<Choice>(s.upscaler);
    g_aside = aside & gfx::kAsideUpscale;
    g_started = true;
    if (!s.wants_swap()) return;  // "game": nothing is installed, nothing runs
    const char* name = gfx::upscaler_name(s.upscaler);
    if (g_aside) {
        g_why = static_cast<uint32_t>(Why::aside);
        std::string d = "Kiln steps aside for " + gfx::aside_names(g_aside);
        if (!aside_files.empty()) d += " (" + aside_files + ")";
        set_detail(d);
        logf(KILN_LOG_INFO, "graphics: upscaler: %s is chosen; %s: the game's own FSR 3 runs", name, d.c_str());
        return;
    }
    g_backend = make_backend(g_start);
    if (!g_backend) {
        g_why = static_cast<uint32_t>(Why::unavailable);
        set_detail(std::string(name) + " is not in this build of Kiln");
        logf(KILN_LOG_WARN, "graphics: upscaler: %s is not in this build of Kiln: the game's own FSR 3 runs", name);
        return;
    }
    uintptr_t record = 0, fsr = 0, exec = 0, flush = 0, alloc = 0, create = 0;
    int64_t d[12] = {};
    const bool ok = feat("upscale.swap") && sym("upscale/client.PassRecord", &record) && sym("upscale/client.Fsr3Dispatch", &fsr) &&
                    sym("upscale/client.CmdExecute", &exec) && sym("upscale/client.FlushBarriers", &flush) &&
                    sym("upscale/client.CmdChunkAlloc", &alloc) && sym("upscale/client.Fsr3Create", &create) &&
                    cst("upscale/client.ctx.writer", &L.writer) && cst("upscale/client.writer.chunk", &L.chunk) &&
                    cst("upscale/client.tex.view", &L.view) && cst("upscale/client.tex.image", &L.image) && cst("upscale/client.tex.width", &L.w) &&
                    cst("upscale/client.tex.height", &L.h) && cst("upscale/client.tex.format", &L.fmt) && cst("upscale/client.handle.table", &L.table) &&
                    cst("upscale/client.cmd.last", &L.last) && cst("upscale/client.create.render", &L.create_render) &&
                    cst("upscale/client.create.output", &L.create_output) && cst("upscale/client.desc.jitter", &d[0]) &&
                    cst("upscale/client.desc.prev_render", &d[1]) && cst("upscale/client.desc.render", &d[2]) && cst("upscale/client.desc.upscale", &d[3]) &&
                    cst("upscale/client.desc.reset", &d[4]) && cst("upscale/client.desc.fov", &d[5]) && cst("upscale/client.desc.near", &d[6]) &&
                    cst("upscale/client.desc.far", &d[7]) && cst("upscale/client.desc.exposure", &d[8]) && cst("upscale/client.desc.mv_scale", &d[9]) &&
                    cst("upscale/client.desc.frame_ns", &d[10]) && cst("upscale/client.desc.sharpness", &d[11]);
    L.desc = DescLayout{d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11]};
    if (!ok || L.last >= kCmdType || !L.desc.sane() || L.create_render < 0 || L.create_output < 0 || L.create_render > 0x40 ||
        L.create_output > 0x40) {
        g_signatures = false;
        g_why = static_cast<uint32_t>(Why::no_signatures);
        set_detail("sigdb feature upscale.swap is off on this game build");
        logf(KILN_LOG_WARN, "graphics: upscaler: %s is chosen, but this game build's FSR 3 pass was not found (sigdb feature upscale.swap): "
             "the game's own FSR 3 runs", name);
        return;
    }
    // another tool already on the game's FSR 3 pass: step aside (Kiln never hooks over another tool)
    const uintptr_t mine[] = {record, fsr, exec, create};
    for (uintptr_t at : mine) {
        uint8_t b[16] = {};
        if (copy_guarded(b, reinterpret_cast<const void*>(at), sizeof b) && gfx::patched_entry(b)) {
            g_aside_run.fetch_or(gfx::kAsideUpscaler);
            g_why = static_cast<uint32_t>(Why::aside);
            set_detail("another module already patched the game's FSR 3 pass");
            logf(KILN_LOG_WARN, "graphics: upscaler: the game's FSR 3 pass at rva 0x%llx is already patched by another module: Kiln steps aside",
                 static_cast<unsigned long long>(at - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
            g_backend.reset();
            return;
        }
    }
    g_flush = reinterpret_cast<FlushFn>(flush);
    g_alloc = reinterpret_cast<AllocFn>(alloc);
    // the executor first (it must know Kiln's command before any is written), the record side last
    struct HookDef { kiln::RawHook* h; uintptr_t at; void* fn; const char* name; } hs[] = {
        {&h_exec, exec, reinterpret_cast<void*>(&hk_exec), "upscale/client.CmdExecute"},
        {&h_create, create, reinterpret_cast<void*>(&hk_create), "upscale/client.Fsr3Create"},
        {&h_fsr, fsr, reinterpret_cast<void*>(&hk_fsr), "upscale/client.Fsr3Dispatch"},
        {&h_record, record, reinterpret_cast<void*>(&hk_record), "upscale/client.PassRecord"},
    };
    for (auto& h : hs) {
        if (h.h->install(g_api, g_owner, h.at, h.fn, h.name)) continue;
        logf(KILN_LOG_ERROR, "graphics: upscaler: hooking %s failed (%s): the game's own FSR 3 runs", h.name, h.h->error() ? h.h->error() : "?");
        for (auto& o : hs) o.h->revoke();
        g_why = static_cast<uint32_t>(Why::load_failed);
        set_detail(std::string("hooking ") + h.name + " failed");
        g_backend.reset();
        return;
    }
    g_hooked = true;
    logf(KILN_LOG_INFO, "graphics: upscaler: %s is chosen: Kiln hooks the game's FSR 3 pass (PassRecord, Fsr3Dispatch, CmdExecute, Fsr3Create) "
         "and checks %s's files when the game makes its Vulkan instance", name, g_backend->component());
}

void upscale_settings(const gfx::Settings& s) {
    g_choice = static_cast<uint32_t>(s.upscaler);
    g_pin = static_cast<uint32_t>(s.amd_pin);
}

void upscale_status(KilnUiGraphicsStatus* out) {
    if (!out || out->size < KILN_UI_GRAPHICS_STATUS_SIZE_V2) return;
    const Choice chosen = choice_now();
    out->upscaler = static_cast<uint32_t>(chosen);
    out->amd_upscaler = g_pin.load();
    const uint64_t t = now_ms();
    const bool fsr_seen = g_hooked.load() && g_last_fsr_ms.load() && t - g_last_fsr_ms.load() < 2000;
    const bool kiln_live = g_last_kiln_ms.load() && t - g_last_kiln_ms.load() < 2000;
    StatusIn in;
    in.chosen = chosen;
    in.hooked = g_hooked.load();
    in.hooked_for = g_start;
    in.aside = g_aside | g_aside_run.load();
    in.signatures = g_signatures.load();
    in.fsr_seen = fsr_seen;
    in.failed = g_failed.load();
    in.why = static_cast<Why>(g_why.load());
    // the choice now is not the one the hooks and the device were made for: from the next start
    if (in.hooked && chosen != Choice::game && chosen != g_start) in.why = Why::restart;
    // the latched choice (the game's last FSR 3 set-up) is not the one now: it applies at the next set-up
    else if (in.hooked && chosen == g_start && (in.why == Why::none || in.why == Why::waiting) && g_latched_any.load() &&
             static_cast<Choice>(g_latched.load()) != chosen)
        in.why = Why::waiting;
    in.latched = static_cast<Choice>(g_latched.load());
    in.started = kiln_live && in.latched == g_start;
    in.backend_running = static_cast<Running>(g_running.load());
    if (!g_started.load() && chosen != Choice::game) in.why = Why::restart;
    const StatusOut o = decide_status(in);
    out->upscale_running = static_cast<uint32_t>(o.running);
    out->upscale_reason = static_cast<uint32_t>(o.why);
    uint32_t flags = 0;
    if (in.hooked) flags |= KILN_UI_UPSCALE_HOOKED;
    if (fsr_seen) flags |= KILN_UI_UPSCALE_FSR_SEEN;
    if (g_loaded.load()) flags |= KILN_UI_UPSCALE_FILES_OK;
    if (g_device_ready.load()) flags |= KILN_UI_UPSCALE_DEVICE_OK;
    if (in.failed) flags |= KILN_UI_UPSCALE_FELL_BACK;
    if (o.running == Running::amd_fsr3 && g_amd_pinned.load()) flags |= KILN_UI_UPSCALE_AMD_PINNED;
    out->upscale_flags = flags;
    out->aside_upscale = in.aside;
    const bool kiln_runs = o.running == Running::amd_fsr4 || o.running == Running::amd_fsr3 || o.running == Running::xess;
    out->upscale_version = kiln_runs ? g_version.load() : 0;
    if (fsr_seen) {
        out->upscale_render_width = g_rw.load(std::memory_order_relaxed);
        out->upscale_render_height = g_rh.load(std::memory_order_relaxed);
        out->upscale_output_width = g_ow.load(std::memory_order_relaxed);
        out->upscale_output_height = g_oh.load(std::memory_order_relaxed);
    }
    std::string detail;
    if (o.why != Why::none) {
        std::lock_guard lk(g_mu);
        detail = g_detail;
    }
    if (detail.empty() && o.why != Why::none) detail = why_text(o.why);
    copy_detail(out->upscale_detail, sizeof out->upscale_detail, detail);
}

}  // namespace kui

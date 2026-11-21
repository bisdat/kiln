// SPDX-License-Identifier: MIT
#include "overlay_guard_runtime.h"
// kiln.ui: the game's Vulkan presents (docs/re/client-render.md).
//
// Interception
//  - Wine/Proton: vulkan-1.dll's exports forward to winevulkan.dll, and winevulkan's vkGetDeviceProcAddr returns those
//    same exported functions, so inline hooks on them (resolved through vulkan-1 with GetProcAddress) see every call
//    the game makes, however it got its pointers.
//  - Windows: the loader's exports are trampolines the game does not use for device calls; Kiln hooks
//    vkGetInstanceProcAddr / vkGetDeviceProcAddr and hands out its own functions for the few it needs.
// Drawing (only while something is to be drawn)
//  - per swapchain of the game window: image views, a render pass that LOADs the game's picture and keeps it in
//    PRESENT_SRC, framebuffers, one command buffer + fence + semaphore per image;
//  - in vkQueuePresentKHR: build the ImGui frame, record it into the presented image, submit on the present queue
//    waiting on the game's semaphores, present waiting on Kiln's semaphore instead.
// Before the overlay: subscribers of the pre-overlay callback (kiln_ui.h) chain their own submit in the same way.
// Only on a queue the game got whose family has graphics (vk_present.h: frame generation presents from queues of its
// own); the first failure (a submit, a fault, ImGui's backend) turns the drawing off for the session and every
// present after it goes on as the game made it (ui.cpp overlay_turned_off: one log line, status, Mods caption, guard).
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <memory>
#include <utility>

#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"
#include "safetyhook.hpp"
#include "image_budget.h"
#include "ui.h"
#include "upscale.h"
#include "upscale_reentry.h"
#include "vk_hooks.h"
#include "vk_present.h"

namespace kui {
namespace {

const char* vk_name(VkResult r) {
    switch (r) {
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        default: return "another Vulkan error";
    }
}

// The overlay's drawing is off for the rest of the session (presents keep passing through untouched): fail() sets it
// with its reason, written once under g_fail_mu; hk_present tells it once, outside g_mu (announce_failure).
std::atomic<bool> g_failed{false};
std::atomic<bool> g_fail_said{false};
// draw() is making the overlay's frame (past its checks, until guarded_draw returns): vk_drawing, for overlay_menu_ask,
// which waits for a frame still being made (the first of the process starts ImGui and the menu's code)
std::atomic<bool> g_drawing{false};
std::mutex g_fail_mu;
bool g_fail_taken = false;
int g_fail_level = KILN_LOG_ERROR;
char g_fail_reason[128] = "";
char g_fail_detail[512] = "";

// Turns the overlay's drawing off; the first reason is the one told. `reason` is short (the Mods caption, the status),
// the detail is for kiln.log. Returns false, for draw()'s `return fail(...)`.
bool fail(int level, const char* reason, const char* detail_fmt, ...) {
    // formatted before the lock: draw() runs under a fault guard, and a fault with the lock held would keep it held
    char detail[sizeof g_fail_detail];
    va_list ap;
    va_start(ap, detail_fmt);
    std::vsnprintf(detail, sizeof detail, detail_fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lk(g_fail_mu);
        if (!g_fail_taken) {
            g_fail_taken = true;
            g_fail_level = level;
            std::snprintf(g_fail_reason, sizeof g_fail_reason, "%s", reason);
            std::memcpy(g_fail_detail, detail, sizeof detail);
        }
    }
    g_failed.store(true, std::memory_order_release);
    return false;
}

// Once, on a present call after fail(), outside g_mu.
void announce_failure() {
    if (!g_failed.load(std::memory_order_acquire) || g_fail_said.exchange(true)) return;
    int level = KILN_LOG_ERROR;
    char reason[sizeof g_fail_reason], detail[sizeof g_fail_detail];
    {
        std::lock_guard<std::mutex> lk(g_fail_mu);
        level = g_fail_level;
        std::memcpy(reason, g_fail_reason, sizeof reason);
        std::memcpy(detail, g_fail_detail, sizeof detail);
    }
    overlay_turned_off(level, reason, detail);
}

// ImGui's Vulkan backend reports every call's result here (it ignored them before 0.2.0-dev.29.15): a failed
// vkAllocateDescriptorSets leaves a texture with no descriptor set, which style.cpp then never draws (image_budget.h). Each
// error code is logged once a process; VK_SUCCESS and the positive codes are not errors. A lost device, or a call Vulkan
// calls not valid (VK_ERROR_UNKNOWN: ImGui's texture upload on a queue of another family, inbox 80), turns the overlay's
// drawing off before Kiln submits a frame of its own.
std::atomic<uint32_t> g_vk_logged{0};
void check_vk(VkResult err) {
    if (err >= 0) return;
    const uint32_t bit = 1u << (static_cast<uint32_t>(-static_cast<int32_t>(err)) & 31u);
    if (!(g_vk_logged.fetch_or(bit) & bit))
        logf(KILN_LOG_WARN, "ui: a Vulkan call of the overlay's ImGui backend failed (VkResult %d); what it made is not drawn", static_cast<int>(err));
    if (err == VK_ERROR_DEVICE_LOST || err == VK_ERROR_UNKNOWN)
        fail(KILN_LOG_ERROR, "a Vulkan call of its ImGui backend failed",
             "a Vulkan call of the overlay's ImGui backend (its texture uploads and buffers) returned %d (%s)", static_cast<int>(err), vk_name(err));
}

std::atomic<bool> g_wine{false};
std::atomic<bool> g_installed{false};

struct Real {
    std::atomic<PFN_vkGetInstanceProcAddr> gipa{nullptr};
    std::atomic<PFN_vkGetDeviceProcAddr> gdpa{nullptr};
    std::atomic<PFN_vkCreateInstance> create_instance{nullptr};
    std::atomic<PFN_vkDestroyInstance> destroy_instance{nullptr};
    std::atomic<PFN_vkCreateDevice> create_device{nullptr};
    std::atomic<PFN_vkDestroyDevice> destroy_device{nullptr};
    std::atomic<PFN_vkGetDeviceQueue> get_queue{nullptr};
    std::atomic<PFN_vkGetDeviceQueue2> get_queue2{nullptr};
    std::atomic<PFN_vkCreateSwapchainKHR> create_swapchain{nullptr};
    std::atomic<PFN_vkDestroySwapchainKHR> destroy_swapchain{nullptr};
    std::atomic<PFN_vkQueuePresentKHR> present{nullptr};
    std::atomic<PFN_vkCreateWin32SurfaceKHR> create_surface{nullptr};
} R;

struct HookState {
    // Only the atomics are shared with the loader notification; everything else is under g_install_mu.
    std::atomic<uintptr_t> target{0};
    std::atomic<uintptr_t> owner{0};
    std::atomic<uint64_t> loads{0}, unloads{0};
    uint64_t seen_loads = 0, seen_unloads = 0;
    SafetyHookInline* hook = nullptr;  // published hooks/trampolines are never moved or destroyed
    vk_hooks::Snapshot bytes;
    bool told_skip = false;
};
static_assert(std::atomic<uintptr_t>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free);
HookState h_destroy_instance, h_gipa, h_gdpa, h_create_instance, h_create_device, h_destroy_device, h_get_queue,
    h_get_queue2, h_create_swapchain, h_destroy_swapchain, h_present, h_create_surface;
HookState* const g_hooks[] = {&h_destroy_instance, &h_gipa, &h_gdpa, &h_create_instance, &h_create_device,
    &h_destroy_device, &h_get_queue, &h_get_queue2, &h_create_swapchain, &h_destroy_swapchain, &h_present, &h_create_surface};

// Device-level functions Kiln calls itself (from the game device's vkGetDeviceProcAddr).
struct DeviceFns {
#define KUI_FN(n) PFN_##n n = nullptr;
    KUI_FN(vkGetSwapchainImagesKHR) KUI_FN(vkCreateImageView) KUI_FN(vkDestroyImageView) KUI_FN(vkCreateRenderPass)
    KUI_FN(vkDestroyRenderPass) KUI_FN(vkCreateFramebuffer) KUI_FN(vkDestroyFramebuffer) KUI_FN(vkCreateCommandPool)
    KUI_FN(vkDestroyCommandPool) KUI_FN(vkAllocateCommandBuffers) KUI_FN(vkFreeCommandBuffers)
    KUI_FN(vkBeginCommandBuffer) KUI_FN(vkEndCommandBuffer) KUI_FN(vkResetCommandBuffer) KUI_FN(vkCmdBeginRenderPass)
    KUI_FN(vkCmdEndRenderPass) KUI_FN(vkQueueSubmit) KUI_FN(vkCreateFence) KUI_FN(vkDestroyFence)
    KUI_FN(vkWaitForFences) KUI_FN(vkResetFences) KUI_FN(vkCreateSemaphore) KUI_FN(vkDestroySemaphore)
    KUI_FN(vkDeviceWaitIdle)
#undef KUI_FN
};

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;  // real
    DeviceFns fn;
    std::map<VkQueue, uint32_t> queue_family;  // the queues seen handed out on it, with their families (vk_present.h)
    std::vector<uint32_t> families;            // each queue family's VkQueueFlags (empty: not read)
    std::vector<VkQueue> tool_queues;          // handed out while it was being made: a tool's own, never drawn on
    // the queues the game asked for in its create info (hk_create_device), looked up once a present's queue is not
    // known (present_queue)
    std::vector<vk_present::Requested> requested;
    bool looked_up = false;
    // Kiln did not see this device being created and took it over at its swapchain (adopt_device): only the queues
    // it saw handed out are drawn with, never a guessed family.
    bool adopted = false;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint32_t pool_family = UINT32_MAX;
    bool imgui_vk = false;      // ImGui_ImplVulkan_Init done for this device
    VkRenderPass imgui_pass = VK_NULL_HANDLE;  // render pass the ImGui pipeline was made for
    VkFormat imgui_format = VK_FORMAT_UNDEFINED;
};

struct Swap {
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    HWND hwnd = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    uint32_t min_images = 2;
    bool drawable = false;  // created with COLOR_ATTACHMENT usage
    VkImageUsageFlags usage = 0;
    std::vector<VkImage> listed;  // the driver's image list, fetched once for the pre-overlay callback
    bool ready = false;     // resources below exist
    VkRenderPass pass = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkCommandBuffer> commands;
    std::vector<VkFence> fences;
    std::vector<VkSemaphore> done;
};

std::mutex g_mu;  // device/swapchain maps (creation threads vs. the render thread)
VkInstance g_instance = VK_NULL_HANDLE;
uint32_t g_instance_api = VK_API_VERSION_1_0;
// Queues handed out (vkGetDeviceQueue) on devices Kiln did not see being created, with their families: adopt_device.
std::map<VkDevice, std::map<VkQueue, uint32_t>> g_unseen_queues;
std::map<VkDevice, Device*> g_devices;  // every live device the game created
Device* g_dev = nullptr;  // the device ImGui's backend is set up for (the drawable swapchain's)
std::atomic<VkDevice> g_swap_device{VK_NULL_HANDLE};  // the device of the game's last swapchain (the upscaler swap's check)
std::map<VkSwapchainKHR, Swap> g_swaps;
std::map<VkSurfaceKHR, HWND> g_surfaces;
std::atomic<HMODULE> g_vulkan{nullptr};

std::atomic<uint64_t> g_presents{0}, g_frames{0}, g_swapchain_count{0}, g_last_draw_ms{0};
std::atomic<bool> g_has_device{false};
std::atomic<uint32_t> g_swap_count{0};  // live swapchains (status only)
std::atomic<uint32_t> g_width{0}, g_height{0}, g_format{0}, g_colorspace{0};
std::atomic<float> g_dpi{1.0f};
std::atomic<double> g_frame_ms{0};
bool g_logged_draw = false;

struct PreOverlaySub {
    KilnHandle handle;
    KilnUiPreOverlayFn fn;
    KilnUiSwapchainGoneFn gone;
    void* user;
};
std::vector<PreOverlaySub> g_pre_overlay;  // under g_mu
std::atomic<uint32_t> g_pre_demand{0};
std::atomic<uint32_t> g_extra_usage{0};

template <typename T>
T dfn(Device& d, const char* name) {
    return reinterpret_cast<T>(d.gdpa ? d.gdpa(d.device, name) : nullptr);
}

void load_device_fns(Device& d) {
#define KUI_FN(n) d.fn.n = dfn<PFN_##n>(d, #n);
    KUI_FN(vkGetSwapchainImagesKHR) KUI_FN(vkCreateImageView) KUI_FN(vkDestroyImageView) KUI_FN(vkCreateRenderPass)
    KUI_FN(vkDestroyRenderPass) KUI_FN(vkCreateFramebuffer) KUI_FN(vkDestroyFramebuffer) KUI_FN(vkCreateCommandPool)
    KUI_FN(vkDestroyCommandPool) KUI_FN(vkAllocateCommandBuffers) KUI_FN(vkFreeCommandBuffers)
    KUI_FN(vkBeginCommandBuffer) KUI_FN(vkEndCommandBuffer) KUI_FN(vkResetCommandBuffer) KUI_FN(vkCmdBeginRenderPass)
    KUI_FN(vkCmdEndRenderPass) KUI_FN(vkQueueSubmit) KUI_FN(vkCreateFence) KUI_FN(vkDestroyFence)
    KUI_FN(vkWaitForFences) KUI_FN(vkResetFences) KUI_FN(vkCreateSemaphore) KUI_FN(vkDestroySemaphore)
    KUI_FN(vkDeviceWaitIdle)
#undef KUI_FN
}

bool device_fns_ok(const Device& d) {
    const void* const* p = reinterpret_cast<const void* const*>(&d.fn);
    for (size_t i = 0; i < sizeof(DeviceFns) / sizeof(void*); ++i)
        if (!p[i]) return false;
    return true;
}

// ImGui's function loader: instance functions through the real vkGetInstanceProcAddr, the rest through the device,
// last the module's exports (on Wine every function is exported by winevulkan).
PFN_vkVoidFunction imgui_loader(const char* name, void*) {
    PFN_vkVoidFunction f = nullptr;
    if (g_dev && g_dev->gdpa) f = g_dev->gdpa(g_dev->device, name);
    if (HMODULE m = g_vulkan.load(); !f && m) f = reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(m, name));
    if (!f && R.gipa && g_dev && g_dev->instance) f = R.gipa.load()(g_dev->instance, name);
    return f;
}

void destroy_swap_resources(Device& d, Swap& s) {
    if (!s.ready) return;
    std::vector<VkFence> fences;  // a fence that could not be made again after a failed submit is null (draw)
    for (auto f : s.fences)
        if (f) fences.push_back(f);
    if (!fences.empty()) d.fn.vkWaitForFences(d.device, static_cast<uint32_t>(fences.size()), fences.data(), VK_TRUE, 1000000000ull);
    for (auto f : s.framebuffers) d.fn.vkDestroyFramebuffer(d.device, f, nullptr);
    for (auto v : s.views) d.fn.vkDestroyImageView(d.device, v, nullptr);
    for (auto f : fences) d.fn.vkDestroyFence(d.device, f, nullptr);
    for (auto x : s.done) d.fn.vkDestroySemaphore(d.device, x, nullptr);
    if (!s.commands.empty() && d.pool) d.fn.vkFreeCommandBuffers(d.device, d.pool, static_cast<uint32_t>(s.commands.size()), s.commands.data());
    // the render pass may be ImGui's pipeline pass: keep it while the ImGui backend uses it
    if (s.pass && s.pass != d.imgui_pass) d.fn.vkDestroyRenderPass(d.device, s.pass, nullptr);
    s.framebuffers.clear();
    s.views.clear();
    s.fences.clear();
    s.done.clear();
    s.commands.clear();
    s.images.clear();
    s.pass = VK_NULL_HANDLE;
    s.ready = false;
}

VkRenderPass make_pass(Device& d, VkFormat format) {
    VkAttachmentDescription a{};
    a.format = format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // keep the game's frame
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1;
    ci.pAttachments = &a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    VkRenderPass p = VK_NULL_HANDLE;
    return d.fn.vkCreateRenderPass(d.device, &ci, nullptr, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

bool ensure_pool(Device& d, uint32_t family) {
    if (d.pool && d.pool_family == family) return true;
    if (d.pool) {
        // a different queue family than before: rebuild everything that uses the pool
        d.fn.vkDeviceWaitIdle(d.device);
        for (auto& [k, s] : g_swaps)
            if (s.device == d.device) destroy_swap_resources(d, s);
        d.fn.vkDestroyCommandPool(d.device, d.pool, nullptr);
        d.pool = VK_NULL_HANDLE;
    }
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = family;
    if (d.fn.vkCreateCommandPool(d.device, &ci, nullptr, &d.pool) != VK_SUCCESS) return false;
    d.pool_family = family;
    return true;
}

bool ensure_swap_resources(Device& d, Swap& s) {
    if (s.ready) return true;
    uint32_t n = 0;
    if (d.fn.vkGetSwapchainImagesKHR(d.device, s.swapchain, &n, nullptr) != VK_SUCCESS || n == 0 || n > 16) return false;
    s.images.resize(n);
    if (d.fn.vkGetSwapchainImagesKHR(d.device, s.swapchain, &n, s.images.data()) != VK_SUCCESS) return false;
    s.pass = (d.imgui_pass && d.imgui_format == s.format) ? d.imgui_pass : make_pass(d, s.format);
    if (!s.pass) return false;
    s.views.assign(n, VK_NULL_HANDLE);
    s.framebuffers.assign(n, VK_NULL_HANDLE);
    s.fences.assign(n, VK_NULL_HANDLE);
    s.done.assign(n, VK_NULL_HANDLE);
    s.commands.assign(n, VK_NULL_HANDLE);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = d.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = n;
    if (d.fn.vkAllocateCommandBuffers(d.device, &ai, s.commands.data()) != VK_SUCCESS) {
        s.commands.clear();
        return false;
    }
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = s.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = s.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (d.fn.vkCreateImageView(d.device, &vi, nullptr, &s.views[i]) != VK_SUCCESS) return false;
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = s.pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &s.views[i];
        fi.width = s.extent.width;
        fi.height = s.extent.height;
        fi.layers = 1;
        if (d.fn.vkCreateFramebuffer(d.device, &fi, nullptr, &s.framebuffers[i]) != VK_SUCCESS) return false;
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (d.fn.vkCreateFence(d.device, &fc, nullptr, &s.fences[i]) != VK_SUCCESS) return false;
        VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (d.fn.vkCreateSemaphore(d.device, &sc, nullptr, &s.done[i]) != VK_SUCCESS) return false;
    }
    s.ready = true;
    logf(KILN_LOG_INFO, "ui: overlay resources for swapchain %ux%u format %d (%u images)", s.extent.width, s.extent.height,
         static_cast<int>(s.format), n);
    return true;
}

bool ensure_imgui_vulkan(Device& d, Swap& s, VkQueue queue, uint32_t family) {
    if (d.imgui_vk && d.imgui_format == s.format) return true;
    if (d.imgui_vk) {
        // the swapchain format changed (e.g. SDR -> HDR): rebuild ImGui's pipeline for a compatible render pass
        ImGui_ImplVulkan_PipelineInfo pi{};
        pi.RenderPass = s.pass;
        pi.Subpass = 0;
        pi.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ImGui_ImplVulkan_CreateMainPipeline(&pi);
        VkRenderPass old = d.imgui_pass;
        d.imgui_pass = s.pass;
        d.imgui_format = s.format;
        bool still_used = false;
        for (auto& [k, o] : g_swaps)
            if (o.pass == old) still_used = true;
        if (old && !still_used) d.fn.vkDestroyRenderPass(d.device, old, nullptr);
        logf(KILN_LOG_INFO, "ui: ImGui pipeline rebuilt for format %d", static_cast<int>(s.format));
        return true;
    }
    static bool loaded = false;
    if (!loaded) {
        loaded = ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, &imgui_loader, nullptr);
        if (!loaded) {
            set_error("ImGui could not load Vulkan functions");
            return false;
        }
    }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_0;
    ii.Instance = d.instance;
    ii.PhysicalDevice = d.physical;
    ii.Device = d.device;
    ii.QueueFamily = family;
    ii.Queue = queue;
    ii.DescriptorPoolSize = img::kDescriptorPoolSize;  // every image Kiln lets exist, plus ImGui's own (image_budget.h)
    ii.CheckVkResultFn = &check_vk;
    ii.MinImageCount = std::max<uint32_t>(2, s.min_images);
    ii.ImageCount = std::max<uint32_t>(ii.MinImageCount, static_cast<uint32_t>(s.images.size()));
    ii.PipelineInfoMain.RenderPass = s.pass;
    ii.PipelineInfoMain.Subpass = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        set_error("ImGui_ImplVulkan_Init failed");
        return false;
    }
    d.imgui_vk = true;
    d.imgui_pass = s.pass;
    d.imgui_format = s.format;
    logf(KILN_LOG_INFO, "ui: ImGui Vulkan backend ready (queue family %u, %zu images)", family, s.images.size());
    return true;
}

// Kiln's own lookup of the game's queues (present_queue, g_mu held) on this thread: hk_get_queue does not remember them
// again (it would take g_mu), should the real vkGetDeviceQueue lead back into Kiln's hook through another tool's patch.
thread_local bool t_looking_up = false;

bool guarded_get_queue(PFN_vkGetDeviceQueue fn, VkDevice dev, uint32_t family, uint32_t index, VkQueue* out) {
    __try {
        fn(dev, family, index, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = VK_NULL_HANDLE;
        return false;
    }
}

// What the overlay may do with a present's queue (draw, run_pre_overlay; g_mu held): vk_present::classify. A queue Kiln
// did not see the game get, or saw only while the device was being made, is looked up once per device among the queues
// the game asked for when it made it (vk_present.h look_up_requested): the game's own are drawn on as before
// 0.2.0-dev.29.18, with their own family; a tool's are not.
vk_present::QueueUse present_queue(Device& d, VkQueue queue, uint32_t* family) {
    using vk_present::QueueUse;
    QueueUse use = vk_present::classify(d.queue_family, d.families, d.tool_queues, queue, family);
    if ((use != QueueUse::unknown && use != QueueUse::tool) || d.adopted || d.looked_up || d.requested.empty()) return use;
    d.looked_up = true;
    const auto get = R.get_queue.load();
    if (!get) return use;
    const QueueUse before = use;
    t_looking_up = true;
    vk_present::look_up_requested(d.requested, [&](uint32_t f, uint32_t i, VkQueue* q) { return guarded_get_queue(get, d.device, f, i, q); },
                                  d.queue_family, d.tool_queues);
    t_looking_up = false;
    use = vk_present::classify(d.queue_family, d.families, d.tool_queues, queue, family);
    const bool own = use != QueueUse::unknown && use != QueueUse::tool;
    logf(own ? KILN_LOG_INFO : KILN_LOG_WARN, "ui: the game presents with a queue Kiln %s; among the queues it asked for when it made device %p "
         "(%zu queue families) it is %s", before == QueueUse::unknown ? "did not see it get" : "saw handed out only while the device was being made",
         reinterpret_cast<void*>(d.device), d.requested.size(), own ? "there: the game's own" : "not there: a tool's own");
    return use;
}

// The overlay's work inside one present. False = present as the game asked.
bool draw(VkQueue queue, const VkPresentInfoKHR* info, VkSemaphore* out_wait, uint64_t t0) {
    // the drawing went off on another present thread (frame generation's) while this one waited for g_mu: no frame
    // callbacks, no ImGui frame (it would set the input flags overlay_turned_off clears), no submit
    if (g_failed.load(std::memory_order_acquire)) return false;
    Swap* s = nullptr;
    uint32_t index = 0;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        auto it = g_swaps.find(info->pSwapchains[i]);
        if (it != g_swaps.end() && it->second.drawable && it->second.hwnd) {
            s = &it->second;
            index = info->pImageIndices[i];
            break;
        }
    }
    if (!s) return false;
    auto dit = g_devices.find(s->device);
    if (dit == g_devices.end() || !device_fns_ok(*dit->second)) return false;
    Device& d = *dit->second;
    // Kiln's frame goes to the queue the frame is presented from: only one the game got, of a family with graphics,
    // never a tool's own or a guessed family (vk_present.h)
    uint32_t family = 0;
    switch (present_queue(d, queue, &family)) {
        case vk_present::QueueUse::unknown:
            return fail(KILN_LOG_WARN, "the game presents from a queue Kiln does not know",
                        d.adopted ? "the game presents with a queue Kiln did not see handed out on the device it took over, so its family is not known"
                                  : "the game presents with a queue Kiln did not see handed out and that is not one the game asked for when it "
                                    "made its device (a tool between the game and Vulkan added it), so its family is not known and a frame "
                                    "drawn for another family would fail on it");
        case vk_present::QueueUse::tool: {
            // the layer Kiln steps aside for, when it is one Kiln found next to the game at the start (overlay_guard_tools)
            bool optiscaler = false, streamline = false;
            overlay_guard_tools(&optiscaler, &streamline);
            return fail(KILN_LOG_WARN, "frame generation presents the game's frames from its own queue",
                        "the game's frames are presented from a queue a tool under Kiln added to the game's device while it was being "
                        "made (queue family %u%s): frame generation (OptiScaler's, or another tool's) presents the game's frames and its "
                        "own from its own thread and queue, where Kiln's overlay does not draw%s", family,
                        family < d.families.size() && !(d.families[family] & VK_QUEUE_GRAPHICS_BIT) ? ", which cannot draw" : "",
                        optiscaler   ? "; OptiScaler is installed next to the game, and its frame generation presents so"
                        : streamline ? "; NVIDIA Streamline is installed next to the game, and its frame generation presents so"
                                     : "");
        }
        case vk_present::QueueUse::no_graphics:
            return fail(KILN_LOG_WARN, "the game presents from a queue that cannot draw",
                        "the game's frames are presented from queue family %u, which has no graphics, so Kiln's overlay cannot draw there",
                        family);
        case vk_present::QueueUse::drawable:
            break;
    }
    // From here on the overlay does work of its own (its resources, ImGui, the frame, the submit). The crash guard watches
    // the first such draw of the process, also one whose frame turns out empty (one atomic read after that). A present Kiln
    // got no further on than the checks above (no swapchain or device of its own seen, a queue it does not draw on) is no
    // draw of the overlay: a start that dies with only those reads "swapchain created", which never locks (since
    // 0.2.0-dev.30.2; before, the guard took any present that wanted a frame as the overlay's first draw).
    overlay_guard_first_draw();
    g_drawing.store(true, std::memory_order_relaxed);
    if (g_dev && g_dev != &d && g_dev->imgui_vk) {
        // the game draws with another device now: move ImGui's backend over
        g_dev->fn.vkDeviceWaitIdle(g_dev->device);
        ImGui_ImplVulkan_Shutdown();
        g_dev->imgui_vk = false;
    }
    g_dev = &d;
    static bool logged_steps = false;
    const bool steps = !logged_steps;
    logged_steps = true;
    if (steps) logf(KILN_LOG_INFO, "ui: first draw: device %p, swapchain %p image %u", reinterpret_cast<void*>(d.device), reinterpret_cast<void*>(s->swapchain), index);
    if (steps) logf(KILN_LOG_INFO, "ui: first draw: queue family %u", family);
    if (!ensure_pool(d, family) || !ensure_swap_resources(d, *s) || index >= s->images.size())
        return fail(KILN_LOG_ERROR, "its Vulkan resources could not be made",
                    "the overlay's command pool, image views, framebuffers, fences or semaphores for the %ux%u swapchain could not be made",
                    s->extent.width, s->extent.height);
    const float dpi = g_dpi.load();
    if (steps) logf(KILN_LOG_INFO, "ui: first draw: resources ready, creating ImGui");
    if (!imgui_ready() && !imgui_create(s->hwnd, dpi)) return fail(KILN_LOG_ERROR, "ImGui could not start", "ImGui's Win32 backend could not start");
    if (!ensure_imgui_vulkan(d, *s, queue, family)) return fail(KILN_LOG_ERROR, "ImGui could not start", "ImGui's Vulkan backend could not start");
    input_attach(s->hwnd);
    if (steps) logf(KILN_LOG_INFO, "ui: first draw: ImGui backends ready, building the frame");
    // ImGui frame (callbacks run here)
    ImGui_ImplVulkan_NewFrame();
    if (!build_frame(static_cast<float>(s->extent.width), static_cast<float>(s->extent.height), dpi)) return false;
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0) {
        // nothing visible this frame, but new or freed textures still go to the GPU, as RenderDrawData would do first:
        // an image is drawn only once uploaded (image_budget.h), so an image that is all a frame would draw (the main
        // menu footer's mark) would otherwise never be uploaded and never drawn
        if (dd && dd->Textures)
            for (ImTextureData* tex : *dd->Textures)
                if (tex->Status != ImTextureStatus_OK) ImGui_ImplVulkan_UpdateTexture(tex);
        return false;
    }
    if (g_failed.load(std::memory_order_acquire)) return false;  // ImGui's backend failed building it (check_vk)
    if (steps) logf(KILN_LOG_INFO, "ui: first draw: frame built (%d lists), recording", dd->CmdListsCount);

    VkFence fence = s->fences[index];
    if (!fence || d.fn.vkWaitForFences(d.device, 1, &fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) {
        set_error("overlay fence timeout");
        return false;
    }
    VkCommandBuffer cb = s->commands[index];
    d.fn.vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (const VkResult r = d.fn.vkBeginCommandBuffer(cb, &bi); r != VK_SUCCESS)
        return fail(KILN_LOG_ERROR, "its Vulkan commands could not be recorded", "vkBeginCommandBuffer returned %d (%s)", static_cast<int>(r), vk_name(r));
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = s->pass;
    rp.framebuffer = s->framebuffers[index];
    rp.renderArea.extent = s->extent;
    d.fn.vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(dd, cb);
    d.fn.vkCmdEndRenderPass(cb);
    if (const VkResult r = d.fn.vkEndCommandBuffer(cb); r != VK_SUCCESS)
        return fail(KILN_LOG_ERROR, "its Vulkan commands could not be recorded", "vkEndCommandBuffer returned %d (%s)", static_cast<int>(r), vk_name(r));
    // ImGui's backend failed while recording (a texture upload: check_vk): nothing is submitted, the fence stays signalled
    if (g_failed.load(std::memory_order_acquire)) return false;

    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = info->waitSemaphoreCount;
    si.pWaitSemaphores = info->pWaitSemaphores;
    si.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s->done[index];
    d.fn.vkResetFences(d.device, 1, &fence);
    const VkResult r = d.fn.vkQueueSubmit(queue, 1, &si, fence);
    if (r != VK_SUCCESS) {
        // The present goes on with the game's own semaphores (vk_present.h): a failed submit left them as they were, and
        // Kiln's semaphore was not signalled, so nothing waits on it. The fence was reset but never submitted: made again
        // signalled, so that freeing the swapchain's resources does not wait a second on it (null when that fails too:
        // destroy_swap_resources skips it).
        d.fn.vkDestroyFence(d.device, fence, nullptr);
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkFence fresh = VK_NULL_HANDLE;
        s->fences[index] = d.fn.vkCreateFence(d.device, &fc, nullptr, &fresh) == VK_SUCCESS ? fresh : VK_NULL_HANDLE;
        return fail(KILN_LOG_ERROR, "its Vulkan submit failed",
                    "vkQueueSubmit of the overlay's frame returned %d (%s) on queue family %u", static_cast<int>(r), vk_name(r), family);
    }
    *out_wait = s->done[index];
    g_frames.fetch_add(1, std::memory_order_relaxed);
    g_last_draw_ms.store(now_ms(), std::memory_order_relaxed);
    g_frame_ms.store(static_cast<double>(GetTickCount64() - t0));
    if (!g_logged_draw) {
        g_logged_draw = true;
        logf(KILN_LOG_INFO, "ui: first overlay frame drawn (%ux%u)", s->extent.width, s->extent.height);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Work before the overlay (kiln_ui.h, subscribe_pre_overlay)

int fault_filter(EXCEPTION_POINTERS* ep);
uint32_t g_fault_code = 0;
void* g_fault_addr = nullptr;

// What was asked for with add_swapchain_usage, as far as this surface supports it: a swapchain the surface would refuse must not fail because of Kiln.
VkImageUsageFlags supported_extra_usage(VkDevice dev, VkSurfaceKHR surface) {
    const uint32_t extra = g_extra_usage.load();
    if (!extra || !R.gipa) return 0;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_devices.find(dev);
        if (it == g_devices.end()) return 0;
        instance = it->second->instance ? it->second->instance : g_instance;
        physical = it->second->physical;
    }
    auto caps_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(R.gipa.load()(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    VkSurfaceCapabilitiesKHR caps{};
    if (!caps_fn || caps_fn(physical, surface, &caps) != VK_SUCCESS) return 0;
    return extra & caps.supportedUsageFlags;
}

bool guarded_gone(KilnUiSwapchainGoneFn fn, void* device, void* swapchain, void* user) {
    __try {
        fn(device, swapchain, user);
        return true;
    } __except (fault_filter(GetExceptionInformation())) {
        return false;
    }
}

// g_mu held.
void notify_gone(VkDevice dev, VkSwapchainKHR swapchain) {
    for (const PreOverlaySub& s : g_pre_overlay)
        if (s.gone && !guarded_gone(s.gone, dev, swapchain, s.user)) logf(KILN_LOG_ERROR, "ui: a pre-overlay subscriber faulted (0x%08x) as a swapchain went", g_fault_code);
}

bool guarded_pre_overlay_fn(KilnUiPreOverlayFn fn, const KilnUiPreOverlay* p, void** out, void* user, bool* faulted) {
    __try {
        return fn(p, out, user);
    } __except (fault_filter(GetExceptionInformation())) {
        *faulted = true;
        return false;
    }
}

// True: *wait replaces the game's semaphores in this present.
bool run_pre_overlay(VkQueue queue, const VkPresentInfoKHR* info, VkSemaphore* wait) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_pre_overlay.empty() || g_failed.load(std::memory_order_acquire)) return false;  // off since (another thread)
    Swap* s = nullptr;
    uint32_t index = 0;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        auto it = g_swaps.find(info->pSwapchains[i]);
        if (it != g_swaps.end() && it->second.drawable && it->second.hwnd) {
            s = &it->second;
            index = info->pImageIndices[i];
            break;
        }
    }
    if (!s) return false;
    auto dit = g_devices.find(s->device);
    if (dit == g_devices.end() || !dit->second->fn.vkGetSwapchainImagesKHR) return false;
    Device& d = *dit->second;
    // the subscribers' work goes to the present's queue with its family: never a guessed one (vk_present.h)
    uint32_t family = 0;
    if (present_queue(d, queue, &family) != vk_present::QueueUse::drawable) return false;
    if (s->listed.empty()) {
        uint32_t n = 0;
        if (d.fn.vkGetSwapchainImagesKHR(d.device, s->swapchain, &n, nullptr) != VK_SUCCESS || n == 0) return false;
        std::vector<VkImage> listed(n);
        if (d.fn.vkGetSwapchainImagesKHR(d.device, s->swapchain, &n, listed.data()) != VK_SUCCESS) return false;
        s->listed = std::move(listed);
    }
    if (index >= s->listed.size()) return false;

    KilnUiPreOverlay p{};
    p.size = sizeof p;
    p.image_index = index;
    p.instance = d.instance ? d.instance : g_instance;
    p.physical_device = d.physical;
    p.device = d.device;
    p.get_instance_proc_addr = reinterpret_cast<void*>(R.gipa.load());
    p.get_device_proc_addr = reinterpret_cast<void*>(d.gdpa);
    p.queue = queue;
    p.swapchain = s->swapchain;
    p.image = s->listed[index];
    p.wait_semaphores = reinterpret_cast<void* const*>(info->pWaitSemaphores);
    p.wait_count = info->waitSemaphoreCount;
    p.queue_family = family;
    p.format = static_cast<uint32_t>(s->format);
    p.color_space = static_cast<uint32_t>(s->color_space);
    p.width = s->extent.width;
    p.height = s->extent.height;
    p.usage = s->usage;
    bool submitted = false;
    for (size_t i = 0; i < g_pre_overlay.size();) {
        const PreOverlaySub sub = g_pre_overlay[i];
        void* out = nullptr;
        bool faulted = false;
        const bool took = guarded_pre_overlay_fn(sub.fn, &p, &out, sub.user, &faulted);
        if (faulted) {
            logf(KILN_LOG_ERROR, "ui: a pre-overlay subscriber faulted (0x%08x at %p): removed", g_fault_code, g_fault_addr);
            g_pre_overlay.erase(g_pre_overlay.begin() + static_cast<ptrdiff_t>(i));
            if (g_pre_overlay.empty()) g_pre_demand = 0;
            continue;
        }
        ++i;
        if (!took || !out) continue;
        // the next one in line waits on this one's work
        *wait = static_cast<VkSemaphore>(out);
        p.wait_semaphores = reinterpret_cast<void* const*>(wait);
        p.wait_count = 1;
        submitted = true;
    }
    return submitted;
}

// ---------------------------------------------------------------------------------------------------------------
// Detours

VkResult VKAPI_CALL hk_create_instance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    // the upscaler backend's library making an instance of its own while Kiln asks it for its needs (inside the game's
    // vkCreateInstance, upscale_reentry.h): straight to the real function, not the game's instance
    if (upscale::reentry::inside()) return R.create_instance.load()(ci, a, out);
    // Kiln's upscaler swap (upscale.cpp): the chosen backend's instance needs, only when it was chosen at the game's
    // start; otherwise ci goes through unchanged. A create info of Kiln's that fails is tried again as the game made it.
    void* upscale_scope = nullptr;
    const VkInstanceCreateInfo* use = upscale_instance_begin(ci, R.gipa.load(), &upscale_scope);
    VkResult r = R.create_instance.load()(use, a, out);
    if (use != ci && r != VK_SUCCESS) {
        logf(KILN_LOG_WARN, "ui: vkCreateInstance with the upscaler's extensions failed (%d): the game's own create info is used", static_cast<int>(r));
        use = ci;
        r = R.create_instance.load()(ci, a, out);
    }
    upscale_instance_end(upscale_scope, use, r, r == VK_SUCCESS && out ? *out : VK_NULL_HANDLE);
    if (r == VK_SUCCESS && out) {
        std::lock_guard<std::mutex> lk(g_mu);
        overlay_guard_stage(overlay_guard::Stage::instance);
        g_instance = *out;
        if (ci && ci->pApplicationInfo && ci->pApplicationInfo->apiVersion) g_instance_api = ci->pApplicationInfo->apiVersion;
        logf(KILN_LOG_INFO, "ui: game Vulkan instance %p (api %u.%u)", reinterpret_cast<void*>(*out),
             VK_API_VERSION_MAJOR(g_instance_api), VK_API_VERSION_MINOR(g_instance_api));
    }
    return r;
}

void VKAPI_CALL hk_destroy_instance(VkInstance inst, const VkAllocationCallbacks* a) {
    if (upscale::reentry::inside()) return R.destroy_instance.load()(inst, a);  // the backend's own (upscale_reentry.h)
    upscale_instance_destroyed(inst);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_instance == inst) g_instance = VK_NULL_HANDLE;
        for (auto& [k, d] : g_devices)
            if (d->instance == inst) d->instance = VK_NULL_HANDLE;
    }
    R.destroy_instance.load()(inst, a);
}

// Each queue family's VkQueueFlags of a physical device (empty when they cannot be read). No lock is needed.
std::vector<uint32_t> queue_family_flags(VkInstance instance, VkPhysicalDevice phys) {
    std::vector<uint32_t> out;
    const auto gipa = R.gipa.load();
    if (!gipa || !instance || !phys) return out;
    const auto fn = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!fn) return out;
    uint32_t n = 0;
    fn(phys, &n, nullptr);
    if (!n || n > 64) return out;
    std::vector<VkQueueFamilyProperties> props(n);
    fn(phys, &n, props.data());
    for (uint32_t i = 0; i < n && i < props.size(); ++i) out.push_back(static_cast<uint32_t>(props[i].queueFlags));
    return out;
}

// Queues handed out on this thread while a device is being made (hk_create_device): a tool under Kiln can add queues to
// the device and get them inside the creation call, before Kiln has the device on its list (OptiScaler's frame
// generation: vk_present.h).
using HandedQueue = vk_present::Handed<VkDevice, VkQueue>;
thread_local std::vector<HandedQueue>* t_creating = nullptr;

VkResult VKAPI_CALL hk_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a,
                                     VkDevice* out) {
    // the upscaler backend's library making a device of its own while Kiln asks it (upscale_reentry.h): not the game's
    if (upscale::reentry::inside()) return R.create_device.load()(phys, ci, a, out);
    std::vector<HandedQueue> handed;
    // the queues the game asks for, read before the call from the game's own create info: a tool under Kiln may change
    // the create info it passes on (the upscaler's copy below carries the game's queues unchanged)
    std::vector<vk_present::Requested> requested =
        vk_present::requested_queues(ci ? ci->pQueueCreateInfos : nullptr, ci ? ci->queueCreateInfoCount : 0);
    // Kiln's upscaler swap (upscale.cpp): the chosen backend's device extensions and features, merged into a copy of the
    // game's create info (device_merge.h), only for the game's own device and only when the backend was chosen at the
    // game's start. A copy that fails is tried again as the game made it.
    void* upscale_scope = nullptr;
    const VkDeviceCreateInfo* use = upscale_device_begin(g_instance, phys, ci, R.gipa.load(), &upscale_scope);
    std::vector<HandedQueue>* const outer = t_creating;
    t_creating = &handed;
    VkResult r = R.create_device.load()(phys, use, a, out);
    if (use != ci && r != VK_SUCCESS) {
        logf(KILN_LOG_WARN, "ui: vkCreateDevice with the upscaler's extensions and features failed (%d): the game's own create info is used",
             static_cast<int>(r));
        handed.clear();  // queues a tool handed out inside the failed call belong to no device
        use = ci;
        r = R.create_device.load()(phys, ci, a, out);
    }
    t_creating = outer;
    upscale_device_end(upscale_scope, use, r, r == VK_SUCCESS && out ? *out : VK_NULL_HANDLE, R.gdpa.load(),
                       ci && ci->queueCreateInfoCount > 0 ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0);
    if (r != VK_SUCCESS || !out) return r;
    VkInstance instance = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        instance = g_instance;
    }
    std::vector<uint32_t> families = queue_family_flags(instance, phys);  // outside g_mu: it calls into the driver
    std::lock_guard<std::mutex> lk(g_mu);
    auto* d = new Device();
    d->instance = g_instance;
    d->physical = phys;
    d->device = *out;
    d->gdpa = R.gdpa;
    d->families = std::move(families);
    d->requested = std::move(requested);
    load_device_fns(*d);
    g_unseen_queues.erase(*out);  // of an older device that had this handle
    vk_present::take_handed(handed, *out, d->queue_family, d->tool_queues);
    for (const HandedQueue& h : handed) {  // another device's (unusual): to the device being made around this one, or
        if (h.device == *out) continue;    // kept for adopt_device as before
        if (outer) outer->push_back(h);
        else if (g_devices.find(h.device) != g_devices.end()) vk_present::game_got(h.queue, h.family, g_devices[h.device]->queue_family, g_devices[h.device]->tool_queues);
        else if (g_unseen_queues.size() < 8 || g_unseen_queues.count(h.device)) g_unseen_queues[h.device][h.queue] = h.family;
    }
    g_devices[*out] = d;
    overlay_guard_stage(overlay_guard::Stage::device);
    g_has_device = true;
    logf(KILN_LOG_INFO, "ui: game Vulkan device %p (%u queue families requested, functions %s; %zu live)", reinterpret_cast<void*>(*out),
         ci ? ci->queueCreateInfoCount : 0, device_fns_ok(*d) ? "ok" : "MISSING", g_devices.size());
    if (!d->tool_queues.empty())
        logf(KILN_LOG_INFO, "ui: %zu queue(s) of device %p were handed out while it was being made: a tool under Kiln added queues of its "
             "own to it, as frame generation does; Kiln's overlay does not draw on one of them unless the game asks for it too",
             d->tool_queues.size(), reinterpret_cast<void*>(*out));
    return r;
}

void VKAPI_CALL hk_destroy_device(VkDevice dev, const VkAllocationCallbacks* a) {
    if (upscale::reentry::inside()) return R.destroy_device.load()(dev, a);  // the backend's own (upscale_reentry.h)
    upscale_device_destroy(dev);  // the upscaler backend's Vulkan objects on this device go first (it waits for the GPU)
    VkDevice was = dev;
    g_swap_device.compare_exchange_strong(was, VK_NULL_HANDLE);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_unseen_queues.erase(dev);
        auto dit = g_devices.find(dev);
        if (dit != g_devices.end()) {
            Device& d = *dit->second;
            if (device_fns_ok(d)) {
                d.fn.vkDeviceWaitIdle(dev);
                notify_gone(dev, nullptr);
                for (auto it = g_swaps.begin(); it != g_swaps.end();) {
                    if (it->second.device == dev) {
                        destroy_swap_resources(d, it->second);
                        it = g_swaps.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (d.imgui_vk) {
                    ImGui_ImplVulkan_Shutdown();
                    d.imgui_vk = false;
                }
                if (d.imgui_pass) d.fn.vkDestroyRenderPass(dev, d.imgui_pass, nullptr);
                if (d.pool) d.fn.vkDestroyCommandPool(dev, d.pool, nullptr);
            }
            if (g_dev == &d) g_dev = nullptr;
            delete dit->second;
            g_devices.erase(dit);
            g_has_device = !g_devices.empty();
            g_swap_count = static_cast<uint32_t>(g_swaps.size());
            logf(KILN_LOG_INFO, "ui: game Vulkan device %p destroyed (%zu live)", reinterpret_cast<void*>(dev), g_devices.size());
        }
    }
    R.destroy_device.load()(dev, a);
}

void remember_queue(VkDevice dev, uint32_t family, VkQueue q) {
    if (!q) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (t_creating && g_devices.find(dev) == g_devices.end()) {
        t_creating->push_back({dev, family, q});  // the device being made on this thread: hk_create_device takes it
        return;
    }
    if (auto it = g_devices.find(dev); it != g_devices.end()) vk_present::game_got(q, family, it->second->queue_family, it->second->tool_queues);
    else if (g_unseen_queues.size() < 8 || g_unseen_queues.count(dev)) g_unseen_queues[dev][q] = family;  // adopt_device
}

void VKAPI_CALL hk_get_queue(VkDevice dev, uint32_t family, uint32_t index, VkQueue* out) {
    R.get_queue.load()(dev, family, index, out);
    if (out && !t_looking_up) remember_queue(dev, family, *out);
}

void VKAPI_CALL hk_get_queue2(VkDevice dev, const VkDeviceQueueInfo2* info, VkQueue* out) {
    R.get_queue2.load()(dev, info, out);
    if (out && info && !t_looking_up) remember_queue(dev, info->queueFamilyIndex, *out);
}

VkResult VKAPI_CALL hk_create_surface(VkInstance inst, const VkWin32SurfaceCreateInfoKHR* ci, const VkAllocationCallbacks* a,
                                      VkSurfaceKHR* out) {
    const VkResult r = R.create_surface.load()(inst, ci, a, out);
    if (r == VK_SUCCESS && out && ci) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_surfaces[*out] = ci->hwnd;
        g_hwnd.store(ci->hwnd);
        if (!g_instance) g_instance = inst;
        logf(KILN_LOG_INFO, "ui: game window %p has a Vulkan surface", reinterpret_cast<void*>(ci->hwnd));
    }
    return r;
}

void describe_patch(const void* target, char* out, size_t cap);

// The game made a swapchain on a device Kiln never saw being created: another module patched vkCreateDevice after
// Kiln and does not pass the call on (seen on a Steam Deck with ReShade and ShroudForge's own loader next to Kiln:
// Kiln's hooks on the surface, the swapchain and the present still ran, the ones on the instance and the device did
// not, and every Kiln menu stayed empty). What the overlay needs can still be had without guessing:
//  - the device: from this call;
//  - the instance: from the window surface's creation (hk_create_surface);
//  - the queues' families: from vkGetDeviceQueue, kept for unknown devices too (g_unseen_queues);
//  - the physical device: only when the instance has exactly one. A device cannot be asked which GPU it belongs to,
//    and memory types differ between GPUs, so with several the overlay stays off.
// g_mu held.
void adopt_device(VkDevice dev) {
    static VkDevice said_for = VK_NULL_HANDLE;  // one line per device: swapchains are made again on every resize
    const bool say = said_for != dev;
    said_for = dev;
    char who[MAX_PATH + 48] = "another module";
    if (const uintptr_t t = h_create_device.target.load()) describe_patch(reinterpret_cast<const void*>(t), who, sizeof(who));
    const char* why = nullptr;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    const auto gipa = R.gipa.load();
    const auto queues = g_unseen_queues.find(dev);
    if (!g_instance || !gipa) {
        why = "its Vulkan instance is not known either";
    } else if (queues == g_unseen_queues.end() || queues->second.empty()) {
        why = "none of its queues was seen handed out";
    } else {
        const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(g_instance, "vkEnumeratePhysicalDevices"));
        uint32_t n = 0;
        if (!enumerate || enumerate(g_instance, &n, nullptr) != VK_SUCCESS) {
            why = "the graphics devices could not be listed";
        } else if (n != 1) {
            why = "this PC has more than one graphics device and a Vulkan device does not say which one it uses";
        } else if (enumerate(g_instance, &n, &phys) != VK_SUCCESS || n != 1 || !phys) {
            why = "the graphics device could not be read";
        }
    }
    if (!why) {
        auto* d = new Device();
        d->instance = g_instance;
        d->physical = phys;
        d->device = dev;
        d->gdpa = R.gdpa;
        d->adopted = true;
        d->queue_family = queues->second;
        d->families = queue_family_flags(g_instance, phys);
        load_device_fns(*d);
        if (device_fns_ok(*d)) {
            g_unseen_queues.erase(queues);
            g_devices[dev] = d;
            overlay_guard_stage(overlay_guard::Stage::device);
            g_has_device = true;
            logf(KILN_LOG_WARN, "ui: game Vulkan device %p taken over at its swapchain: Kiln did not see it being created "
                 "(vkCreateDevice is patched by %s). %zu queue(s) known; %zu live", reinterpret_cast<void*>(dev), who,
                 d->queue_family.size(), g_devices.size());
            return;
        }
        delete d;
        why = "its functions could not be loaded";
    }
    if (say)
        logf(KILN_LOG_WARN, "ui: the game draws with a Vulkan device Kiln did not see being created (vkCreateDevice is patched by %s), "
             "and Kiln cannot take it over: %s. Kiln's overlay does not draw: its menus and the in-game Mods page stay empty. "
             "The mod list is in the Kiln Launcher too", who, why);
}

VkResult VKAPI_CALL hk_create_swapchain(VkDevice dev, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a,
                                        VkSwapchainKHR* out) {
    if (!ci || !out) return R.create_swapchain.load()(dev, ci, a, out);
    VkSwapchainCreateInfoKHR c = *ci;
    c.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | supported_extra_usage(dev, ci->surface);
    VkResult r = R.create_swapchain.load()(dev, &c, a, out);
    bool drawable = r == VK_SUCCESS;
    VkImageUsageFlags usage = c.imageUsage;
    if (r != VK_SUCCESS && c.imageUsage != ci->imageUsage) {
        r = R.create_swapchain.load()(dev, ci, a, out);
        drawable = (ci->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        usage = ci->imageUsage;
    }
    if (r != VK_SUCCESS) return r;
    overlay_guard_stage(overlay_guard::Stage::swapchain);
    g_swapchain_count.fetch_add(1);
    g_swap_device.store(dev);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_devices.find(dev) == g_devices.end()) adopt_device(dev);
    if (ci->oldSwapchain) {
        auto it = g_swaps.find(ci->oldSwapchain);
        auto dit = g_devices.find(dev);
        if (it != g_swaps.end() && dit != g_devices.end()) destroy_swap_resources(*dit->second, it->second);
        notify_gone(dev, ci->oldSwapchain);
    }
    Swap s;
    s.usage = usage;
    s.device = dev;
    s.swapchain = *out;
    s.format = ci->imageFormat;
    s.color_space = ci->imageColorSpace;
    s.extent = ci->imageExtent;
    s.min_images = ci->minImageCount;
    s.drawable = drawable;
    auto sf = g_surfaces.find(ci->surface);
    s.hwnd = sf != g_surfaces.end() ? sf->second : g_hwnd.load();
    g_swaps[*out] = s;
    g_swap_count = static_cast<uint32_t>(g_swaps.size());
    g_width.store(s.extent.width);
    g_height.store(s.extent.height);
    g_format.store(static_cast<uint32_t>(s.format));
    g_colorspace.store(static_cast<uint32_t>(s.color_space));
    if (s.hwnd) {
        const UINT dpi = GetDpiForWindow(s.hwnd);
        float scale = dpi ? dpi / 96.0f : 1.0f;
        // Wine often reports 96 dpi on large screens: scale with the height too
        scale = std::max(scale, static_cast<float>(s.extent.height) / 1440.0f);
        g_dpi.store(std::clamp(scale, 1.0f, 3.0f));
    }
    logf(KILN_LOG_INFO, "ui: swapchain %ux%u format %d colour space %d, %u+ images, window %p%s", s.extent.width,
         s.extent.height, static_cast<int>(s.format), static_cast<int>(s.color_space), s.min_images,
         reinterpret_cast<void*>(s.hwnd), drawable ? "" : " (no colour-attachment usage: overlay off)");
    return r;
}

void VKAPI_CALL hk_destroy_swapchain(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks* a) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_swaps.find(sc);
        if (it != g_swaps.end()) {
            if (auto dit = g_devices.find(dev); dit != g_devices.end()) destroy_swap_resources(*dit->second, it->second);
            notify_gone(dev, sc);
            g_swaps.erase(it);
            g_swap_count = static_cast<uint32_t>(g_swaps.size());
        }
    }
    R.destroy_swapchain.load()(dev, sc, a);
}

// draw() under g_mu and a fault guard: an access violation in the overlay turns the overlay off (logged) instead of
// taking the game down. No C++ object with a destructor lives in this frame.
int fault_filter(EXCEPTION_POINTERS* ep) {
    g_fault_code = ep->ExceptionRecord->ExceptionCode;
    g_fault_addr = ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

bool guarded_draw(VkQueue queue, const VkPresentInfoKHR* info, VkSemaphore* wait, uint64_t t0, bool* faulted) {
    bool r = false;
    g_mu.lock();
    __try {
        r = draw(queue, info, wait, t0);
    } __except (fault_filter(GetExceptionInformation())) {
        r = false;
        *faulted = true;
    }
    g_mu.unlock();
    g_drawing.store(false, std::memory_order_relaxed);
    return r;
}

VkResult VKAPI_CALL hk_present(VkQueue queue, const VkPresentInfoKHR* info) {
    g_presents.fetch_add(1, std::memory_order_relaxed);
    // The window must be subclassed even while nothing is drawn (the menu key opens the overlay); checked once a
    // second, which also re-subclasses if the game set its own WndProc again.
    static std::atomic<uint64_t> last_attach{0};
    const uint64_t t = now_ms();
    if (t - last_attach.load(std::memory_order_relaxed) >= 1000) {
        last_attach.store(t, std::memory_order_relaxed);
        if (HWND h = g_hwnd.load()) input_attach(h);
    }
    VkPresentInfoKHR after_pre_overlay;
    VkSemaphore pre_overlay_wait = VK_NULL_HANDLE;
    // the subscribers' work is part of the overlay's drawing: none after it turned off for the session
    if (info && g_pre_demand.load(std::memory_order_relaxed) > 0 && !g_failed.load(std::memory_order_relaxed) &&
        run_pre_overlay(queue, info, &pre_overlay_wait)) {
        after_pre_overlay = *info;
        after_pre_overlay.waitSemaphoreCount = 1;
        after_pre_overlay.pWaitSemaphores = &pre_overlay_wait;
        info = &after_pre_overlay;
    }
    const bool failed = g_failed.load(std::memory_order_acquire);
    if (!info || failed || !want_draw()) {
        if (failed) {
            announce_failure();  // once: the drawing went off outside a draw (ImGui's backend, check_vk)
            if (wheel_active()) wheel_cancel_undrawable();  // it can never be drawn or picked: release the input
        }
        // capture just ended with nothing left to draw: still give the game its cursor clip back (render thread)
        if (imgui_ready()) input_capture_changed();
        const VkResult result = R.present.load()(queue, info);
        // Kiln's frame cap: the frame is shown as soon as it is done, then the game waits for its next slot before it
        // starts the next one, as a limiter in the game would (one relaxed load while off). No lock is held here.
        graphics_after_present();
        return result;
    }
    // the crash guard watches the overlay's first draw of this process from inside draw(), once its checks found a
    // swapchain, device and queue of its own to draw with (overlay_guard_first_draw)
    const uint64_t t0 = GetTickCount64();
    VkSemaphore wait = VK_NULL_HANDLE;
    bool faulted = false;
    const bool drew = guarded_draw(queue, info, &wait, t0, &faulted);
    if (faulted) {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(g_fault_addr), &m);
        wchar_t path[MAX_PATH] = L"?";
        if (m) GetModuleFileNameW(m, path, MAX_PATH);
        fail(KILN_LOG_ERROR, "it faulted", "the overlay faulted (0x%08x at %p = %ls+0x%llx)", g_fault_code, g_fault_addr,
             wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path,
             m ? static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_fault_addr) - reinterpret_cast<uintptr_t>(m)) : 0ull);
    }
    announce_failure();  // once, after the draw that turned the drawing off (g_mu was released by guarded_draw)
    // Kiln's frame went in: the present waits on Kiln's semaphore. It did not (nothing to draw, or the drawing failed):
    // the game's present goes on untouched, with the game's own semaphores (vk_present.h). draw() sets `wait` right after
    // its submit succeeded, so a fault after that still presents on Kiln's semaphore: the game's were waited on by it.
    const bool submitted = wait != VK_NULL_HANDLE;
    const auto waits = vk_present::present_waits(submitted, info->pWaitSemaphores, info->waitSemaphoreCount, &wait);
    VkPresentInfoKHR p = *info;
    p.waitSemaphoreCount = waits.count;
    p.pWaitSemaphores = waits.semaphores;
    const VkResult result = R.present.load()(queue, submitted ? &p : info);
    if (drew && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) overlay_guard_drawn();
    graphics_after_present();  // Kiln's frame cap after the present (g_mu was released by guarded_draw)
    return result;
}

// Windows (Khronos loader): hand out Kiln's functions for the names it needs.
PFN_vkVoidFunction swap_in(const char* name, PFN_vkVoidFunction real) {
    if (!real || !name) return real;
#define KUI_SWAP(n, field, hk)                                                               \
    if (std::strcmp(name, n) == 0) {                                                         \
        auto empty = decltype(R.field.load()){nullptr};                                      \
        if (real != reinterpret_cast<PFN_vkVoidFunction>(&hk))                               \
            R.field.compare_exchange_strong(empty, reinterpret_cast<decltype(empty)>(real)); \
        return R.field ? reinterpret_cast<PFN_vkVoidFunction>(&hk) : nullptr;                 \
    }
    KUI_SWAP("vkCreateDevice", create_device, hk_create_device)
    KUI_SWAP("vkDestroyDevice", destroy_device, hk_destroy_device)
    KUI_SWAP("vkGetDeviceQueue", get_queue, hk_get_queue)
    KUI_SWAP("vkGetDeviceQueue2", get_queue2, hk_get_queue2)
    KUI_SWAP("vkCreateSwapchainKHR", create_swapchain, hk_create_swapchain)
    KUI_SWAP("vkDestroySwapchainKHR", destroy_swapchain, hk_destroy_swapchain)
    KUI_SWAP("vkQueuePresentKHR", present, hk_present)
    KUI_SWAP("vkCreateWin32SurfaceKHR", create_surface, hk_create_surface)
#undef KUI_SWAP
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return real;  // the wrapped one is returned below
    return real;
}

PFN_vkVoidFunction VKAPI_CALL hk_gdpa(VkDevice dev, const char* name) {
    return swap_in(name, R.gdpa.load()(dev, name));
}

PFN_vkVoidFunction VKAPI_CALL hk_gipa(VkInstance inst, const char* name) {
    PFN_vkVoidFunction real = R.gipa.load()(inst, name);
    if (name && std::strcmp(name, "vkGetDeviceProcAddr") == 0 && real)
        return R.gdpa ? reinterpret_cast<PFN_vkVoidFunction>(&hk_gdpa) : real;
    if (name && std::strcmp(name, "vkCreateInstance") == 0 && real) {
        PFN_vkCreateInstance empty = nullptr;
        if (real != reinterpret_cast<PFN_vkVoidFunction>(&hk_create_instance))
            R.create_instance.compare_exchange_strong(empty, reinterpret_cast<PFN_vkCreateInstance>(real));
        return R.create_instance ? reinterpret_cast<PFN_vkVoidFunction>(&hk_create_instance) : nullptr;
    }
    return swap_in(name, real);
}

// LoadLibraryExW can return inside another loader operation or a Vulkan call. Never wait for another installer
// while holding those locks, or re-enter installation on the same thread. This mutex does not protect detour calls:
// their originals are atomic, published before enable(), and their trampoline allocations live for the process.
std::mutex g_install_mu;
std::vector<SafetyHookInline*> g_retained_hooks;
int g_installs = 0;
bool g_input_installed = false;

struct ModuleRef {
    HMODULE module = nullptr;
    ModuleRef() = default;
    ModuleRef(const ModuleRef&) = delete;
    ModuleRef& operator=(const ModuleRef&) = delete;
    ModuleRef(ModuleRef&& other) noexcept : module(std::exchange(other.module, nullptr)) {}
    ~ModuleRef() { if (module) FreeLibrary(module); }
};

struct PreparedHook {
    HookState* state;
    ModuleRef owner;
    const char* name;
    vk_hooks::Canary reason;
    bool required;
};

bool read_code(const void* at, uint8_t* out, size_t size) {
    __try {
        std::memcpy(out, at, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The file name of the module `at` lies in ("" when it is in none).
void module_name_of(const void* at, char* out, size_t cap) {
    out[0] = 0;
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(at), &m) || !m || !GetModuleFileNameW(m, path, MAX_PATH))
        return;
    const wchar_t* name = path;
    for (const wchar_t* c = path; *c; ++c)
        if (*c == L'\\' || *c == L'/') name = c + 1;
    WideCharToMultiByte(CP_UTF8, 0, name, -1, out, static_cast<int>(cap) - 1, nullptr, nullptr);
    out[cap - 1] = 0;
}

// Who patched a function, for the log: its first instruction is followed through up to four jumps (jmp rel32, jmp
// rel8, jmp [rip+x], mov rax/r11,imm64 + jmp) until one lands in a module other than the function's own. "<file>",
// "memory outside any module" for a trampoline that leads nowhere readable, or "an unknown patch".
void describe_patch(const void* target, char* out, size_t cap) {
    char own[MAX_PATH] = {};
    module_name_of(target, own, sizeof(own));
    const uint8_t* p = static_cast<const uint8_t*>(target);
    bool jumped = false;
    for (int hop = 0; hop < 4 && p; ++hop) {
        uint8_t b[16] = {};
        if (!read_code(p, b, sizeof(b))) break;
        const uint8_t* next = nullptr;
        int32_t rel = 0;
        if (b[0] == 0xE9) {
            std::memcpy(&rel, b + 1, 4);
            next = p + 5 + rel;
        } else if (b[0] == 0xEB) {
            next = p + 2 + static_cast<int8_t>(b[1]);
        } else if (b[0] == 0xFF && b[1] == 0x25) {
            std::memcpy(&rel, b + 2, 4);
            uint64_t abs = 0;
            if (read_code(p + 6 + rel, reinterpret_cast<uint8_t*>(&abs), sizeof(abs))) next = reinterpret_cast<const uint8_t*>(abs);
        } else if ((b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) ||
                   (b[0] == 0x49 && b[1] == 0xBB && b[10] == 0x41 && b[11] == 0xFF && b[12] == 0xE3)) {
            uint64_t abs = 0;
            std::memcpy(&abs, b + 2, 8);
            next = reinterpret_cast<const uint8_t*>(abs);
        }
        if (!next) break;
        jumped = true;
        p = next;
        char name[MAX_PATH] = {};
        module_name_of(p, name, sizeof(name));
        if (name[0] && _stricmp(name, own) != 0) {
            snprintf(out, cap, "%s", name);
            return;
        }
    }
    snprintf(out, cap, "%s", jumped ? "memory outside any module" : "an unknown patch");
}

const char* canary_name(vk_hooks::Canary c) {
    using enum vk_hooks::Canary;
    switch (c) {
        case First: return "first hook";
        case Moved: return "new address after code-owner replacement";
        case Ours: return "still Kiln's patch";
        case Pristine: return "restored original code";
        case Reloaded: return "new code after code-owner replacement";
        case Foreign: return "unverified code/patch";
        default: return "unreadable";
    }
}

struct LdrUStr {
    USHORT Length, MaximumLength;
    PWSTR Buffer;
};
struct LdrNotice {
    ULONG Flags;
    const LdrUStr* FullDllName;
    const LdrUStr* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
using LdrNoticeFn = VOID(CALLBACK*)(ULONG, const LdrNotice*, PVOID);
using LdrRegisterFn = LONG(NTAPI*)(ULONG, LdrNoticeFn, PVOID, PVOID*);

// Runs under the loader lock: no allocation, logging, locks or module calls. GetProcAddress can forward into
// winevulkan.dll, so watch the actual code owner, not every unrelated DLL named vulkan-1.dll. An unload notification
// runs BEFORE unmapping: for replacement at the same base, require a subsequent load, not just an unload notice.
// Counters are never cleared: a notice arriving during installation remains visible to the next check.
VOID CALLBACK on_dll_notice(ULONG reason, const LdrNotice* d, PVOID) {
    if ((reason != 1 && reason != 2) || !d) return;  // 1: loaded, 2: unloading
    const uintptr_t base = reinterpret_cast<uintptr_t>(d->DllBase);
    for (HookState* h : g_hooks) {
        const uintptr_t owner = h->owner.load();
        if (!owner) continue;
        if (reason == 1 && (base == owner || vk_hooks::contains(base, d->SizeOfImage, h->target.load()))) ++h->loads;
        if (reason == 2 && base == owner) ++h->unloads;
    }
}

void watch_unloads() {
    static bool registered = false, told_failure = false;
    if (registered) return;
    if (auto reg = reinterpret_cast<LdrRegisterFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"))) {
        PVOID cookie = nullptr;
        registered = reg(0, &on_dll_notice, nullptr, &cookie) >= 0;
    }
    if (!registered && !told_failure) {
        told_failure = true;
        logf(KILN_LOG_WARN, "ui: cannot watch Vulkan code-owner unloads; unknown replacements will not be hooked");
    }
}

template <typename T>
bool hook(HookState& state, void* target, void* detour, std::atomic<T>& original, const char* what, bool& attempted,
          std::vector<PreparedHook>& prepared, bool required = true) {
    // Keep the owner mapped until after inspection/patching. The vulkan-1 ref in vk_install also keeps its forwarder
    // dependencies alive across the whole pass, without pinning the DLL forever (the desktop probe must unload).
    ModuleRef owner;
    vk_hooks::Bytes before{};
    const bool readable = target && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(target), &owner.module) && read_code(target, before.data(), before.size());
    const uint64_t unloads = state.unloads.load();
    const uint64_t loads = state.loads.load();
    const bool replaced = vk_hooks::replaced({state.bytes.address, state.owner.load(), state.seen_loads, state.seen_unloads},
        {reinterpret_cast<uintptr_t>(target), reinterpret_cast<uintptr_t>(owner.module), loads, unloads});
    auto c = vk_hooks::classify(state.bytes, reinterpret_cast<uintptr_t>(target), readable ? &before : nullptr, replaced);

    // Seeing a live patch consumes only the sampled epoch. Otherwise that old notice could later authorise a
    // re-hook after an overlay covers this same live patch. A newer notification is never erased.
    if (c == vk_hooks::Canary::Ours) {
        state.seen_loads = loads;
        state.seen_unloads = unloads;
        return true;
    }
    if (readable && state.hook && state.hook->target() == target &&
        vk_hooks::own_jump(before, reinterpret_cast<uintptr_t>(target), state.hook->trampoline().address(),
                           state.hook->trampoline().size(), state.hook->destination_address())) {
        state.seen_loads = loads;
        state.seen_unloads = unloads;
        return true;
    }

    // Also recognise every retained Kiln trampoline, including aliases/old generations. A changed tail or a new
    // export address is never permission to put a second Kiln detour in front of one already calling us.
    if (readable) {
        for (const SafetyHookInline* h : g_retained_hooks) {
            if (!vk_hooks::own_jump(before, reinterpret_cast<uintptr_t>(target), h->trampoline().address(),
                                    h->trampoline().size(), h->destination_address())) continue;
            c = vk_hooks::Canary::Foreign;
            state.seen_loads = loads;
            state.seen_unloads = unloads;
            break;
        }
    }
    if (!vk_hooks::may_hook(c)) {
        if (!state.told_skip) {
            state.told_skip = true;
            char who[MAX_PATH + 48] = "";
            if (readable) describe_patch(target, who, sizeof(who));
            logf(KILN_LOG_WARN, "ui: %s at %p is %s%s%s: not re-hooked (an existing hook may be underneath)", what, target, canary_name(c),
                 who[0] ? ", it now leads into " : "", who);
        }
        return false;
    }

    attempted = true;
    auto h = std::make_unique<SafetyHookInline>(safetyhook::create_inline(target, detour, SafetyHookInline::StartDisabled));
    if (!*h) {
        logf(KILN_LOG_ERROR, "ui: hooking %s failed", what);
        return false;
    }
    const auto& saved = h->original_bytes();
    vk_hooks::Bytes check{};
    // An overlay may have patched between classification and trampoline creation. Do not publish or enable a
    // trampoline built from a different prologue. A final re-read also narrows the race with an external patcher.
    if (saved.empty() || saved.size() > before.size() || std::memcmp(saved.data(), before.data(), saved.size()) ||
        !read_code(target, check.data(), saved.size()) || std::memcmp(saved.data(), check.data(), saved.size())) {
        logf(KILN_LOG_WARN, "ui: %s changed while preparing its hook: not hooked", what);
        return false;
    }
    // Retain before publication: even if enable fails, no observer may ever see a freed trampoline. Moving a live
    // h_gipa/h_gdpa used to deadlock against .call()'s mutex, or temporarily expose an empty hook to the detour.
    // Allocate the pending entry before publishing anything; its module ref stays held through activation.
    prepared.push_back({&state, std::move(owner), what, c, required});
    g_retained_hooks.push_back(h.get());
    state.hook = h.release();
    state.bytes = {};
    state.bytes.address = reinterpret_cast<uintptr_t>(target);
    state.bytes.size = saved.size();
    std::copy(saved.begin(), saved.end(), state.bytes.original.begin());
    state.owner = reinterpret_cast<uintptr_t>(prepared.back().owner.module);
    state.target = state.bytes.address;
    state.seen_loads = loads;
    state.seen_unloads = unloads;
    original.store(state.hook->original<T>());
    return true;
}

bool enable_hook(const PreparedHook& p) {
    HookState& state = *p.state;
    vk_hooks::Bytes check{};
    if (!read_code(state.hook->target(), check.data(), state.bytes.size) ||
        std::memcmp(check.data(), state.bytes.original.data(), state.bytes.size)) {
        logf(KILN_LOG_WARN, "ui: %s changed before enabling its hook: not hooked", p.name);
        return false;
    }
    if (!state.hook->enable()) {
        logf(KILN_LOG_ERROR, "ui: enabling %s failed", p.name);
        return false;
    }
    // Capture only our own emitted jump, never an overlay that raced us after enable(). Without a fingerprint a
    // later unknown patch is left alone; the retained trampoline check above still recognises our actual jump.
    state.bytes.have_patch = read_code(state.hook->target(), state.bytes.patch.data(), state.bytes.size) &&
        vk_hooks::own_jump(state.bytes.patch, state.bytes.address, state.hook->trampoline().address(),
                           state.hook->trampoline().size(), state.hook->destination_address());
    state.told_skip = false;
    logf(KILN_LOG_INFO, "ui: hooking %s at %p (%s)", p.name, state.hook->target(), canary_name(p.reason));
    return true;
}

}  // namespace

bool vk_install() {
    if (!overlay_drawing_on()) return false; // future device-only mode selects its installer here
    static thread_local bool installing = false;
    if (installing) return g_installed.load();
    installing = true;
    struct Leave { bool& flag; ~Leave() { flag = false; } } leave{installing};
    std::unique_lock<std::mutex> lk(g_install_mu, std::try_to_lock);
    if (!lk.owns_lock()) return g_installed.load();
    // Temporary refs prevent an unload between GetProcAddress/read/enable. Drop them before releasing the recursion
    // guard; FreeLibrary can itself invoke loader code. No locks are taken by on_dll_notice.
    ModuleRef vulkan;
    if (!GetModuleHandleExW(0, L"vulkan-1.dll", &vulkan.module)) return false;
    g_vulkan = vulkan.module;
    const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    g_wine = wine;
    overlay_guard_stage(overlay_guard::Stage::begun);
    watch_unloads();
    if (!g_input_installed) {
        g_input_installed = true;
        input_install_iat();
    }
    auto P = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(vulkan.module, n)); };
    bool ok = true, attempted = false;
    std::vector<PreparedHook> prepared;
    // Wine's proc-address exports are not detoured. Do not overwrite the Windows trampoline originals with their
    // patched entry points on every LoadLibraryExW (that would immediately recurse).
    if (wine) {
        R.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(P("vkGetInstanceProcAddr"));
        R.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(P("vkGetDeviceProcAddr"));
    }
    // Each export decides independently: one fresh prologue must never retire/re-hook the other live exports.
    // Prepare every original before enabling ANY new hook: a concurrent Windows proc-address lookup or device
    // creation must not observe null/stale resolver/command originals halfway through an install.
    if (!wine) {
        ok &= hook(h_gdpa, P("vkGetDeviceProcAddr"), reinterpret_cast<void*>(&hk_gdpa), R.gdpa, "vkGetDeviceProcAddr", attempted, prepared);
        ok &= hook(h_gipa, P("vkGetInstanceProcAddr"), reinterpret_cast<void*>(&hk_gipa), R.gipa, "vkGetInstanceProcAddr", attempted, prepared);
    }
    const bool instance_ok = hook(h_create_instance, P("vkCreateInstance"), reinterpret_cast<void*>(&hk_create_instance), R.create_instance, "vkCreateInstance", attempted, prepared, wine);
    if (wine) ok &= instance_ok;
    hook(h_destroy_instance, P("vkDestroyInstance"), reinterpret_cast<void*>(&hk_destroy_instance), R.destroy_instance, "vkDestroyInstance", attempted, prepared, false);
    ok &= hook(h_create_device, P("vkCreateDevice"), reinterpret_cast<void*>(&hk_create_device), R.create_device, "vkCreateDevice", attempted, prepared);
    ok &= hook(h_destroy_device, P("vkDestroyDevice"), reinterpret_cast<void*>(&hk_destroy_device), R.destroy_device, "vkDestroyDevice", attempted, prepared);
    ok &= hook(h_get_queue, P("vkGetDeviceQueue"), reinterpret_cast<void*>(&hk_get_queue), R.get_queue, "vkGetDeviceQueue", attempted, prepared);
    hook(h_get_queue2, P("vkGetDeviceQueue2"), reinterpret_cast<void*>(&hk_get_queue2), R.get_queue2, "vkGetDeviceQueue2", attempted, prepared, false);
    ok &= hook(h_create_swapchain, P("vkCreateSwapchainKHR"), reinterpret_cast<void*>(&hk_create_swapchain), R.create_swapchain, "vkCreateSwapchainKHR", attempted, prepared);
    ok &= hook(h_destroy_swapchain, P("vkDestroySwapchainKHR"), reinterpret_cast<void*>(&hk_destroy_swapchain), R.destroy_swapchain, "vkDestroySwapchainKHR", attempted, prepared);
    ok &= hook(h_present, P("vkQueuePresentKHR"), reinterpret_cast<void*>(&hk_present), R.present, "vkQueuePresentKHR", attempted, prepared);
    const bool surface_ok = hook(h_create_surface, P("vkCreateWin32SurfaceKHR"), reinterpret_cast<void*>(&hk_create_surface), R.create_surface, "vkCreateWin32SurfaceKHR", attempted, prepared, wine);
    if (wine) ok &= surface_ok;
    for (const PreparedHook& p : prepared) {
        const bool enabled = enable_hook(p);
        if (p.required) ok &= enabled;
    }
    overlay_guard_stage(overlay_guard::Stage::hooks);
    g_installed = ok;
    if (attempted) {
        ++g_installs;
        logf(ok ? KILN_LOG_INFO : KILN_LOG_WARN, "ui: Vulkan hooks %s (%s path, vulkan-1 at %p, install #%d)", ok ? "installed" : "INCOMPLETE",
             wine ? "Wine: winevulkan exports" : "Windows: proc-address wrappers", reinterpret_cast<void*>(vulkan.module), g_installs);
    }
    return ok;
}

KilnResult vk_subscribe_pre_overlay(KilnUiPreOverlayFn fn, KilnUiSwapchainGoneFn gone, void* user, KilnHandle* out) {
    if (!fn) return KILN_ERR_INVALID_ARGUMENT;
    const KilnHandle h = next_handle();
    std::lock_guard<std::mutex> lk(g_mu);
    g_pre_overlay.push_back({h, fn, gone, user});
    if (out) *out = h;
    return KILN_OK;
}

bool vk_unsubscribe(KilnHandle handle) {
    if (!overlay_drawing_on()) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    return std::erase_if(g_pre_overlay, [&](const PreOverlaySub& s) { return s.handle == handle; }) > 0;
}

void vk_set_pre_overlay_demand(uint32_t count) { if (!overlay_drawing_on()) return; g_pre_demand.store(count, std::memory_order_relaxed); }
void vk_add_swapchain_usage(uint32_t usage) { if (!overlay_drawing_on()) return; g_extra_usage.fetch_or(usage); }

uint64_t vk_presents() { return g_presents.load(std::memory_order_relaxed); }
bool vk_drawing() { return g_drawing.load(std::memory_order_relaxed); }
VkDevice vk_swapchain_device() { return g_swap_device.load(); }
int vk_install_count() { return g_installs; }

void vk_extent(uint32_t* width, uint32_t* height) {
    if (width) *width = g_width.load();
    if (height) *height = g_height.load();
}

void vk_status(KilnUiStatus* out) {
    if (!overlay_drawing_on()) return;
    out->hooked = g_installed.load() ? 1 : 0;
    out->wine = g_wine ? 1 : 0;
    // no lock here: frame callbacks (which may ask for the status) run while draw() holds g_mu
    out->device = g_has_device.load() ? 1 : 0;
    out->swapchain = g_swap_count.load() > 0 ? 1 : 0;
    out->width = g_width.load();
    out->height = g_height.load();
    out->format = g_format.load();
    out->color_space = g_colorspace.load();
    out->dpi_scale = g_dpi.load();
    out->presents = g_presents.load();
    out->frames_drawn = g_frames.load();
    out->swapchains = g_swapchain_count.load();
    out->frame_ms = g_frame_ms.load();
    out->drawing = now_ms() - g_last_draw_ms.load() < 1000 ? 1 : 0;
}

}  // namespace kui

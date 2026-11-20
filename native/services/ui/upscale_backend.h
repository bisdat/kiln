// SPDX-License-Identifier: MIT
// kiln.ui: the interface between Kiln's upscaler swap and the backends that upscale in place of the game's FSR 3 pass
// (docs/kiln/graphics.md "Upscaler behind the game's FSR 3"; 0.2.0-dev.29.18, phase B).
//
// The core (upscale.cpp) hooks the game's FSR 3 pass (sigdb client.d/upscale.json: PassRecord, Fsr3Dispatch,
// CmdExecute, Fsr3Create) and vk.cpp's instance and device hooks. A backend never hooks anything itself. The order:
//  1. load: at the game's first vkCreateInstance while this backend is the choice, the core checks its vendor files
//     (vendor_file.h verify_component: <game>/kiln/vendor/<vendor>/<version>/, size and SHA-256 pinned by vendor_pins.h,
//     generated from launcher/src/data/vendor-components.json; the handles stay open with writes denied for the
//     session) and hands them to load(). A backend whose needs come from its DLL (XeSS: xessVKGetRequired*) maps it
//     here with vendor::load_verified; one whose needs are fixed (AMD FSR: FFX's Vulkan API has no requirements query)
//     may keep the paths and map its DLLs later, in device_ready (the game unloads vulkan-1.dll and maps it again
//     early in its start, and AMD's DLLs import vulkan-1.dll).
//  2. instance_needs, device_needs: DECLARATIVE needs, merged by the core into the game's create infos in vk.cpp's
//     vkCreateInstance / vkCreateDevice hooks (device_merge.h InstanceMerge / DeviceMerge fold them in without writing
//     the game's memory), only for the backend chosen at that moment;
//  3. device_ready: the game's device exists, with what was really enabled on it (DeviceMerge's Report);
//  4. created (create) at each of the game's Fsr3Create calls (world load, FSR 3 chosen, FSR quality, resolution and
//     3D render scale changes), with the sizes; destroy() comes first when a context exists;
//  5. asked to record (record) each frame into the game's own VkCommandBuffer, at the place the game's FSR 3 passes
//     had, from the core's hook on the game's render-command executor;
//  6. destroyed (destroy, then device_lost) on a change of choice, a failure, and before the game's device is destroyed.
//
// Threads: the game records and executes its frame on its main thread; create, record, destroy and reset all come
// from that thread (the core serialises them; a backend needs no lock of its own for them). needs/load/device_ready
// come from the thread that creates the game's instance and device (the main thread too, seen on the real client).
// device_lost comes from vk.cpp's vkDestroyDevice hook, after vkDeviceWaitIdle.
// Faults: the core calls every method under a fault guard; a fault, or `false` from create or record, puts the
// game's own FSR 3 back for the session (Status reason FAULT / CREATE_FAILED / RUNTIME_FAILED). The frame in which
// record fails gets a plain blit of the colour to the output, so it is never empty.
// Lifetime: the backend object lives for the whole session. After `false` from create or record (a session fall-back)
// it gets only what frees its objects: destroy(), tick() and, later, device_lost(); never needs, device_ready, create,
// record or reset again, also not for a Vulkan instance or device the game makes after that. After a FAULT inside the
// backend the core never calls it again (no destroy, tick or device_lost: it may hold a lock), so its contexts and
// Vulkan objects outlive the device until the game ends: a known leak, accepted (a fault is rare and the session ends
// with the game). upscale_logic.h may_call holds these rules. A backend's destructor must not call its vendor.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace kui::upscale {

// kiln.json graphics.upscaler, the same numbers as KILN_UI_UPSCALER_* (kiln_ui.h).
enum class Choice : uint32_t { game = 0, amd = 1, xess = 2 };
// kiln.json graphics.amdUpscaler, the same numbers as KILN_UI_AMD_UPSCALER_*.
enum class AmdPin : uint32_t { automatic = 0, fsr3 = 1 };
// What runs behind the game's FSR 3 option now: KILN_UI_UPSCALE_RUN_* (kiln_ui.h). A backend says which of the last
// three it is (Backend::running).
enum class Running : uint32_t { off = 0, not_fsr = 1, game_fsr3 = 2, amd_fsr4 = 3, amd_fsr3 = 4, xess = 5 };
// Why the chosen upscaler does not run: KILN_UI_UPSCALE_WHY_* (kiln_ui.h). Stable numbers, shared with the UI and
// the docs; never renumbered, only added to.
enum class Why : uint32_t {
    none = 0,             // it runs (or "game" is chosen)
    restart = 1,          // chosen while the game runs, on a device created without its needs (or before Kiln's hooks
                          // went in): it runs from the next start of the game
    files_missing = 2,    // its vendor files are not downloaded (the launcher's Settings, Graphics)
    files_bad = 3,        // a vendor file is not the pinned one (size or SHA-256), or is a link
    load_failed = 4,      // a vendor DLL did not load, or lacks an export
    device = 5,           // the GPU or driver lacks an extension or feature the backend must have
    create_failed = 6,    // the backend refused to create its context for these sizes
    runtime_failed = 7,   // the backend failed while upscaling: the game's FSR 3 for the rest of the session
    fault = 8,            // a fault in the backend or in Kiln's code around it: the game's FSR 3 for the session
    aside = 9,            // stepped aside for another tool (KilnUiGraphicsStatus.aside_upscale says which)
    no_signatures = 10,   // this game build's FSR 3 pass was not found (sigdb feature upscale.swap off)
    inputs = 11,          // the game's textures are not what the backend takes (format, size, a texture missing)
    unavailable = 12,     // this backend is not in this build of Kiln, or not on this system (XeSS: no Vulkan 1.1)
    waiting = 13,         // ready: it starts at the game's next FSR 3 set-up (load a world, or change FSR's quality)
    no_vulkan = 14,       // Kiln did not see the game's Vulkan device being created (Kiln's Vulkan hooks came late)
};

// ---- device set-up (declarative) ----------------------------------------------------------------------------------
// One Vulkan feature struct (VkPhysicalDevice...Features, or VkPhysicalDeviceFeatures2 for the 1.0 features), sType
// first: each VkBool32 set to VK_TRUE is asked for; pNext is ignored (the merger links its own chain). `required`:
// without every feature of it the backend cannot run (Why::device); else it is taken where the device has it.
struct FeatureNeed {
    std::vector<uint8_t> bytes;
    bool required = false;
};
struct ExtensionNeed {
    std::string name;
    bool required = false;
};
struct InstanceNeeds {
    uint32_t min_api_version = 0;  // VK_MAKE_API_VERSION; the game asks for 1.2 (0: no minimum)
    std::vector<ExtensionNeed> extensions;
};
struct DeviceNeeds {
    std::vector<ExtensionNeed> extensions;
    std::vector<FeatureNeed> features;
    // Adds one feature struct: want(VkPhysicalDeviceShaderIntegerDotProductFeatures{sType, nullptr, VK_TRUE}).
    template <class T>
    void want(const T& s, bool required = false) {
        FeatureNeed f;
        f.bytes.resize(sizeof(T));
        std::memcpy(f.bytes.data(), &s, sizeof(T));
        f.required = required;
        features.push_back(std::move(f));
    }
    void want_extension(const char* name, bool required = false) { extensions.push_back({name, required}); }
};
// The physical device the game is about to create its device on (for a backend whose needs depend on it).
struct PhysicalDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;  // unhooked
    uint32_t instance_api = 0;                 // the apiVersion of the instance the physical device came from, as
                                               // created (after Kiln's merge)
    VkPhysicalDeviceProperties props{};
};

// The game's device as created, and what is on it (the game's own and the merged needs).
struct VkContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;  // unhooked: Vulkan's own (winevulkan's under Proton)
    PFN_vkGetDeviceProcAddr gdpa = nullptr;    // unhooked
    uint32_t instance_api = 0;                 // apiVersion of the device's instance as created (after Kiln's merge)
    std::vector<std::string> instance_extensions;  // enabled on the device's instance as created (after Kiln's merge)
    uint32_t graphics_family = 0;              // the family of the game's first queue (its graphics queue)
    VkPhysicalDeviceProperties props{};
    std::vector<std::string> extensions;       // enabled on the device
    std::vector<FeatureNeed> features;         // enabled on the device, one struct per type (VK_TRUE = on); 1.0 features
                                               // as a VkPhysicalDeviceFeatures2
    bool needs_merged = false;                 // the device was made with this backend's device_needs merged in (false:
                                               // the backend was not the choice at the game's start, or the merge was
                                               // refused): a backend that relies on its optional needs says RESTART
    bool has_extension(const char* name) const {
        for (const auto& e : extensions)
            if (e == name) return true;
        return false;
    }
    bool has_instance_extension(const char* name) const {
        for (const auto& e : instance_extensions)
            if (e == name) return true;
        return false;
    }
    // Whether the VkBool32 at `offset` (offsetof) of the enabled struct of `type` is on.
    bool has_feature(VkStructureType type, size_t offset) const {
        for (const auto& f : features) {
            if (f.bytes.size() < offset + sizeof(VkBool32) || f.bytes.size() < sizeof(VkStructureType)) continue;
            VkStructureType t;
            std::memcpy(&t, f.bytes.data(), sizeof t);
            if (t != type) continue;
            VkBool32 v;
            std::memcpy(&v, f.bytes.data() + offset, sizeof v);
            return v == VK_TRUE;
        }
        return false;
    }
};

// ---- the vendor files ---------------------------------------------------------------------------------------------
// What load gets: the component's folder and its files, in vendor-components.json's order, each verified (size and
// SHA-256 through a handle that denies writes) and kept open with writes denied for the session: vendor::Verified's
// {dir, paths, version} (vendor_file.h).
struct Files {
    std::wstring dir;                // <game>\kiln\vendor\<vendor>\<version>
    std::vector<std::wstring> paths; // full paths
    std::string version;             // the component's version ("2.4.0", "3.0.2")
};

// ---- one frame ----------------------------------------------------------------------------------------------------
// A texture of the game's FSR 3 pass, from the game's texture object (sigdb constants upscale/client.tex.*).
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;  // a 2D view of the whole image (1 mip, 1 layer), the image's format
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;     // the image's size (may be larger than the frame's render size)
    VkImageUsageFlags usage = 0;        // the usage the game created it with, 0 when Kiln did not see it created
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // at record: SHADER_READ_ONLY_OPTIMAL for inputs, GENERAL for
                                        // the output. Leave each in it (the game's next barriers expect that).
    bool present() const { return image != VK_NULL_HANDLE; }
};

// What the game's FSR 3 pass hands over, every frame (the game's FSR description at Fsr3Dispatch). On the real client
// (B0): colour R16G16B16A16_SFLOAT linear HDR, depth R32_SFLOAT reverse-Z (about near/z: an infinite far plane), motion
// R16G16_SNORM in UV units current -> previous with +y down and no jitter, reactive and transparency R8_UNORM, output
// R16G16B16A16_SFLOAT.
struct Frame {
    uint64_t frame = 0;          // Kiln's count of FSR frames
    Texture color, depth, motion, reactive, transparency, output;
    float jitter_x = 0, jitter_y = 0;          // render pixels, FSR 3.1's units and sign (Halton(2,3) - 0.5)
    float mv_scale_x = 0, mv_scale_y = 0;      // the game's: the render size (MVs in UV units)
    uint32_t render_width = 0, render_height = 0;  // this frame's (dynamic resolution may make it smaller than color's)
    uint32_t output_width = 0, output_height = 0;
    float frame_ms = 0;          // > 0: the game's frame time, else Kiln's own measured delta (0 for ~500 frames after a load)
    float camera_near = 0, camera_far = 0;  // 0.01 and 10000 on the real client
    float fov_y = 0;             // radians (the game's FOV setting; vertical, likely)
    float pre_exposure = 1;      // > 0 (the game's value, 1 where the game gives 0 or a value that is not usable)
    float game_exposure = 0;     // the game's description's value as it is (0 seen: no pre-exposure), for kiln.log
    float sharpness = 0;         // 0..1, 0 = no extra sharpening (the game sets 0)
    bool reset = false;          // drop history: set by the core on the first frame after create, a world load, a size change
    VkCommandBuffer cb = VK_NULL_HANDLE;  // the game's, recording; Kiln's place in it is where the FSR passes were
};

// ---- creation -----------------------------------------------------------------------------------------------------
struct CreateInfo {
    uint32_t max_render_width = 0, max_render_height = 0;  // the colour texture's size
    uint32_t output_width = 0, output_height = 0;          // the upscale size (the FSR description's, = the game's
                                               // Fsr3Create output; the 3D render scale makes it smaller than the output
                                               // texture). A frame with another upscale size gets a new context first.
    uint32_t output_texture_width = 0, output_texture_height = 0;  // the output texture's size
    float ratio = 0;            // output / render width at creation (1.5 Quality ... 3.0 Ultra Performance; 1 native)
    bool hdr = true;            // the colour is linear HDR (R16G16B16A16_SFLOAT)
    bool depth_inverted = true; // reverse-Z
    bool depth_infinite = false;  // the game's own FSR 3 port takes its two planes as they are (far 10000)
    bool dynamic_resolution = true;  // the render size may be below the texture size
    AmdPin amd = AmdPin::automatic;  // the AMD backend's pin (ignored by others)
    VkFormat color_format = VK_FORMAT_UNDEFINED, output_format = VK_FORMAT_UNDEFINED;
};

class Backend {
  public:
    virtual ~Backend() = default;
    virtual Choice choice() const = 0;
    // The vendor component it loads from (vendor-components.json id: "amd-fsr", "intel-xess").
    virtual const char* component() const = 0;

    // 1. Its verified files (vendor::Verified's dir, paths, version), before any needs are asked. False: `why`
    //    (load_failed, unavailable...) and `detail` (English, for kiln.log and the status); nothing else is called then.
    virtual bool load(const Files& files, Why* why, std::string* detail) = 0;
    // 2. Before the game's instance, then its device, are created (only when this backend is the choice then).
    virtual InstanceNeeds instance_needs() = 0;
    virtual DeviceNeeds device_needs(const PhysicalDevice& gpu) = 0;
    // 3. The game's device exists, with what is enabled on it. False: the device lacks something the backend must
    //    have (why = device or restart, detail names it).
    virtual bool device_ready(const VkContext& vk, Why* why, std::string* detail) = 0;
    // 4. A context for these sizes (the game's Fsr3Create). The core calls destroy() first when one exists.
    virtual bool create(const CreateInfo& ci, std::string* detail) = 0;
    // 5. Record the upscale of `f` into f.cb. False: nothing usable was recorded (the core blits this frame and puts
    //    the game's FSR 3 back). The backend leaves the six textures in the layouts they came in.
    virtual bool record(const Frame& f, std::string* detail) = 0;
    // History is dropped at the next record (also set per frame through Frame::reset).
    virtual void reset() = 0;
    // 6. The context goes (a change, a failure, the device's end). The GPU may still run the last frames: a backend
    //    that frees Vulkan objects here must know they are done (or keep them until device_lost).
    virtual void destroy() = 0;
    // Every Vulkan object of the backend goes; the device is idle and about to be destroyed (vk.cpp's hook).
    virtual void device_lost(VkDevice device) = 0;
    // Each of the game's FSR 3 frames, whether or not the backend records it (the core's frame count, as Frame::frame):
    // free what destroy() kept for the GPU once it is surely done (a context last used kRetireFrames frames ago).
    virtual void tick(uint64_t frame) { (void)frame; }

    // What it runs now (amd_fsr4 / amd_fsr3 / xess) and its version: (major << 22) | (minor << 12) | patch.
    virtual Running running() const = 0;
    virtual uint32_t version() const = 0;
    // "AMD FSR 4.1.2", for kiln.log (never "DLSS").
    virtual std::string name() const = 0;
};

// The backends (one file each; each returns null when it is not in this build).
std::unique_ptr<Backend> make_amd_backend();   // upscale_amd.cpp (B1)
std::unique_ptr<Backend> make_xess_backend();  // upscale_xess.cpp (B2)

inline constexpr uint32_t make_version(uint32_t major, uint32_t minor, uint32_t patch) {
    return (major << 22) | (minor << 12) | patch;
}

}  // namespace kui::upscale

// SPDX-License-Identifier: MIT
// kiln.ui: the parts of Intel XeSS Super Resolution's Vulkan interface Kiln calls (libxess.dll, XeSS SDK 3.0.2), declared
// by Kiln from the SDK's public documentation (doc/xess_sr_developer_guide_english.md, "Initialization Vulkan specific",
// "Execution Vulkan specific") for interoperability with Intel's unmodified DLL. No file of Intel's SDK is in Kiln's
// repository: its headers are not licensed for copying, and the DLL itself (Intel Simplified Software License) may be
// used only unmodified, so Kiln never hooks or patches it; it only calls these exports.
// Layout: the SDK packs its structs at 8 bytes (x64, MSVC ABI); the static_asserts below hold the sizes and offsets the
// 3.0.2 interface has.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

namespace kui::xess {

using Context = struct OpaqueContext*;  // xess_context_handle_t

// xess_result_t (4 bytes). Negative: an error; 0 success; positive: a warning.
enum Result : int32_t {
    kWarnNonexistingFolder = 1,
    kWarnOldDriver = 2,
    kOk = 0,
    kErrUnsupportedDevice = -1,
    kErrUnsupportedDriver = -2,
    kErrUninitialized = -3,
    kErrInvalidArgument = -4,
    kErrDeviceOutOfMemory = -5,
    kErrDevice = -6,
    kErrNotImplemented = -7,
    kErrInvalidContext = -8,
    kErrOperationInProgress = -9,
    kErrUnsupported = -10,
    kErrCantLoadLibrary = -11,
    kErrWrongCallOrder = -12,
    kErrUnknown = -1000,
};
inline const char* result_name(int32_t r) {
    switch (r) {
        case kWarnNonexistingFolder: return "warning: no such folder";
        case kWarnOldDriver: return "warning: an old driver";
        case kOk: return "success";
        case kErrUnsupportedDevice: return "the GPU is not supported";
        case kErrUnsupportedDriver: return "the driver is not supported";
        case kErrUninitialized: return "not initialised";
        case kErrInvalidArgument: return "an invalid argument";
        case kErrDeviceOutOfMemory: return "out of GPU memory";
        case kErrDevice: return "a Vulkan call failed";
        case kErrNotImplemented: return "not implemented";
        case kErrInvalidContext: return "an invalid context";
        case kErrOperationInProgress: return "still in progress";
        case kErrUnsupported: return "not supported in this configuration";
        case kErrCantLoadLibrary: return "a library could not be loaded";
        case kErrWrongCallOrder: return "called in the wrong order";
        default: return "an unknown failure";
    }
}

// xess_quality_settings_t (4 bytes)
enum Quality : int32_t {
    kUltraPerformance = 100,
    kPerformance = 101,
    kBalanced = 102,
    kQuality = 103,
    kUltraQuality = 104,
    kUltraQualityPlus = 105,
    kNativeAA = 106,
};

// xess_init_flags_t bits
enum InitFlag : uint32_t {
    kFlagHighResMv = 1u << 0,
    kFlagInvertedDepth = 1u << 1,
    kFlagExposureScaleTexture = 1u << 2,
    kFlagResponsivePixelMask = 1u << 3,
    kFlagNdcVelocity = 1u << 4,
    kFlagExternalDescriptorHeap = 1u << 5,  // not on Vulkan
    kFlagLdrInputColor = 1u << 6,
    kFlagJitteredMv = 1u << 7,
    kFlagAutoExposure = 1u << 8,
};

// xess_logging_level_t
enum LogLevel : int32_t { kLogDebug = 0, kLogInfo = 1, kLogWarning = 2, kLogError = 3 };
using LogFn = void (*)(const char* message, int32_t level);  // UTF-8; any thread, several at once

#pragma pack(push, 8)
struct Version {  // xess_version_t
    uint16_t major, minor, patch, reserved;
};
// The Super Resolution major version this interface is, as xessGetVersion reports it. That is not the SDK's number: the
// XeSS SDK 3.0.2's libxess.dll (the file Kiln pins, its size and SHA-256 checked before it loads: vendor_file.h) reports
// 2.0.2, because Intel's 3.0 SDK added multi-frame generation in other libraries and kept Super Resolution at 2. The
// version check only guards against a wrong or old library: a major version is the interface's, so 1.x (older, not
// what Kiln declares here) and a later major (may change these structs) are refused; any 2.x with every required export
// (Api::missing) loads.
inline constexpr uint16_t kSuperResolutionMajor = 2;
struct Dim {  // xess_2d_t, also xess_coord_t
    uint32_t x, y;
};
struct ImageInfo {  // xess_vk_image_view_info
    VkImageView view;
    VkImage image;
    VkImageSubresourceRange range;
    VkFormat format;
    uint32_t width, height;
};
struct ExecuteParams {  // xess_vk_execute_params_t
    ImageInfo color;       // SHADER_READ_ONLY_OPTIMAL
    ImageInfo velocity;    // SHADER_READ_ONLY_OPTIMAL
    ImageInfo depth;       // SHADER_READ_ONLY_OPTIMAL (needed without kFlagHighResMv)
    ImageInfo exposure;    // 1x1, with kFlagExposureScaleTexture
    ImageInfo responsive;  // with kFlagResponsivePixelMask
    ImageInfo output;      // GENERAL
    float jitter_x, jitter_y;  // [-0.5, 0.5], input pixels
    float exposure_scale;      // 1 by default
    uint32_t reset_history;
    uint32_t input_width, input_height;
    Dim color_base, velocity_base, depth_base, responsive_base, reserved0, output_base;
};
struct InitParams {  // xess_vk_init_params_t
    Dim output;
    int32_t quality;  // Quality
    uint32_t flags;   // InitFlag
    uint32_t creation_node_mask, visible_node_mask;
    VkDeviceMemory temp_buffer_heap;  // VK_NULL_HANDLE: XeSS allocates its own
    uint64_t buffer_heap_offset;
    VkDeviceMemory temp_texture_heap;
    uint64_t texture_heap_offset;
    VkPipelineCache pipeline_cache;   // optional
};
struct Properties {  // xess_properties_t
    uint32_t required_descriptor_count;
    uint64_t temp_buffer_heap_size, temp_texture_heap_size;
};
#pragma pack(pop)

static_assert(sizeof(Version) == 8 && sizeof(Dim) == 8);
static_assert(sizeof(ImageInfo) == 48 && offsetof(ImageInfo, image) == 8 && offsetof(ImageInfo, range) == 16 &&
              offsetof(ImageInfo, format) == 36 && offsetof(ImageInfo, width) == 40);
static_assert(offsetof(ExecuteParams, velocity) == 48 && offsetof(ExecuteParams, output) == 240 && offsetof(ExecuteParams, jitter_x) == 288 &&
              offsetof(ExecuteParams, reset_history) == 300 && offsetof(ExecuteParams, input_width) == 304 &&
              offsetof(ExecuteParams, color_base) == 312 && offsetof(ExecuteParams, output_base) == 352 && sizeof(ExecuteParams) == 360);
static_assert(offsetof(InitParams, quality) == 8 && offsetof(InitParams, flags) == 12 && offsetof(InitParams, temp_buffer_heap) == 24 &&
              offsetof(InitParams, pipeline_cache) == 56 && sizeof(InitParams) == 64);
static_assert(sizeof(Properties) == 24);

// The exports (C linkage, x64: one calling convention). Names as libxess.dll exports them.
using PFN_GetVersion = int32_t (*)(Version*);
using PFN_GetIntelXeFXVersion = int32_t (*)(Context, Version*);
using PFN_GetOptimalInputResolution = int32_t (*)(Context, const Dim* output, int32_t quality, Dim* optimal, Dim* min, Dim* max);
using PFN_SetVelocityScale = int32_t (*)(Context, float x, float y);
using PFN_SetJitterScale = int32_t (*)(Context, float x, float y);
using PFN_SetExposureMultiplier = int32_t (*)(Context, float scale);
using PFN_SetMaxResponsiveMaskValue = int32_t (*)(Context, float value);
using PFN_SetLoggingCallback = int32_t (*)(Context, int32_t level, LogFn fn);
using PFN_IsOptimalDriver = int32_t (*)(Context);
using PFN_DestroyContext = int32_t (*)(Context);
using PFN_VKGetRequiredInstanceExtensions = int32_t (*)(uint32_t* count, const char* const** names, uint32_t* min_api_version);
using PFN_VKGetRequiredDeviceExtensions = int32_t (*)(VkInstance, VkPhysicalDevice, uint32_t* count, const char* const** names);
using PFN_VKGetRequiredDeviceFeatures = int32_t (*)(VkInstance, VkPhysicalDevice, void** features);
using PFN_VKCreateContext = int32_t (*)(VkInstance, VkPhysicalDevice, VkDevice, Context*);
using PFN_VKBuildPipelines = int32_t (*)(Context, VkPipelineCache, bool blocking, uint32_t flags);
using PFN_VKInit = int32_t (*)(Context, const InitParams*);
using PFN_VKExecute = int32_t (*)(Context, VkCommandBuffer, const ExecuteParams*);

// The table Kiln fills from the DLL (GetProcAddress), or a test from its fakes. Required: every one but the last four.
struct Api {
    PFN_GetVersion get_version = nullptr;
    PFN_VKGetRequiredInstanceExtensions vk_instance_extensions = nullptr;
    PFN_VKGetRequiredDeviceExtensions vk_device_extensions = nullptr;
    PFN_VKGetRequiredDeviceFeatures vk_device_features = nullptr;
    PFN_VKCreateContext vk_create_context = nullptr;
    PFN_VKInit vk_init = nullptr;
    PFN_VKExecute vk_execute = nullptr;
    PFN_DestroyContext destroy_context = nullptr;
    PFN_GetOptimalInputResolution optimal_input_resolution = nullptr;
    PFN_SetVelocityScale set_velocity_scale = nullptr;
    // optional
    PFN_SetLoggingCallback set_logging_callback = nullptr;
    PFN_IsOptimalDriver is_optimal_driver = nullptr;
    PFN_GetIntelXeFXVersion intel_xefx_version = nullptr;
    PFN_SetMaxResponsiveMaskValue set_max_responsive_mask = nullptr;

    // The first required export that is missing, or nullptr.
    const char* missing() const {
        if (!get_version) return "xessGetVersion";
        if (!vk_instance_extensions) return "xessVKGetRequiredInstanceExtensions";
        if (!vk_device_extensions) return "xessVKGetRequiredDeviceExtensions";
        if (!vk_device_features) return "xessVKGetRequiredDeviceFeatures";
        if (!vk_create_context) return "xessVKCreateContext";
        if (!vk_init) return "xessVKInit";
        if (!vk_execute) return "xessVKExecute";
        if (!destroy_context) return "xessDestroyContext";
        if (!optimal_input_resolution) return "xessGetOptimalInputResolution";
        if (!set_velocity_scale) return "xessSetVelocityScale";
        return nullptr;
    }
};

}  // namespace kui::xess

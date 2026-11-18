// SPDX-License-Identifier: MIT
// kiln.ui upscaler swap, AMD FSR backend (upscale_amd.cpp): the plain part. What Kiln asks of the device for AMD's
// FSR API, and how a frame of the game's FSR 3 pass maps onto AMD's FSR API structs (FidelityFX SDK 2.4.0,
// native/third_party/ffx). No windows.h, no calls into AMD's DLLs: host test tests/upscale_test.cpp.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "api/include/ffx_api.h"
#include "api/include/ffx_api_types.h"
#include "api/include/vk/ffx_api_vk.h"
#include "upscale_backend.h"
#include "upscalers/include/ffx_upscale.h"

namespace kui::upscale::amd {

// ---- the device ---------------------------------------------------------------------------------------------------
// AMD's FSR API (2.4.0) has no "what must the device have" query for Vulkan. Its Vulkan backend reads which of these
// extensions the GPU *offers* and then uses them (ffx_vk.cpp GetDeviceCapabilities), so each one the GPU offers must be
// on the device: Kiln asks for all of them (optional: taken where the GPU has them). FSR 4's own (cooperative matrix,
// FP8, integer dot product, compute-shader derivatives, 16-bit storage) are those AMD's sample turns on
// (Cauldron2 device_vk.cpp); without them AMD's selection runs FSR 3.1.
struct UsedIfOffered {
    const char* extension;
    VkStructureType type;  // 0: the extension has no feature struct (offering the extension is enough for FFX)
    uint32_t offset;       // the VkBool32 FFX reads from the GPU's features and then relies on
    uint32_t core13;       // the same feature's offset in VkPhysicalDeviceVulkan13Features (0: not promoted to 1.3)
};
inline const std::vector<UsedIfOffered>& used_if_offered() {
    static const std::vector<UsedIfOffered> v = {
        {VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES,
         static_cast<uint32_t>(offsetof(VkPhysicalDeviceSubgroupSizeControlFeatures, subgroupSizeControl)),
         static_cast<uint32_t>(offsetof(VkPhysicalDeviceVulkan13Features, subgroupSizeControl))},
        {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
         static_cast<uint32_t>(offsetof(VkPhysicalDeviceSynchronization2Features, synchronization2)),
         static_cast<uint32_t>(offsetof(VkPhysicalDeviceVulkan13Features, synchronization2))},
        {VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD,
         static_cast<uint32_t>(offsetof(VkPhysicalDeviceCoherentMemoryFeaturesAMD, deviceCoherentMemory)), 0},
        {VK_AMD_BUFFER_MARKER_EXTENSION_NAME, static_cast<VkStructureType>(0), 0, 0},
    };
    return v;
}
// What the device lacks of the above, given what the GPU offers (`offered`: its extensions; `supported(type, offset)`:
// the GPU has that feature) and what the device has on (`vk`). Empty: AMD's backend finds nothing it would misuse.
template <class Supported>
inline std::string missing_used_if_offered(const VkContext& vk, const std::vector<std::string>& offered, Supported&& supported) {
    for (const auto& u : used_if_offered()) {
        if (std::find(offered.begin(), offered.end(), u.extension) == offered.end()) continue;
        if (u.type && !supported(u.type, u.offset)) continue;  // FFX reads the feature as off: it does not use it
        const bool feature_on = !u.type || vk.has_feature(u.type, u.offset) ||
                                (u.core13 && vk.has_feature(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, u.core13));
        const bool ext_on = vk.has_extension(u.extension) || (u.core13 && vk.instance_api >= VK_API_VERSION_1_3 && feature_on);
        if (!ext_on || !feature_on) return u.extension;
    }
    return {};
}

inline DeviceNeeds device_needs() {
    DeviceNeeds n;
    // what FSR 3.1's Vulkan backend uses when the GPU offers it
    n.want_extension(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    n.want(VkPhysicalDeviceSubgroupSizeControlFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, nullptr, VK_TRUE, VK_TRUE});
    n.want_extension(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    n.want(VkPhysicalDeviceShaderFloat16Int8Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, nullptr, VK_TRUE, VK_TRUE});
    n.want_extension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    n.want(VkPhysicalDeviceSynchronization2Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, nullptr, VK_TRUE});
    n.want_extension(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME);
    n.want(VkPhysicalDeviceCoherentMemoryFeaturesAMD{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD, nullptr, VK_TRUE});
    n.want_extension(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
    n.want_extension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    {
        VkPhysicalDeviceDescriptorIndexingFeatures d{}; d.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        d.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        d.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        d.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
        d.descriptorBindingPartiallyBound = VK_TRUE;
        d.runtimeDescriptorArray = VK_TRUE;
        n.want(d);
    }
    n.want_extension(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
    {
        VkPhysicalDevice16BitStorageFeatures s{}; s.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
        s.storageBuffer16BitAccess = VK_TRUE;
        s.uniformAndStorageBuffer16BitAccess = VK_TRUE;
        n.want(s);
    }
    n.want_extension(VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME);
    n.want(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, nullptr, VK_TRUE});
    {
        VkPhysicalDeviceFeatures2 f{}; f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f.features.shaderInt16 = VK_TRUE;
        f.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;  // also Kiln's motion-vector pass (upscale_mv.h)
        f.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
        n.want(f);
    }
    // FSR 4 (AMD's sample's set)
    n.want_extension(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    n.want(VkPhysicalDeviceCooperativeMatrixFeaturesKHR{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, nullptr, VK_TRUE, VK_FALSE});
    n.want_extension(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
    n.want(VkPhysicalDeviceShaderFloat8FeaturesEXT{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT, nullptr, VK_TRUE, VK_TRUE});
    n.want_extension(VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME);
    n.want(VkPhysicalDeviceShaderIntegerDotProductFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, nullptr, VK_TRUE});
    n.want_extension(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
    n.want(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR, nullptr,
                                                              VK_TRUE, VK_TRUE});
    n.want_extension(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
    {
        VkPhysicalDevice8BitStorageFeatures s{}; s.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES;
        s.storageBuffer8BitAccess = VK_TRUE;
        s.uniformAndStorageBuffer8BitAccess = VK_TRUE;
        n.want(s);
    }
    n.want_extension(VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME);
    {
        VkPhysicalDeviceVulkanMemoryModelFeatures m{}; m.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
        m.vulkanMemoryModel = VK_TRUE;
        m.vulkanMemoryModelDeviceScope = VK_TRUE;
        n.want(m);
    }
    n.want_extension(VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    n.want(VkPhysicalDeviceMaintenance4Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, nullptr, VK_TRUE});
    return n;
}

// ---- creation -----------------------------------------------------------------------------------------------------
inline uint32_t create_flags(const CreateInfo& ci) {
    uint32_t f = 0;
    if (ci.hdr) f |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (ci.depth_inverted) f |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (ci.depth_infinite) f |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (ci.dynamic_resolution) f |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    return f;  // render-size MVs (no DISPLAY_RESOLUTION_MOTION_VECTORS), no jitter in them (no JITTER_CANCELLATION)
}

// A version's major number from AMD's name for it ("4.1.2", "3.1.6", "2.3.4"); 0 when not a version.
inline uint32_t version_major(const char* name) {
    if (!name || name[0] < '0' || name[0] > '9') return 0;
    uint32_t m = 0;
    for (const char* p = name; *p >= '0' && *p <= '9'; ++p) m = m * 10 + static_cast<uint32_t>(*p - '0');
    return m;
}
inline Running running_of(const char* name) {
    const uint32_t m = version_major(name);
    return m >= 4 ? Running::amd_fsr4 : m == 3 ? Running::amd_fsr3 : Running::off;
}
// For the FSR 3.1 pin: the index of the newest 3.x of AMD's list (`names`, as ffxQueryDescGetVersions gives them);
// -1 when there is none.
inline int pick_fsr3(const std::vector<std::string>& names) {
    int best = -1;
    uint32_t best_v = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        if (version_major(names[i].c_str()) != 3) continue;
        uint32_t v = 0, part = 0, k = 0;
        for (const char c : names[i]) {
            if (c >= '0' && c <= '9') part = part * 10 + static_cast<uint32_t>(c - '0');
            else if (c == '.') v = v * 1024 + part, part = 0, ++k;
            else break;
        }
        v = v * 1024 + part;
        for (; k < 2; ++k) v *= 1024;
        if (best < 0 || v > best_v) best = static_cast<int>(i), best_v = v;
    }
    return best;
}

// ---- one frame ----------------------------------------------------------------------------------------------------
inline bool is_depth(VkFormat f) { return ffxApiIsDepthFormat(f); }
// An input in SHADER_READ_ONLY_OPTIMAL (AMD's COMPUTE_READ), or the output in GENERAL (UNORDERED_ACCESS). AMD's backend
// makes its own views of the image and puts each back in the state given here when it is done.
inline FfxApiResource resource(const Texture& t, bool output) {
    if (!t.present()) return FfxApiResource{};
    FfxApiResourceDescription d{};
    d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    d.format = ffxApiGetSurfaceFormatVK(t.format);
    d.width = t.width;
    d.height = t.height;
    d.depth = 1;
    d.mipCount = 1;
    d.flags = FFX_API_RESOURCE_FLAGS_NONE;
    d.usage = output ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    if (is_depth(t.format)) d.usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
    return ffxApiGetResourceVK(reinterpret_cast<void*>(t.image), d,
                               output ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ);
}

// The dispatch for `f`; `motion`: the motion vectors as AMD gets them (Kiln's R16G16_SFLOAT copy, upscale_mv.h).
inline ffxDispatchDescUpscale dispatch(const Frame& f, const Texture& motion, bool reset) {
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.header.pNext = nullptr;
    d.commandList = reinterpret_cast<void*>(f.cb);
    d.color = resource(f.color, false);
    d.depth = resource(f.depth, false);
    d.motionVectors = resource(motion, false);
    d.exposure = FfxApiResource{};  // none: the pre-exposure below (the game has no exposure texture for FSR)
    d.reactive = resource(f.reactive, false);
    d.transparencyAndComposition = resource(f.transparency, false);
    d.output = resource(f.output, true);
    d.jitterOffset = {f.jitter_x, f.jitter_y};
    d.motionVectorScale = {f.mv_scale_x, f.mv_scale_y};
    d.renderSize = {f.render_width, f.render_height};
    d.upscaleSize = {f.output_width, f.output_height};
    d.enableSharpening = f.sharpness > 0.0f;
    d.sharpness = f.sharpness;
    d.frameTimeDelta = f.frame_ms;
    d.preExposure = f.pre_exposure;
    d.reset = reset || f.reset;
    d.cameraNear = f.camera_near;
    d.cameraFar = f.camera_far;
    d.cameraFovAngleVertical = f.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;  // the game's units are metres
    d.flags = 0;
    return d;
}

inline const char* code_name(uint32_t r) {
    switch (r) {
        case FFX_API_RETURN_OK: return "ok";
        case FFX_API_RETURN_ERROR: return "error";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown description type";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "a Vulkan or effect error";
        case FFX_API_RETURN_NO_PROVIDER: return "no provider";
        case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
        case FFX_API_RETURN_ERROR_PARAMETER: return "a parameter was not valid";
        case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "the provider is too old for this description";
        default: return "?";
    }
}

}  // namespace kui::upscale::amd

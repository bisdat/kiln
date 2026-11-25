// SPDX-License-Identifier: MIT
// kiln.ui upscaler swap, AMD FSR backend (upscale_amd.cpp): the plain part. What Kiln asks of the device for AMD's
// FSR API, and how a frame of the game's FSR 3 pass maps onto AMD's FSR API structs (FidelityFX SDK 2.4.0,
// native/third_party/ffx). No windows.h, no calls into AMD's DLLs: host test tests/upscale_test.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
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
        // promoted to 1.3: core on a device that runs as 1.3 (the lower of the instance's and the device's versions)
        const uint32_t api = std::min(vk.instance_api, vk.props.apiVersion ? vk.props.apiVersion : vk.instance_api);
        const bool ext_on = vk.has_extension(u.extension) || (u.core13 && api >= VK_API_VERSION_1_3 && feature_on);
        if (!ext_on || !feature_on) return u.extension;
    }
    return {};
}

// The six extensions (with their features) of AMD's sample for FSR 4 (Cauldron2 device_vk.cpp): cooperative matrix, FP8,
// compute-shader derivatives, 8-bit storage, the memory model, maintenance4. With all of them, AMD's Vulkan FSR 4 (SDK
// 2.4.0, 4.1.2) drew dark scenes too dark on an RX 9070 XT (29.19); OptiScaler 0.9.5's pure-Vulkan FSR 4 from the same
// DLL ran FP8 FSR 4 correctly on the same card on a device without them (its log: the game's 26 device extensions, of
// these only float16/int8 and integer dot product). Kiln's own passes and XeSS need none of the six.
inline const std::vector<const char*>& fsr4_native_extensions() {
    static const std::vector<const char*> v = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,
                                               VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME, VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
                                               VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME, VK_KHR_MAINTENANCE_4_EXTENSION_NAME};
    return v;
}

// ---- FSR 4's sets on the game's device (0.2.0-dev.31.13) -----------------------------------------------------------
// AMD's selection in amd_fidelityfx_upscaler_vk.dll 4.1.2 (SDK 2.4.0) has four FSR 4 providers (their RTTI names), each
// with its own model and shaders (the SPIR-V the DLL carries, deflated and read: the dev.31 wave's out/fsr4-black/REPORT.md):
//  - ffxProvider_FSR4, the FP8 model: an AMD GPU that is not integrated, offers VK_KHR_cooperative_matrix and
//    VK_EXT_shader_float8, and has cooperativeMatrix (its IsSupported; RX 9000). Its shaders declare cooperative matrix,
//    FP8, the memory model, 64-bit atomics and more;
//  - ffxProvider_FSR4_Int8 (RDNA 3, 3.5: RX 7000, Strix Halo), ffxProvider_FSR4_Int8_Navi2 (RDNA 2 discrete: RX 6000)
//    and ffxProvider_FSR4Lite_Int8 (the smaller APUs), chosen by the driver's chip family and revision. Their networks
//    declare integer dot product (4x8 packed) and float16 (RDNA 3's also Int8: shaderInt8); RDNA 3's and the lite
//    model's prepass also ComputeDerivativeGroupLinearKHR (SPV_KHR_compute_shader_derivatives, for quad operations);
//  - FSR 4's own auto exposure (every model, with FFX_UPSCALE_ENABLE_AUTO_EXPOSURE, which Kiln sets) declares
//    ComputeDerivativeGroupLinearKHR too, as FSR 3.1.6's own SPD passes do.
// No FSR 4 INT8 shader declares 8-bit storage, the memory model or anything of maintenance4. So where AMD's selection
// takes an INT8 model, the game's device gets compute-shader derivatives (computeDerivativeGroupLinear: the "INT8 set"),
// and the FP8 device keeps 0.2.0-dev.29.21's: none of the six.
enum class Fsr4Set : uint32_t { none = 0, int8 = 1, native = 2 };
// kiln/control/amd "fsr4ext" (read when the game makes its device): auto (the set this GPU's FSR 4 model needs), off
// (none of the six, as 0.2.0-dev.29.21 to 31.12 everywhere), on (AMD's sample's six, as up to 29.20).
enum class Fsr4Ext : uint32_t { automatic = 0, off = 1, on = 2 };
inline constexpr uint32_t kVendorAmd = 0x1002;
// What Kiln reads of the GPU before the game's device exists (the physical device; AMD's DLL is mapped only later).
struct GpuFacts {
    uint32_t vendor = 0;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    bool coop_matrix_ext = false, float8_ext = false;  // offered: VK_KHR_cooperative_matrix, VK_EXT_shader_float8
    bool coop_matrix = false;                          // VkPhysicalDeviceCooperativeMatrixFeaturesKHR.cooperativeMatrix
    bool derivatives_ext = false;                      // offered: VK_KHR_compute_shader_derivatives
    bool derivative_linear = false;                    // ...computeDerivativeGroupLinear
};
// AMD's own test for its FP8 provider (ffxProvider_FSR4::IsSupported in the 2.4.0 DLL): the FP8 model runs here.
inline bool amd_fp8_gpu(const GpuFacts& g) {
    return g.vendor == kVendorAmd && g.type != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && g.coop_matrix_ext && g.float8_ext && g.coop_matrix;
}
struct Fsr4SetChoice {
    Fsr4Set set = Fsr4Set::none;
    std::string why;  // for kiln.log, at the game's start
};
// Which set the game's device gets, and why. Not the player's FSR 4 / FSR 3.1 choice: that can change while the game
// runs (it applies at the next FSR set-up), the device cannot.
inline Fsr4SetChoice choose_fsr4_set(const GpuFacts& g, Fsr4Ext ext) {
    if (ext == Fsr4Ext::on)
        return {Fsr4Set::native, "kiln/control/amd \"fsr4ext on\": AMD's sample's whole FSR 4 set, as up to 0.2.0-dev.29.20"};
    if (ext == Fsr4Ext::off)
        return {Fsr4Set::none, "kiln/control/amd \"fsr4ext off\": none of FSR 4's sets, as 0.2.0-dev.29.21 to 31.12"};
    if (g.vendor != kVendorAmd) return {Fsr4Set::none, "not an AMD GPU: AMD's selection runs FSR 3.1 here"};
    if (amd_fp8_gpu(g))
        return {Fsr4Set::none, "AMD's selection takes FSR 4's FP8 model on this GPU (cooperative matrix + FP8): its device stays without "
                               "AMD's sample's set, as since 0.2.0-dev.29.21 (with it FSR 4 drew dark scenes too dark on an RX 9070 XT)"};
    std::string why = "AMD's selection takes an FSR 4 INT8 model on this GPU (no cooperative matrix + FP8), else FSR 3.1: the INT8 "
                      "shaders and FSR 4's auto exposure declare compute-shader derivatives (as FSR 3.1's SPD passes do), not 8-bit "
                      "storage, the memory model or maintenance4";
    if (!g.derivatives_ext || !g.derivative_linear)
        why += "; the GPU does not offer VK_KHR_compute_shader_derivatives with computeDerivativeGroupLinear, so it cannot be added";
    return {Fsr4Set::int8, why};
}
// What the set is on the game's device, in words (the AMD FSR set-up line names it).
inline std::string fsr4_device_text(Fsr4Set set, bool derivatives_on) {
    switch (set) {
        case Fsr4Set::native: return "device with AMD's sample's FSR 4 set (fsr4ext on)";
        case Fsr4Set::int8:
            return derivatives_on ? "INT8 device: compute-shader derivatives on" : "INT8 device: compute-shader derivatives not on (the GPU lacks them)";
        case Fsr4Set::none: break;
    }
    return "device without FSR 4's sets";
}

inline DeviceNeeds device_needs(Fsr4Set set = Fsr4Set::none) {
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
    // integer dot product (OptiScaler's device had it too)
    n.want_extension(VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME);
    n.want(VkPhysicalDeviceShaderIntegerDotProductFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, nullptr, VK_TRUE});
    if (set == Fsr4Set::int8) {
        // the INT8 set: what FSR 4's INT8 prepass and its auto exposure declare beyond the above (choose_fsr4_set)
        n.want_extension(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
        n.want(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR, nullptr,
                                                                  VK_FALSE, VK_TRUE});
        return n;
    }
    if (set != Fsr4Set::native) return n;
    // FSR 4's native set (AMD's sample's), only with "fsr4ext on": see fsr4_native_extensions
    n.want_extension(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    n.want(VkPhysicalDeviceCooperativeMatrixFeaturesKHR{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, nullptr, VK_TRUE, VK_FALSE});
    n.want_extension(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
    n.want(VkPhysicalDeviceShaderFloat8FeaturesEXT{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT, nullptr, VK_TRUE, VK_TRUE});
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
// The exposure. The game gives its FSR 3 no exposure texture and a pre-exposure of 0 (its own port reads 1): its colour
// is the lit scene before the game's own exposure, which the game's tone mapping applies after the upscaler, so its
// level follows the scene (a cave at night is far darker than a field at noon). AMD FSR needs the exposure the picture
// will be seen with ("poorly selected exposure values can have a drastic impact", AMD's FSR docs), FSR 4 above all: its
// network works on the exposed signal. With no exposure (1.0) FSR 4 drew a dark scene almost black (SleepyPuppy,
// RX 9070 XT, 29.19 beta 1). Kiln asks for AMD's own auto-exposure, AMD's recommendation; the output stays in the
// input's domain (AMD reverses its internal tone mapping), so the game's tone mapping after it is unchanged.
enum class Exposure : uint32_t { automatic = 0, one = 1 };
// FSR 4's inputs: OptiScaler's known-good set. In this game FSR 3.1 is right with the FSR 3 pass's inputs as they are
// (29.19 beta 1 A/B), FSR 4 is not: dark scenes far too dark (betas 1 and 3, RX 9070 XT, also with auto exposure). FSR 4
// 4.1.2 through OptiScaler, fed from the game's DLSS path, is not dark on the same card (29.19 beta 3 report
// "optiscaler-fsr4": its FSR 3 w/Dx12 backend, FFX 4.1.2, FP8 model). It differs from Kiln's in what it gets:
//  - the pre-exposure: 1 (the game gives DLSS 0, NGX's helper makes it 1: client DOSSIER section 3), where the FSR 3
//    description carries the game's eye adaptation (1 right after a load, 0.077 in the world). FSR 3.1 uses that only as
//    the ratio between frames (ffx_fsr3upscaler DeltaPreExposure), as the game's own port does (fsr_dispatch.txt
//    0x82080); FSR 4's use of it is not public (a signed DLL);
//  - the masks: none (the DLSS path has no reactive or transparency mask; OptiScaler then runs with its reactive mask
//    off), where the FSR 3 pass has both (R8, made for the game's own FSR 3);
//  - the same colour image (view +0x140), depth, motion vectors, HDR, inverted depth, auto exposure (it has no
//    exposure texture), linear colour; its FSR 4 ran through DX12 (Kiln's through AMD's Vulkan DLL).
// So for FSR 4 Kiln gives pre-exposure 1 and no masks by default, FSR 3.1 keeps the game's; each knob below undoes one
// part for an A/B (which one matters is not proven yet).
enum class PreExposure : uint32_t { automatic = 0, game = 1, one = 2 };
enum class Mask : uint32_t { automatic = 0, on = 1, off = 2 };
// What the colour is said to be (create flag NON_LINEAR_COLORSPACE plus the dispatch's colour flag): linear (the game's,
// the default), or sRGB / PQ for an A/B only.
enum class Colour : uint32_t { linear = 0, srgb = 1, pq = 2 };
// Test knobs: <game>/kiln/control/amd, one per line ('#' starts a comment), read at each of the game's FSR set-ups;
// nothing reads them in normal play, each is named in kiln.log's set-up line (the first value is the default):
//   exposure auto|one               auto: AMD's auto exposure; one: none (exposure 1, as up to 29.19 beta 1)
//   preexposure auto|game|one       auto: 1 for FSR 4, the game's for FSR 3.1; game / one: that for both
//   reactive auto|on|off            auto: none for FSR 4, the game's for FSR 3.1; on / off: that for both
//   transparency auto|on|off        the same for the transparency & composition mask
//   masks auto|on|off               both masks at once
//   hdr on|off                      on: the HDR flag (the colour is linear HDR); off: without it
//   colour linear|srgb|pq           linear; srgb / pq: tell FSR the colour is perceptual
//   dynres auto|on|off              auto: no DYNAMIC_RESOLUTION flag for FSR 4 (with it FSR 4 runs its dynamic-resolution
//                                   network, OptiScaler's FSR4ModelSelection.cpp; OptiScaler never sets it), the flag
//                                   for FSR 3.1
//   planes auto|game|swap           auto: the camera planes swapped for FSR 4 (OptiScaler does that with inverted depth,
//                                   FSR31Feature_VkOn12.cpp), the game's for FSR 3.1
//   usage game|rw                   game: inputs read-only, depth a depth target only in a depth format; rw: as NGX's
//                                   ReadWrite resources OptiScaler hands FFX (UAV on every input, depth always a depth
//                                   target)
//   maxrender texture|output        maxRenderSize: the colour texture's, or the output's (OptiScaler: the display size)
//   keyvalues off|optiscaler        OptiScaler's five FFX key-values (velocity 0.5, reactiveness 0.25, shading change
//                                   0.5/ratio, accumulation ratio/10, disocclusion ratio/20)
//   debug off|on                    FSR's own debug view in the output (what it makes of the inputs)
//   fsr4ext auto|off|on             read when the game makes its device (the next game start): auto, the set this
//                                   GPU's FSR 4 model needs (choose_fsr4_set: compute-shader derivatives where AMD's
//                                   selection takes an INT8 model, nothing on the FP8 one); off, none (as 29.21 to
//                                   31.12); on, AMD's sample's whole set (fsr4_native_extensions, as up to 29.20)
//   check auto|off                  auto: the output check after each set-up (upscale_probe.h: a black or NaN picture
//                                   from FSR 4 makes FSR 3.1 run for the rest of the game); off: no check, for an A/B
enum class Planes : uint32_t { automatic = 0, game = 1, swap = 2 };
struct Tuning {
    Exposure exposure = Exposure::automatic;
    PreExposure pre_exposure = PreExposure::automatic;
    Mask reactive = Mask::automatic, transparency = Mask::automatic;
    bool hdr_off = false;
    Colour colour = Colour::linear;
    Mask dynres = Mask::automatic;
    Planes planes = Planes::automatic;
    bool usage_rw = false, maxrender_output = false, keyvalues = false, debug = false;
    Fsr4Ext fsr4ext = Fsr4Ext::automatic;  // read when the game makes its device (from the next game start)
    bool check_off = false;                // no output check after a set-up
    std::vector<std::string> notes;  // lines Kiln did not understand
    bool changed() const {
        return exposure != Exposure::automatic || pre_exposure != PreExposure::automatic || reactive != Mask::automatic ||
               transparency != Mask::automatic || hdr_off || colour != Colour::linear || dynres != Mask::automatic ||
               planes != Planes::automatic || usage_rw || maxrender_output || keyvalues || debug || fsr4ext != Fsr4Ext::automatic || check_off;
    }
};
inline Tuning parse_tuning(std::string_view text) {
    Tuning t;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string line(text.substr(at, end - at));
        at = end + 1;
        if (const size_t h = line.find('#'); h != std::string::npos) line.resize(h);
        std::vector<std::string> w;
        std::string cur;
        for (char c : line) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '=') {
                if (!cur.empty()) w.push_back(cur), cur.clear();
            } else {
                cur.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
            }
        }
        if (!cur.empty()) w.push_back(cur);
        if (w.empty()) continue;
        const std::string v = w.size() > 1 ? w[1] : "";
        if (w[0] == "exposure" && v == "auto") t.exposure = Exposure::automatic;
        else if (w[0] == "exposure" && v == "one") t.exposure = Exposure::one;
        else if (w[0] == "preexposure" && v == "auto") t.pre_exposure = PreExposure::automatic;
        else if (w[0] == "preexposure" && v == "game") t.pre_exposure = PreExposure::game;
        else if (w[0] == "preexposure" && v == "one") t.pre_exposure = PreExposure::one;
        else if (w[0] == "hdr" && (v == "on" || v == "off")) t.hdr_off = v == "off";
        else if ((w[0] == "reactive" || w[0] == "transparency" || w[0] == "masks") && (v == "auto" || v == "on" || v == "off")) {
            const Mask m = v == "on" ? Mask::on : v == "off" ? Mask::off : Mask::automatic;
            if (w[0] != "transparency") t.reactive = m;
            if (w[0] != "reactive") t.transparency = m;
        }
        else if ((w[0] == "colour" || w[0] == "color") && v == "linear") t.colour = Colour::linear;
        else if ((w[0] == "colour" || w[0] == "color") && v == "srgb") t.colour = Colour::srgb;
        else if ((w[0] == "colour" || w[0] == "color") && v == "pq") t.colour = Colour::pq;
        else if (w[0] == "dynres" && (v == "auto" || v == "on" || v == "off")) t.dynres = v == "on" ? Mask::on : v == "off" ? Mask::off : Mask::automatic;
        else if (w[0] == "usage" && (v == "game" || v == "rw")) t.usage_rw = v == "rw";
        else if (w[0] == "maxrender" && (v == "texture" || v == "output")) t.maxrender_output = v == "output";
        else if (w[0] == "keyvalues" && (v == "off" || v == "optiscaler")) t.keyvalues = v == "optiscaler";
        else if (w[0] == "debug" && (v == "off" || v == "on")) t.debug = v == "on";
        else if (w[0] == "fsr4ext" && (v == "auto" || v == "off" || v == "on"))
            t.fsr4ext = v == "on" ? Fsr4Ext::on : v == "off" ? Fsr4Ext::off : Fsr4Ext::automatic;
        else if (w[0] == "check" && (v == "auto" || v == "off")) t.check_off = v == "off";
        else if (w[0] == "planes" && (v == "auto" || v == "game" || v == "swap"))
            t.planes = v == "game" ? Planes::game : v == "swap" ? Planes::swap : Planes::automatic;
        else t.notes.push_back(line);
    }
    return t;
}

// The pre-exposure AMD FSR gets this frame, for what runs (`game`: the game's value, already > 0: Frame::pre_exposure).
inline float pre_exposure_for(Running running, const Tuning& t, float game) {
    if (t.pre_exposure == PreExposure::game) return game;
    if (t.pre_exposure == PreExposure::one) return 1.0f;
    return running == Running::amd_fsr4 ? 1.0f : game;
}
// Whether a mask of the game goes to AMD FSR, for what runs.
inline bool mask_used(Running running, Mask m) { return m == Mask::on || (m == Mask::automatic && running != Running::amd_fsr4); }
// Whether the camera planes go swapped (near = the far plane), for what runs.
inline bool planes_swapped(Running running, const Tuning& t) {
    return t.planes == Planes::swap || (t.planes == Planes::automatic && running == Running::amd_fsr4);
}
// What AMD FSR gets beyond the flags, in words, for kiln.log's set-up line.
inline std::string tuning_text(Running running, const Tuning& t) {
    const bool game_pe = pre_exposure_for(running, t, 0.5f) == 0.5f;
    std::string s = game_pe ? "pre-exposure from the game" : "pre-exposure 1";
    const bool r = mask_used(running, t.reactive), tr = mask_used(running, t.transparency);
    s += r && tr ? ", the game's masks" : r ? ", the game's reactive mask only" : tr ? ", the game's transparency mask only" : ", no masks";
    if (t.colour == Colour::srgb) s += ", colour said sRGB";
    if (t.colour == Colour::pq) s += ", colour said PQ";
    if (planes_swapped(running, t)) s += ", planes swapped";
    if (t.usage_rw) s += ", inputs read-write";
    if (t.maxrender_output) s += ", maxRenderSize the output's";
    if (t.keyvalues) s += ", OptiScaler's key-values";
    if (t.debug) s += ", debug view";
    if (t.check_off) s += ", no output check";
    return s;
}

inline uint32_t create_flags(const CreateInfo& ci, const Tuning& t = {}, Running running = Running::amd_fsr3) {
    uint32_t f = 0;
    if (ci.hdr && !t.hdr_off) f |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (ci.depth_inverted) f |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (ci.depth_infinite) f |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (ci.dynamic_resolution && (t.dynres == Mask::on || (t.dynres == Mask::automatic && running != Running::amd_fsr4)))
        f |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    if (t.colour != Colour::linear) f |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    if (t.exposure == Exposure::automatic) f |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;  // the game has no exposure texture
    if (t.debug) f |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
    return f;  // render-size MVs (no DISPLAY_RESOLUTION_MOTION_VECTORS), no jitter in them (no JITTER_CANCELLATION), linear
               // colour (no NON_LINEAR_COLORSPACE)
}
// The flags in words, for kiln.log at each set-up ("HDR, inverted depth, dynamic resolution, auto exposure").
inline std::string flags_text(uint32_t f) {
    std::string s;
    auto add = [&](bool on, const char* n) {
        if (on) s += (s.empty() ? "" : ", ") + std::string(n);
    };
    add(f & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE, "HDR");
    add(f & FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE, "non-linear colour");
    add(f & FFX_UPSCALE_ENABLE_DEPTH_INVERTED, "inverted depth");
    add(f & FFX_UPSCALE_ENABLE_DEPTH_INFINITE, "infinite depth");
    add(f & FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION, "dynamic resolution");
    add(f & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS, "display-size motion vectors");
    add(f & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION, "jittered motion vectors");
    s += (s.empty() ? "" : ", ") + std::string(f & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE ? "auto exposure" : "exposure 1 (no auto exposure)");
    return s;
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
// What AMD's selection will run, before the context exists (the create flags depend on it): the first of AMD's list
// (ffxQueryDescGetVersions gives the newest first), or FSR 3.1 where Kiln pins it.
inline Running expected_running(const std::vector<std::string>& names, bool pin_fsr3) {
    if (names.empty()) return Running::off;
    return pin_fsr3 ? Running::amd_fsr3 : running_of(names.front().c_str());
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
inline FfxApiResource resource(const Texture& t, bool output, bool rw = false, bool depth_role = false) {
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
    if (rw) d.usage |= FFX_API_RESOURCE_USAGE_UAV;
    if (is_depth(t.format) || (rw && depth_role)) d.usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
    return ffxApiGetResourceVK(reinterpret_cast<void*>(t.image), d,
                               output ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ);
}

// The dispatch for `f`; `motion`: the motion vectors as AMD gets them (Kiln's R16G16_SFLOAT copy, upscale_mv.h).
// The context's maxRenderSize: the colour texture's, or with "maxrender output" the output's (OptiScaler's choice).
inline FfxApiDimensions2D max_render(const CreateInfo& ci, const Tuning& t) {
    return t.maxrender_output ? FfxApiDimensions2D{ci.output_width, ci.output_height} : FfxApiDimensions2D{ci.max_render_width, ci.max_render_height};
}
// OptiScaler's key-values for an upscale ratio (output / render), in the order it sets them.
inline std::vector<std::pair<uint64_t, float>> optiscaler_keyvalues(float ratio) {
    if (!(ratio > 0.0f) || !std::isfinite(ratio)) ratio = 1.0f;
    return {{FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR, 0.5f},
            {FFX_API_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE, 0.25f},
            {FFX_API_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE, 0.5f / ratio},
            {FFX_API_CONFIGURE_UPSCALE_KEY_FACCUMULATIONADDEDPERFRAME, ratio / 10.0f},
            {FFX_API_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION, ratio / 20.0f}};
}

inline ffxDispatchDescUpscale dispatch(const Frame& f, const Texture& motion, bool reset, Running running = Running::amd_fsr3,
                                       const Tuning& t = {}) {
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.header.pNext = nullptr;
    d.commandList = reinterpret_cast<void*>(f.cb);
    d.color = resource(f.color, false, t.usage_rw);
    d.depth = resource(f.depth, false, t.usage_rw, true);
    d.motionVectors = resource(motion, false, t.usage_rw);
    d.exposure = FfxApiResource{};  // none: the pre-exposure below (the game has no exposure texture for FSR)
    d.reactive = mask_used(running, t.reactive) ? resource(f.reactive, false, t.usage_rw) : FfxApiResource{};
    d.transparencyAndComposition = mask_used(running, t.transparency) ? resource(f.transparency, false, t.usage_rw) : FfxApiResource{};
    d.output = resource(f.output, true);
    d.jitterOffset = {f.jitter_x, f.jitter_y};
    d.motionVectorScale = {f.mv_scale_x, f.mv_scale_y};
    d.renderSize = {f.render_width, f.render_height};
    d.upscaleSize = {f.output_width, f.output_height};
    d.enableSharpening = f.sharpness > 0.0f;
    d.sharpness = f.sharpness;
    d.frameTimeDelta = f.frame_ms;
    d.preExposure = pre_exposure_for(running, t, f.pre_exposure);
    d.reset = reset || f.reset;
    d.cameraNear = planes_swapped(running, t) ? f.camera_far : f.camera_near;
    d.cameraFar = planes_swapped(running, t) ? f.camera_near : f.camera_far;
    d.cameraFovAngleVertical = f.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;  // the game's units are metres
    d.flags = t.colour == Colour::srgb ? static_cast<uint32_t>(FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB)
              : t.colour == Colour::pq   ? static_cast<uint32_t>(FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ)
                                         : 0u;
    if (t.debug) d.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
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

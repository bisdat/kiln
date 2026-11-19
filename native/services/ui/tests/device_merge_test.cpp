// SPDX-License-Identifier: MIT
// Host test of device_merge.h: a backend's Vulkan needs folded into the game's create infos. On a chain shaped like the
// game's real one (B0: Features2, Vulkan11, Vulkan12, then extension structs, several of them promoted to 1.3, on an
// instance of API 1.2): the game's memory is never written, no struct type appears twice, nothing promoted sits next to
// its core struct, every wanted feature lands under one of its names, extensions are added once; required needs the
// device lacks leave the create info as the game made it; the instance's extensions and apiVersion; the device query
// through a fake vkGetInstanceProcAddr.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../device_merge.h"

using namespace kui;
using namespace kui::device_merge;

static int g_checks = 0;
#define CHECK(...)                                                                                   \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                        \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #__VA_ARGS__);     \
            std::exit(1);                                                                            \
        }                                                                                            \
    } while (0)

template <class T>
static T make(VkStructureType t) {
    T s{};
    s.sType = t;
    return s;
}
template <class T>
static upscale::FeatureNeed blob(const T& s, bool required = false) {
    upscale::FeatureNeed f;
    f.bytes.assign(reinterpret_cast<const uint8_t*>(&s), reinterpret_cast<const uint8_t*>(&s) + sizeof s);
    reinterpret_cast<VkBaseOutStructure*>(f.bytes.data())->pNext = nullptr;
    f.required = required;
    return f;
}

// The game's device create info, as B0 logged it (the structs that matter here, in its order).
struct GameDevice {
    VkPhysicalDeviceFeatures2 f2 = make<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    VkPhysicalDeviceVulkan11Features v11 = make<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    VkPhysicalDeviceVulkan12Features v12 = make<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures demote =
        make<VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES);
    VkPhysicalDeviceDynamicRenderingFeatures dyn = make<VkPhysicalDeviceDynamicRenderingFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES);
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swm =
        make<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR);
    VkPhysicalDeviceMemoryPriorityFeaturesEXT prio = make<VkPhysicalDeviceMemoryPriorityFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT);
    VkPhysicalDeviceImageRobustnessFeatures robust = make<VkPhysicalDeviceImageRobustnessFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES);
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh = make<VkPhysicalDeviceMeshShaderFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT);
    VkPhysicalDeviceRayQueryFeaturesKHR rq = make<VkPhysicalDeviceRayQueryFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR);
    std::vector<const char*> exts = {"VK_KHR_swapchain", "VK_KHR_swapchain_maintenance1", "VK_EXT_shader_demote_to_helper_invocation",
                                     "VK_KHR_dynamic_rendering", "VK_EXT_memory_priority", "VK_EXT_image_robustness", "VK_EXT_mesh_shader",
                                     "VK_KHR_acceleration_structure", "VK_KHR_ray_query", "VK_KHR_push_descriptor"};
    VkDeviceCreateInfo ci = make<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    GameDevice() {
        f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        f2.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
        f2.features.shaderInt64 = VK_TRUE;
        v12.shaderFloat16 = VK_TRUE;
        v12.bufferDeviceAddress = VK_TRUE;
        v12.timelineSemaphore = VK_TRUE;
        demote.shaderDemoteToHelperInvocation = VK_TRUE;
        dyn.dynamicRendering = VK_TRUE;
        robust.robustImageAccess = VK_TRUE;
        mesh.meshShader = VK_TRUE;
        rq.rayQuery = VK_TRUE;
        link();
    }
    GameDevice(const GameDevice&) = delete;
    void link() {
        f2.pNext = &v11;
        v11.pNext = &v12;
        v12.pNext = &demote;
        demote.pNext = &dyn;
        dyn.pNext = &swm;
        swm.pNext = &prio;
        prio.pNext = &robust;
        robust.pNext = &mesh;
        mesh.pNext = &rq;
        rq.pNext = nullptr;
        ci.pNext = &f2;
        ci.queueCreateInfoCount = 0;
        ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
    }
    // Every byte the game owns, to show the merge wrote none of them.
    std::vector<uint8_t> snapshot() const {
        std::vector<uint8_t> s;
        auto add = [&](const void* p, size_t n) { s.insert(s.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
        add(&f2, sizeof f2), add(&v11, sizeof v11), add(&v12, sizeof v12), add(&demote, sizeof demote), add(&dyn, sizeof dyn);
        add(&swm, sizeof swm), add(&prio, sizeof prio), add(&robust, sizeof robust), add(&mesh, sizeof mesh), add(&rq, sizeof rq);
        add(&ci, sizeof ci);
        add(exts.data(), exts.size() * sizeof(const char*));
        return s;
    }
};

// What an RTX 3080 Ti under Proton offers, for the structs these tests ask about (all supported unless removed).
static DeviceSupport rtx(uint32_t api = VK_API_VERSION_1_2) {
    DeviceSupport s;
    s.api_version = api;
    s.extensions = {"VK_KHR_swapchain", "VK_KHR_swapchain_maintenance1", "VK_EXT_shader_demote_to_helper_invocation", "VK_KHR_dynamic_rendering",
                    "VK_EXT_memory_priority", "VK_EXT_image_robustness", "VK_EXT_mesh_shader", "VK_KHR_acceleration_structure",
                    "VK_KHR_ray_query", "VK_KHR_push_descriptor", "VK_EXT_mutable_descriptor_type", "VK_KHR_shader_integer_dot_product",
                    "VK_KHR_shader_float16_int8", "VK_KHR_16bit_storage", "VK_EXT_subgroup_size_control", "VK_KHR_cooperative_matrix",
                    "VK_KHR_compute_shader_derivatives"};
    auto all_on = [](auto s) {
        auto* b = reinterpret_cast<uint8_t*>(&s);
        for (size_t o = kHeader; o + 4 <= sizeof s; o += 4) set_at(b, sizeof s, static_cast<uint32_t>(o));
        return s;
    };
    s.features.push_back(blob(all_on(make<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2))));
    s.features.push_back(blob(all_on(make<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES))));
    s.features.push_back(blob(all_on(make<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES))));
    s.features.push_back(blob(all_on(make<VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT))));
    s.features.push_back(blob(all_on(make<VkPhysicalDeviceShaderIntegerDotProductFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES))));
    s.features.push_back(blob(all_on(make<VkPhysicalDeviceSubgroupSizeControlFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES))));
    s.features.push_back(blob(all_on(make<VkPhysicalDeviceCooperativeMatrixFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR))));
    s.features.push_back(blob(all_on(make<VkPhysicalDeviceShaderFloat16Int8Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES))));
    if (api >= VK_API_VERSION_1_3)
        s.features.push_back(blob(all_on(make<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES))));
    return s;
}

// The chain of a create info as (type, pointer) in order.
static std::vector<const VkBaseInStructure*> chain_of(const VkDeviceCreateInfo* ci) {
    std::vector<const VkBaseInStructure*> v;
    for (auto* p = static_cast<const VkBaseInStructure*>(ci->pNext); p; p = p->pNext) v.push_back(p);
    return v;
}
static const VkBaseInStructure* find(const VkDeviceCreateInfo* ci, VkStructureType t) {
    for (auto* p : chain_of(ci))
        if (p->sType == t) return p;
    return nullptr;
}
static std::vector<std::string> exts_of(const VkDeviceCreateInfo* ci) {
    std::vector<std::string> v;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) v.emplace_back(ci->ppEnabledExtensionNames[i]);
    return v;
}
// Vulkan's rules for a valid chain: no type twice; no promoted struct next to the core struct it went into.
static void valid_chain(const VkDeviceCreateInfo* ci) {
    std::set<uint32_t> seen;
    std::set<uint32_t> cores, promoted_into;
    for (auto* p : chain_of(ci)) {
        CHECK(seen.insert(static_cast<uint32_t>(p->sType)).second);
        if (const StructInfo* s = find_struct(p->sType)) {
            if (s->core && s->type != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) cores.insert(s->core);
            if (s->promoted) promoted_into.insert(s->promoted);
        }
    }
    for (uint32_t c : cores) CHECK(!promoted_into.count(c));
    CHECK(!(ci->pEnabledFeatures && find(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)));
    const auto e = exts_of(ci);
    CHECK(std::set<std::string>(e.begin(), e.end()).size() == e.size());
}
static bool on(const VkBaseInStructure* p, uint32_t offset) {
    const StructInfo* s = find_struct(p->sType);
    return s && bool_at(reinterpret_cast<const uint8_t*>(p), s->size, offset);
}

// XeSS's needs as its guide lists them.
static upscale::DeviceNeeds xess_needs() {
    upscale::DeviceNeeds n;
    n.want_extension("VK_EXT_mutable_descriptor_type", true);
    auto f2 = make<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    n.want(f2, true);
    auto m = make<VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT);
    m.mutableDescriptorType = VK_TRUE;
    n.want(m, true);
    // as a Vulkan 1.3 struct: the game's chain cannot take one (it holds 1.3-promoted structs)
    auto v13 = make<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    v13.shaderIntegerDotProduct = VK_TRUE;
    n.want(v13);
    // as the extension struct: the game has Vulkan12Features, where it goes
    auto i8 = make<VkPhysicalDeviceShaderFloat16Int8Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES);
    i8.shaderInt8 = VK_TRUE;
    n.want(i8);
    return n;
}

static void xess_on_the_game_device() {
    GameDevice g;
    const auto before = g.snapshot();
    DeviceMerge m;
    Report r;
    const VkDeviceCreateInfo* out = m.merge(&g.ci, xess_needs(), rtx(), &r);
    CHECK(r.ok() && r.changed && out != &g.ci);
    CHECK(g.snapshot() == before);  // not one byte of the game's
    valid_chain(out);
    // extensions: the game's, then Kiln's two, once each
    const auto e = exts_of(out);
    CHECK(e.size() == g.exts.size() + 2);
    CHECK(std::equal(g.exts.begin(), g.exts.end(), e.begin(), [](const char* a, const std::string& b) { return b == a; }));
    CHECK(r.added_extensions == std::vector<std::string>({"VK_EXT_mutable_descriptor_type", "VK_KHR_shader_integer_dot_product"}));
    // features
    const auto* mut = find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT);
    CHECK(mut && on(mut, offsetof(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT, mutableDescriptorType)));
    const auto* dp = find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES);
    CHECK(dp && on(dp, offsetof(VkPhysicalDeviceShaderIntegerDotProductFeatures, shaderIntegerDotProduct)));
    CHECK(!find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES));
    CHECK(!find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES));
    const auto* v12 = find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    CHECK(v12 && v12 != reinterpret_cast<const VkBaseInStructure*>(&g.v12));  // Kiln's copy...
    CHECK(on(v12, offsetof(VkPhysicalDeviceVulkan12Features, shaderInt8)));    // ...with shaderInt8 on
    CHECK(on(v12, offsetof(VkPhysicalDeviceVulkan12Features, shaderFloat16)) && on(v12, offsetof(VkPhysicalDeviceVulkan12Features, bufferDeviceAddress)));
    CHECK(!g.v12.shaderInt8);
    // the 1.0 feature was on already: the game's Features2 is not changed, so its copy is not needed for it, but it sits
    // before Vulkan12Features, which is changed: copied, same content
    const auto* f2 = find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    CHECK(f2 && f2 != reinterpret_cast<const VkBaseInStructure*>(&g.f2));
    CHECK(std::memcmp(reinterpret_cast<const uint8_t*>(f2) + kHeader, reinterpret_cast<const uint8_t*>(&g.f2) + kHeader, sizeof g.f2 - kHeader) == 0);
    // the rest of the game's chain is the game's own memory, in its order
    const auto c = chain_of(out);
    size_t at = 0;
    while (at < c.size() && c[at] != reinterpret_cast<const VkBaseInStructure*>(&g.demote)) ++at;
    CHECK(at < c.size());
    const void* rest[] = {&g.demote, &g.dyn, &g.swm, &g.prio, &g.robust, &g.mesh, &g.rq};
    CHECK(c.size() - at == 7);
    for (size_t i = 0; i < 7; ++i) CHECK(c[at + i] == rest[i]);
    CHECK(out->pEnabledFeatures == nullptr);
    // the report's view of the device (VkContext.features)
    bool saw_mut = false, saw_v12_int8 = false;
    for (const auto& f : r.features) {
        const auto t = type_of(f.bytes.data(), f.bytes.size());
        if (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT) saw_mut = true;
        if (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            saw_v12_int8 = bool_at(f.bytes.data(), f.bytes.size(), offsetof(VkPhysicalDeviceVulkan12Features, shaderInt8));
        CHECK(reinterpret_cast<const VkBaseInStructure*>(f.bytes.data())->pNext == nullptr);
    }
    CHECK(saw_mut && saw_v12_int8);
    CHECK(r.extensions == e);
    // names for the log
    bool named = false;
    for (const auto& n : r.added_features) named |= n == "VkPhysicalDeviceVulkan12Features.shaderInt8";
    CHECK(named);
    // the merge object's copy outlives nothing it points to in the game's memory beyond `rest`: merge again, fresh
    Report r2;
    const VkDeviceCreateInfo* again = m.merge(&g.ci, xess_needs(), rtx(), &r2);
    CHECK(r2.ok() && again == out);
    valid_chain(again);
}

static void required_missing_changes_nothing() {
    GameDevice g;
    const auto before = g.snapshot();
    DeviceSupport s = rtx();
    s.extensions.erase(std::find(s.extensions.begin(), s.extensions.end(), "VK_EXT_mutable_descriptor_type"));
    for (auto& f : s.features)
        if (type_of(f.bytes.data(), f.bytes.size()) == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT)
            std::memset(f.bytes.data() + kHeader, 0, f.bytes.size() - kHeader);
    DeviceMerge m;
    Report r;
    const VkDeviceCreateInfo* out = m.merge(&g.ci, xess_needs(), s, &r);
    CHECK(out == &g.ci && !r.ok() && !r.changed && r.added_extensions.empty());
    CHECK(std::find(r.missing_required.begin(), r.missing_required.end(), "VK_EXT_mutable_descriptor_type") != r.missing_required.end());
    bool feat = false;
    for (const auto& x : r.missing_required) feat |= x.find("VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT") == 0;
    CHECK(feat);
    CHECK(g.snapshot() == before);
    CHECK(r.extensions.size() == g.exts.size());  // the device as the game asked
}

static void optional_missing_is_skipped() {
    GameDevice g;
    DeviceSupport s = rtx();
    s.extensions.erase(std::find(s.extensions.begin(), s.extensions.end(), "VK_KHR_shader_integer_dot_product"));
    for (auto& f : s.features)
        if (type_of(f.bytes.data(), f.bytes.size()) == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES)
            std::memset(f.bytes.data() + kHeader, 0, f.bytes.size() - kHeader);
    DeviceMerge m;
    Report r;
    const VkDeviceCreateInfo* out = m.merge(&g.ci, xess_needs(), s, &r);
    CHECK(r.ok() && r.changed);
    valid_chain(out);
    CHECK(!find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES));
    CHECK(find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT));
    CHECK(r.skipped_optional.size() == 1 && r.skipped_optional[0] == "VkPhysicalDeviceVulkan13Features.shaderIntegerDotProduct");
}

static void nothing_to_do() {
    GameDevice g;
    upscale::DeviceNeeds n;
    n.want_extension("VK_KHR_swapchain", true);  // the game's own
    auto f2 = make<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;  // on already
    n.want(f2, true);
    auto fi = make<VkPhysicalDeviceShaderFloat16Int8Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES);
    fi.shaderFloat16 = VK_TRUE;  // on already, in Vulkan12Features
    n.want(fi, true);
    DeviceMerge m;
    Report r;
    CHECK(m.merge(&g.ci, n, rtx(), &r) == &g.ci && r.ok() && !r.changed && r.added_features.empty());
    CHECK(m.merge(&g.ci, upscale::DeviceNeeds{}, rtx(), &r) == &g.ci && r.ok() && !r.changed);
}

static void enabled_features_path() {
    // a game that uses pEnabledFeatures and no Features2
    VkPhysicalDeviceFeatures base{};
    base.samplerAnisotropy = VK_TRUE;
    const char* exts[] = {"VK_KHR_swapchain"};
    auto ci = make<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    ci.pEnabledFeatures = &base;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = exts;
    DeviceMerge m;
    Report r;
    const VkDeviceCreateInfo* out = m.merge(&ci, xess_needs(), rtx(), &r);
    CHECK(r.ok() && r.changed);
    valid_chain(out);
    CHECK(out->pEnabledFeatures && out->pEnabledFeatures != &base);
    CHECK(out->pEnabledFeatures->samplerAnisotropy && out->pEnabledFeatures->shaderStorageImageWriteWithoutFormat);
    CHECK(!base.shaderStorageImageWriteWithoutFormat);
    CHECK(!find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2));
    // with no struct for 1.2 in the chain, shaderInt8 comes as the extension struct, with its extension
    const auto* fi = find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES);
    CHECK(fi && on(fi, offsetof(VkPhysicalDeviceShaderFloat16Int8Features, shaderInt8)));
    const auto e = exts_of(out);
    CHECK(std::find(e.begin(), e.end(), "VK_KHR_shader_float16_int8") != e.end());
    // enabled_features reports the 1.0 features as a Features2
    bool saw = false;
    for (const auto& f : r.features)
        if (type_of(f.bytes.data(), f.bytes.size()) == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            saw = bool_at(f.bytes.data(), f.bytes.size(), kHeader + offsetof(VkPhysicalDeviceFeatures, shaderStorageImageWriteWithoutFormat));
    CHECK(saw);
    // neither pEnabledFeatures nor Features2: Kiln's own pEnabledFeatures
    ci.pEnabledFeatures = nullptr;
    out = m.merge(&ci, xess_needs(), rtx(), &r);
    CHECK(r.ok() && out->pEnabledFeatures && out->pEnabledFeatures->shaderStorageImageWriteWithoutFormat && !out->pEnabledFeatures->samplerAnisotropy);
}

static void unknown_structs() {
    // a struct Kiln does not know the size of, before one it must change: no copy is safe
    struct Odd {
        VkStructureType sType;
        void* pNext;
        uint32_t whatever[7];
    } odd{static_cast<VkStructureType>(1999999000), nullptr, {}};
    auto v12 = make<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    odd.pNext = &v12;
    auto ci = make<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    ci.pNext = &odd;
    upscale::DeviceNeeds n;
    auto fi = make<VkPhysicalDeviceShaderFloat16Int8Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES);
    fi.shaderInt8 = VK_TRUE;
    n.want(fi, true);
    DeviceMerge m;
    Report r;
    CHECK(m.merge(&ci, n, rtx(), &r) == &ci && !r.ok());
    CHECK(r.missing_required.back().find("cannot copy the game's feature struct sType 1999999000") != std::string::npos);
    // after the last struct Kiln changes, an unknown one is left where it is
    auto ci2 = make<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    v12.pNext = &odd;
    odd.pNext = nullptr;
    ci2.pNext = &v12;
    const VkDeviceCreateInfo* out = m.merge(&ci2, n, rtx(), &r);
    CHECK(r.ok() && out != &ci2);
    const auto c = chain_of(out);
    CHECK(c.size() == 2 && c[1] == reinterpret_cast<const VkBaseInStructure*>(&odd) && on(c[0], offsetof(VkPhysicalDeviceVulkan12Features, shaderInt8)));
    // a need of a type Kiln does not know: required -> missing, optional -> skipped
    upscale::FeatureNeed u;
    u.bytes.resize(32, 0);
    const VkStructureType t = static_cast<VkStructureType>(1999999001);
    std::memcpy(u.bytes.data(), &t, sizeof t);
    upscale::DeviceNeeds n2;
    n2.features.push_back(u);
    m.merge(&ci2, n2, rtx(), &r);
    CHECK(r.ok() && r.skipped_optional.size() == 1);
    n2.features[0].required = true;
    CHECK(m.merge(&ci2, n2, rtx(), &r) == &ci2 && !r.ok());
}

static void core_only_fields() {
    // Vulkan12Features.drawIndirectCount has no extension struct: a new Vulkan12Features, unless a 1.2-promoted struct is
    // in the chain (they would conflict) or the device runs below 1.2
    auto tl = make<VkPhysicalDeviceTimelineSemaphoreFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
    tl.timelineSemaphore = VK_TRUE;
    auto ci = make<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    upscale::DeviceNeeds n;
    auto v12 = make<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    v12.drawIndirectCount = VK_TRUE;
    n.want(v12, true);
    DeviceMerge m;
    Report r;
    const VkDeviceCreateInfo* out = m.merge(&ci, n, rtx(), &r);
    CHECK(r.ok() && find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES));
    ci.pNext = &tl;
    CHECK(m.merge(&ci, n, rtx(), &r) == &ci && !r.ok());
    ci.pNext = nullptr;
    DeviceSupport old = rtx(VK_API_VERSION_1_1);
    CHECK(m.merge(&ci, n, old, &r) == &ci && !r.ok());
    // a Vulkan12Features need where the chain has the promoted struct: the field goes there
    upscale::DeviceNeeds n3;
    auto w = make<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    w.shaderInt8 = VK_TRUE;
    n3.want(w, true);
    auto fi = make<VkPhysicalDeviceShaderFloat16Int8Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES);
    fi.shaderFloat16 = VK_TRUE;
    ci.pNext = &fi;
    out = m.merge(&ci, n3, rtx(), &r);
    CHECK(r.ok());
    valid_chain(out);
    const auto* got = find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES);
    CHECK(got && got != reinterpret_cast<const VkBaseInStructure*>(&fi) && on(got, offsetof(VkPhysicalDeviceShaderFloat16Int8Features, shaderInt8)) &&
          on(got, offsetof(VkPhysicalDeviceShaderFloat16Int8Features, shaderFloat16)));
    CHECK(!fi.shaderInt8 && !find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES));
    // and a device on 1.3 with a Vulkan13Features in the chain takes a 1.3 field there
    auto g13 = make<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    ci.pNext = &g13;
    upscale::DeviceNeeds n4;
    auto dp = make<VkPhysicalDeviceShaderIntegerDotProductFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES);
    dp.shaderIntegerDotProduct = VK_TRUE;
    n4.want(dp, true);
    out = m.merge(&ci, n4, rtx(VK_API_VERSION_1_3), &r);
    CHECK(r.ok());
    valid_chain(out);
    CHECK(on(find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES), offsetof(VkPhysicalDeviceVulkan13Features, shaderIntegerDotProduct)));
    CHECK(!find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES));
    CHECK(r.added_extensions.empty());  // a field of a core struct needs no extension
}

static void core_and_promoted_added_together() {
    // on an empty chain: a core-only 1.2 field (a new Vulkan12Features) and a 1.2-promoted one asked as its extension
    // struct, in either order: one Vulkan12Features holds both, no Float16Int8 next to it
    for (int order = 0; order < 2; ++order) {
        upscale::DeviceNeeds n;
        auto fi = make<VkPhysicalDeviceShaderFloat16Int8Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES);
        fi.shaderInt8 = VK_TRUE;
        auto v12 = make<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        v12.drawIndirectCount = VK_TRUE;
        if (order == 0) n.want(fi, true), n.want(v12, true);
        else n.want(v12, true), n.want(fi, true);
        auto ci = make<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        DeviceMerge m;
        Report r;
        const VkDeviceCreateInfo* out = m.merge(&ci, n, rtx(), &r);
        CHECK(r.ok() && r.changed);
        valid_chain(out);
        const auto* got = find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        CHECK(got && on(got, offsetof(VkPhysicalDeviceVulkan12Features, drawIndirectCount)) && on(got, offsetof(VkPhysicalDeviceVulkan12Features, shaderInt8)));
        CHECK(!find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES) && chain_of(out).size() == 1);
    }
    // two needs naming the same new extension struct by its two names: one struct
    upscale::DeviceNeeds n;
    auto v13 = make<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    v13.shaderIntegerDotProduct = VK_TRUE;
    n.want(v13);
    auto dp = make<VkPhysicalDeviceShaderIntegerDotProductFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES);
    dp.shaderIntegerDotProduct = VK_TRUE;
    n.want(dp, true);
    GameDevice g;
    DeviceMerge m;
    Report r;
    const VkDeviceCreateInfo* out = m.merge(&g.ci, n, rtx(), &r);
    CHECK(r.ok());
    valid_chain(out);
    CHECK(find(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES) && r.added_extensions.size() == 1);
}

static void extensions_once() {
    GameDevice g;
    upscale::DeviceNeeds n;
    n.want_extension("VK_EXT_mutable_descriptor_type", true);
    n.want_extension("VK_EXT_mutable_descriptor_type", false);
    n.want_extension("VK_KHR_push_descriptor", true);  // the game's
    n.want_extension("VK_NV_nothing_like_it");         // optional, not there
    DeviceMerge m;
    Report r;
    const VkDeviceCreateInfo* out = m.merge(&g.ci, n, rtx(), &r);
    CHECK(r.ok() && r.changed && r.added_extensions == std::vector<std::string>({"VK_EXT_mutable_descriptor_type"}));
    CHECK(r.skipped_optional == std::vector<std::string>({"VK_NV_nothing_like_it"}));
    valid_chain(out);
    CHECK(out->pNext == g.ci.pNext);  // only extensions: the game's chain as it is
}

// ---- the instance ----
static void instance_merge() {
    auto app = make<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.apiVersion = VK_API_VERSION_1_2;
    app.pEngineName = "Holistic";
    const char* exts[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    auto ci = make<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;
    InstanceSupport s;
    s.loader_version = VK_API_VERSION_1_4;
    s.extensions = {"VK_KHR_surface", "VK_KHR_win32_surface", "VK_KHR_get_surface_capabilities2", "VK_EXT_debug_utils"};
    InstanceMerge m;
    Report r;
    // XeSS's likely needs: Vulkan 1.1, no extension: nothing changes
    upscale::InstanceNeeds n;
    n.min_api_version = VK_API_VERSION_1_1;
    CHECK(m.merge(&ci, n, s, &r) == &ci && r.ok() && !r.changed && r.api_version == VK_API_VERSION_1_2);
    // an extension and Vulkan 1.3
    n.min_api_version = VK_API_VERSION_1_3;
    n.extensions = {{"VK_KHR_get_surface_capabilities2", true}, {"VK_KHR_surface", true}, {"VK_EXT_nope", false}};
    const VkInstanceCreateInfo* out = m.merge(&ci, n, s, &r);
    CHECK(out != &ci && r.ok() && r.changed && r.api_version == VK_API_VERSION_1_3);
    CHECK(out->pApplicationInfo != &app && out->pApplicationInfo->apiVersion == VK_API_VERSION_1_3 &&
          std::strcmp(out->pApplicationInfo->pEngineName, "Holistic") == 0 && app.apiVersion == VK_API_VERSION_1_2);
    CHECK(out->enabledExtensionCount == 3 && std::strcmp(out->ppEnabledExtensionNames[2], "VK_KHR_get_surface_capabilities2") == 0);
    CHECK(r.skipped_optional == std::vector<std::string>({"VK_EXT_nope"}) && ci.enabledExtensionCount == 2);
    // the loader is too old for the minimum, or a required extension is missing: unchanged
    s.loader_version = VK_API_VERSION_1_2;
    CHECK(m.merge(&ci, n, s, &r) == &ci && !r.ok() && r.missing_required[0].find("Vulkan 1.3") == 0);
    s.loader_version = VK_API_VERSION_1_4;
    n.extensions.push_back({"VK_EXT_required_but_absent", true});
    CHECK(m.merge(&ci, n, s, &r) == &ci && !r.ok());
    // no application info at all (Vulkan 1.0): Kiln's own, with the minimum
    auto bare = make<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    upscale::InstanceNeeds n2;
    n2.min_api_version = VK_API_VERSION_1_1;
    out = m.merge(&bare, n2, s, &r);
    CHECK(r.ok() && out->pApplicationInfo && out->pApplicationInfo->apiVersion == VK_API_VERSION_1_1 &&
          out->pApplicationInfo->sType == VK_STRUCTURE_TYPE_APPLICATION_INFO);
}

// ---- the queries, through a fake Vulkan ----
namespace fake {
VkPhysicalDevice kPhys = reinterpret_cast<VkPhysicalDevice>(0x1234);
VkInstance kInst = reinterpret_cast<VkInstance>(0x5678);
std::vector<VkStructureType> asked;
VKAPI_ATTR void VKAPI_CALL props(VkPhysicalDevice, VkPhysicalDeviceProperties* p) {
    *p = {};
    p->apiVersion = VK_API_VERSION_1_4;
    std::strcpy(p->deviceName, "Fake RTX");
}
VKAPI_ATTR VkResult VKAPI_CALL enum_dev_ext(VkPhysicalDevice, const char*, uint32_t* n, VkExtensionProperties* out) {
    static const char* e[] = {"VK_EXT_mutable_descriptor_type", "VK_KHR_shader_integer_dot_product"};
    if (!out) return *n = 2, VK_SUCCESS;
    for (uint32_t i = 0; i < *n && i < 2; ++i) std::strcpy(out[i].extensionName, e[i]);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL feats2(VkPhysicalDevice, VkPhysicalDeviceFeatures2* f) {
    f->features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    asked.clear();
    for (auto* p = static_cast<VkBaseOutStructure*>(f->pNext); p; p = p->pNext) {
        asked.push_back(p->sType);
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT)
            reinterpret_cast<VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT*>(p)->mutableDescriptorType = VK_TRUE;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(p)->shaderInt8 = VK_TRUE;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL inst_version(uint32_t* v) { return *v = VK_API_VERSION_1_3, VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL enum_inst_ext(const char*, uint32_t* n, VkExtensionProperties* out) {
    if (!out) return *n = 1, VK_SUCCESS;
    std::strcpy(out[0].extensionName, "VK_KHR_surface");
    return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance i, const char* name) {
    const std::string n = name;
    if (!i) {
        if (n == "vkEnumerateInstanceVersion") return reinterpret_cast<PFN_vkVoidFunction>(&inst_version);
        if (n == "vkEnumerateInstanceExtensionProperties") return reinterpret_cast<PFN_vkVoidFunction>(&enum_inst_ext);
        return nullptr;
    }
    if (n == "vkGetPhysicalDeviceProperties") return reinterpret_cast<PFN_vkVoidFunction>(&props);
    if (n == "vkEnumerateDeviceExtensionProperties") return reinterpret_cast<PFN_vkVoidFunction>(&enum_dev_ext);
    if (n == "vkGetPhysicalDeviceFeatures2") return reinterpret_cast<PFN_vkVoidFunction>(&feats2);
    return nullptr;
}
}  // namespace fake

static void queries() {
    upscale::PhysicalDevice gpu;
    gpu.instance = fake::kInst;
    gpu.physical = fake::kPhys;
    gpu.gipa = &fake::gipa;
    gpu.instance_api = VK_API_VERSION_1_2;
    const DeviceSupport s = query_support(gpu, xess_needs());
    CHECK(s.api_version == VK_API_VERSION_1_2);  // the instance's 1.2, though the device runs 1.4
    CHECK(s.extensions.size() == 2 && s.has_extension("VK_EXT_mutable_descriptor_type"));
    // asked about: mutable (its extension is there), the dot-product struct (its extension), Vulkan12 and Float16Int8
    // (shaderInt8's two names: both valid on 1.2, the extension struct being promoted to it); not Vulkan13 (above the
    // instance's version)
    std::set<VkStructureType> a(fake::asked.begin(), fake::asked.end());
    CHECK(a.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT) && a.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) &&
          a.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES) && a.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES));
    CHECK(!a.count(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES));
    CHECK(s.has(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, kHeader + offsetof(VkPhysicalDeviceFeatures, shaderStorageImageWriteWithoutFormat)));
    CHECK(s.has(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, offsetof(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT, mutableDescriptorType)));
    // shaderInt8 asked as the extension struct, answered through Vulkan12Features
    CHECK(s.has(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, offsetof(VkPhysicalDeviceShaderFloat16Int8Features, shaderInt8)));
    CHECK(!s.has(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, offsetof(VkPhysicalDeviceVulkan13Features, shaderIntegerDotProduct)));
    // the merge through the device query
    GameDevice g;
    DeviceMerge m;
    Report r;
    const VkDeviceCreateInfo* out = m.merge(&g.ci, xess_needs(), gpu, &r);
    CHECK(r.ok() && r.changed);
    valid_chain(out);
    CHECK(r.skipped_optional.size() == 1);  // the dot product: the fake device has the extension but says the feature is off
    // the instance query
    const InstanceSupport is = query_instance_support(&fake::gipa);
    CHECK(is.loader_version == VK_API_VERSION_1_3 && is.extensions == std::vector<std::string>({"VK_KHR_surface"}));
    CHECK(query_instance_support(nullptr).extensions.empty());
}

static void tables() {
    // every struct's size and every promoted field's two offsets are within their structs; no type twice
    std::set<uint32_t> types;
    for (const auto& s : structs()) {
        CHECK(types.insert(static_cast<uint32_t>(s.type)).second);
        CHECK(s.size >= kHeader + sizeof(VkBool32) && (s.size - kHeader) % sizeof(VkBool32) == 0);
        CHECK(!(s.core && s.promoted));
    }
    for (const auto& p : promoted()) {
        const StructInfo* c = find_struct(p.core);
        CHECK(c && c->core && p.core_offset + 4 <= c->size);
        if (p.ext) {
            const StructInfo* e = find_struct(p.ext);
            CHECK(e && e->promoted == c->core && p.ext_offset + 4 <= e->size);
        }
    }
    CHECK(field_name(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, kHeader + offsetof(VkPhysicalDeviceFeatures, shaderStorageImageWriteWithoutFormat)) ==
          "VkPhysicalDeviceFeatures.shaderStorageImageWriteWithoutFormat");
    CHECK(field_name(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, offsetof(VkPhysicalDeviceShaderIntegerDotProductFeatures, shaderIntegerDotProduct)) ==
          "VkPhysicalDeviceShaderIntegerDotProductFeatures.shaderIntegerDotProduct");
}

int main() {
    tables();
    xess_on_the_game_device();
    required_missing_changes_nothing();
    optional_missing_is_skipped();
    nothing_to_do();
    enabled_features_path();
    unknown_structs();
    core_only_fields();
    core_and_promoted_added_together();
    extensions_once();
    instance_merge();
    queries();
    std::printf("device_merge_test: %d checks passed\n", g_checks);
    return 0;
}

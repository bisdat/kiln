// SPDX-License-Identifier: MIT
// kiln.ui: folds an upscaler backend's Vulkan needs (upscale_backend.h InstanceNeeds / DeviceNeeds) into the game's
// vkCreateInstance / vkCreateDevice create infos, without writing the game's memory (0.2.0-dev.29.18 phase B).
//
// The game's device chains VkPhysicalDeviceFeatures2 + Vulkan11Features + Vulkan12Features (no Vulkan13Features) and
// a dozen extension structs, from an instance created with apiVersion 1.2 (B0's run on the real client). A backend's
// needs come as whole feature structs (VK_TRUE = wanted), possibly of a type the game's chain holds under another name:
// Vulkan's rules allow either the core VulkanXYFeatures struct or the extension structs promoted into it, never both
// (VUID-VkDeviceCreateInfo-pNext-02829/02830/06532). So each wanted feature goes, in this order:
//  1. into the game's struct of the same type;
//  2. into the game's struct that holds the same feature under its other name (core <-> promoted extension struct);
//  3. into a new extension struct (its extension is enabled with it when the device lists it), or, for a core-only
//     feature, a new VulkanXYFeatures struct when the device's version allows it and the chain has no struct it would
//     conflict with.
// The game's structs are never written: the merge copies the start of the game's chain up to the last struct it
// changes (those must be of a type Kiln knows the size of), links its own new structs in front, and leaves the rest of
// the game's chain where it is. Extensions the game enables stay; a need's extension is added once.
// A required need the device does not have (or Kiln cannot place) leaves the game's create info as it was: the backend
// cannot run, and the report says what is missing. Optional needs are taken where the device has them.
// Plain C++ over Vulkan's headers (no windows.h): host test tests/device_merge_test.cpp.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "upscale_backend.h"

namespace kui::device_merge {

// ---- what Kiln knows about Vulkan's feature structs ---------------------------------------------------------------
struct StructInfo {
    VkStructureType type;
    uint32_t size;
    uint32_t core;      // a core VulkanXYFeatures struct: its version (VK_API_VERSION_1_x); 0 otherwise
    uint32_t promoted;  // an extension struct promoted to core: that version; 0 when never promoted
    const char* ext;    // the device extension that brings the struct (nullptr: core only)
    const char* name;
};

#define KUI_CORE(T, S, V) {S, sizeof(T), V, 0, nullptr, #T}
#define KUI_EXT(T, S, P, E) {S, sizeof(T), 0, P, E, #T}
inline const std::vector<StructInfo>& structs() {
    static const std::vector<StructInfo> t = {
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, sizeof(VkPhysicalDeviceFeatures2), VK_API_VERSION_1_0, 0, nullptr,
         "VkPhysicalDeviceFeatures2"},
        KUI_CORE(VkPhysicalDeviceVulkan11Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VK_API_VERSION_1_1),
        KUI_CORE(VkPhysicalDeviceVulkan12Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VK_API_VERSION_1_2),
        KUI_CORE(VkPhysicalDeviceVulkan13Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VK_API_VERSION_1_3),
        KUI_CORE(VkPhysicalDeviceVulkan14Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, VK_API_VERSION_1_4),
        // promoted to 1.1
        KUI_EXT(VkPhysicalDevice16BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VK_API_VERSION_1_1, "VK_KHR_16bit_storage"),
        KUI_EXT(VkPhysicalDeviceMultiviewFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, VK_API_VERSION_1_1, "VK_KHR_multiview"),
        KUI_EXT(VkPhysicalDeviceVariablePointersFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, VK_API_VERSION_1_1, "VK_KHR_variable_pointers"),
        KUI_EXT(VkPhysicalDeviceProtectedMemoryFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, VK_API_VERSION_1_1, nullptr),
        KUI_EXT(VkPhysicalDeviceSamplerYcbcrConversionFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, VK_API_VERSION_1_1, "VK_KHR_sampler_ycbcr_conversion"),
        KUI_EXT(VkPhysicalDeviceShaderDrawParametersFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, VK_API_VERSION_1_1, "VK_KHR_shader_draw_parameters"),
        // promoted to 1.2
        KUI_EXT(VkPhysicalDevice8BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VK_API_VERSION_1_2, "VK_KHR_8bit_storage"),
        KUI_EXT(VkPhysicalDeviceShaderAtomicInt64Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, VK_API_VERSION_1_2, "VK_KHR_shader_atomic_int64"),
        KUI_EXT(VkPhysicalDeviceShaderFloat16Int8Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VK_API_VERSION_1_2, "VK_KHR_shader_float16_int8"),
        KUI_EXT(VkPhysicalDeviceDescriptorIndexingFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VK_API_VERSION_1_2, "VK_EXT_descriptor_indexing"),
        KUI_EXT(VkPhysicalDeviceScalarBlockLayoutFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, VK_API_VERSION_1_2, "VK_EXT_scalar_block_layout"),
        KUI_EXT(VkPhysicalDeviceImagelessFramebufferFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, VK_API_VERSION_1_2, "VK_KHR_imageless_framebuffer"),
        KUI_EXT(VkPhysicalDeviceUniformBufferStandardLayoutFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, VK_API_VERSION_1_2, "VK_KHR_uniform_buffer_standard_layout"),
        KUI_EXT(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, VK_API_VERSION_1_2, "VK_KHR_shader_subgroup_extended_types"),
        KUI_EXT(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, VK_API_VERSION_1_2, "VK_KHR_separate_depth_stencil_layouts"),
        KUI_EXT(VkPhysicalDeviceHostQueryResetFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, VK_API_VERSION_1_2, "VK_EXT_host_query_reset"),
        KUI_EXT(VkPhysicalDeviceTimelineSemaphoreFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, VK_API_VERSION_1_2, "VK_KHR_timeline_semaphore"),
        KUI_EXT(VkPhysicalDeviceBufferDeviceAddressFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VK_API_VERSION_1_2, "VK_KHR_buffer_device_address"),
        KUI_EXT(VkPhysicalDeviceVulkanMemoryModelFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, VK_API_VERSION_1_2, "VK_KHR_vulkan_memory_model"),
        // promoted to 1.3
        KUI_EXT(VkPhysicalDeviceImageRobustnessFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, VK_API_VERSION_1_3, "VK_EXT_image_robustness"),
        KUI_EXT(VkPhysicalDeviceInlineUniformBlockFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, VK_API_VERSION_1_3, "VK_EXT_inline_uniform_block"),
        KUI_EXT(VkPhysicalDevicePipelineCreationCacheControlFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES, VK_API_VERSION_1_3, "VK_EXT_pipeline_creation_cache_control"),
        KUI_EXT(VkPhysicalDevicePrivateDataFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, VK_API_VERSION_1_3, "VK_EXT_private_data"),
        KUI_EXT(VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, VK_API_VERSION_1_3, "VK_EXT_shader_demote_to_helper_invocation"),
        KUI_EXT(VkPhysicalDeviceShaderTerminateInvocationFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES, VK_API_VERSION_1_3, "VK_KHR_shader_terminate_invocation"),
        KUI_EXT(VkPhysicalDeviceSubgroupSizeControlFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, VK_API_VERSION_1_3, "VK_EXT_subgroup_size_control"),
        KUI_EXT(VkPhysicalDeviceSynchronization2Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VK_API_VERSION_1_3, "VK_KHR_synchronization2"),
        KUI_EXT(VkPhysicalDeviceTextureCompressionASTCHDRFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES, VK_API_VERSION_1_3, "VK_EXT_texture_compression_astc_hdr"),
        KUI_EXT(VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES, VK_API_VERSION_1_3, "VK_KHR_zero_initialize_workgroup_memory"),
        KUI_EXT(VkPhysicalDeviceDynamicRenderingFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, VK_API_VERSION_1_3, "VK_KHR_dynamic_rendering"),
        KUI_EXT(VkPhysicalDeviceShaderIntegerDotProductFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, VK_API_VERSION_1_3, "VK_KHR_shader_integer_dot_product"),
        KUI_EXT(VkPhysicalDeviceMaintenance4Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VK_API_VERSION_1_3, "VK_KHR_maintenance4"),
        // promoted to 1.4 (Kiln maps none of their fields into Vulkan14Features: they are placed as themselves)
        KUI_EXT(VkPhysicalDeviceGlobalPriorityQueryFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GLOBAL_PRIORITY_QUERY_FEATURES, VK_API_VERSION_1_4, "VK_KHR_global_priority"),
        KUI_EXT(VkPhysicalDeviceShaderSubgroupRotateFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_ROTATE_FEATURES, VK_API_VERSION_1_4, "VK_KHR_shader_subgroup_rotate"),
        KUI_EXT(VkPhysicalDeviceShaderFloatControls2Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT_CONTROLS_2_FEATURES, VK_API_VERSION_1_4, "VK_KHR_shader_float_controls2"),
        KUI_EXT(VkPhysicalDeviceShaderExpectAssumeFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EXPECT_ASSUME_FEATURES, VK_API_VERSION_1_4, "VK_KHR_shader_expect_assume"),
        KUI_EXT(VkPhysicalDeviceLineRasterizationFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES, VK_API_VERSION_1_4, "VK_KHR_line_rasterization"),
        KUI_EXT(VkPhysicalDeviceVertexAttributeDivisorFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES, VK_API_VERSION_1_4, "VK_KHR_vertex_attribute_divisor"),
        KUI_EXT(VkPhysicalDeviceIndexTypeUint8Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES, VK_API_VERSION_1_4, "VK_KHR_index_type_uint8"),
        KUI_EXT(VkPhysicalDeviceDynamicRenderingLocalReadFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES, VK_API_VERSION_1_4, "VK_KHR_dynamic_rendering_local_read"),
        KUI_EXT(VkPhysicalDeviceMaintenance5Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES, VK_API_VERSION_1_4, "VK_KHR_maintenance5"),
        KUI_EXT(VkPhysicalDeviceMaintenance6Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES, VK_API_VERSION_1_4, "VK_KHR_maintenance6"),
        KUI_EXT(VkPhysicalDevicePipelineProtectedAccessFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROTECTED_ACCESS_FEATURES, VK_API_VERSION_1_4, "VK_EXT_pipeline_protected_access"),
        KUI_EXT(VkPhysicalDevicePipelineRobustnessFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES, VK_API_VERSION_1_4, "VK_EXT_pipeline_robustness"),
        KUI_EXT(VkPhysicalDeviceHostImageCopyFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES, VK_API_VERSION_1_4, "VK_EXT_host_image_copy"),
        // extension structs (never promoted) the upscalers ask for, and the ones the game's own chain holds
        KUI_EXT(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, 0, "VK_EXT_mutable_descriptor_type"),
        KUI_EXT(VkPhysicalDeviceCooperativeMatrixFeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, 0, "VK_KHR_cooperative_matrix"),
        KUI_EXT(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR, 0, "VK_KHR_compute_shader_derivatives"),
        KUI_EXT(VkPhysicalDeviceShaderFloat8FeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT, 0, "VK_EXT_shader_float8"),
        KUI_EXT(VkPhysicalDeviceShaderBfloat16FeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_BFLOAT16_FEATURES_KHR, 0, "VK_KHR_shader_bfloat16"),
        KUI_EXT(VkPhysicalDeviceAntiLagFeaturesAMD, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD, 0, "VK_AMD_anti_lag"),
        KUI_EXT(VkPhysicalDevicePresentIdFeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, 0, "VK_KHR_present_id"),
        KUI_EXT(VkPhysicalDevicePresentWaitFeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, 0, "VK_KHR_present_wait"),
        KUI_EXT(VkPhysicalDevicePresentId2FeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, 0, "VK_KHR_present_id2"),
        KUI_EXT(VkPhysicalDevicePresentWait2FeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR, 0, "VK_KHR_present_wait2"),
        KUI_EXT(VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR, 0, "VK_KHR_swapchain_maintenance1"),
        KUI_EXT(VkPhysicalDeviceRobustness2FeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR, 0, "VK_KHR_robustness2"),
        KUI_EXT(VkPhysicalDeviceMemoryPriorityFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT, 0, "VK_EXT_memory_priority"),
        KUI_EXT(VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT, 0, "VK_EXT_pageable_device_local_memory"),
        KUI_EXT(VkPhysicalDeviceMeshShaderFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, 0, "VK_EXT_mesh_shader"),
        KUI_EXT(VkPhysicalDeviceAccelerationStructureFeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, 0, "VK_KHR_acceleration_structure"),
        KUI_EXT(VkPhysicalDeviceRayQueryFeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, 0, "VK_KHR_ray_query"),
        KUI_EXT(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT, 0, "VK_EXT_shader_atomic_float"),
    };
    return t;
}
#undef KUI_CORE
#undef KUI_EXT

inline const StructInfo* find_struct(VkStructureType t) {
    for (const auto& s : structs())
        if (s.type == t) return &s;
    return nullptr;
}

// One feature that is in a core VulkanXYFeatures struct and in the extension struct promoted into it, same name.
struct Promoted {
    VkStructureType core;
    uint32_t core_offset;
    VkStructureType ext;  // 0: core only (no extension struct holds it)
    uint32_t ext_offset;
    const char* name;
};
#define KUI_P(C, CS, E, ES, F) {CS, static_cast<uint32_t>(offsetof(C, F)), ES, static_cast<uint32_t>(offsetof(E, F)), #F}
#define KUI_C(C, CS, F) {CS, static_cast<uint32_t>(offsetof(C, F)), static_cast<VkStructureType>(0), 0, #F}
inline const std::vector<Promoted>& promoted() {
    using V11 = VkPhysicalDeviceVulkan11Features;
    using V12 = VkPhysicalDeviceVulkan12Features;
    using V13 = VkPhysicalDeviceVulkan13Features;
    constexpr auto S11 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    constexpr auto S12 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    constexpr auto S13 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    static const std::vector<Promoted> t = {
        KUI_P(V11, S11, VkPhysicalDevice16BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, storageBuffer16BitAccess),
        KUI_P(V11, S11, VkPhysicalDevice16BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, uniformAndStorageBuffer16BitAccess),
        KUI_P(V11, S11, VkPhysicalDevice16BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, storagePushConstant16),
        KUI_P(V11, S11, VkPhysicalDevice16BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, storageInputOutput16),
        KUI_P(V11, S11, VkPhysicalDeviceMultiviewFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, multiview),
        KUI_P(V11, S11, VkPhysicalDeviceMultiviewFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, multiviewGeometryShader),
        KUI_P(V11, S11, VkPhysicalDeviceMultiviewFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, multiviewTessellationShader),
        KUI_P(V11, S11, VkPhysicalDeviceVariablePointersFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, variablePointersStorageBuffer),
        KUI_P(V11, S11, VkPhysicalDeviceVariablePointersFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, variablePointers),
        KUI_P(V11, S11, VkPhysicalDeviceProtectedMemoryFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, protectedMemory),
        KUI_P(V11, S11, VkPhysicalDeviceSamplerYcbcrConversionFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, samplerYcbcrConversion),
        KUI_P(V11, S11, VkPhysicalDeviceShaderDrawParametersFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, shaderDrawParameters),
        KUI_C(V12, S12, samplerMirrorClampToEdge),
        KUI_C(V12, S12, drawIndirectCount),
        KUI_P(V12, S12, VkPhysicalDevice8BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, storageBuffer8BitAccess),
        KUI_P(V12, S12, VkPhysicalDevice8BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, uniformAndStorageBuffer8BitAccess),
        KUI_P(V12, S12, VkPhysicalDevice8BitStorageFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, storagePushConstant8),
        KUI_P(V12, S12, VkPhysicalDeviceShaderAtomicInt64Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, shaderBufferInt64Atomics),
        KUI_P(V12, S12, VkPhysicalDeviceShaderAtomicInt64Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, shaderSharedInt64Atomics),
        KUI_P(V12, S12, VkPhysicalDeviceShaderFloat16Int8Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, shaderFloat16),
        KUI_P(V12, S12, VkPhysicalDeviceShaderFloat16Int8Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, shaderInt8),
        KUI_C(V12, S12, descriptorIndexing),
#define KUI_DI(F) KUI_P(V12, S12, VkPhysicalDeviceDescriptorIndexingFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, F)
        KUI_DI(shaderInputAttachmentArrayDynamicIndexing), KUI_DI(shaderUniformTexelBufferArrayDynamicIndexing),
        KUI_DI(shaderStorageTexelBufferArrayDynamicIndexing), KUI_DI(shaderUniformBufferArrayNonUniformIndexing),
        KUI_DI(shaderSampledImageArrayNonUniformIndexing), KUI_DI(shaderStorageBufferArrayNonUniformIndexing),
        KUI_DI(shaderStorageImageArrayNonUniformIndexing), KUI_DI(shaderInputAttachmentArrayNonUniformIndexing),
        KUI_DI(shaderUniformTexelBufferArrayNonUniformIndexing), KUI_DI(shaderStorageTexelBufferArrayNonUniformIndexing),
        KUI_DI(descriptorBindingUniformBufferUpdateAfterBind), KUI_DI(descriptorBindingSampledImageUpdateAfterBind),
        KUI_DI(descriptorBindingStorageImageUpdateAfterBind), KUI_DI(descriptorBindingStorageBufferUpdateAfterBind),
        KUI_DI(descriptorBindingUniformTexelBufferUpdateAfterBind), KUI_DI(descriptorBindingStorageTexelBufferUpdateAfterBind),
        KUI_DI(descriptorBindingUpdateUnusedWhilePending), KUI_DI(descriptorBindingPartiallyBound),
        KUI_DI(descriptorBindingVariableDescriptorCount), KUI_DI(runtimeDescriptorArray),
#undef KUI_DI
        KUI_C(V12, S12, samplerFilterMinmax),
        KUI_P(V12, S12, VkPhysicalDeviceScalarBlockLayoutFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, scalarBlockLayout),
        KUI_P(V12, S12, VkPhysicalDeviceImagelessFramebufferFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, imagelessFramebuffer),
        KUI_P(V12, S12, VkPhysicalDeviceUniformBufferStandardLayoutFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, uniformBufferStandardLayout),
        KUI_P(V12, S12, VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, shaderSubgroupExtendedTypes),
        KUI_P(V12, S12, VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, separateDepthStencilLayouts),
        KUI_P(V12, S12, VkPhysicalDeviceHostQueryResetFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, hostQueryReset),
        KUI_P(V12, S12, VkPhysicalDeviceTimelineSemaphoreFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, timelineSemaphore),
        KUI_P(V12, S12, VkPhysicalDeviceBufferDeviceAddressFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, bufferDeviceAddress),
        KUI_P(V12, S12, VkPhysicalDeviceBufferDeviceAddressFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, bufferDeviceAddressCaptureReplay),
        KUI_P(V12, S12, VkPhysicalDeviceBufferDeviceAddressFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, bufferDeviceAddressMultiDevice),
        KUI_P(V12, S12, VkPhysicalDeviceVulkanMemoryModelFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, vulkanMemoryModel),
        KUI_P(V12, S12, VkPhysicalDeviceVulkanMemoryModelFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, vulkanMemoryModelDeviceScope),
        KUI_P(V12, S12, VkPhysicalDeviceVulkanMemoryModelFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, vulkanMemoryModelAvailabilityVisibilityChains),
        KUI_C(V12, S12, shaderOutputViewportIndex),
        KUI_C(V12, S12, shaderOutputLayer),
        KUI_C(V12, S12, subgroupBroadcastDynamicId),
        KUI_P(V13, S13, VkPhysicalDeviceImageRobustnessFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, robustImageAccess),
        KUI_P(V13, S13, VkPhysicalDeviceInlineUniformBlockFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, inlineUniformBlock),
        KUI_P(V13, S13, VkPhysicalDeviceInlineUniformBlockFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, descriptorBindingInlineUniformBlockUpdateAfterBind),
        KUI_P(V13, S13, VkPhysicalDevicePipelineCreationCacheControlFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES, pipelineCreationCacheControl),
        KUI_P(V13, S13, VkPhysicalDevicePrivateDataFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, privateData),
        KUI_P(V13, S13, VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, shaderDemoteToHelperInvocation),
        KUI_P(V13, S13, VkPhysicalDeviceShaderTerminateInvocationFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES, shaderTerminateInvocation),
        KUI_P(V13, S13, VkPhysicalDeviceSubgroupSizeControlFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, subgroupSizeControl),
        KUI_P(V13, S13, VkPhysicalDeviceSubgroupSizeControlFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, computeFullSubgroups),
        KUI_P(V13, S13, VkPhysicalDeviceSynchronization2Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, synchronization2),
        KUI_P(V13, S13, VkPhysicalDeviceTextureCompressionASTCHDRFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES, textureCompressionASTC_HDR),
        KUI_P(V13, S13, VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES, shaderZeroInitializeWorkgroupMemory),
        KUI_P(V13, S13, VkPhysicalDeviceDynamicRenderingFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, dynamicRendering),
        KUI_P(V13, S13, VkPhysicalDeviceShaderIntegerDotProductFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, shaderIntegerDotProduct),
        KUI_P(V13, S13, VkPhysicalDeviceMaintenance4Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, maintenance4),
    };
    return t;
}
#undef KUI_P
#undef KUI_C

// The other name of a feature: the extension struct's for a core field, the core struct's for a promoted field.
inline const Promoted* find_promoted(VkStructureType type, uint32_t offset) {
    for (const auto& p : promoted())
        if ((p.core == type && p.core_offset == offset) || (p.ext && p.ext == type && p.ext_offset == offset)) return &p;
    return nullptr;
}

inline constexpr uint32_t kHeader = static_cast<uint32_t>(sizeof(VkBaseOutStructure));  // sType + pNext

inline VkStructureType type_of(const uint8_t* bytes, size_t size) {
    VkStructureType t = static_cast<VkStructureType>(0);
    if (size >= sizeof t) std::memcpy(&t, bytes, sizeof t);
    return t;
}
inline bool bool_at(const uint8_t* bytes, size_t size, uint32_t offset) {
    VkBool32 v = VK_FALSE;
    if (offset + sizeof v <= size) std::memcpy(&v, bytes + offset, sizeof v);
    return v == VK_TRUE;
}
inline void set_at(uint8_t* bytes, size_t size, uint32_t offset) {
    const VkBool32 v = VK_TRUE;
    if (offset + sizeof v <= size) std::memcpy(bytes + offset, &v, sizeof v);
}
// "VkPhysicalDeviceVulkan12Features.shaderInt8", for the log and the report.
inline std::string field_name(VkStructureType type, uint32_t offset) {
    const StructInfo* s = find_struct(type);
    std::string n = s ? s->name : "sType " + std::to_string(static_cast<uint32_t>(type));
    if (const Promoted* p = find_promoted(type, offset)) return n + "." + p->name;
    if (type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 && offset >= kHeader) {
        static const char* f10[] = {"robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader",
            "tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect", "drawIndirectFirstInstance",
            "depthClamp", "depthBiasClamp", "fillModeNonSolid", "depthBounds", "wideLines", "largePoints", "alphaToOne", "multiViewport",
            "samplerAnisotropy", "textureCompressionETC2", "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise",
            "pipelineStatisticsQuery", "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics",
            "shaderTessellationAndGeometryPointSize", "shaderImageGatherExtended", "shaderStorageImageExtendedFormats",
            "shaderStorageImageMultisample", "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat",
            "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing", "shaderStorageBufferArrayDynamicIndexing",
            "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance", "shaderCullDistance", "shaderFloat64", "shaderInt64",
            "shaderInt16", "shaderResourceResidency", "shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer",
            "sparseResidencyImage2D", "sparseResidencyImage3D", "sparseResidency2Samples", "sparseResidency4Samples",
            "sparseResidency8Samples", "sparseResidency16Samples", "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries"};
        static_assert(sizeof f10 / sizeof f10[0] == sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32));
        const uint32_t i = (offset - kHeader) / sizeof(VkBool32);
        if (i < sizeof f10 / sizeof f10[0]) return std::string("VkPhysicalDeviceFeatures.") + f10[i];
    }
    return n + " +" + std::to_string(offset);
}

// ---- the report -----------------------------------------------------------------------------------------------------
struct Report {
    bool changed = false;                         // the create info passed on is Kiln's copy
    std::vector<std::string> added_extensions;    // enabled by Kiln, not by the game
    std::vector<std::string> added_features;      // "VkPhysicalDeviceVulkan12Features.shaderInt8"
    std::vector<std::string> missing_required;    // what a required need asks for that is not there: nothing was changed
    std::vector<std::string> skipped_optional;    // optional needs the device does not have
    std::vector<std::string> notes;               // English, for kiln.log
    uint32_t api_version = 0;                     // instance: the apiVersion as created
    std::vector<std::string> extensions;          // enabled in the end (the game's and Kiln's)
    std::vector<upscale::FeatureNeed> features;   // device: enabled in the end, one struct per type, pNext zeroed, the
                                                  // 1.0 features as a VkPhysicalDeviceFeatures2 (upscale::VkContext.features)
    bool ok() const { return missing_required.empty(); }
};

// What the physical device offers, for the needs' struct types (query_support, or made up by a test).
struct DeviceSupport {
    uint32_t api_version = 0;                    // min(the instance's apiVersion, the device's): what the device runs as
    std::vector<std::string> extensions;         // vkEnumerateDeviceExtensionProperties
    std::vector<upscale::FeatureNeed> features;  // one struct per queried type, as vkGetPhysicalDeviceFeatures2 filled it

    bool has_extension(const std::string& e) const { return std::find(extensions.begin(), extensions.end(), e) != extensions.end(); }
    // Whether the device has the feature, read from whichever of its two names was queried.
    bool has(VkStructureType type, uint32_t offset) const {
        auto look = [&](VkStructureType t, uint32_t o, bool* found) {
            for (const auto& f : features)
                if (type_of(f.bytes.data(), f.bytes.size()) == t) {
                    *found = true;
                    return bool_at(f.bytes.data(), f.bytes.size(), o);
                }
            *found = false;
            return false;
        };
        bool found = false;
        if (look(type, offset, &found)) return true;
        if (const Promoted* p = find_promoted(type, offset)) {
            bool f2 = false;
            if (look(p->core, p->core_offset, &f2)) return true;
            if (p->ext && look(p->ext, p->ext_offset, &f2)) return true;
        }
        return false;
    }
};

// The feature struct types query_support asks the device about for these needs: each need's own type, and the other
// name of each of its fields where that name can be asked about on this device.
inline bool queryable(const StructInfo& s, uint32_t api, const std::vector<std::string>& exts) {
    if (s.core) return api >= s.core;
    if (s.promoted && api >= s.promoted) return true;
    return s.ext && std::find(exts.begin(), exts.end(), s.ext) != exts.end();
}

// Asks the device (through the unhooked gipa) what it has. Works on any thread; no device needed.
inline DeviceSupport query_support(const upscale::PhysicalDevice& gpu, const upscale::DeviceNeeds& needs) {
    DeviceSupport s;
    if (!gpu.gipa || !gpu.instance || !gpu.physical) return s;
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gpu.gipa(gpu.instance, "vkGetPhysicalDeviceProperties"));
    auto enum_ext = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gpu.gipa(gpu.instance, "vkEnumerateDeviceExtensionProperties"));
    auto feats2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gpu.gipa(gpu.instance, "vkGetPhysicalDeviceFeatures2"));
    if (!feats2) feats2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gpu.gipa(gpu.instance, "vkGetPhysicalDeviceFeatures2KHR"));
    VkPhysicalDeviceProperties p = gpu.props;
    if (!p.apiVersion && props) props(gpu.physical, &p);
    const uint32_t inst = gpu.instance_api ? gpu.instance_api : VK_API_VERSION_1_0;
    s.api_version = (std::min)(inst, p.apiVersion);
    uint32_t n = 0;
    if (enum_ext && enum_ext(gpu.physical, nullptr, &n, nullptr) == VK_SUCCESS && n) {
        std::vector<VkExtensionProperties> e(n);
        if (enum_ext(gpu.physical, nullptr, &n, e.data()) >= VK_SUCCESS)
            for (uint32_t i = 0; i < n; ++i) s.extensions.emplace_back(e[i].extensionName);
    }
    if (!feats2) return s;
    std::vector<VkStructureType> types;
    auto add = [&](VkStructureType t) {
        const StructInfo* si = find_struct(t);
        if (!si || t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 || !queryable(*si, s.api_version, s.extensions)) return;
        if (std::find(types.begin(), types.end(), t) == types.end()) types.push_back(t);
    };
    for (const auto& f : needs.features) {
        const VkStructureType t = type_of(f.bytes.data(), f.bytes.size());
        add(t);
        const StructInfo* si = find_struct(t);
        if (!si) continue;
        for (uint32_t o = kHeader; o + sizeof(VkBool32) <= std::min<size_t>(si->size, f.bytes.size()); o += sizeof(VkBool32)) {
            if (!bool_at(f.bytes.data(), f.bytes.size(), o)) continue;
            if (const Promoted* pr = find_promoted(t, o)) {
                add(pr->core);
                if (pr->ext) add(pr->ext);
            }
        }
    }
    std::vector<std::vector<uint8_t>> blobs;
    for (VkStructureType t : types) {
        std::vector<uint8_t> b(find_struct(t)->size, 0);
        std::memcpy(b.data(), &t, sizeof t);
        blobs.push_back(std::move(b));
    }
    VkPhysicalDeviceFeatures2 root{};
    root.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    void* next = nullptr;
    for (size_t i = blobs.size(); i-- > 0;) {
        reinterpret_cast<VkBaseOutStructure*>(blobs[i].data())->pNext = static_cast<VkBaseOutStructure*>(next);
        next = blobs[i].data();
    }
    root.pNext = next;
    feats2(gpu.physical, &root);
    upscale::FeatureNeed r;
    root.pNext = nullptr;
    r.bytes.assign(reinterpret_cast<uint8_t*>(&root), reinterpret_cast<uint8_t*>(&root) + sizeof root);
    s.features.push_back(std::move(r));
    for (auto& b : blobs) {
        reinterpret_cast<VkBaseOutStructure*>(b.data())->pNext = nullptr;
        upscale::FeatureNeed f;
        f.bytes = std::move(b);
        s.features.push_back(std::move(f));
    }
    return s;
}

// The feature structs a create info enables (for upscale::VkContext.features): every struct of a type Kiln knows, pNext
// zeroed, the 1.0 features as a VkPhysicalDeviceFeatures2 (from pEnabledFeatures when the chain has none).
inline std::vector<upscale::FeatureNeed> enabled_features(const VkDeviceCreateInfo& ci) {
    std::vector<upscale::FeatureNeed> out;
    bool have10 = false;
    for (auto* p = static_cast<const VkBaseInStructure*>(ci.pNext); p; p = p->pNext) {
        const StructInfo* s = find_struct(p->sType);
        if (!s) continue;
        upscale::FeatureNeed f;
        f.bytes.assign(reinterpret_cast<const uint8_t*>(p), reinterpret_cast<const uint8_t*>(p) + s->size);
        reinterpret_cast<VkBaseOutStructure*>(f.bytes.data())->pNext = nullptr;
        have10 |= p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        out.push_back(std::move(f));
    }
    if (!have10) {
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        if (ci.pEnabledFeatures) f2.features = *ci.pEnabledFeatures;
        upscale::FeatureNeed f;
        f.bytes.assign(reinterpret_cast<uint8_t*>(&f2), reinterpret_cast<uint8_t*>(&f2) + sizeof f2);
        out.insert(out.begin(), std::move(f));
    }
    return out;
}

// ---- the device -----------------------------------------------------------------------------------------------------
// One merge at the game's vkCreateDevice. The create info it returns, and everything it points to, belong to this
// object: keep it alive until the real vkCreateDevice has returned. Not copyable.
class DeviceMerge {
  public:
    DeviceMerge() = default;
    DeviceMerge(const DeviceMerge&) = delete;
    DeviceMerge& operator=(const DeviceMerge&) = delete;

    // Queries the device (query_support), then merges.
    const VkDeviceCreateInfo* merge(const VkDeviceCreateInfo* game, const upscale::DeviceNeeds& needs, const upscale::PhysicalDevice& gpu,
                                    Report* report) {
        return merge(game, needs, query_support(gpu, needs), report);
    }

    // `game` itself when nothing is to change or a required need is missing; else Kiln's copy.
    const VkDeviceCreateInfo* merge(const VkDeviceCreateInfo* game, const upscale::DeviceNeeds& needs, const DeviceSupport& support,
                                    Report* report) {
        Report local;
        Report& r = report ? *report : local;
        r = Report{};
        blobs_.clear();
        names_.clear();
        exts_.clear();
        if (!game) return game;
        auto finish_unchanged = [&]() {
            r.changed = false;
            for (uint32_t i = 0; i < game->enabledExtensionCount; ++i) r.extensions.emplace_back(game->ppEnabledExtensionNames[i]);
            r.features = enabled_features(*game);
            return game;
        };

        // the game's chain, as it is
        std::vector<const VkBaseInStructure*> chain;
        for (auto* p = static_cast<const VkBaseInStructure*>(game->pNext); p; p = p->pNext) {
            chain.push_back(p);
            if (chain.size() > 4096) {
                r.missing_required.push_back("the game's feature chain does not end");
                return finish_unchanged();
            }
        }
        auto in_chain = [&](VkStructureType t) -> int {
            for (size_t i = 0; i < chain.size(); ++i)
                if (chain[i]->sType == t) return static_cast<int>(i);
            return -1;
        };
        std::vector<std::string> enabled;
        for (uint32_t i = 0; i < game->enabledExtensionCount; ++i) enabled.emplace_back(game->ppEnabledExtensionNames[i]);
        auto is_enabled = [&](const std::string& e) { return std::find(enabled.begin(), enabled.end(), e) != enabled.end(); };
        std::vector<std::string> add_ext;  // extensions Kiln adds
        auto want_ext = [&](const std::string& e) {
            if (!is_enabled(e) && std::find(add_ext.begin(), add_ext.end(), e) == add_ext.end()) add_ext.push_back(e);
        };

        // 1. extensions
        for (const auto& e : needs.extensions) {
            if (e.name.empty() || is_enabled(e.name)) continue;
            if (support.has_extension(e.name)) want_ext(e.name);
            else if (e.required) r.missing_required.push_back(e.name);
            else r.skipped_optional.push_back(e.name);
        }

        // 2. features: every VK_TRUE of every need, checked against the device
        struct Want {
            VkStructureType type;
            uint32_t offset;
            bool required;
        };
        std::vector<Want> wants;
        for (const auto& f : needs.features) {
            const VkStructureType t = type_of(f.bytes.data(), f.bytes.size());
            const StructInfo* si = find_struct(t);
            if (!si || f.bytes.size() < kHeader) {
                const std::string what = "a Vulkan feature struct Kiln does not know (sType " + std::to_string(static_cast<uint32_t>(t)) + ")";
                (f.required ? r.missing_required : r.skipped_optional).push_back(what);
                continue;
            }
            std::vector<Want> mine;
            std::vector<std::string> lacking;
            const size_t end = std::min<size_t>(si->size, f.bytes.size());
            for (uint32_t o = kHeader; o + sizeof(VkBool32) <= end; o += sizeof(VkBool32)) {
                if (!bool_at(f.bytes.data(), f.bytes.size(), o)) continue;
                if (support.has(t, o)) mine.push_back({t, o, f.required});
                else lacking.push_back(field_name(t, o));
            }
            if (f.required && !lacking.empty()) {
                r.missing_required.insert(r.missing_required.end(), lacking.begin(), lacking.end());
                continue;  // a required struct is all or nothing
            }
            r.skipped_optional.insert(r.skipped_optional.end(), lacking.begin(), lacking.end());
            wants.insert(wants.end(), mine.begin(), mine.end());
        }

        // 3. where each goes (a plan first: nothing is copied until every required one has a place)
        enum class Where { chain, added, enabled10 };
        struct Place {
            Where where;
            int index;            // chain index, or index into `added`
            uint32_t offset;
            std::string name;
        };
        std::vector<Place> plan;
        std::vector<VkStructureType> added;  // new structs, in order
        bool own10 = false;                  // Kiln's own copy of pEnabledFeatures (the game used it, or neither)
        auto added_index = [&](VkStructureType t) -> int {
            for (size_t i = 0; i < added.size(); ++i)
                if (added[i] == t) return static_cast<int>(i);
            return -1;
        };
        auto conflicts_core = [&](uint32_t version) {  // the chain (or Kiln's additions) has a struct promoted into it
            for (auto* p : chain)
                if (const StructInfo* s = find_struct(p->sType); s && s->promoted == version) return true;
            for (auto t : added)
                if (find_struct(t)->promoted == version) return true;
            return false;
        };
        auto already_on = [&](VkStructureType t, uint32_t o) {
            const int i = in_chain(t);
            if (i < 0) return false;
            const StructInfo* s = find_struct(t);
            return s && bool_at(reinterpret_cast<const uint8_t*>(chain[static_cast<size_t>(i)]), s->size, o);
        };
        for (const Want& w : wants) {
            const std::string name = field_name(w.type, w.offset);
            if (w.type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                const int i = in_chain(w.type);
                if (i >= 0) {
                    if (!already_on(w.type, w.offset)) plan.push_back({Where::chain, i, w.offset, name});
                } else {
                    const uint32_t o = w.offset - kHeader;  // into VkPhysicalDeviceFeatures
                    if (!(game->pEnabledFeatures && bool_at(reinterpret_cast<const uint8_t*>(game->pEnabledFeatures), sizeof(VkPhysicalDeviceFeatures), o))) {
                        own10 = true;
                        plan.push_back({Where::enabled10, 0, o, name});
                    }
                }
                continue;
            }
            const Promoted* pr = find_promoted(w.type, w.offset);
            // same type in the chain; else the other name in the chain
            if (in_chain(w.type) >= 0) {
                if (!already_on(w.type, w.offset)) plan.push_back({Where::chain, in_chain(w.type), w.offset, name});
                continue;
            }
            if (pr) {
                const bool is_core = pr->core == w.type;
                const VkStructureType other = is_core ? pr->ext : pr->core;
                const uint32_t other_offset = is_core ? pr->ext_offset : pr->core_offset;
                if (other && in_chain(other) >= 0) {
                    if (!already_on(other, other_offset)) plan.push_back({Where::chain, in_chain(other), other_offset, name});
                    continue;
                }
            }
            // a new struct: prefer the extension struct (valid on the game's 1.2 instance and next to any core struct
            // it does not belong to)
            VkStructureType put = w.type;
            uint32_t put_offset = w.offset;
            const StructInfo* si = find_struct(w.type);
            if (si->core && pr && pr->ext) {
                put = pr->ext;
                put_offset = pr->ext_offset;
            }
            const StructInfo* pi = find_struct(put);
            bool ok = true;
            std::string why;
            if (pi->core) {
                // a core-only feature: a new VulkanXYFeatures struct, if the device runs that version and nothing in the
                // chain is promoted into it
                if (support.api_version < pi->core) ok = false, why = name + " (needs Vulkan 1." + std::to_string(VK_API_VERSION_MINOR(pi->core)) + ")";
                else if (conflicts_core(pi->core)) ok = false, why = name + " (Kiln cannot add " + pi->name + " next to the game's structs)";
            } else {
                // (an extension struct promoted into a core struct Kiln added would conflict: Kiln adds core structs only
                // for core-only fields, and never next to a struct promoted into them)
                if (pi->ext && !is_enabled(pi->ext)) {
                    if (support.has_extension(pi->ext)) want_ext(pi->ext);
                    else if (!(pi->promoted && support.api_version >= pi->promoted)) ok = false, why = name + " (" + pi->ext + " is missing)";
                }
            }
            if (!ok) {
                (w.required ? r.missing_required : r.skipped_optional).push_back(why);
                continue;
            }
            int a = added_index(put);
            if (a < 0) {
                added.push_back(put);
                a = static_cast<int>(added.size()) - 1;
            }
            plan.push_back({Where::added, a, put_offset, name});
        }
        if (!r.ok()) {
            r.notes.push_back("the game's device is created as the game asked: a required need is missing");
            r.added_extensions.clear();
            return finish_unchanged();
        }
        if (plan.empty() && add_ext.empty()) return finish_unchanged();

        // 4. copy the start of the game's chain up to the last struct Kiln changes
        int last = -1;
        for (const Place& p : plan)
            if (p.where == Where::chain) last = (std::max)(last, p.index);
        for (int i = 0; i <= last; ++i) {
            const StructInfo* s = find_struct(chain[static_cast<size_t>(i)]->sType);
            if (!s) {
                // a struct of a type Kiln does not know the size of sits before one Kiln must change: no copy is safe
                bool required = false;
                for (const Want& w : wants) required |= w.required;
                const std::string what = "Kiln cannot copy the game's feature struct sType " +
                                         std::to_string(static_cast<uint32_t>(chain[static_cast<size_t>(i)]->sType));
                (required ? r.missing_required : r.notes).push_back(what);
                r.added_extensions.clear();
                return finish_unchanged();
            }
            blobs_.emplace_back(reinterpret_cast<const uint8_t*>(chain[static_cast<size_t>(i)]),
                                reinterpret_cast<const uint8_t*>(chain[static_cast<size_t>(i)]) + s->size);
        }
        const size_t first_added = blobs_.size();
        for (VkStructureType t : added) {
            std::vector<uint8_t> b(find_struct(t)->size, 0);
            std::memcpy(b.data(), &t, sizeof t);
            blobs_.push_back(std::move(b));
        }
        std::vector<uint8_t*> copies;
        for (auto& b : blobs_) copies.push_back(b.data());
        features10_ = game->pEnabledFeatures ? *game->pEnabledFeatures : VkPhysicalDeviceFeatures{};
        for (const Place& p : plan) {
            uint8_t* dst = p.where == Where::chain ? copies[static_cast<size_t>(p.index)]
                         : p.where == Where::added ? copies[first_added + static_cast<size_t>(p.index)]
                                                   : reinterpret_cast<uint8_t*>(&features10_);
            const size_t size = p.where == Where::enabled10 ? sizeof features10_
                              : find_struct(type_of(dst, kHeader))->size;
            set_at(dst, size, p.offset);
            r.added_features.push_back(p.name);
        }
        // link: Kiln's new structs, then the copies, then the rest of the game's chain where it is
        const void* rest = last + 1 < static_cast<int>(chain.size()) ? chain[static_cast<size_t>(last + 1)] : nullptr;
        std::vector<uint8_t*> order;
        for (size_t i = first_added; i < copies.size(); ++i) order.push_back(copies[i]);
        for (size_t i = 0; i < first_added; ++i) order.push_back(copies[i]);
        for (size_t i = 0; i < order.size(); ++i)
            reinterpret_cast<VkBaseOutStructure*>(order[i])->pNext =
                reinterpret_cast<VkBaseOutStructure*>(i + 1 < order.size() ? order[i + 1] : const_cast<void*>(rest));
        ci_ = *game;
        ci_.pNext = order.empty() ? game->pNext : order[0];
        if (own10) ci_.pEnabledFeatures = &features10_;
        names_ = enabled;
        for (const auto& e : add_ext) names_.push_back(e);
        for (const auto& n : names_) exts_.push_back(n.c_str());
        ci_.enabledExtensionCount = static_cast<uint32_t>(exts_.size());
        ci_.ppEnabledExtensionNames = exts_.data();
        r.changed = true;
        r.added_extensions = add_ext;
        r.extensions = names_;
        r.features = enabled_features(ci_);
        return &ci_;
    }

  private:
    std::vector<std::vector<uint8_t>> blobs_;
    std::vector<std::string> names_;
    std::vector<const char*> exts_;
    VkPhysicalDeviceFeatures features10_{};
    VkDeviceCreateInfo ci_{};
};

// ---- the instance ---------------------------------------------------------------------------------------------------
// The instance extensions the loader offers, and its version (through the unhooked gipa; no instance needed).
struct InstanceSupport {
    uint32_t loader_version = VK_API_VERSION_1_0;
    std::vector<std::string> extensions;
};
inline InstanceSupport query_instance_support(PFN_vkGetInstanceProcAddr gipa) {
    InstanceSupport s;
    if (!gipa) return s;
    if (auto ver = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"))) {
        uint32_t v = 0;
        if (ver(&v) == VK_SUCCESS && v) s.loader_version = v;
    }
    auto enum_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    uint32_t n = 0;
    if (enum_ext && enum_ext(nullptr, &n, nullptr) == VK_SUCCESS && n) {
        std::vector<VkExtensionProperties> e(n);
        if (enum_ext(nullptr, &n, e.data()) >= VK_SUCCESS)
            for (uint32_t i = 0; i < n; ++i) s.extensions.emplace_back(e[i].extensionName);
    }
    return s;
}

// One merge at the game's vkCreateInstance: extensions added once, the apiVersion raised to the needs' minimum (only
// when the game asked for less). Same lifetime rule as DeviceMerge.
class InstanceMerge {
  public:
    InstanceMerge() = default;
    InstanceMerge(const InstanceMerge&) = delete;
    InstanceMerge& operator=(const InstanceMerge&) = delete;

    const VkInstanceCreateInfo* merge(const VkInstanceCreateInfo* game, const upscale::InstanceNeeds& needs, PFN_vkGetInstanceProcAddr gipa,
                                      Report* report) {
        return merge(game, needs, query_instance_support(gipa), report);
    }

    const VkInstanceCreateInfo* merge(const VkInstanceCreateInfo* game, const upscale::InstanceNeeds& needs, const InstanceSupport& support,
                                      Report* report) {
        Report local;
        Report& r = report ? *report : local;
        r = Report{};
        names_.clear();
        exts_.clear();
        if (!game) return game;
        const uint32_t asked = game->pApplicationInfo && game->pApplicationInfo->apiVersion ? game->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
        std::vector<std::string> enabled;
        for (uint32_t i = 0; i < game->enabledExtensionCount; ++i) enabled.emplace_back(game->ppEnabledExtensionNames[i]);
        auto unchanged = [&]() {
            r.changed = false;
            r.api_version = asked;
            r.extensions = enabled;
            return game;
        };
        std::vector<std::string> add;
        for (const auto& e : needs.extensions) {
            if (e.name.empty() || std::find(enabled.begin(), enabled.end(), e.name) != enabled.end() ||
                std::find(add.begin(), add.end(), e.name) != add.end())
                continue;
            if (std::find(support.extensions.begin(), support.extensions.end(), e.name) != support.extensions.end()) add.push_back(e.name);
            else if (e.required) r.missing_required.push_back(e.name);
            else r.skipped_optional.push_back(e.name);
        }
        uint32_t api = asked;
        if (needs.min_api_version > asked) {
            if (support.loader_version >= needs.min_api_version) api = needs.min_api_version;
            else
                r.missing_required.push_back("Vulkan " + std::to_string(VK_API_VERSION_MAJOR(needs.min_api_version)) + "." +
                                             std::to_string(VK_API_VERSION_MINOR(needs.min_api_version)) + " (the loader offers " +
                                             std::to_string(VK_API_VERSION_MAJOR(support.loader_version)) + "." +
                                             std::to_string(VK_API_VERSION_MINOR(support.loader_version)) + ")");
        }
        if (!r.ok()) {
            r.notes.push_back("the game's instance is created as the game asked: a required need is missing");
            return unchanged();
        }
        if (add.empty() && api == asked) return unchanged();
        ci_ = *game;
        if (api != asked) {
            app_ = VkApplicationInfo{};
            app_.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            if (game->pApplicationInfo) app_ = *game->pApplicationInfo;
            app_.apiVersion = api;
            ci_.pApplicationInfo = &app_;
            r.notes.push_back("apiVersion raised from " + std::to_string(VK_API_VERSION_MAJOR(asked)) + "." + std::to_string(VK_API_VERSION_MINOR(asked)) +
                              " to " + std::to_string(VK_API_VERSION_MAJOR(api)) + "." + std::to_string(VK_API_VERSION_MINOR(api)));
        }
        names_ = enabled;
        for (const auto& e : add) names_.push_back(e);
        for (const auto& n : names_) exts_.push_back(n.c_str());
        ci_.enabledExtensionCount = static_cast<uint32_t>(exts_.size());
        ci_.ppEnabledExtensionNames = exts_.data();
        r.changed = true;
        r.api_version = api;
        r.added_extensions = add;
        r.extensions = names_;
        return &ci_;
    }

  private:
    std::vector<std::string> names_;
    std::vector<const char*> exts_;
    VkApplicationInfo app_{};
    VkInstanceCreateInfo ci_{};
};

}  // namespace kui::device_merge

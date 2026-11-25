// SPDX-License-Identifier: MIT
// kiln.ui upscaler swap: the output check on the GPU (0.2.0-dev.31.12; plain logic and thresholds in
// upscale_probe_logic.h). A small compute pass (shaders/output_probe.comp) recorded once after a set-up into the game's
// command buffer, right after the upscaler's work: it reads the game's colour through the game's own view
// (SHADER_READ_ONLY_OPTIMAL) and the upscaler's output through the game's own view as a storage image (GENERAL), leaves
// both in those layouts, and writes 8 KB into a host-visible buffer of Kiln's that is read some frames later. Costs one
// 256-thread dispatch per check; nothing per frame otherwise.
//
// Needs on the device: nothing beyond Vulkan 1.0 (an rgba16f storage image read is core; the output is the game's
// R16G16B16A16_SFLOAT, which the upscalers write as a storage image).
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

#include "upscale_backend.h"
#include "upscale_probe_logic.h"

namespace kui::upscale {

class OutputProbe {
  public:
    // The device's functions (from vk.gdpa) and its memory types. False: a function is missing.
    bool init(const VkContext& vk, std::string* detail);
    // Whether this frame's images can be checked (the output's format and the sizes): null when they can, else why not.
    static const char* unsupported(const Frame& f);
    // Records the check of `f` into `cb` (after the upscaler's work in it) with `tag` (not 0). False: nothing recorded.
    bool record(VkCommandBuffer cb, const Frame& f, uint32_t tag, std::string* detail);
    // The result of the check with `tag`, once the GPU wrote all of it (false: not yet).
    bool read(uint32_t tag, probe::Result* out) const;
    // Every Vulkan object goes (the device is idle).
    void destroy();

  private:
    bool objects(std::string* detail);

    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem_{};
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkShaderModule shader_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kSets = 2;  // in turn: a set is written again only after the check before it was read
    VkDescriptorSet sets_[kSets]{};
    uint32_t next_set_ = 0;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    bool ready_ = false;  // the pipeline, the buffer and the sets are made

#define KUI_PROBE_FN(n) PFN_##n n = nullptr;
    KUI_PROBE_FN(vkCreateBuffer) KUI_PROBE_FN(vkDestroyBuffer) KUI_PROBE_FN(vkGetBufferMemoryRequirements) KUI_PROBE_FN(vkAllocateMemory)
    KUI_PROBE_FN(vkFreeMemory) KUI_PROBE_FN(vkBindBufferMemory) KUI_PROBE_FN(vkMapMemory) KUI_PROBE_FN(vkUnmapMemory)
    KUI_PROBE_FN(vkCreateSampler) KUI_PROBE_FN(vkDestroySampler) KUI_PROBE_FN(vkCreateDescriptorSetLayout)
    KUI_PROBE_FN(vkDestroyDescriptorSetLayout) KUI_PROBE_FN(vkCreatePipelineLayout) KUI_PROBE_FN(vkDestroyPipelineLayout)
    KUI_PROBE_FN(vkCreateShaderModule) KUI_PROBE_FN(vkDestroyShaderModule) KUI_PROBE_FN(vkCreateComputePipelines)
    KUI_PROBE_FN(vkDestroyPipeline) KUI_PROBE_FN(vkCreateDescriptorPool) KUI_PROBE_FN(vkDestroyDescriptorPool)
    KUI_PROBE_FN(vkAllocateDescriptorSets) KUI_PROBE_FN(vkUpdateDescriptorSets) KUI_PROBE_FN(vkCmdPipelineBarrier)
    KUI_PROBE_FN(vkCmdBindPipeline) KUI_PROBE_FN(vkCmdBindDescriptorSets) KUI_PROBE_FN(vkCmdPushConstants) KUI_PROBE_FN(vkCmdDispatch)
#undef KUI_PROBE_FN
};

}  // namespace kui::upscale

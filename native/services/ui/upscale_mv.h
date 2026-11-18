// SPDX-License-Identifier: MIT
// kiln.ui upscaler swap: the game's motion vectors in a format an upscaler takes (upscale_mv.cpp). The game renders
// them as R16G16_SNORM (UV units, B0), which AMD's FSR API cannot describe (FfxApiSurfaceFormat has no R16G16_SNORM).
// A small compute pass (shaders/mv_convert.comp) copies them, unchanged in value, into Kiln's own R16G16_SFLOAT image,
// recorded into the game's command buffer right before the backend's dispatch. Any backend may use it.
//
// Needs on the device: shaderStorageImageWriteWithoutFormat (the game enables it; want_mv_needs adds it to a backend's
// needs to be sure). The source is read through the game's own view in SHADER_READ_ONLY_OPTIMAL, left so.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

#include "upscale_backend.h"

namespace kui::upscale {

// Adds what the conversion needs to a backend's device needs.
void want_mv_needs(DeviceNeeds* needs);

class MotionConverter {
  public:
    // The device's functions (from vk.gdpa) and its memory types. False: a function is missing.
    bool init(const VkContext& vk, std::string* detail);
    // Records the copy of `in` (the game's MVs, SHADER_READ_ONLY_OPTIMAL) into `cb` and gives Kiln's image in `out`
    // (R16G16_SFLOAT, SHADER_READ_ONLY_OPTIMAL after the pass, the same size). `frame`: the core's frame count (old
    // images and descriptor sets are freed only once the GPU is done with them). False: nothing usable recorded.
    bool record(VkCommandBuffer cb, const Texture& in, uint64_t frame, Texture* out, std::string* detail);
    // Old images the GPU is surely done with go (each FSR frame, whether or not the pass ran).
    void tick(uint64_t frame) { if (dev_) reap(frame); }
    // Every Vulkan object goes (the device is idle).
    void destroy();

  private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        uint64_t last_frame = 0;
    };
    bool pipeline(std::string* detail);
    bool image_for(uint32_t w, uint32_t h, uint64_t frame, std::string* detail);
    void free_image(Image& i);
    void reap(uint64_t frame);

    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem_{};
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkShaderModule shader_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kSets = 8;  // one per frame in turn: a set is written again only 8 frames later
    VkDescriptorSet sets_[kSets]{};
    uint32_t next_set_ = 0;
    Image cur_;
    std::vector<Image> old_;
    bool ready_ = false;    // the pipeline, the pool and its sets are made

#define KUI_MV_FN(n) PFN_##n n = nullptr;
    KUI_MV_FN(vkCreateImage) KUI_MV_FN(vkDestroyImage) KUI_MV_FN(vkCreateImageView) KUI_MV_FN(vkDestroyImageView)
    KUI_MV_FN(vkGetImageMemoryRequirements) KUI_MV_FN(vkAllocateMemory) KUI_MV_FN(vkFreeMemory) KUI_MV_FN(vkBindImageMemory)
    KUI_MV_FN(vkCreateSampler) KUI_MV_FN(vkDestroySampler) KUI_MV_FN(vkCreateDescriptorSetLayout) KUI_MV_FN(vkDestroyDescriptorSetLayout)
    KUI_MV_FN(vkCreatePipelineLayout) KUI_MV_FN(vkDestroyPipelineLayout) KUI_MV_FN(vkCreateShaderModule) KUI_MV_FN(vkDestroyShaderModule)
    KUI_MV_FN(vkCreateComputePipelines) KUI_MV_FN(vkDestroyPipeline) KUI_MV_FN(vkCreateDescriptorPool) KUI_MV_FN(vkDestroyDescriptorPool)
    KUI_MV_FN(vkAllocateDescriptorSets) KUI_MV_FN(vkUpdateDescriptorSets) KUI_MV_FN(vkCmdPipelineBarrier) KUI_MV_FN(vkCmdBindPipeline)
    KUI_MV_FN(vkCmdBindDescriptorSets) KUI_MV_FN(vkCmdPushConstants) KUI_MV_FN(vkCmdDispatch)
    PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = nullptr;
#undef KUI_MV_FN
};

}  // namespace kui::upscale

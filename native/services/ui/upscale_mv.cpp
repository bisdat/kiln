// SPDX-License-Identifier: MIT
// kiln.ui upscaler swap: the motion-vector conversion pass (upscale_mv.h).
#include "upscale_mv.h"

#include <algorithm>
#include <cstring>

#include "shaders/mv_convert_spv.h"

namespace kui::upscale {

void want_mv_needs(DeviceNeeds* needs) {
    VkPhysicalDeviceFeatures2 f{}; f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    needs->want(f, false);
}

bool MotionConverter::init(const VkContext& vk, std::string* detail) {
    dev_ = vk.device;
    phys_ = vk.physical;
    bool ok = vk.gdpa && dev_;
#define KUI_MV_LOAD(n) if (ok) ok = (n = reinterpret_cast<PFN_##n>(vk.gdpa(dev_, #n))) != nullptr;
    KUI_MV_LOAD(vkCreateImage) KUI_MV_LOAD(vkDestroyImage) KUI_MV_LOAD(vkCreateImageView) KUI_MV_LOAD(vkDestroyImageView)
    KUI_MV_LOAD(vkGetImageMemoryRequirements) KUI_MV_LOAD(vkAllocateMemory) KUI_MV_LOAD(vkFreeMemory) KUI_MV_LOAD(vkBindImageMemory)
    KUI_MV_LOAD(vkCreateSampler) KUI_MV_LOAD(vkDestroySampler) KUI_MV_LOAD(vkCreateDescriptorSetLayout) KUI_MV_LOAD(vkDestroyDescriptorSetLayout)
    KUI_MV_LOAD(vkCreatePipelineLayout) KUI_MV_LOAD(vkDestroyPipelineLayout) KUI_MV_LOAD(vkCreateShaderModule) KUI_MV_LOAD(vkDestroyShaderModule)
    KUI_MV_LOAD(vkCreateComputePipelines) KUI_MV_LOAD(vkDestroyPipeline) KUI_MV_LOAD(vkCreateDescriptorPool) KUI_MV_LOAD(vkDestroyDescriptorPool)
    KUI_MV_LOAD(vkAllocateDescriptorSets) KUI_MV_LOAD(vkUpdateDescriptorSets) KUI_MV_LOAD(vkCmdPipelineBarrier) KUI_MV_LOAD(vkCmdBindPipeline)
    KUI_MV_LOAD(vkCmdBindDescriptorSets) KUI_MV_LOAD(vkCmdPushConstants) KUI_MV_LOAD(vkCmdDispatch)
#undef KUI_MV_LOAD
    auto mem_props = vk.gipa ? reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(vk.gipa(vk.instance, "vkGetPhysicalDeviceMemoryProperties")) : nullptr;
    vkGetPhysicalDeviceFormatProperties =
        vk.gipa ? reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(vk.gipa(vk.instance, "vkGetPhysicalDeviceFormatProperties")) : nullptr;
    if (!ok || !mem_props || !vkGetPhysicalDeviceFormatProperties) {
        if (detail) *detail = "a Vulkan function for the motion-vector pass is missing";
        return false;
    }
    mem_props(phys_, &mem_);
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_R16G16_SFLOAT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        if (detail) *detail = "the GPU cannot write R16G16_SFLOAT storage images (Kiln's motion-vector pass)";
        return false;
    }
    return true;
}

bool MotionConverter::pipeline(std::string* detail) {
    if (ready_) return true;
    auto fail = [&](const char* what) {
        if (detail) *detail = std::string("the motion-vector pass: ") + what + " failed";
        return false;
    };
    VkSamplerCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    if (!sampler_ && vkCreateSampler(dev_, &si, nullptr, &sampler_) != VK_SUCCESS) return fail("vkCreateSampler");
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[0].pImmutableSamplers = &sampler_;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 2;
    li.pBindings = b;
    if (!set_layout_ && vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_) != VK_SUCCESS) return fail("vkCreateDescriptorSetLayout");
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl{}; pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    if (!layout_ && vkCreatePipelineLayout(dev_, &pl, nullptr, &layout_) != VK_SUCCESS) return fail("vkCreatePipelineLayout");
    VkShaderModuleCreateInfo sm{}; sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = sizeof kMvConvertSpv;
    sm.pCode = kMvConvertSpv;
    if (!shader_ && vkCreateShaderModule(dev_, &sm, nullptr, &shader_) != VK_SUCCESS) return fail("vkCreateShaderModule");
    VkComputePipelineCreateInfo cp{}; cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = shader_;
    cp.stage.pName = "main";
    cp.layout = layout_;
    if (!pipe_ && vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &cp, nullptr, &pipe_) != VK_SUCCESS) {
        pipe_ = VK_NULL_HANDLE;
        return fail("vkCreateComputePipelines");
    }
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets}};
    VkDescriptorPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kSets;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    if (!pool_ && vkCreateDescriptorPool(dev_, &pi, nullptr, &pool_) != VK_SUCCESS) return fail("vkCreateDescriptorPool");
    VkDescriptorSetLayout layouts[kSets];
    std::fill(std::begin(layouts), std::end(layouts), set_layout_);
    VkDescriptorSetAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = kSets;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(dev_, &ai, sets_) != VK_SUCCESS) return fail("vkAllocateDescriptorSets");
    ready_ = true;
    return true;
}

void MotionConverter::free_image(Image& i) {
    if (i.view) vkDestroyImageView(dev_, i.view, nullptr);
    if (i.image) vkDestroyImage(dev_, i.image, nullptr);
    if (i.memory) vkFreeMemory(dev_, i.memory, nullptr);
    i = Image{};
}

void MotionConverter::reap(uint64_t frame) {
    for (size_t k = 0; k < old_.size();) {
        if (frame >= old_[k].last_frame + 4) {
            free_image(old_[k]);
            old_.erase(old_.begin() + static_cast<ptrdiff_t>(k));
        } else {
            ++k;
        }
    }
}

bool MotionConverter::image_for(uint32_t w, uint32_t h, uint64_t frame, std::string* detail) {
    if (cur_.image && cur_.width == w && cur_.height == h) return true;
    if (cur_.image) {
        old_.push_back(cur_);  // the GPU may still read it: freed 4 frames on
        cur_ = Image{};
    }
    auto fail = [&](const char* what) {
        if (detail) *detail = std::string("the motion-vector image: ") + what + " failed";
        free_image(cur_);
        return false;
    };
    VkImageCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R16G16_SFLOAT;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ci, nullptr, &cur_.image) != VK_SUCCESS) return fail("vkCreateImage");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev_, cur_.image, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < mem_.memoryTypeCount && type == UINT32_MAX; ++t)
        if ((req.memoryTypeBits & (1u << t)) && (mem_.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
    VkMemoryAllocateInfo ma{}; ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ma.allocationSize = req.size;
    ma.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(dev_, &ma, nullptr, &cur_.memory) != VK_SUCCESS) return fail("vkAllocateMemory");
    if (vkBindImageMemory(dev_, cur_.image, cur_.memory, 0) != VK_SUCCESS) return fail("vkBindImageMemory");
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = cur_.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev_, &vi, nullptr, &cur_.view) != VK_SUCCESS) return fail("vkCreateImageView");
    cur_.width = w;
    cur_.height = h;
    cur_.last_frame = frame;
    return true;
}

bool MotionConverter::record(VkCommandBuffer cb, const Texture& in, uint64_t frame, Texture* out, std::string* detail) {
    if (!dev_ || !in.view || !in.width || !in.height) {
        if (detail) *detail = "the motion-vector pass has no device or no source";
        return false;
    }
    reap(frame);
    if (!pipeline(detail) || !image_for(in.width, in.height, frame, detail)) return false;
    cur_.last_frame = frame;
    const VkDescriptorSet set = sets_[next_set_];
    next_set_ = (next_set_ + 1) % kSets;
    VkDescriptorImageInfo src{VK_NULL_HANDLE, in.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo dst{VK_NULL_HANDLE, cur_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{};
    for (auto& x : w) {
        x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        x.dstSet = set;
        x.descriptorCount = 1;
    }
    w[0].dstBinding = 0;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &src;
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[1].pImageInfo = &dst;
    vkUpdateDescriptorSets(dev_, 2, w, 0, nullptr);

    // Kiln's image: its last contents are not needed (UNDEFINED), after the previous frame's reads by the upscaler
    VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = cur_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // the game's MVs: written by its passes before the FSR pass (its flush made them SHADER_READ_ONLY with their own
    // barrier); Kiln reads them in a compute shader, so only an execution dependency on compute is added here
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
    const uint32_t size[2] = {in.width, in.height};
    vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof size, size);
    vkCmdDispatch(cb, (in.width + 7) / 8, (in.height + 7) / 8, 1);
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    out->image = cur_.image;
    out->view = cur_.view;
    out->format = VK_FORMAT_R16G16_SFLOAT;
    out->width = in.width;
    out->height = in.height;
    out->usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    out->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

void MotionConverter::destroy() {
    if (!dev_ || !vkDestroyImage) return;
    for (auto& i : old_) free_image(i);
    old_.clear();
    free_image(cur_);
    if (pool_) vkDestroyDescriptorPool(dev_, pool_, nullptr);  // frees the sets
    if (pipe_) vkDestroyPipeline(dev_, pipe_, nullptr);
    if (shader_) vkDestroyShaderModule(dev_, shader_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev_, layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
    if (sampler_) vkDestroySampler(dev_, sampler_, nullptr);
    pool_ = VK_NULL_HANDLE;
    pipe_ = VK_NULL_HANDLE;
    shader_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    std::fill(std::begin(sets_), std::end(sets_), VkDescriptorSet{VK_NULL_HANDLE});
    ready_ = false;
}

}  // namespace kui::upscale

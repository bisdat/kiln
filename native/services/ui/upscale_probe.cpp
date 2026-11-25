// SPDX-License-Identifier: MIT
// kiln.ui upscaler swap: the output check on the GPU (upscale_probe.h).
#include "upscale_probe.h"

#include <algorithm>
#include <cstring>

#include "shaders/output_probe_spv.h"

namespace kui::upscale {

bool OutputProbe::init(const VkContext& vk, std::string* detail) {
    dev_ = vk.device;
    bool ok = vk.gdpa && dev_;
#define KUI_PROBE_LOAD(n) if (ok) ok = (n = reinterpret_cast<PFN_##n>(vk.gdpa(dev_, #n))) != nullptr;
    KUI_PROBE_LOAD(vkCreateBuffer) KUI_PROBE_LOAD(vkDestroyBuffer) KUI_PROBE_LOAD(vkGetBufferMemoryRequirements) KUI_PROBE_LOAD(vkAllocateMemory)
    KUI_PROBE_LOAD(vkFreeMemory) KUI_PROBE_LOAD(vkBindBufferMemory) KUI_PROBE_LOAD(vkMapMemory) KUI_PROBE_LOAD(vkUnmapMemory)
    KUI_PROBE_LOAD(vkCreateSampler) KUI_PROBE_LOAD(vkDestroySampler) KUI_PROBE_LOAD(vkCreateDescriptorSetLayout)
    KUI_PROBE_LOAD(vkDestroyDescriptorSetLayout) KUI_PROBE_LOAD(vkCreatePipelineLayout) KUI_PROBE_LOAD(vkDestroyPipelineLayout)
    KUI_PROBE_LOAD(vkCreateShaderModule) KUI_PROBE_LOAD(vkDestroyShaderModule) KUI_PROBE_LOAD(vkCreateComputePipelines)
    KUI_PROBE_LOAD(vkDestroyPipeline) KUI_PROBE_LOAD(vkCreateDescriptorPool) KUI_PROBE_LOAD(vkDestroyDescriptorPool)
    KUI_PROBE_LOAD(vkAllocateDescriptorSets) KUI_PROBE_LOAD(vkUpdateDescriptorSets) KUI_PROBE_LOAD(vkCmdPipelineBarrier)
    KUI_PROBE_LOAD(vkCmdBindPipeline) KUI_PROBE_LOAD(vkCmdBindDescriptorSets) KUI_PROBE_LOAD(vkCmdPushConstants) KUI_PROBE_LOAD(vkCmdDispatch)
#undef KUI_PROBE_LOAD
    auto mem_props = vk.gipa ? reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(vk.gipa(vk.instance, "vkGetPhysicalDeviceMemoryProperties")) : nullptr;
    if (!ok || !mem_props) {
        if (detail) *detail = "a Vulkan function for the output check is missing";
        dev_ = VK_NULL_HANDLE;
        return false;
    }
    mem_props(vk.physical, &mem_);
    return true;
}

const char* OutputProbe::unsupported(const Frame& f) {
    if (!f.color.view || !f.output.view) return "no view of the colour or the output";
    if (f.output.format != VK_FORMAT_R16G16B16A16_SFLOAT) return "the output is not R16G16B16A16_SFLOAT";
    if (!f.render_width || !f.render_height || !f.output_width || !f.output_height) return "a size is 0";
    if (f.render_width > f.color.width || f.render_height > f.color.height || f.output_width > f.output.width ||
        f.output_height > f.output.height)
        return "a size is outside its texture";
    return nullptr;
}

bool OutputProbe::objects(std::string* detail) {
    if (ready_) return true;
    auto fail = [&](const char* what) {
        if (detail) *detail = std::string("the output check: ") + what + " failed";
        return false;
    };
    VkSamplerCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    if (!sampler_ && vkCreateSampler(dev_, &si, nullptr, &sampler_) != VK_SUCCESS) return fail("vkCreateSampler");
    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[0].pImmutableSamplers = &sampler_;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[2].binding = 2;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].descriptorCount = 1;
    b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo li{}; li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings = b;
    if (!set_layout_ && vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_) != VK_SUCCESS) return fail("vkCreateDescriptorSetLayout");
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
    VkPipelineLayoutCreateInfo pl{}; pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    if (!layout_ && vkCreatePipelineLayout(dev_, &pl, nullptr, &layout_) != VK_SUCCESS) return fail("vkCreatePipelineLayout");
    VkShaderModuleCreateInfo sm{}; sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = sizeof kOutputProbeSpv;
    sm.pCode = kOutputProbeSpv;
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
    // the result buffer: host-visible and coherent, mapped for the session
    if (!buffer_) {
        VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = probe::kBufferBytes;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(dev_, &bi, nullptr, &buffer_) != VK_SUCCESS) {
            buffer_ = VK_NULL_HANDLE;
            return fail("vkCreateBuffer");
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev_, buffer_, &req);
        constexpr VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < mem_.memoryTypeCount && type == UINT32_MAX; ++t)
            if ((req.memoryTypeBits & (1u << t)) && (mem_.memoryTypes[t].propertyFlags & want) == want) type = t;
        VkMemoryAllocateInfo ma{}; ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ma.allocationSize = req.size;
        ma.memoryTypeIndex = type;
        if (type == UINT32_MAX || vkAllocateMemory(dev_, &ma, nullptr, &memory_) != VK_SUCCESS) {
            memory_ = VK_NULL_HANDLE;
            return fail("vkAllocateMemory (host-visible)");
        }
        if (vkBindBufferMemory(dev_, buffer_, memory_, 0) != VK_SUCCESS) return fail("vkBindBufferMemory");
        if (vkMapMemory(dev_, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_) != VK_SUCCESS || !mapped_) {
            mapped_ = nullptr;
            return fail("vkMapMemory");
        }
        std::memset(mapped_, 0, probe::kBufferBytes);
    }
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSets}};
    VkDescriptorPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kSets;
    pi.poolSizeCount = 3;
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

bool OutputProbe::record(VkCommandBuffer cb, const Frame& f, uint32_t tag, std::string* detail) {
    if (!dev_ || !tag) {
        if (detail) *detail = "the output check has no device";
        return false;
    }
    if (const char* why = unsupported(f)) {
        if (detail) *detail = std::string("the output check cannot read this frame: ") + why;
        return false;
    }
    if (!objects(detail)) return false;
    // the slots of the check before (read already) go: a slot carries this check's tag only once the GPU wrote it
    std::memset(mapped_, 0, probe::kBufferBytes);
    const VkDescriptorSet set = sets_[next_set_];
    next_set_ = (next_set_ + 1) % kSets;
    VkDescriptorImageInfo colour{VK_NULL_HANDLE, f.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo output{VK_NULL_HANDLE, f.output.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo buf{buffer_, 0, probe::kBufferBytes};
    VkWriteDescriptorSet w[3]{};
    for (auto& x : w) {
        x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        x.dstSet = set;
        x.descriptorCount = 1;
    }
    w[0].dstBinding = 0;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &colour;
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[1].pImageInfo = &output;
    w[2].dstBinding = 2;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[2].pBufferInfo = &buf;
    vkUpdateDescriptorSets(dev_, 3, w, 0, nullptr);
    // the upscaler's writes of the output (its compute passes; FFX leaves it in GENERAL with no barrier after its last
    // pass) before Kiln's reads; no layout changes: both images stay as the game's next barriers expect them
    VkMemoryBarrier before{}; before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
    const uint32_t pcv[5] = {f.render_width, f.render_height, f.output_width, f.output_height, tag};
    vkCmdPushConstants(cb, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcv, pcv);
    vkCmdDispatch(cb, 1, 1, 1);
    // Kiln's buffer to the host (read frames later, after the game's fence for this command buffer)
    VkBufferMemoryBarrier after{}; after.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    after.srcQueueFamilyIndex = after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    after.buffer = buffer_;
    after.offset = 0;
    after.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &after, 0, nullptr);
    return true;
}

bool OutputProbe::read(uint32_t tag, probe::Result* out) const {
    if (!mapped_ || !tag) return false;
    probe::Slot s[probe::kThreads];
    std::memcpy(s, mapped_, sizeof s);
    return probe::reduce(s, probe::kThreads, tag, out);
}

void OutputProbe::destroy() {
    if (!dev_ || !vkDestroyBuffer) return;
    if (mapped_) vkUnmapMemory(dev_, memory_);
    mapped_ = nullptr;
    if (buffer_) vkDestroyBuffer(dev_, buffer_, nullptr);
    if (memory_) vkFreeMemory(dev_, memory_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev_, pool_, nullptr);  // frees the sets
    if (pipe_) vkDestroyPipeline(dev_, pipe_, nullptr);
    if (shader_) vkDestroyShaderModule(dev_, shader_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev_, layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
    if (sampler_) vkDestroySampler(dev_, sampler_, nullptr);
    buffer_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    pipe_ = VK_NULL_HANDLE;
    shader_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    std::fill(std::begin(sets_), std::end(sets_), VkDescriptorSet{VK_NULL_HANDLE});
    ready_ = false;
    dev_ = VK_NULL_HANDLE;
}

}  // namespace kui::upscale

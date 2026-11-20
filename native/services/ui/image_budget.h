// SPDX-License-Identifier: MIT
// kiln.ui: how many overlay images can exist, and how many are uploaded per frame (style.cpp's image registry, vk.cpp's
// descriptor pool). Pure: the host test (tests/image_budget_test.cpp) checks the numbers and the rules, and Kiln.Ui's tests
// read kMaxImages from this file (its item icon cache must stay well under it).
//
// Each image is one texture with one descriptor set from ImGui's Vulkan backend pool. A set the pool cannot give is
// VK_NULL_HANDLE: ImGui's backend then marks the texture uploaded with a null TexID, and drawing it binds a null set (a crash
// on some drivers). So the pool is sized well above every image Kiln lets exist, the fonts' atlas keeps a reserve, an image
// is drawn only once its set exists (state Ready), and new uploads (a vkQueueSubmit and vkQueueWaitIdle each in the
// backend) are spread over frames.
#pragma once
#include <cstddef>
#include <cstdint>

namespace kui::img {

constexpr uint32_t kDescriptorPoolSize = 1024;  // ImGui_ImplVulkan_InitInfo.DescriptorPoolSize (vk.cpp)
constexpr uint32_t kReserved = 64;              // ImGui's own textures: the fonts' atlas and its re-creations
constexpr uint32_t kMaxImages = kDescriptorPoolSize - kReserved;  // Kiln's images at once (freed ones until destroyed)
constexpr uint32_t kUploadsPerFrame = 8;        // new textures a frame creates; the rest wait for the next frames

static_assert(kMaxImages + kReserved <= kDescriptorPoolSize);

// image_state (kiln_ui.h)
enum State : int32_t {
    kUnknown = 0,  // no such image (never loaded, or freed)
    kWaiting = 1,  // loaded, its texture not uploaded yet (a frame or a few)
    kReady = 2,    // uploaded with its descriptor set: drawn
    kFailed = 3,   // uploaded without a descriptor set (the pool was full): never drawn; free it
};

// Whether one more image may be loaded while `live` images exist and `dying` freed ones still hold their texture.
constexpr bool can_add(size_t live, size_t dying) { return live + dying < kMaxImages; }

// An image's state from its texture: none created yet, the backend's status OK or not, and whether it got a descriptor set.
constexpr State state_of(bool failed, bool created, bool uploaded, bool has_set) {
    if (failed) return kFailed;
    if (!created || !uploaded) return kWaiting;
    return has_set ? kReady : kFailed;
}

// One frame's uploads: calls create(x) for the first kUploadsPerFrame elements of [first, last) that wait(x); returns how
// many it created. The others wait for the next frames.
template <class It, class Waits, class Create>
size_t upload_some(It first, It last, Waits waits, Create create) {
    size_t n = 0;
    for (; first != last && n < kUploadsPerFrame; ++first)
        if (waits(*first)) {
            create(*first);
            ++n;
        }
    return n;
}

}  // namespace kui::img

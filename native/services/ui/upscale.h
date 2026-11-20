// SPDX-License-Identifier: MIT
// kiln.ui: the upscaler swap's entry points for graphics.cpp and vk.cpp (upscale.cpp; docs/kiln/graphics.md "Upscaler
// behind the game's FSR 3"). Apart from ui.h so that ui.h needs no Vulkan headers.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <string>

#include "services/kiln_ui.h"

namespace kui {
namespace gfx { struct Settings; }  // graphics_settings.h

// graphics_start: the settings at the game's start and what steps aside; the swap's hooks go in when an upscaler other
// than the game's own is chosen then (and nothing steps aside), else nothing below does any work.
void upscale_start(const gfx::Settings& s, uint32_t aside, const std::string& aside_files);
void upscale_settings(const gfx::Settings& s);  // graphics_set: the choice now (applies at the game's next FSR 3 set-up)
// vk.cpp's hooks. begin: the create info to use (the game's, or Kiln's merged copy held by *scope until end).
const VkInstanceCreateInfo* upscale_instance_begin(const VkInstanceCreateInfo* ci, PFN_vkGetInstanceProcAddr gipa, void** scope);
void upscale_instance_end(void* scope, const VkInstanceCreateInfo* used, VkResult r, VkInstance instance);
void upscale_instance_destroyed(VkInstance instance);
const VkDeviceCreateInfo* upscale_device_begin(VkInstance instance, VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                               PFN_vkGetInstanceProcAddr gipa, void** scope);
void upscale_device_end(void* scope, const VkDeviceCreateInfo* used, VkResult r, VkDevice device, PFN_vkGetDeviceProcAddr gdpa,
                        uint32_t graphics_family);
void upscale_device_destroy(VkDevice device);  // vk.cpp's vkDestroyDevice hook, before the real call
// graphics_status: the phase B fields (out->size >= KILN_UI_GRAPHICS_STATUS_SIZE_V2)
void upscale_status(KilnUiGraphicsStatus* out);
VkDevice vk_swapchain_device();  // vk.cpp: the device of the game's last swapchain (any thread)
int vk_install_count();           // vk.cpp: how many times Kiln's Vulkan hooks went in (once per mapping of vulkan-1.dll)

}  // namespace kui

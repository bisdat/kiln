// SPDX-License-Identifier: MIT
// kiln.ui: Kiln's calls into an upscaler backend's library during the game's Vulkan set-up, marked on the calling
// thread, so the library's own Vulkan calls go through Kiln's hooks (vk.cpp) to the real functions untouched.
// Why: Kiln asks the backend for its needs inside the game's vkCreateInstance and vkCreateDevice
// (upscale.cpp upscale_instance_begin / upscale_device_begin), and Intel's libxess.dll makes Vulkan calls of its own
// while it answers. One of them came back into Kiln's vkCreateInstance hook, which loaded nothing new and asked XeSS
// again, until the game's main thread ran out of stack (0.2.0-dev.29.18's trial build, the owner's game: a stack
// overflow in load_backend 18 ms after libxess.dll loaded, before XeSS's instance needs were logged). With the mark
// set, Kiln's vkCreateInstance, vkDestroyInstance, vkCreateDevice and vkDestroyDevice hooks only pass the call on.
// Plain C++: tests/xess_test.cpp runs the XeSS backend against a fake libxess that calls back into a hook.
#pragma once

namespace kui::upscale::reentry {

// How deep this thread is in Kiln's calls into the backend (0: not inside).
inline thread_local int t_depth = 0;

inline bool inside() { return t_depth > 0; }

// Marks this thread as inside the backend for the scope's life.
class Scope {
  public:
    Scope() { ++t_depth; }
    ~Scope() { --t_depth; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace kui::upscale::reentry

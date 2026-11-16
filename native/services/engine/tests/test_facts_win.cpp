// SPDX-License-Identifier: MIT
// Test-only facts for kiln-engine-test.dll (headless Wine; never shipped, never built by default): the host names the
// exact identity and the fake engine image it built, so the real primitives (SEH copy, Interlocked words, SEH tracker
// call, the gate) run against it. The production DLL takes them from the loaded image (facts_win.cpp).
#include <windows.h>

#include <atomic>
#include <cstring>

#include "engine_state.h"
#include "memory.h"

namespace {
kiln::engine::ImageFacts g_facts;
std::atomic<std::uintptr_t> g_image{0};
}  // namespace

extern "C" __declspec(dllexport) void KILN_CALL kiln_engine_test_set(const char* target, const char* sha256,
                                                                       std::uint32_t timestamp, std::uint32_t size,
                                                                       std::uintptr_t image) {
    g_facts = {};
    g_facts.headers = true;
    g_facts.target = target;
    g_facts.sha256 = sha256;
    g_facts.timestamp = timestamp;
    g_facts.size_of_image = size;
    g_facts.archive_format = 4;
    g_facts.side = std::strcmp(target, "enshrouded.exe") == 0 ? KILN_SIDE_CLIENT : KILN_SIDE_SERVER;
    g_image.store(image);
}

namespace kiln::engine {
ImageFacts gather_facts(const KilnApi*) { return g_facts; }
namespace mem {
std::uintptr_t image_base() { return g_image.load(); }
}  // namespace mem
}  // namespace kiln::engine

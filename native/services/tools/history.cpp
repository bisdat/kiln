// SPDX-License-Identifier: MIT
#include <windows.h>
#include <cstring>
#include "history.h"
#include "history_create.h"
#include "kiln_hook.hpp"
#include "include/kiln_tools_policy.h"

namespace {
using DiscardFn = void (*)(uintptr_t storage);
using ExactFn = bool(KILN_CALL*)(const void* materials);
using RegisterExactFn = void(KILN_CALL*)(ExactFn);
kiln::tools::HistoryTags<> g_tags;
kiln::RawHook g_begin;
DiscardFn g_discard = nullptr;
KilnHandle g_world = 0;
bool g_installed = false;

uintptr_t history_begin(uintptr_t allocator, uintptr_t storage) {
    // Read the sole policy TLS, owned by the tools impact binding. The original history creation is also observed for
    // vanilla actions: a ring slot reused by vanilla must never inherit a previous tool action's exact-refund tag.
    const bool policy = kiln_tools_active_policy() != nullptr;
    return kiln::tools::create_history(g_tags,
        [](uintptr_t a, uintptr_t s) { return g_begin.call<uintptr_t>(a, s); },
        [](uintptr_t s) { g_discard(s); }, allocator, storage, policy);
}

void KILN_CALL world_changed(const KilnWorldEvent* event, void*) {
    if (event && event->kind == KILN_WORLD_SERVER &&
        (event->event == KILN_WORLD_STOPPING || event->event == KILN_WORLD_STARTED)) g_tags.reset();
}

KilnStr str(const char* text) { return {text, std::strlen(text)}; }
}

extern "C" KILN_EXPORT bool KILN_CALL kiln_tools_history_exact(const void* materials) {
    return g_tags.exact(reinterpret_cast<uintptr_t>(materials));
}

bool kiln_tools_history_install(const KilnApi* api, KilnOwner owner) {
    if (g_installed) return true;
    if (!api || !api->feature || !api->symbol || !api->constant || !api->session || !api->subscribe_world ||
        !api->feature(str("tools.history"))) return false;
    KilnSessionInfo session{};
    session.size = sizeof session;
    if (api->session(&session) != KILN_OK || !session.lifecycle) return false;
    uintptr_t begin = 0, discard = 0;
    int64_t materials = 0;
    if (api->symbol(str("tools/HistoryBegin"), &begin) != KILN_OK || !begin ||
        api->symbol(str("tools/HistoryDiscard"), &discard) != KILN_OK || !discard ||
        api->constant(str("tools/history.materials"), &materials) != KILN_OK || materials != 0x110) return false;
    // Rules can be absent; then there are no building multipliers to suppress. If it is present, require the exact
    // bridge on this build instead of silently allowing a doubled refund or waived yield takeback.
    const HMODULE rules = GetModuleHandleW(L"kiln-rules.dll");
    RegisterExactFn register_exact = nullptr;
    if (rules) {
        register_exact = reinterpret_cast<RegisterExactFn>(GetProcAddress(rules, "kiln_rules_exact_history"));
        if (!register_exact) return false;
    }
    if (api->subscribe_world(owner, &world_changed, nullptr, &g_world) != KILN_OK) return false;
    g_tags.reset();
    g_discard = reinterpret_cast<DiscardFn>(discard);
    if (!g_begin.install(api, owner, begin, reinterpret_cast<void*>(&history_begin), "tools/HistoryBegin")) return false;
    if (register_exact) register_exact(&kiln_tools_history_exact);
    g_installed = true;
    return true;
}

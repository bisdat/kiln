// SPDX-License-Identifier: MIT
#include <windows.h>
#include <cstring>
#include "history.h"
#include "history_create.h"
#include "qualified_entry.h"
#include "kiln_hook.hpp"
#include "include/kiln_tools_policy.h"

namespace {
using DiscardFn = void (*)(uintptr_t storage);
using ExactFn = bool(KILN_CALL*)(const void* materials);
using RegisterExactFn = void(KILN_CALL*)(ExactFn);
kiln::tools::HistoryTags<> g_tags;
kiln::RawHook g_begin;
DiscardFn g_discard = nullptr;
RegisterExactFn g_register_exact = nullptr;
const KilnApi* g_api = nullptr;
const KilnHooksApi* g_hooks = nullptr;
KilnOwner g_child = 0;
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

void kiln_tools_history_uninstall() {
    // The caller has closed/drained policy admission AND quiesced native undo/world jobs. Do not use this for a
    // mod unregister: its already-recorded native history must keep exact refunds until the world has ended.
    kiln_tools_report_hook(KILN_TOOL_HOOK_HISTORY_COST, 0);
    if (g_register_exact) g_register_exact(nullptr);
    g_register_exact = nullptr;
    if (g_child && g_hooks) g_hooks->revoke_owner(g_child);
    if (g_child && g_api) g_api->owner_revoke(g_child);
    g_child = 0;
    g_world = 0;
    g_tags.reset();
    g_discard = nullptr;
    g_hooks = nullptr;
    g_api = nullptr;
    g_installed = false;
}

extern "C" KILN_EXPORT bool KILN_CALL kiln_tools_history_exact(const void* materials) {
    return g_tags.exact(reinterpret_cast<uintptr_t>(materials));
}

bool kiln_tools_history_install(const KilnApi* api, KilnOwner owner) {
    if (g_installed) return true;
    if (!api || !owner || !api->feature || !api->symbol || !api->constant || !api->session ||
        !api->subscribe_world || !api->owner_create || !api->owner_revoke || !api->feature(str("tools.history"))) return false;
    const auto* hooks = kiln::hooks_v2(api);
    if (!hooks || !hooks->revoke_owner) return false;
    KilnSessionInfo session{};
    session.size = sizeof session;
    if (api->session(&session) != KILN_OK || !session.lifecycle) return false;
    uintptr_t begin = 0, discard = 0;
    int64_t materials = 0;
    if (api->symbol(str("tools/HistoryBegin"), &begin) != KILN_OK || !begin ||
        api->symbol(str("tools/HistoryDiscard"), &discard) != KILN_OK || !discard ||
        api->constant(str("tools/history.materials"), &materials) != KILN_OK || materials != 0x110) return false;
    // Both the hooked allocator and the direct discard call must retain qualified executable entries.
    constexpr uint8_t begin_entry[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18};
    constexpr uint8_t discard_entry[]={0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x57,0x41,0x56,0x41,0x57};
    if(!kt::qualified_entry(hooks,begin,begin_entry) || !kt::qualified_entry(hooks,discard,discard_entry))return false;
    // Rules can be absent; then there are no building multipliers to suppress. If it is present, require the exact
    // bridge on this build instead of silently allowing a doubled refund or waived yield takeback.
    const HMODULE rules = GetModuleHandleW(L"kiln-rules.dll");
    RegisterExactFn register_exact = nullptr;
    if (rules) {
        register_exact = reinterpret_cast<RegisterExactFn>(GetProcAddress(rules, "kiln_rules_exact_history"));
        if (!register_exact) return false;
    }
    const KilnOwner child = api->owner_create(str("kiln.tools.history"));
    if (!child || child == owner) return false;
    g_api = api;
    g_hooks = hooks;
    g_child = child;
    if (api->subscribe_world(child, &world_changed, nullptr, &g_world) != KILN_OK) {
        kiln_tools_history_uninstall();
        return false;
    }
    g_tags.reset();
    g_discard = reinterpret_cast<DiscardFn>(discard);
    if (!g_begin.install(api, child, begin, reinterpret_cast<void*>(&history_begin), "tools/HistoryBegin")) {
        kiln_tools_history_uninstall();
        return false;
    }
    g_register_exact = register_exact;
    if (register_exact) register_exact(&kiln_tools_history_exact);
    g_installed = true;
    return true;
}

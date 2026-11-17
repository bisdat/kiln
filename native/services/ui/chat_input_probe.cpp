// SPDX-License-Identifier: MIT
// Issue #44: opt-in, read-only probe for the native Enshrouded chat input.
// NO text is read or logged, no key is intercepted, and no game data is modified.
// After the two call sites are confirmed in a running client, replace this with
// the scoped history adapter using chat_keys.h and the managed ChatHistory model.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "kiln_hook.hpp"
#include "ui.h"

namespace kui {
namespace {

kiln::RawHook g_input;
uintptr_t g_hud_ret = 0, g_social_ret = 0;
std::atomic<bool> g_hud_seen{false}, g_social_seen{false};

struct GameText { const char* data; uint64_t length; uint64_t hash; };
static_assert(sizeof(GameText) == 24);

void __fastcall hk_input(void* widget, void* result, const GameText* label, uint32_t mode) {
    // The hook is deliberately transparent. The caller must be captured before
    // continuing the rest of Kiln's hook chain.
    const uintptr_t caller = g_input.caller();
    g_input.call<void>(widget, result, label, mode);
    if (caller == g_hud_ret && !g_hud_seen.exchange(true))
        logf(KILN_LOG_INFO, "ui: chat input probe: native HUD chat call observed (read-only)");
    else if (caller == g_social_ret && !g_social_seen.exchange(true))
        logf(KILN_LOG_INFO, "ui: chat input probe: native Social chat call observed (read-only)");
}

bool symbol(const char* name, uintptr_t* out) {
    *out = 0;
    return g_api->symbol(KilnStr{name, std::strlen(name)}, out) == KILN_OK && *out;
}

} // namespace

bool chat_input_probe_install() {
    char enabled[8]{};
    if (GetEnvironmentVariableA("KILN_CHAT_INPUT_PROBE", enabled, sizeof enabled) == 0 || enabled[0] != '1')
        return false; // default: do not hook anything
    if (!g_api->feature(KilnStr{"chat.ui.input", sizeof("chat.ui.input") - 1})) {
        logf(KILN_LOG_WARN, "ui: chat input probe requested, but chat.ui.input symbols are unavailable");
        return false;
    }
    uintptr_t input = 0;
    if (!symbol("chatui/TextInputProcess", &input) ||
        !symbol("chatui/HudInputReturn", &g_hud_ret) ||
        !symbol("chatui/SocialInputReturn", &g_social_ret) || g_hud_ret == g_social_ret) {
        logf(KILN_LOG_WARN, "ui: chat input probe requested, but scoped call sites were not resolved");
        return false;
    }
    if (!g_input.install(g_api, g_owner, input, reinterpret_cast<void*>(&hk_input), "chatui/TextInputProcess")) {
        logf(KILN_LOG_WARN, "ui: chat input probe hook unavailable: %s", g_input.error());
        return false;
    }
    logf(KILN_LOG_INFO, "ui: chat input probe installed for HUD and Social (read-only; no history keys intercepted)");
    return true;
}

} // namespace kui

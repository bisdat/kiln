// SPDX-License-Identifier: MIT
// Native Windows execution of the actual history hook with a fake engine continuation and hook registry.
#include "../history.h"
#include "../include/kiln_tools_policy.h"
#include "services/kiln_hooks.h"
#include <windows.h>
#include <limits>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int checks = 0, discarded = 0, begins = 0, owners_retired = 0, revoked = 0;
bool history_reported = true;
KilnOwner child_owner = 99;
bool policy = false, feature_on = true, symbols_on = true, lifecycle = true, hooks_on = true;
int64_t materials_offset = 0x110;
uintptr_t next_entry = 0x1000;
using BeginFn = uintptr_t (*)(uintptr_t, uintptr_t);
BeginFn hooked = nullptr;
KilnWorldEventFn world_callback = nullptr;
KilnHooksApi hooks{};
uint32_t live_code = KILN_CODE_ORIGINAL;
bool wrong_original = false, original_unreadable = false, state_unreadable = false;
uintptr_t bad_target = 0;
void discard_original(uintptr_t storage);
KilnResult KILN_CALL read_original(uintptr_t target, void* out, size_t size) {
    constexpr uint8_t begin[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18};
    constexpr uint8_t discard[] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x57,0x41,0x56,0x41,0x57};
    if (original_unreadable || size != sizeof(begin)) return KILN_ERR_UNAVAILABLE;
    std::memcpy(out, target == reinterpret_cast<uintptr_t>(&discard_original) ? discard : begin, size);
    if (wrong_original) static_cast<uint8_t*>(out)[0] ^= 1;
    return KILN_OK;
}
KilnResult KILN_CALL code_state(uintptr_t target, size_t, uint32_t* state, uint64_t* generation) {
    if (state_unreadable) return KILN_ERR_UNAVAILABLE;
    *state = target == bad_target ? KILN_CODE_FOREIGN : live_code; *generation = 1;
    return KILN_OK;
}

void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    ++checks;
}
bool named(KilnStr value, const char* name) {
    return value.len == std::strlen(name) && std::memcmp(value.ptr, name, value.len) == 0;
}
uintptr_t begin_original(uintptr_t allocator, uintptr_t storage) {
    check(allocator == 0xabc && storage == 0xdef, "Win64 continuation preserves allocator and player history");
    ++begins;
    return next_entry;
}
void discard_original(uintptr_t storage) {
    check(storage == 0xdef, "history failure discards only the native player's new entry");
    ++discarded;
}
KilnResult KILN_CALL detour(KilnOwner owner, uintptr_t target, const KilnHookOrder*, void* handler,
                            void** original, KilnHandle* out) {
    check(owner == child_owner && owner != 99 && target == reinterpret_cast<uintptr_t>(&begin_original),
          "resolved target and child owner reach hook registry");
    if (!hooks_on) return KILN_ERR_UNAVAILABLE;
    hooked = reinterpret_cast<BeginFn>(handler);
    *original = reinterpret_cast<void*>(&begin_original);
    *out = 1;
    return KILN_OK;
}
const void* KILN_CALL service(KilnStr name, uint32_t version) {
    return named(name, "kiln.hooks") && version == 2 ? &hooks : nullptr;
}
bool KILN_CALL feature(KilnStr name) { return feature_on && named(name, "tools.history"); }
KilnResult KILN_CALL symbol(KilnStr name, uintptr_t* out) {
    if (!symbols_on) return KILN_ERR_NOT_FOUND;
    if (named(name, "tools/HistoryBegin")) *out = reinterpret_cast<uintptr_t>(&begin_original);
    else if (named(name, "tools/HistoryDiscard")) *out = reinterpret_cast<uintptr_t>(&discard_original);
    else return KILN_ERR_NOT_FOUND;
    return KILN_OK;
}
KilnResult KILN_CALL constant(KilnStr name, int64_t* out) {
    if (!named(name, "tools/history.materials")) return KILN_ERR_NOT_FOUND;
    *out = materials_offset;
    return KILN_OK;
}
KilnResult KILN_CALL session(KilnSessionInfo* out) { out->lifecycle = lifecycle; return KILN_OK; }
KilnResult KILN_CALL subscribe_world(KilnOwner owner, KilnWorldEventFn callback, void*, KilnHandle* out) {
    check(owner == child_owner && owner != 99, "world reset subscription uses only the history child owner");
    world_callback = callback;
    *out = 2;
    return KILN_OK;
}
KilnOwner KILN_CALL owner_create(KilnStr name) {
    check(named(name, "kiln.tools.history"), "history gets a dedicated child owner");
    return ++child_owner;
}
void KILN_CALL owner_revoke(KilnOwner owner) {
    check(owner == child_owner && owner != 99, "teardown retires only the history child, never the tools parent");
    ++owners_retired;
    world_callback = nullptr;
}
KilnResult KILN_CALL revoke_owner(KilnOwner owner) {
    check(owner == child_owner && owner != 99 && !history_reported,
          "teardown clears capability before revoking only the child owner's hooks");
    ++revoked;
    return KILN_OK;
}
KilnResult KILN_CALL revoke(KilnHandle) { return KILN_OK; }
void world(uint32_t kind, uint32_t event) {
    KilnWorldEvent ev{};
    ev.size = sizeof ev; ev.kind = kind; ev.event = event;
    world_callback(&ev, nullptr);
}
const void* list(uintptr_t entry) { return reinterpret_cast<const void*>(entry + 0x110); }
}

// The real tools DLL provides this single scoped TLS reader. The test substitutes only its boolean availability;
// history.cpp never dereferences this sentinel or keeps the policy view.
const KilnToolPolicyView* kiln_tools_active_policy() {
    return policy ? reinterpret_cast<const KilnToolPolicyView*>(1) : nullptr;
}

void kiln_tools_report_hook(uint32_t hook, int installed) {
    check(hook == KILN_TOOL_HOOK_HISTORY_COST, "teardown reports only the native history hook");
    history_reported = installed != 0;
}

int main(int argc, char** argv) {
    check(argc == 3, "native bridge fixture paths are provided");
    KilnApi api{};
    api.size = sizeof api; api.abi_version = KILN_ABI_VERSION;
    api.feature = &feature; api.symbol = &symbol; api.constant = &constant; api.session = &session;
    api.subscribe_world = &subscribe_world; api.owner_create = &owner_create; api.owner_revoke = &owner_revoke;
    api.service = &service;
    hooks.header = {sizeof hooks, 2}; hooks.raw_detour = &detour; hooks.revoke = &revoke; hooks.revoke_owner = &revoke_owner;
    hooks.read_original = &read_original; hooks.code_state = &code_state;
    for (auto state : {KILN_CODE_FOREIGN, KILN_CODE_UNKNOWN}) {
        live_code = state;
        check(!kiln_tools_history_install(&api, 99), "unqualified history code refuses");
    }
    live_code = KILN_CODE_ORIGINAL;
    for (auto target : {reinterpret_cast<uintptr_t>(&begin_original), reinterpret_cast<uintptr_t>(&discard_original)}) {
        bad_target = target;
        check(!kiln_tools_history_install(&api, 99), "each history seam independently refuses foreign code");
    }
    bad_target = 0; wrong_original = true;
    check(!kiln_tools_history_install(&api, 99), "wrong history prologue refuses"); wrong_original = false;
    original_unreadable = true;
    check(!kiln_tools_history_install(&api, 99), "unreadable original history code refuses"); original_unreadable = false;
    state_unreadable = true;
    check(!kiln_tools_history_install(&api, 99), "unknown history provenance refuses"); state_unreadable = false;
    hooks.read_original = nullptr;
    check(!kiln_tools_history_install(&api, 99), "missing original code API refuses"); hooks.read_original = &read_original;
    hooks.code_state = nullptr;
    check(!kiln_tools_history_install(&api, 99), "missing provenance API refuses"); hooks.code_state = &code_state;
    live_code = KILN_CODE_MANAGED;
    check(!kiln_tools_history_install(nullptr, 99), "missing core refuses");
    feature_on = false;
    check(!kiln_tools_history_install(&api, 99), "missing history capability refuses");
    feature_on = true; symbols_on = false;
    check(!kiln_tools_history_install(&api, 99), "missing native mapping refuses");
    symbols_on = true; materials_offset = 0x118;
    check(!kiln_tools_history_install(&api, 99), "mismatched history layout refuses");
    materials_offset = 0x110; lifecycle = false;
    check(!kiln_tools_history_install(&api, 99), "unobservable world teardown refuses");
    lifecycle = true; hooks_on = false;
    check(!kiln_tools_history_install(&api, 99), "hook installation failure refuses");
    check(owners_retired == 1, "failed hook installation releases its world subscription");
    hooks_on = true;
    check(kiln_tools_history_install(&api, 99) && hooked && world_callback, "native history hook installs");
    history_reported = true;  // the parent reports successful installation
    check(kiln_tools_history_install(&api, 99), "history installation is idempotent");
    policy = true;
    check(hooked(0xabc, 0xdef) == 0x1000 && kiln_tools_history_exact(list(0x1000)), "tool add native history is exact");
    policy = false; next_entry = 0x2000;
    check(hooked(0xabc, 0xdef) == 0x2000 && !kiln_tools_history_exact(list(0x2000)), "interleaved vanilla history is not tagged");
    check(kiln_tools_history_exact(list(0x1000)), "vanilla interleaving preserves preceding tool tag");
    policy = true; next_entry = 0x3000;
    check(hooked(0xabc, 0xdef) == 0x3000 && kiln_tools_history_exact(list(0x3000)), "another player's remove history is exact");
    policy = false; next_entry = 0x1000;
    check(hooked(0xabc, 0xdef) == 0x1000 && !kiln_tools_history_exact(list(0x1000)), "native ring reuse by vanilla clears old tool identity");
    check(kiln_tools_history_exact(list(0x3000)) && kiln_tools_history_exact(list(0x3000)), "repeated undo list queries never mint another receipt");
    world(KILN_WORLD_CLIENT, KILN_WORLD_STOPPING);
    check(kiln_tools_history_exact(list(0x3000)), "client teardown cannot erase hosted server history");
    world(KILN_WORLD_SERVER, KILN_WORLD_STOPPING);
    check(!kiln_tools_history_exact(list(0x3000)), "server teardown invalidates descriptor identities");
    policy = true; next_entry = 0;
    check(hooked(0xabc, 0xdef) == 0 && discarded == 0, "native begin failure does not discard preceding history");
    for (uintptr_t i = 1; i <= 2048; ++i) {
        next_entry = i * 0x200;
        check(hooked(0xabc, 0xdef) == next_entry, "bounded native tool history identity is retained");
    }
    next_entry = 2049 * 0x200;
    const int before_overflow = begins;
    check(hooked(0xabc, 0xdef) == 0 && discarded == 0 && begins == before_overflow,
          "full sidecar refuses before native history allocation and eviction");
    check(!kiln_tools_history_exact(list(next_entry)), "refused history cannot create an exact refund identity");
    world(KILN_WORLD_SERVER, KILN_WORLD_STARTED);
    next_entry = std::numeric_limits<uintptr_t>::max() - 0x100;
    check(hooked(0xabc, 0xdef) == 0 && discarded == 1,
          "untaggable nonzero native entry is discarded once and never escapes as pending history");
    check(!kiln_tools_history_exact(list(0x200)), "new server session cannot inherit old identities");
    next_entry = 0x1000;
    check(hooked(0xabc, 0xdef) == next_entry && kiln_tools_history_exact(list(next_entry)),
          "remaining tool history stays exact before global teardown");
    kiln_tools_history_uninstall();
    check(!kiln_tools_history_exact(list(next_entry)) && !history_reported && revoked == 2 && owners_retired == 2,
          "global quiesced teardown revokes hook, subscription, tags and capability");
    kiln_tools_history_uninstall();
    check(revoked == 2 && owners_retired == 2, "global teardown is idempotent");
    policy = false;
    check(hooked(0xabc, 0xdef) == next_entry, "a retained vanilla continuation remains safe after revoke");
    check(kiln_tools_history_install(&api, 99), "history can install again after complete global teardown");
    kiln_tools_history_uninstall();
    check(revoked == 3 && owners_retired == 3, "reinstalled history owns only its new hook and subscription");
    HMODULE rules = LoadLibraryA(argv[1]);
    check(rules && GetModuleHandleW(L"kiln-rules.dll") == rules, "missing-setter rules fixture loads under the production DLL name");
    check(!kiln_tools_history_install(&api, 99) && !history_reported && revoked == 3 && owners_retired == 3,
          "present rules without exact setter refuse before allocating hook or subscription");
    check(FreeLibrary(rules) != 0, "missing-setter fixture is released by its test owner");
    rules = LoadLibraryA(argv[2]);
    check(rules && GetModuleHandleW(L"kiln-rules.dll") == rules, "exact-setter rules fixture loads under the production DLL name");
    using ProbeFn = bool(KILN_CALL*)(const void*);
    const auto probe = reinterpret_cast<ProbeFn>(GetProcAddress(rules, "history_bridge_exact"));
    check(probe && !probe(list(next_entry)), "new rules fixture has no predicate before history installation");
    check(kiln_tools_history_install(&api, 99), "history installs with a previously loaded rules setter");
    history_reported = true;
    policy = true; next_entry = 0x4000;
    check(hooked(0xabc, 0xdef) == next_entry && probe(list(next_entry)), "rules calls the installed exact predicate for tool history");
    policy = false; next_entry = 0x5000;
    check(hooked(0xabc, 0xdef) == next_entry && !probe(list(next_entry)) && probe(list(0x4000)),
          "rules predicate isolates interleaved vanilla lists from exact tool lists");
    kiln_tools_history_uninstall();
    check(!probe(list(0x4000)) && !history_reported, "global shutdown clears rules callback before tags and service teardown");
    check(kiln_tools_history_install(&api, 99), "history reinstalls against resident rules and a retained detour");
    history_reported = true;
    policy = true; next_entry = 0x6000;
    check(hooked(0xabc, 0xdef) == next_entry && probe(list(next_entry)) && !probe(list(0x4000)),
          "reinstallation publishes only new exact identities");
    kiln_tools_history_uninstall();
    check(!probe(list(next_entry)) && revoked == 5 && owners_retired == 5,
          "reinstalled bridge clears its callback and retires only its own registrations");
    check(FreeLibrary(rules) != 0, "exact-setter fixture unloads only after callback removal");
    std::printf("%d checks passed; %d native continuation calls; %d pending-entry discards\n", checks, begins, discarded);
}

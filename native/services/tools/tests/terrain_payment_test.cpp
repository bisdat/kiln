// SPDX-License-Identifier: MIT
// Runs the real payment detours over an injected cost/stock engine, never a game process.
#include "../hooks/terrain_payment.h"
#include "../hooks/terrain_cost_math.h"
#include "kiln_tools_policy.h"
#include "kiln_hook.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace kiln_tools_payment;
thread_local const KilnToolPolicyView* active = nullptr;
uint32_t reported = 0;
CostFn builder = nullptr;
WrapperFn wrapper = nullptr;
std::array<Material, 8> storage{};
int vanilla_builds = 0, appends = 0, debits = 0, voxels = 0, wrapper_calls = 0;
bool append_fails = false, append_faults = false, apply_fails = false, bypass_builder = false, corrupt_final = false;
bool partial_append_fails = false, partial_append_faults = false, wrapper_faults = false;
bool nested_cost = false, threaded_cost = false, inner_ok = false;
uintptr_t bad_state_target = 0, bad_original_target = 0;
int append_fail_at = 0;
bool fail_wrapper_install = false, mapped = true, wrong_original = false, refuse_original = false, refuse_state = false;
uint32_t live_state = KILN_CODE_ORIGINAL;
KilnHandle next_handle = 0, builder_handle = 0, wrapper_handle = 0;
std::string revoke_order;
KilnOwner created_owner = 100;
int owners_created = 0, owners_retired = 0;
bool fail_owner_create = false, parent_owner = false, lifecycle_ok = true;
KilnOwner KILN_CALL owner_create(KilnStr name) {
    lifecycle_ok &= std::string_view(name.ptr, name.len) == "kiln.tools.payment";
    if (fail_owner_create) return 0;
    if (parent_owner) return 1;
    ++owners_created; return ++created_owner;
}
void KILN_CALL owner_revoke(KilnOwner owner) {
    lifecycle_ok &= owner == created_owner && !reported && !revoke_order.empty() && revoke_order.back() == 'H';
    revoke_order += 'O'; ++owners_retired;
}
float forced_fraction = -1;
uint32_t bag = 0, magic = 0, ordinary = 100, station = 100;

Material* append(void* raw, const Material* record) {
    ++appends;
#ifdef _WIN32
    if (append_faults) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#endif
    if (append_fails || (append_fail_at && appends == append_fail_at)) return nullptr;
    auto* list = static_cast<MaterialList*>(raw);
    if (list->count >= list->capacity) return nullptr;
    auto* added = &list->data[list->count++];
    *added = *record;
#ifdef _WIN32
    if (partial_append_faults) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#endif
    return partial_append_fails ? nullptr : added;
}
void vanilla_builder(void* raw, void*, void*, float volume) {
    ++vanilla_builds;
    Material m{}; m.item = 999; m.count = static_cast<uint32_t>(std::ceil(volume / 16));
    if (m.count) append(raw, &m);
}
float vanilla_wrapper(void* raw, void* op, void*, void*, void*, void* desc, float volume, void*) {
    ++wrapper_calls;
    if (nested_cost || threaded_cost) {
        const bool on_thread = threaded_cost;
        nested_cost = threaded_cost = false;
        auto inner = [&] {
            const auto* outer_policy = active;
            KilnToolPoolEntry entry{}; entry.item = 1234; entry.cell = 8; entry.voxels_per_item = 16;
            KilnToolPolicyView policy{}; policy.size = sizeof policy; policy.mode = 2; policy.pool_count = 1; policy.pool = &entry;
            active = on_thread ? nullptr : &policy;
            std::array<Material, 8> local{};
            MaterialList list{local.data(), 0, local.size(), nullptr};
            float fraction = wrapper(&list, op, nullptr, nullptr, nullptr, desc, 16, nullptr);
            inner_ok = fraction == 1 && list.count == 1 && list.data[0].item == (on_thread ? 999u : entry.item);
            active = outer_policy;
        };
        if (on_thread) { std::thread worker(inner); worker.join(); }
        else inner();
    }
    if (!bypass_builder) builder(raw, op, desc, volume);
#ifdef _WIN32
    if (wrapper_faults) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#endif
    auto* list = static_cast<MaterialList*>(raw);
    float fraction = list->count && list->data[0].count ?
        std::min(1.0f, static_cast<float>(bag + magic) / list->data[0].count) : 1.0f;
    if (forced_fraction >= 0) fraction = forced_fraction;
    if (fraction < 1) { list->count = 0; builder(raw, op, desc, volume * fraction); }
    if (corrupt_final && list->count) list->data[0].count = 0;
    return fraction;
}
KilnResult KILN_CALL raw_hook(KilnOwner owner, uintptr_t target, const KilnHookOrder*, void* detour, void** original, KilnHandle* h) {
    lifecycle_ok &= owner == created_owner && owner != 1;
    if (fail_wrapper_install && target == reinterpret_cast<uintptr_t>(&vanilla_wrapper)) return KILN_ERR_UNAVAILABLE;
    *original = reinterpret_cast<void*>(target); *h = ++next_handle;
    if (target == reinterpret_cast<uintptr_t>(&vanilla_builder)) { builder = reinterpret_cast<CostFn>(detour); builder_handle = *h; }
    else if (target == reinterpret_cast<uintptr_t>(&vanilla_wrapper)) { wrapper = reinterpret_cast<WrapperFn>(detour); wrapper_handle = *h; }
    else return KILN_ERR_INVALID_ARGUMENT;
    return KILN_OK;
}
KilnResult KILN_CALL revoke(KilnHandle handle) {
    lifecycle_ok &= !reported;
    if (handle == builder_handle) { builder = &vanilla_builder; builder_handle = 0; revoke_order += 'B'; }
    if (handle == wrapper_handle) { wrapper = &vanilla_wrapper; wrapper_handle = 0; revoke_order += 'W'; }
    return KILN_OK;
}
KilnResult KILN_CALL revoke_owner(KilnOwner owner) {
    lifecycle_ok &= owner == created_owner && owner != 1 && !reported && !builder_handle && !wrapper_handle;
    revoke_order += 'H'; return KILN_OK;
}
KilnResult KILN_CALL read_original(uintptr_t target, void* out, size_t size) {
    if (refuse_original) return KILN_ERR_UNAVAILABLE;
    const unsigned char cost[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57};
    const unsigned char scale[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x41};
    if (size != sizeof cost) return KILN_ERR_INVALID_ARGUMENT;
    std::memcpy(out, target == reinterpret_cast<uintptr_t>(&vanilla_builder) ? cost : scale, size);
    if (wrong_original || target == bad_original_target) static_cast<unsigned char*>(out)[0] ^= 1;
    return KILN_OK;
}
KilnResult KILN_CALL code_state(uintptr_t target, size_t, uint32_t* state, uint64_t* generation) {
    if (refuse_state) return KILN_ERR_UNAVAILABLE;
    *state = target == bad_state_target ? KILN_CODE_FOREIGN : live_state; *generation = 1; return KILN_OK;
}
KilnHooksApi hooks{};
bool KILN_CALL feature(KilnStr) { return mapped; }
KilnResult KILN_CALL symbol(KilnStr name, uintptr_t* out) {
    std::string_view s(name.ptr, name.len);
    if (s == "tools/TerrainCostBuild") *out = reinterpret_cast<uintptr_t>(&vanilla_builder);
    else if (s == "tools/TerrainCostScale") *out = reinterpret_cast<uintptr_t>(&vanilla_wrapper);
    else if (s == "tools/MaterialListAppend") *out = reinterpret_cast<uintptr_t>(&append);
    else return KILN_ERR_NOT_FOUND;
    return KILN_OK;
}
KilnResult KILN_CALL constant(KilnStr name, int64_t* out) {
    if (std::string_view(name.ptr, name.len) != "tools/cost.cell") return KILN_ERR_NOT_FOUND;
    *out = 0x28; return KILN_OK;
}
const void* KILN_CALL service(KilnStr name, uint32_t) {
    return std::string_view(name.ptr, name.len) == "kiln.hooks" ? &hooks : nullptr;
}
void reset() {
    storage = {}; vanilla_builds = appends = debits = voxels = wrapper_calls = 0;
    append_fails = append_faults = apply_fails = bypass_builder = corrupt_final = false;
    partial_append_fails = partial_append_faults = wrapper_faults = false;
    nested_cost = threaded_cost = inner_ok = false;
    append_fail_at = 0; forced_fraction = -1; bag = magic = 0;
}
// An explicit engine model of payment AFTER the wrapper; ordinary/station stock never participates.
uint32_t perform(void* desc, float volume, MaterialList& list, Material& history) {
    float fraction = wrapper(&list, nullptr, nullptr, nullptr, nullptr, desc, volume, nullptr);
    if (!(fraction > 0)) return 0;
    // An empty list is affordable to the real engine. Do not hide a free-edit bug in this test oracle.
    if (!list.count) { ++voxels; return 0; }
    const auto paid = list.data[0];
    if (apply_fails || paid.count > bag + magic) return 0;
    uint32_t b = std::min(bag, paid.count); bag -= b; magic -= paid.count - b;
    ++debits; ++voxels; history = paid; return paid.count;
}
}
extern "C" const KilnToolPolicyView* kiln_tools_active_policy() { return active; }
extern "C" void kiln_tools_report_hook(uint32_t, int installed) { reported = installed ? 1 : 0; }

int main() {
    int total = 0, failed = 0;
    auto check = [&](bool ok, const char* name) {
        ++total; if (!ok) { ++failed; std::fprintf(stderr, "FAIL %s\n", name); }
    };
    uint32_t n = 123;
    check(price(0, 16, n) && n == 0, "zero volume");
    check(price(0.0000001f, 16, n) && n == 1, "positive tiny volume never free");
    check(price(15.99f, 16, n) && n == 1, "below boundary");
    check(price(16, 16, n) && n == 1, "exact boundary");
    check(price(std::nextafter(16.0f, 17.0f), 16, n) && n == 1, "float boundary noise tolerated");
    check(price(16.001f, 16, n) && n == 2, "above tolerance charges next item");
    check(!price(-1, 16, n) && !price(std::numeric_limits<float>::quiet_NaN(), 16, n) &&
          !price(std::numeric_limits<float>::infinity(), 16, n), "invalid volumes refused");
    check(!price(1, 0, n) && !price(std::numeric_limits<float>::max(), 1, n), "invalid rate and count overflow");
    for (uint32_t rate : {16u, 5u, 7u, 8u, 6u, 9u, 10u, 6u, 16u})
        check(price(static_cast<float>(rate) + 0.001f, rate, n) && n == 2, "verified placement rate");
    check(price(std::nextafter(112.0f, std::numeric_limits<float>::infinity()), 7, n) && n == 16,
          "float ULP above a larger exact multiple is not overcharged");
    for (uint32_t rate : {5u, 7u, 16u}) {
        for (uint32_t multiple : {16u, 1000u, 10000u}) {
            float volume = static_cast<float>(rate * multiple);
            check(price(volume, rate, n) && n == multiple, "large exact multiple");
            check(price(std::nextafter(volume, std::numeric_limits<float>::infinity()), rate, n) && n == multiple,
                  "large exact multiple plus one float ULP");
            check(price(volume + 1, rate, n) && n == multiple + 1, "large multiple plus a real voxel");
        }
    }
    hooks.header = {sizeof hooks, 2}; hooks.raw_detour = &raw_hook; hooks.revoke = &revoke;
    hooks.read_original = &read_original; hooks.code_state = &code_state; hooks.revoke_owner = &revoke_owner;
    KilnApi api{}; api.size = sizeof api; api.feature = &feature; api.symbol = &symbol; api.service = &service; api.constant = &constant;
    api.owner_create = &owner_create; api.owner_revoke = &owner_revoke;
    live_state = KILN_CODE_FOREIGN;
    check(!kiln_tools_payment_install(&api, 1) && !reported, "foreign payment code refuses installation");
    live_state = KILN_CODE_UNKNOWN;
    check(!kiln_tools_payment_install(&api, 1) && !reported, "unknown payment code refuses installation");
    live_state = KILN_CODE_ORIGINAL; wrong_original = true;
    check(!kiln_tools_payment_install(&api, 1) && !reported, "wrong original prologue refuses installation");
    wrong_original = false; refuse_original = true;
    check(!kiln_tools_payment_install(&api, 1) && !reported, "unavailable original bytes refuse installation");
    refuse_original = false; refuse_state = true;
    check(!kiln_tools_payment_install(&api, 1) && !reported, "unavailable code state refuses installation");
    refuse_state = false; hooks.header.size = offsetof(KilnHooksApi, code_state);
    check(!kiln_tools_payment_install(&api, 1) && !reported, "short hooks table refuses installation");
    hooks.header.size = sizeof hooks; hooks.code_state = nullptr;
    check(!kiln_tools_payment_install(&api, 1) && !reported, "missing code-state member refuses installation");
    hooks.code_state = &code_state; hooks.read_original = nullptr;
    check(!kiln_tools_payment_install(&api, 1) && !reported, "missing original-code member refuses installation");
    hooks.read_original = &read_original;
    for (auto target : {reinterpret_cast<uintptr_t>(&vanilla_builder), reinterpret_cast<uintptr_t>(&vanilla_wrapper),
                        reinterpret_cast<uintptr_t>(&append)}) {
        bad_state_target = target;
        check(!kiln_tools_payment_install(&api, 1) && !reported, "each payment seam refuses foreign code independently");
    }
    bad_state_target = 0; bad_original_target = reinterpret_cast<uintptr_t>(&vanilla_wrapper);
    check(!kiln_tools_payment_install(&api, 1) && !reported, "wrapper original prologue mismatch refuses");
    bad_original_target = 0; fail_owner_create = true;
    check(!kiln_tools_payment_install(&api, 1) && !reported, "failed child owner creation refuses");
    fail_owner_create = false; api.owner_revoke = nullptr;
    check(!kiln_tools_payment_install(&api, 1) && !reported, "missing owner retirement refuses");
    api.owner_revoke = &owner_revoke; hooks.revoke_owner = nullptr;
    check(!kiln_tools_payment_install(&api, 1) && !reported, "missing hook-owner retirement refuses");
    hooks.revoke_owner = &revoke_owner;
    parent_owner = true;
    check(!kiln_tools_payment_install(&api, 1) && !reported && lifecycle_ok,
          "a parent returned as child is refused without hooking or revoking it");
    parent_owner = false;
    check(kiln_tools_payment_install(&api, 1) && reported, "both detours install before cost is available");
    mapped = false;
    check(!kiln_tools_payment_install(&api, 1) && !reported && builder == &vanilla_builder && wrapper == &vanilla_wrapper,
          "failed requalification removes both old links");
    mapped = true; fail_wrapper_install = true;
    check(!kiln_tools_payment_install(&api, 1) && !reported && builder == &vanilla_builder && wrapper == &vanilla_wrapper,
          "second hook failure rolls back the first hook");
    fail_wrapper_install = false; live_state = KILN_CODE_MANAGED;
    check(kiln_tools_payment_install(&api, 1) && reported, "retry accepts retained managed detours");

    KilnToolPoolEntry row{}; row.item = 1257792166; row.cell = 8; row.voxels_per_item = 16;
    KilnToolPolicyView policy{}; policy.size = sizeof policy; policy.mode = 2; policy.pool_count = 1; policy.pool = &row;
    std::array<unsigned char, 0x30> desc{}; desc[0x28] = 8;
    active = &policy;
    MaterialList list{};
    auto fresh = [&] { reset(); list = {storage.data(), 0, storage.size(), nullptr}; };
    Material history{};
    fresh(); bag = 2; nested_cost = true;
    check(perform(desc.data(), 17, list, history) == 2 && inner_ok && history.item == row.item,
          "nested payment invocation restores outer cost and policy");
    fresh(); bag = 2; threaded_cost = true;
    check(perform(desc.data(), 17, list, history) == 2 && inner_ok && vanilla_builds == 1 && history.item == row.item,
          "another thread cannot inherit a payment invocation");
    fresh(); bag = 2;
    check(perform(desc.data(), 17, list, history) == 2 && bag == 0 && debits == 1 && history.item == row.item,
          "bag-only, charged list is recorded exactly once");
    fresh(); magic = 2;
    check(perform(desc.data(), 17, list, history) == 2 && magic == 0 && debits == 1, "magic-only payment");
    fresh(); bag = magic = 1;
    check(perform(desc.data(), 17, list, history) == 2 && bag == 0 && magic == 0 && debits == 1, "combined payment once");
    fresh();
    check(perform(desc.data(), 17, list, history) == 0 && debits == 0 && voxels == 0 && ordinary == 100 && station == 100,
          "no supplies, ordinary chest and station outputs cannot pay");
    fresh(); bag = 1;
    check(perform(desc.data(), 17, list, history) == 1 && appends == 2 && history.count == 1 && list.count == 1,
          "fraction rebuild replaces full cost with actual reduced volume, no double debit");
    fresh(); bag = 2; forced_fraction = 0.25f;
    check(perform(desc.data(), 32, list, history) == 1 && history.count == 1, "actual changed volume rather than full brush volume");
    fresh(); bag = 2; append_fails = true;
    check(perform(desc.data(), 17, list, history) == 0 && debits == 0 && voxels == 0, "failed append refuses before charge and voxels");
    fresh(); bag = 2; partial_append_fails = true;
    check(perform(desc.data(), 17, list, history) == 0 && debits == 0 && voxels == 0 && list.count == 0,
          "append that writes a record before failing cannot leave a payable partial list");
    fresh(); bag = 1; append_fail_at = 2;
    check(perform(desc.data(), 17, list, history) == 0 && debits == 0 && voxels == 0 && list.count == 0,
          "failed reduced-volume append refuses, not a free partial edit");
    fresh(); bag = 2; apply_fails = true;
    check(perform(desc.data(), 17, list, history) == 0 && bag == 2 && debits == 0 && voxels == 0,
          "game payment failure has no modeled voxel mutation");
    fresh(); bag = 2; bypass_builder = true;
    check(perform(desc.data(), 17, list, history) == 0 && debits == 0 && voxels == 0,
          "a continuation that skips our cost builder refuses");
    fresh(); bag = 2; corrupt_final = true;
    check(perform(desc.data(), 17, list, history) == 0 && debits == 0 && voxels == 0,
          "a changed final cost list cannot make the edit free");
#ifdef _WIN32
    fresh(); bag = 2; append_faults = true;
    check(perform(desc.data(), 17, list, history) == 0 && debits == 0 && voxels == 0, "SEH append fault cannot produce a free edit");
    fresh(); bag = 2; partial_append_faults = true;
    check(perform(desc.data(), 17, list, history) == 0 && debits == 0 && voxels == 0 && list.count == 0,
          "SEH fault after a partial append discards the list");
    fresh(); bag = 2; wrapper_faults = true;
    check(perform(desc.data(), 17, list, history) == 0 && debits == 0 && voxels == 0 && list.count == 0,
          "SEH fault after a completed builder refuses before payment");
#endif
    fresh(); bag = 2; desc[0x28] = 9;
    check(perform(desc.data(), 17, list, history) == 0 && appends == 0 && voxels == 0, "stale non-pool selection refuses");
    desc[0x28] = 8;
    fresh(); bag = 2;
    check(perform(desc.data(), 0, list, history) == 0 && debits == 0 && appends == 0 && wrapper_calls == 0,
          "zero changed volume never reaches the builder");
    fresh(); bag = 1;
    check(perform(desc.data(), std::numeric_limits<float>::denorm_min(), list, history) == 1 && debits == 1 && voxels == 1,
          "even the smallest positive float volume requires a charged item");
    fresh(); bag = 2;
    KilnToolPoolEntry ambiguous[] = {row, row}; policy.pool = ambiguous; policy.pool_count = 2;
    check(perform(desc.data(), 17, list, history) == 0 && debits == 0 && voxels == 0,
          "ambiguous policy cells never let a caller choose its own price");
    policy.pool = &row; policy.pool_count = 1;
    fresh(); active = nullptr; bag = 2;
    check(perform(desc.data(), 16, list, history) == 1 && vanilla_builds == 1 && history.item == 999, "vanilla cost untouched");
    fresh(); active = &policy; policy.mode = 1; bag = 2;
    check(perform(desc.data(), 16, list, history) == 1 && vanilla_builds == 1 && history.item == 999, "remove policy does not override add costs");
    active = nullptr;
    auto saved_wrapper = wrapper; auto saved_builder = builder;
    revoke_order.clear();
    kiln_tools_payment_uninstall();
    check(!reported && wrapper == &vanilla_wrapper && builder == &vanilla_builder && revoke_order == "WBHO",
          "uninstall clears COST, revokes links in reverse order, then retires only the child owner");
    kiln_tools_payment_uninstall();
    check(!reported && revoke_order == "WBHO", "uninstall is idempotent");
    fresh(); bag = 2;
    check(saved_wrapper(&list, nullptr, nullptr, nullptr, nullptr, desc.data(), 16, nullptr) == 1 &&
          list.count == 1 && list.data[0].item == 999, "saved wrapper continuation remains usable after revocation");
    fresh();
    saved_builder(&list, nullptr, desc.data(), 16);
    check(list.count == 1 && list.data[0].item == 999, "saved cost continuation remains usable after revocation");
    check(kiln_tools_payment_install(&api, 1) && reported, "uninstall then reinstall accepts retained managed detours");
    kiln_tools_payment_uninstall();
    check(lifecycle_ok && owners_created == owners_retired, "all child owners are retired without touching the parent");
    std::printf("tool payment detours: %d passed, %d failed\n", total - failed, failed);
    return failed ? 1 : 0;
}

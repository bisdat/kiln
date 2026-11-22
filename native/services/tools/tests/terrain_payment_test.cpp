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
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace kiln_tools_payment;
const KilnToolPolicyView* active = nullptr;
uint32_t reported = 0;
CostFn builder = nullptr;
WrapperFn wrapper = nullptr;
std::array<Material, 8> storage{};
int vanilla_builds = 0, appends = 0, debits = 0, voxels = 0;
bool append_fails = false, append_faults = false, apply_fails = false, bypass_builder = false, corrupt_final = false;
int append_fail_at = 0;
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
    list->data[list->count] = *record;
    return &list->data[list->count++];
}
void vanilla_builder(void* raw, void*, void*, float volume) {
    ++vanilla_builds;
    Material m{}; m.item = 999; m.count = static_cast<uint32_t>(std::ceil(volume / 16));
    if (m.count) append(raw, &m);
}
float vanilla_wrapper(void* raw, void* op, void*, void*, void*, void* desc, float volume, void*) {
    if (!bypass_builder) builder(raw, op, desc, volume);
    auto* list = static_cast<MaterialList*>(raw);
    float fraction = list->count && list->data[0].count ?
        std::min(1.0f, static_cast<float>(bag + magic) / list->data[0].count) : 1.0f;
    if (forced_fraction >= 0) fraction = forced_fraction;
    if (fraction < 1) { list->count = 0; builder(raw, op, desc, volume * fraction); }
    if (corrupt_final && list->count) list->data[0].count = 0;
    return fraction;
}
KilnResult KILN_CALL raw_hook(KilnOwner, uintptr_t target, const KilnHookOrder*, void* detour, void** original, KilnHandle* h) {
    *original = reinterpret_cast<void*>(target); *h = target;
    if (target == reinterpret_cast<uintptr_t>(&vanilla_builder)) builder = reinterpret_cast<CostFn>(detour);
    else if (target == reinterpret_cast<uintptr_t>(&vanilla_wrapper)) wrapper = reinterpret_cast<WrapperFn>(detour);
    else return KILN_ERR_INVALID_ARGUMENT;
    return KILN_OK;
}
KilnResult KILN_CALL revoke(KilnHandle) { return KILN_OK; }
KilnHooksApi hooks{};
bool KILN_CALL feature(KilnStr) { return true; }
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
    storage = {}; vanilla_builds = appends = debits = voxels = 0;
    append_fails = append_faults = apply_fails = bypass_builder = corrupt_final = false;
    append_fail_at = 0; forced_fraction = -1; bag = magic = 0;
}
// An explicit engine model of payment AFTER the wrapper; ordinary/station stock never participates.
uint32_t perform(void* desc, float volume, MaterialList& list, Material& history) {
    float fraction = wrapper(&list, nullptr, nullptr, nullptr, nullptr, desc, volume, nullptr);
    if (!(fraction > 0) || !list.count) return 0;
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
    hooks.header = {sizeof hooks, 2}; hooks.raw_detour = &raw_hook; hooks.revoke = &revoke;
    KilnApi api{}; api.size = sizeof api; api.feature = &feature; api.symbol = &symbol; api.service = &service; api.constant = &constant;
    check(kiln_tools_payment_install(&api, 1) && reported, "both detours install before cost is available");

    KilnToolPoolEntry row{}; row.item = 1257792166; row.cell = 8; row.voxels_per_item = 16;
    KilnToolPolicyView policy{}; policy.size = sizeof policy; policy.mode = 2; policy.pool_count = 1; policy.pool = &row;
    std::array<unsigned char, 0x30> desc{}; desc[0x28] = 8;
    active = &policy;
    MaterialList list{};
    auto fresh = [&] { reset(); list = {storage.data(), 0, storage.size(), nullptr}; };
    Material history{};
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
#endif
    fresh(); bag = 2; desc[0x28] = 9;
    check(perform(desc.data(), 17, list, history) == 0 && appends == 0 && voxels == 0, "stale non-pool selection refuses");
    desc[0x28] = 8;
    fresh(); bag = 2;
    check(perform(desc.data(), 0, list, history) == 0 && debits == 0 && appends == 0, "zero changed volume is not charged");
    fresh(); active = nullptr; bag = 2;
    check(perform(desc.data(), 16, list, history) == 1 && vanilla_builds == 1 && history.item == 999, "vanilla cost untouched");
    fresh(); active = &policy; policy.mode = 1; bag = 2;
    check(perform(desc.data(), 16, list, history) == 1 && vanilla_builds == 1 && history.item == 999, "remove policy does not override add costs");
    std::printf("tool payment detours: %d passed, %d failed\n", total - failed, failed);
    return failed ? 1 : 0;
}

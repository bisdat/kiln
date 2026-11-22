// SPDX-License-Identifier: MIT
// Scope the game's terrain cost builder, not InventoryPay. The game applies this exact material list once, copies it
// into its building history, then mutates voxels. Missing hooks refuse at tool dispatch; failures here return zero
// from the cost wrapper BEFORE ApplyMaterials/history/voxel mutation. See docs/kiln/services/tool-payment.md.
#include "terrain_payment.h"
#include "terrain_cost_math.h"
#include "kiln_tools_policy.h"
#include "kiln_hook.hpp"
#include "services/kiln_caps.h"
#include <cstring>
#ifdef _MSC_VER
#include <windows.h>
#endif

namespace kiln_tools_payment {
namespace {
kiln::RawHook cost_hook, wrapper_hook;
AppendFn append_material = nullptr;
size_t cell_offset = 0;

// This is NOT a second tool-policy TLS: it is the stack of cost-wrapper calls on this thread. The only policy is the
// immutable snapshot provided by kiln.tools. Matching the list/op/description prevents unrelated nested costs from
// inheriting an outer wrapper's payment, and the stack is restored even on a nested call's refusal.
struct Invocation {
    void* list;
    void* operation;
    void* description;
    uint32_t item, rate;
    uint8_t cell;
    bool failed = false;
    uint32_t builds = 0;
    Invocation* previous;
};
thread_local Invocation* current = nullptr;
struct Scope {
    Invocation& invocation;
    explicit Scope(Invocation& i) : invocation(i) { current = &i; }
    ~Scope() { current = invocation.previous; }
};

bool is_add(const KilnToolPolicyView* policy) { return policy && policy->mode == 2; }

// Each SEH frame contains no destructor-bearing local. Never fall back to vanilla costs for a refused custom add.
bool selection(const KilnToolPolicyView* policy, void* description, uint8_t* cell, uint32_t* item, uint32_t* rate) {
#ifdef _MSC_VER
    __try {
#endif
        if (!policy || policy->size < sizeof(*policy) || !description || !policy->pool ||
            !policy->pool_count || policy->pool_count > 256) return false;
        *cell = *(static_cast<const uint8_t*>(description) + cell_offset);
        if (!*cell || *cell >= 128) return false;
        bool found = false;
        for (uint32_t i = 0; i < policy->pool_count; ++i) {
            const auto& entry = policy->pool[i];
            if (entry.cell != *cell) continue;
            if (found || !entry.item || !entry.voxels_per_item) return false;
            *item = entry.item; *rate = entry.voxels_per_item; found = true;
        }
        return found;
#ifdef _MSC_VER
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}
bool empty_list(void* raw) {
#ifdef _MSC_VER
    __try {
#endif
        if (!raw) return false;
        const auto* list = static_cast<const MaterialList*>(raw);
        return list->count == 0 && list->capacity <= 4096 && (!list->capacity || list->data);
#ifdef _MSC_VER
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}
void discard_list(void* raw) {
#ifdef _MSC_VER
    __try {
#endif
        if (raw) static_cast<MaterialList*>(raw)->count = 0;
#ifdef _MSC_VER
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
#endif
}
bool put_cost(Invocation* invocation, float volume) {
#ifdef _MSC_VER
    __try {
#endif
        if (!empty_list(invocation->list) ||
            *(static_cast<const uint8_t*>(invocation->description) + cell_offset) != invocation->cell) return false;
        uint32_t count = 0;
        if (!price(volume, invocation->rate, count)) return false;
        if (!count) return true;
        Material material{};
        material.item = invocation->item; material.count = count;
        // Append with the GAME'S allocator/callback, never an STL allocation in game-owned storage.
        Material* added = append_material(invocation->list, &material);
        const auto* list = static_cast<const MaterialList*>(invocation->list);
        return added && list->count == 1 && list->data && added == list->data &&
               std::memcmp(list->data, &material, sizeof material) == 0;
#ifdef _MSC_VER
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}
void cost_detour(void* list, void* operation, void* description, float volume) {
    Invocation* invocation = current;
    if (!invocation || invocation->list != list || invocation->operation != operation ||
        invocation->description != description) {
        cost_hook.original<CostFn>()(list, operation, description, volume);
        return;
    }
    ++invocation->builds;
    if (invocation->failed || !put_cost(invocation, volume)) invocation->failed = true;
}
bool final_cost(void* raw, uint32_t item, uint32_t count) {
#ifdef _MSC_VER
    __try {
#endif
        const auto* list = static_cast<const MaterialList*>(raw);
        if (!count) return list->count == 0;
        Material expected{}; expected.item = item; expected.count = count;
        return list->count == 1 && list->data && std::memcmp(list->data, &expected, sizeof expected) == 0;
#ifdef _MSC_VER
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}
bool call_wrapper(WrapperFn original, void* list, void* operation, void* actor, void* inventories, void* items,
                  void* description, float volume, void* sources, float* fraction) {
#ifdef _MSC_VER
    __try {
#endif
        *fraction = original(list, operation, actor, inventories, items, description, volume, sources);
        return true;
#ifdef _MSC_VER
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#endif
}
float wrapper_detour(void* list, void* operation, void* actor, void* inventories, void* items,
                     void* description, float volume, void* sources) {
    auto original = wrapper_hook.original<WrapperFn>();
    const KilnToolPolicyView* policy = kiln_tools_active_policy();
    if (!is_add(policy)) return original(list, operation, actor, inventories, items, description, volume, sources);
    uint8_t cell = 0;
    uint32_t item = 0, rate = 0, ignored = 0;
    if (!selection(policy, description, &cell, &item, &rate) || !price(volume, rate, ignored) || !empty_list(list))
        return 0;
    if (volume == 0) return 0;
    Invocation invocation{list, operation, description, item, rate, cell, false, 0, current};
    Scope scope(invocation);
    float fraction = 0;
    // The continuation only builds costs and asks for stock. The actual debit is in AddTerrain's later ApplyMaterials.
    // Its fraction<1 branch clears the list and calls our builder AGAIN on fraction*volume; do not cache the first cost.
    if (!call_wrapper(original, list, operation, actor, inventories, items, description, volume, sources, &fraction) ||
        invocation.failed || !invocation.builds || !std::isfinite(fraction) || fraction < 0 || fraction > 1 ||
        !price(fraction < 1 ? fraction * volume : volume, rate, ignored) ||
        (fraction > 0 && !ignored) || !final_cost(list, item, ignored)) {
        discard_list(list);
        return 0;
    }
    return fraction;
}

KilnStr text(const char* s) { return {s, std::strlen(s)}; }
void report(const KilnApi* api, KilnOwner owner, bool installed, const char* reason) {
    kiln_tools_report_hook(KILN_TOOL_HOOK_COST, installed ? 1 : 0);
    if (!api || !api->service) return;
    const auto* caps = static_cast<const KilnCapsApi*>(api->service(text(KILN_CAPS_SERVICE), KILN_CAPS_VERSION));
    if (caps && caps->header.size >= offsetof(KilnCapsApi, report) + sizeof(caps->report) && caps->report)
        caps->report(owner, text("build.paid_terrain"), installed ? KILN_CAP_AVAILABLE : KILN_CAP_UNAVAILABLE,
                     installed ? KILN_CAP_REASON_NONE : KILN_CAP_REASON_SYMBOL_MISSING, text(reason));
}
bool symbol(const KilnApi* api, const char* name, uintptr_t* out) {
    return api->symbol && api->symbol(text(name), out) == KILN_OK && *out;
}
}
}

extern "C" bool kiln_tools_payment_install(const KilnApi* api, KilnOwner owner) {
    using namespace kiln_tools_payment;
    uintptr_t cost = 0, wrapper = 0, append = 0;
    int64_t offset = -1;
    bool mapped = api && api->feature && api->feature(text("tools.paid_terrain")) &&
                  symbol(api, "tools/TerrainCostBuild", &cost) && symbol(api, "tools/TerrainCostScale", &wrapper) &&
                  symbol(api, "tools/MaterialListAppend", &append) && api->constant &&
                  api->constant(text("tools/cost.cell"), &offset) == KILN_OK && offset == 0x28;
    if (!mapped) {
        report(api, owner, false, "terrain cost builder, affordability wrapper, material-list append or cell layout missing");
        return false;
    }
    cell_offset = static_cast<size_t>(offset);
    append_material = reinterpret_cast<AppendFn>(append);
    if (!cost_hook.install(api, owner, cost, reinterpret_cast<void*>(&cost_detour), "tools/TerrainCostBuild") ||
        !wrapper_hook.install(api, owner, wrapper, reinterpret_cast<void*>(&wrapper_detour), "tools/TerrainCostScale")) {
        wrapper_hook.revoke(); cost_hook.revoke();
        report(api, owner, false, "terrain cost hooks could not both be installed; paid tool actions refuse");
        return false;
    }
    report(api, owner, true, "tool pool item/rate shapes the game's paid and history-recorded material list; bag then connected magic stock");
    return true;
}

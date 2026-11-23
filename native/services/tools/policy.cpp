// SPDX-License-Identifier: MIT
#include "internal.h"
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace kt {
struct ToolPolicySnapshot {
    KilnToolPolicyView view{};
    std::vector<KilnToolPoolEntry> pool;
};

namespace {
using Snapshot = std::shared_ptr<const ToolPolicySnapshot>;
using ToolPolicies = std::array<Snapshot, 2>;
using Bindings = std::unordered_map<uint32_t, ToolPolicies>;
std::atomic<std::shared_ptr<const Bindings>> bindings;
std::mutex binding_mutex;
std::array<std::atomic<bool>, KILN_TOOL_HOOK_COUNT> hooks{};

struct ThreadPolicy {
    const KilnToolPolicyView* active = nullptr;
    Snapshot lookup;
};
// Exactly one TLS instance, compiled only into kiln-tools.dll, shared by all of its hooks.
thread_local ThreadPolicy thread_policy;

Snapshot copy_snapshot(const KilnToolPolicyView& view) noexcept {
    if (view.size < sizeof(KilnToolPolicyView) || view.mode > KILN_TOOL_POLICY_ADD ||
        (view.pool_count && !view.pool))
        return {};
    try {
        auto snapshot = std::make_shared<ToolPolicySnapshot>();
        snapshot->view = view;
        snapshot->view.size = sizeof(KilnToolPolicyView);
        if (view.pool_count) snapshot->pool.assign(view.pool, view.pool + view.pool_count);
        snapshot->view.pool = snapshot->pool.empty() ? nullptr : snapshot->pool.data();
        return snapshot;
    } catch (...) {
        return {};
    }
}
} // namespace

ToolPolicyScope::ToolPolicyScope(const KilnToolPolicyView* snapshot) noexcept
    : previous_(thread_policy.active) {
    if (snapshot) {
        if (thread_policy.lookup && snapshot == &thread_policy.lookup->view)
            snapshot_ = thread_policy.lookup;
        else
            snapshot_ = copy_snapshot(*snapshot);
    }
    thread_policy.active = snapshot_ ? &snapshot_->view : nullptr;
}

ToolPolicyScope::~ToolPolicyScope() {
    thread_policy.active = previous_;
}

bool bind_tool_policy(uint32_t tool_item, uint32_t action, const KilnToolPolicyView& view) noexcept {
    if (!tool_item || action > KILN_TOOL_ACTION_SECONDARY) return false;
    auto snapshot = copy_snapshot(view);
    if (!snapshot) return false;
    try {
        std::lock_guard lock(binding_mutex);
        auto current = bindings.load(std::memory_order_acquire);
        auto next = current ? std::make_shared<Bindings>(*current) : std::make_shared<Bindings>();
        (*next)[tool_item][action] = std::move(snapshot);
        bindings.store(std::move(next), std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

bool unbind_tool(uint32_t tool_item) noexcept {
    if (!tool_item) return false;
    try {
        std::lock_guard lock(binding_mutex);
        auto current = bindings.load(std::memory_order_acquire);
        if (!current || current->find(tool_item) == current->end()) return false;
        auto next = std::make_shared<Bindings>(*current);
        next->erase(tool_item);
        bindings.store(std::move(next), std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

const KilnToolPolicyView* policy_for_tool(uint32_t tool_item, uint32_t action) noexcept {
    auto current = bindings.load(std::memory_order_acquire);
    Snapshot snapshot;
    if (current && action <= KILN_TOOL_ACTION_SECONDARY) {
        auto found = current->find(tool_item);
        if (found != current->end()) snapshot = found->second[action];
    }
    thread_policy.lookup = std::move(snapshot);
    return thread_policy.lookup ? &thread_policy.lookup->view : nullptr;
}
} // namespace kt

extern "C" const KilnToolPolicyView* kiln_tools_active_policy(void) {
    return kt::thread_policy.active;
}

extern "C" void kiln_tools_report_hook(uint32_t hook_id, int installed) {
    if (hook_id < KILN_TOOL_HOOK_COUNT)
        kt::hooks[hook_id].store(installed != 0, std::memory_order_release);
}

extern "C" int kiln_tools_hook_installed(uint32_t hook_id) {
    return hook_id < KILN_TOOL_HOOK_COUNT && kt::hooks[hook_id].load(std::memory_order_acquire);
}

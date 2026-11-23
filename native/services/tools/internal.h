// SPDX-License-Identifier: MIT
#pragma once
#include "include/kiln_tools_policy.h"
#include <memory>

namespace kt {
struct ToolPolicySnapshot;

// Pins an immutable snapshot for this game call and restores the previous active view on return/unwind.
// Construct and destroy on the same thread in stack order. A null view temporarily clears the active policy.
// Views from policy_for_tool are shared without copying; other value snapshots (including pools) are copied.
class ToolPolicyScope final {
public:
    explicit ToolPolicyScope(const KilnToolPolicyView* snapshot) noexcept;
    explicit ToolPolicyScope(const KilnToolPolicyView& snapshot) noexcept : ToolPolicyScope(&snapshot) {}
    ~ToolPolicyScope();
    ToolPolicyScope(const ToolPolicyScope&) = delete;
    ToolPolicyScope& operator=(const ToolPolicyScope&) = delete;
    ToolPolicyScope(ToolPolicyScope&&) = delete;
    ToolPolicyScope& operator=(ToolPolicyScope&&) = delete;

private:
    std::shared_ptr<const ToolPolicySnapshot> snapshot_;
    const KilnToolPolicyView* previous_;
};

// Copies the view and pool into an immutable snapshot, then atomically replaces this tool action's binding.
// action is KILN_TOOL_ACTION_PRIMARY / SECONDARY; the two policies are independent.
// Invalid input or allocation failure returns false without replacing the existing binding. Any thread.
bool bind_tool_policy(uint32_t tool_item, uint32_t action, const KilnToolPolicyView& snapshot) noexcept;

// Atomically removes both actions. False if absent or allocation fails; admitted scopes remain pinned. Any thread.
bool unbind_tool(uint32_t tool_item) noexcept;

// Any thread. The lookup pins its result until the next lookup on this thread; construct a ToolPolicyScope
// immediately to keep it pinned across subsequent lookups/replacements. Never retain the returned pointer.
const KilnToolPolicyView* policy_for_tool(uint32_t tool_item, uint32_t action) noexcept;
} // namespace kt

// SPDX-License-Identifier: MIT
#include "internal.h"
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); \
} } while (false)

namespace {
KilnToolPolicyView policy(uint32_t id = 1) {
    KilnToolPolicyView view{};
    view.size = sizeof(view);
    view.mode = KILN_TOOL_POLICY_REMOVE;
    view.policy_id = id;
    return view;
}

void inactive_outside_scope() {
    CHECK(kiln_tools_active_policy() == nullptr);
    auto view = policy();
    {
        kt::ToolPolicyScope scope(view);
        CHECK(kiln_tools_active_policy()->policy_id == 1);
    }
    CHECK(kiln_tools_active_policy() == nullptr);
}

void nested_scopes_restore() {
    auto first = policy(11), second = policy(22);
    kt::ToolPolicyScope outer(first);
    const auto* original = kiln_tools_active_policy();
    {
        kt::ToolPolicyScope inner(second);
        CHECK(kiln_tools_active_policy()->policy_id == 22);
        {
            kt::ToolPolicyScope disabled(nullptr);
            CHECK(kiln_tools_active_policy() == nullptr);
        }
        CHECK(kiln_tools_active_policy()->policy_id == 22);
    }
    CHECK(kiln_tools_active_policy() == original);
}

void snapshot_owns_pool() {
    KilnToolPoolEntry pool[] = {{1001, 91, {}, 12}};
    auto view = policy(31);
    view.mode = KILN_TOOL_POLICY_ADD;
    view.pool_count = 1;
    view.pool = pool;
    CHECK(kt::bind_tool_policy(100, 0, view));
    pool[0].item = 9999;
    view.policy_id = 9999;
    kt::ToolPolicyScope scope(kt::policy_for_tool(100, 0));
    const auto* active = kiln_tools_active_policy();
    CHECK(active && active->policy_id == 31);
    CHECK(active->pool_count == 1);
    CHECK(active->pool != pool);
    CHECK(active->pool[0].item == 1001);
    CHECK(active->pool[0].cell == 91);
    CHECK(active->pool[0].voxels_per_item == 12);
}

void swap_preserves_active_snapshot() {
    KilnToolPoolEntry old_pool[] = {{2001, 3, {}, 16}};
    auto old_view = policy(41);
    old_view.pool_count = 1;
    old_view.pool = old_pool;
    CHECK(kt::bind_tool_policy(200, 0, old_view));
    kt::ToolPolicyScope scope(kt::policy_for_tool(200, 0));
    const auto* held = kiln_tools_active_policy();
    std::thread writer([] {
        KilnToolPoolEntry new_pool[] = {{2002, 4, {}, 32}};
        auto replacement = policy(42);
        replacement.pool_count = 1;
        replacement.pool = new_pool;
        CHECK(kt::bind_tool_policy(200, 0, replacement));
    });
    writer.join();
    const auto* replacement = kt::policy_for_tool(200, 0);
    CHECK(replacement && replacement->policy_id == 42);
    CHECK(replacement->pool[0].item == 2002);
    CHECK(kiln_tools_active_policy() == held);
    CHECK(held->policy_id == 41 && held->pool[0].item == 2001);
    {
        kt::ToolPolicyScope newer(replacement);
        CHECK(kiln_tools_active_policy()->policy_id == 42);
    }
    CHECK(kiln_tools_active_policy() == held);
    CHECK(held->pool[0].voxels_per_item == 16);
}

void scopes_are_thread_local() {
    auto view = policy(51);
    kt::ToolPolicyScope scope(view);
    const auto* held = kiln_tools_active_policy();
    std::thread other([] {
        CHECK(kiln_tools_active_policy() == nullptr);
        auto local = policy(52);
        {
            kt::ToolPolicyScope scope(local);
            CHECK(kiln_tools_active_policy()->policy_id == 52);
        }
        CHECK(kiln_tools_active_policy() == nullptr);
    });
    other.join();
    CHECK(kiln_tools_active_policy() == held && held->policy_id == 51);
}

void cell_membership() {
    auto view = policy();
    view.remove_cells[0] = 0x81;
    view.remove_cells[11] = 0x08; // cell 91
    view.remove_cells[31] = 0x80;
    for (uint32_t cell = 0; cell < 256; ++cell)
        CHECK(kiln_tools_cell_removable(&view, cell) ==
              (cell == 0 || cell == 7 || cell == 91 || cell == 255));
    CHECK(!kiln_tools_cell_removable(nullptr, 0));
    CHECK(!kiln_tools_cell_removable(&view, 256));
    CHECK(!kiln_tools_cell_removable(&view, UINT32_MAX));
}

void hook_status() {
    for (uint32_t id = 0; id < KILN_TOOL_HOOK_COUNT; ++id) {
        CHECK(!kiln_tools_hook_installed(id));
        std::thread reporter([id] { kiln_tools_report_hook(id, 1); });
        reporter.join();
        CHECK(kiln_tools_hook_installed(id));
        kiln_tools_report_hook(id, 0);
        CHECK(!kiln_tools_hook_installed(id));
    }
    kiln_tools_report_hook(KILN_TOOL_HOOK_COUNT, 1);
    kiln_tools_report_hook(UINT32_MAX, 1);
    CHECK(!kiln_tools_hook_installed(KILN_TOOL_HOOK_COUNT));
    CHECK(!kiln_tools_hook_installed(UINT32_MAX));
}

void actions_are_independent() {
    auto primary = policy(71), secondary = policy(72);
    primary.remove_cells[0] = 0x01;
    secondary.mode = KILN_TOOL_POLICY_ADD;
    secondary.remove_cells[1] = 0x02;
    CHECK(kt::bind_tool_policy(400, 0, primary));
    CHECK(kt::bind_tool_policy(400, 1, secondary));
    kt::ToolPolicyScope first(kt::policy_for_tool(400, 0));
    const auto* active = kiln_tools_active_policy();
    CHECK(active->mode == KILN_TOOL_POLICY_REMOVE && active->policy_id == 71);
    CHECK(kiln_tools_cell_removable(active, 0));
    CHECK(!kiln_tools_cell_removable(active, 9));
    {
        kt::ToolPolicyScope second(kt::policy_for_tool(400, 1));
        active = kiln_tools_active_policy();
        CHECK(active->mode == KILN_TOOL_POLICY_ADD && active->policy_id == 72);
        CHECK(!kiln_tools_cell_removable(active, 0));
        CHECK(kiln_tools_cell_removable(active, 9));
    }
    primary.policy_id = 73;
    CHECK(kt::bind_tool_policy(400, 0, primary));
    CHECK(kt::policy_for_tool(400, 0)->policy_id == 73);
    CHECK(kt::policy_for_tool(400, 1)->policy_id == 72);
    CHECK(kiln_tools_active_policy()->policy_id == 71);
}

void unbind_removes_both_actions() {
    auto view = policy(81);
    CHECK(kt::bind_tool_policy(500, 0, view));
    CHECK(kt::bind_tool_policy(500, 1, view));
    kt::ToolPolicyScope held(kt::policy_for_tool(500, 0));
    CHECK(kt::unbind_tool(500));
    CHECK(kt::policy_for_tool(500, 0) == nullptr);
    CHECK(kt::policy_for_tool(500, 1) == nullptr);
    CHECK(!kt::unbind_tool(500));
    CHECK(!kt::unbind_tool(0));
    CHECK(kiln_tools_active_policy()->policy_id == 81);
}

void invalid_bindings_refuse() {
    CHECK(kt::policy_for_tool(999, 0) == nullptr);
    auto view = policy(61);
    CHECK(!kt::bind_tool_policy(0, 0, view));
    CHECK(!kt::bind_tool_policy(300, 2, view));
    CHECK(!kt::bind_tool_policy(300, UINT32_MAX, view));
    CHECK(kt::policy_for_tool(300, 2) == nullptr);
    CHECK(kt::policy_for_tool(300, UINT32_MAX) == nullptr);
    CHECK(kt::bind_tool_policy(300, 0, view));
    view.size = 0;
    CHECK(!kt::bind_tool_policy(300, 0, view));
    view = policy(62);
    view.pool_count = 1;
    CHECK(!kt::bind_tool_policy(300, 0, view));
    view = policy(62);
    view.mode = 3;
    CHECK(!kt::bind_tool_policy(300, 0, view));
    kt::ToolPolicyScope scope(kt::policy_for_tool(300, 0));
    CHECK(kiln_tools_active_policy()->policy_id == 61);
}
}

int main() {
    inactive_outside_scope();
    nested_scopes_restore();
    snapshot_owns_pool();
    swap_preserves_active_snapshot();
    scopes_are_thread_local();
    cell_membership();
    hook_status();
    actions_are_independent();
    unbind_removes_both_actions();
    invalid_bindings_refuse();
    CHECK(kiln_tools_active_policy() == nullptr);
    std::puts("tools policy: 10 tests passed");
}

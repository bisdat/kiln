/* SPDX-License-Identifier: MIT */
#include "kiln_tools_policy.h"
#include <stddef.h>

_Static_assert(sizeof(KilnToolPoolEntry) == 12, "pool entry layout");
_Static_assert(offsetof(KilnToolPoolEntry, voxels_per_item) == 8, "pool rate layout");
_Static_assert(offsetof(KilnToolPolicyView, remove_cells) == 16, "cell set layout");
_Static_assert(KILN_TOOL_POLICY_NONE == 0 && KILN_TOOL_POLICY_REMOVE == 1 &&
               KILN_TOOL_POLICY_ADD == 2, "policy mode values");
_Static_assert(KILN_TOOL_HOOK_COUNT == 10, "hook count");
_Static_assert(KILN_TOOL_ACTION_PRIMARY == 0 && KILN_TOOL_ACTION_SECONDARY == 1, "action values");

int main(void) {
    KilnToolPolicyView view = {0};
    view.size = sizeof(view);
    view.remove_cells[31] = 0x80;
    return !(kiln_tools_cell_removable(&view, 255) && !kiln_tools_cell_removable(&view, 254));
}

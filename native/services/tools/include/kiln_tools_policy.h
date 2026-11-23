/* SPDX-License-Identifier: MIT */
#ifndef KILN_TOOLS_POLICY_H
#define KILN_TOOLS_POLICY_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum KilnToolPolicyAction {
    KILN_TOOL_ACTION_PRIMARY = 0,
    KILN_TOOL_ACTION_SECONDARY = 1
} KilnToolPolicyAction;

typedef enum KilnToolPolicyMode {
    KILN_TOOL_POLICY_NONE = 0,
    KILN_TOOL_POLICY_REMOVE = 1,
    KILN_TOOL_POLICY_ADD = 2
} KilnToolPolicyMode;

typedef struct KilnToolPoolEntry {
    uint32_t item;
    uint8_t cell;
    uint8_t reserved[3];
    uint32_t voxels_per_item;
} KilnToolPoolEntry;

typedef struct KilnToolPolicyView {
    uint32_t size;
    uint32_t mode; /* KILN_TOOL_POLICY_* */
    uint32_t policy_id;
    uint32_t flags;
    uint8_t remove_cells[32]; /* Bit n permits terrain cell n. */
    uint32_t pool_count;
    const KilnToolPoolEntry* pool; /* Owned by the immutable snapshot. */
} KilnToolPolicyView;

static inline int kiln_tools_cell_removable(const KilnToolPolicyView* view, uint32_t cell) {
    return view && cell < 256u &&
           (view->remove_cells[cell >> 3u] & (1u << (cell & 7u))) != 0;
}

/* Private to the tools service: the view (including pool) is valid only during the hooked game call.
 * It must never be stored. Outside a scoped invocation this returns null. */
const KilnToolPolicyView* kiln_tools_active_policy(void);

typedef enum KilnToolHookId {
    KILN_TOOL_HOOK_KERNEL_FILTER = 0,
    KILN_TOOL_HOOK_BUILDING_SUPPRESS,
    KILN_TOOL_HOOK_COST,
    KILN_TOOL_HOOK_MATERIAL_LOOKUP,
    KILN_TOOL_HOOK_SELECTOR_BUILDER,
    KILN_TOOL_HOOK_PREVIEW,
    KILN_TOOL_HOOK_BRUSH_PICKER,
    KILN_TOOL_HOOK_IDENTITY,
    KILN_TOOL_HOOK_EDGE_CHECK,
    KILN_TOOL_HOOK_HISTORY_COST,
    KILN_TOOL_HOOK_COUNT
} KilnToolHookId;

/* Any thread. Unknown hook ids are ignored and query as not installed. */
void kiln_tools_report_hook(uint32_t hook_id, int installed);
int kiln_tools_hook_installed(uint32_t hook_id);

#ifdef __cplusplus
}
#endif
#endif

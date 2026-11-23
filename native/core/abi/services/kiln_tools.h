/* SPDX-License-Identifier: MIT
 * kiln.tools v1. Since 0.2.0-dev.30.5.
 * Building-tool actions use the game's building-mode input and repeat cadence, never global input hooks.
 * This release supports declarative family policies only. Immutable native snapshots run on authoritative
 * simulation workers; no managed or mod callback is invoked in an engine hook. The game's own terraform operation
 * retains its permission, payment, mining-yield and undo machinery. Missing context or required hooks refuse the
 * action; never fall back to generic digging. The callback registration slots are reserved and unavailable.
 */
#ifndef KILN_TOOLS_H
#define KILN_TOOLS_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif
#define KILN_TOOLS_SERVICE "kiln.tools"
#define KILN_TOOLS_VERSION 1u
#define KILN_TOOL_PRIMARY 0u
#define KILN_TOOL_SECONDARY 1u
#define KILN_TOOL_COMMAND_REFUSE 0u
#define KILN_TOOL_COMMAND_NONE 1u
#define KILN_TOOL_COMMAND_CARVE 2u
#define KILN_TOOL_COMMAND_PLACE 3u
#define KILN_TOOL_REPEAT_HELD 1u

typedef struct KilnToolsStatus {
    uint32_t size;
    uint8_t actions;             /* server context, program binding and command backend qualified */
    uint8_t session;             /* authoritative world is running */
    uint8_t reserved[2];
    uint32_t reason;             /* KILN_CAP_REASON_* */
    uint32_t registrations;
    uint64_t dispatched;
    uint64_t refused;
} KilnToolsStatus;

/* Owned value snapshot; no borrowed game storage. Selection is resolved by the server against the tool's pool.
 * invocation is unique within session; held repeats have distinct invocations at the game's action cadence. */
typedef struct KilnToolActionContext {
    uint32_t size;
    uint32_t action;             /* KILN_TOOL_PRIMARY / SECONDARY */
    uint64_t session;
    uint64_t invocation;
    KilnEntity actor;
    uint32_t tool;
    uint32_t brush;
    uint32_t selected_item;      /* 0: no selection */
    uint32_t selected_material;  /* terrain cell identity; 0 without selection */
    uint32_t reserved;
    KilnVec3 hit;
    double radius;
} KilnToolActionContext;

/* Only a kind is chosen by mod code. Actor, hit, shape, filter, material, payment, access and undo policy always
 * come from the authoritative context and bound tool/brush/pool definitions, never from a client or command. */
typedef struct KilnToolActionCommand {
    uint32_t size;
    uint32_t kind;               /* KILN_TOOL_COMMAND_*; unknown kinds refuse */
} KilnToolActionCommand;

typedef KilnResult(KILN_CALL* KilnToolActionFn)(const KilnToolActionContext* context,
                                              KilnToolActionCommand* command, void* user);
typedef struct KilnToolActionDef {
    uint32_t size;
    uint32_t tool;
    uint32_t action;             /* KILN_TOOL_PRIMARY / SECONDARY */
    uint32_t flags;              /* KILN_TOOL_REPEAT_HELD or 0; cadence is always the game's */
    uint32_t program;            /* Kiln-owned impact program identity */
    uint32_t sequence;           /* Kiln-owned action sequence identity */
    KilnToolActionFn handler;
    void* user;
} KilnToolActionDef;

/* Declarative family values. Every pointer/string is copied before define_family returns. */
#define KILN_TOOL_MAX_MATERIALS 64u
#define KILN_TOOL_SHAPE_SPHERE 0u
#define KILN_TOOL_MODE_NONE 0u
#define KILN_TOOL_MODE_REMOVE 1u
#define KILN_TOOL_MODE_ADD 2u
#define KILN_TOOL_TERRAIN_ONLY 1u

typedef struct KilnToolPoolMaterial {
    uint32_t item;
    uint32_t terrain_material;   /* 1..127; never a building cell */
    uint32_t voxels_per_item;    /* 1..1000000 */
    uint32_t reserved;
} KilnToolPoolMaterial;
typedef struct KilnToolBrush {
    uint32_t item;
    uint32_t shape;              /* KILN_TOOL_SHAPE_SPHERE */
    double radius;              /* metres, finite 0.25..8 */
} KilnToolBrush;
typedef struct KilnToolFamilyAction {
    uint32_t mode;               /* KILN_TOOL_MODE_* */
    uint32_t program;            /* Kiln-owned, unique per tool/action */
    uint8_t remove_cells[32];    /* removal only; terrain cells 1..127 */
    uint32_t flags;              /* KILN_TOOL_TERRAIN_ONLY */
    uint32_t reserved;
} KilnToolFamilyAction;
typedef struct KilnToolFamilyDef {
    uint32_t size;
    uint32_t tool;
    uint32_t pool;
    uint32_t reserved;
    KilnToolFamilyAction primary;
    KilnToolFamilyAction secondary;
    KilnStr name;
    KilnStr description;
    const KilnToolPoolMaterial* materials;
    uint32_t material_count;
    uint32_t reserved2;
    const KilnToolBrush* brushes;
    uint32_t brush_count;
    uint32_t reserved3;
    uint8_t identity[16];        /* root item GUID in game/.NET byte order; FNV-1a must equal tool */
} KilnToolFamilyDef;
typedef struct KilnToolMaterialRow {
    uint32_t item;
    uint32_t terrain_material;
    uint32_t voxels_per_item;
    uint32_t reserved;
    uint64_t backpack;
    uint64_t magic_chests;       /* connected game building stock, never ordinary chests */
} KilnToolMaterialRow;
/* Caller-owned, bounded snapshot. No pointer into game data or an internal/stack array escapes. */
typedef struct KilnToolMaterialSnapshot {
    uint32_t size;
    uint32_t count;
    uint64_t session;
    uint64_t revision;
    KilnEntity actor;
    uint32_t tool;
    uint32_t selected_item;      /* 0 when stale, out of pool or empty stock */
    uint32_t selected_material;
    KilnToolMaterialRow rows[64];
} KilnToolMaterialSnapshot;
typedef struct KilnToolPresentationStatus {
    uint32_t size;
    uint8_t picker;
    uint8_t preview;
    uint8_t brushes;
    uint8_t identity;
    uint32_t reason;             /* KILN_CAP_REASON_* */
    uint32_t reserved;
} KilnToolPresentationStatus;

typedef struct KilnToolsApi {
    KilnServiceHeader header;    /* { sizeof(KilnToolsApi), KILN_TOOLS_VERSION } */
    /* Any thread. Set out->size first. */
    KilnResult(KILN_CALL* status)(KilnToolsStatus* out);
    /* Reserved ABI slot. Always KILN_ERR_UNAVAILABLE, *out=0: declarative tool actions only in this release.
     * No callback is retained or invoked, on any thread. tools.callbacks reports POLICY with that reason. */
    KilnResult(KILN_CALL* register_action)(KilnOwner owner, const KilnToolActionDef* definition, KilnHandle* out);
    /* Reserved ABI slot. No arbitrary handler registrations exist in this release; KILN_ERR_NOT_FOUND. */
    KilnResult(KILN_CALL* unregister_action)(KilnOwner owner, KilnHandle handle);
    /* Tick thread/before first tick. Publishes one immutable declarative family and its two policies. Copies all
     * inputs. Refuses if the complete required engine path is unavailable. No partial publication or generic fallback. */
    KilnResult(KILN_CALL* define_family)(KilnOwner owner, const KilnToolFamilyDef* definition, KilnHandle* out);
    /* Tick thread/before first tick. Stops new family admissions; already pinned invocations finish safely. */
    KilnResult(KILN_CALL* remove_family)(KilnOwner owner, KilnHandle handle);
    /* Any thread; observation only, never payment/edit authority. Set out->size. Unavailable clears all values. */
    KilnResult(KILN_CALL* material_snapshot)(KilnEntity actor, uint32_t tool, KilnToolMaterialSnapshot* out);
    /* Any thread. Set out->size. Installed hooks do not imply an active session or available stock. */
    KilnResult(KILN_CALL* presentation_status)(KilnToolPresentationStatus* out);
    /* Append new rows here. */
} KilnToolsApi;
#ifdef __cplusplus
}
#endif
#endif

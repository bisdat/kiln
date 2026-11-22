/* SPDX-License-Identifier: MIT
 * kiln.tools v1. Since 0.2.0-dev.30.5.
 * Building-tool actions use the game's building-mode input and repeat cadence, never global input hooks.
 * Dispatch is synchronous on the authoritative simulation thread (possibly a worker, NOT the tick thread).
 * A handler must only read the immutable snapshot and return a command: no tick-only APIs, blocking, async work,
 * game pointers, inventory changes or world writes. Commands are validated and committed by Kiln on the server.
 * Missing engine context, permissions, payment or history mappings refuse; never fall back to generic digging.
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

typedef struct KilnToolsApi {
    KilnServiceHeader header;    /* { sizeof(KilnToolsApi), KILN_TOOLS_VERSION } */
    /* Any thread. Set out->size first. */
    KilnResult(KILN_CALL* status)(KilnToolsStatus* out);
    /* Tick thread/before first tick. Copies definition. Owner must be live; nonzero unique tool/action binding.
     * Unavailable unless the whole engine path is qualified. No partial or generic fallback. */
    KilnResult(KILN_CALL* register_action)(KilnOwner owner, const KilnToolActionDef* definition, KilnHandle* out);
    /* Any thread. Closes admission. An already admitted callback remains pinned until it returns; the caller must
     * not free handler/user storage until its owning mod's unload barrier has drained. */
    KilnResult(KILN_CALL* unregister_action)(KilnOwner owner, KilnHandle handle);
    /* Append new rows here. */
} KilnToolsApi;
#ifdef __cplusplus
}
#endif
#endif

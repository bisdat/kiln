/* SPDX-License-Identifier: MIT
 * Client HUD ammunition lists. Since 0.2.0-dev.30.6 (mod API 0.2.15).
 * Served by kiln-tools through the existing checked selector-builder chain, not a second selector detour.
 * Native hooks read owned immutable snapshots only. No callback into a mod occurs inside an engine hook.
 */
#ifndef KILN_UI_AMMO_H
#define KILN_UI_AMMO_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif
#define KILN_UI_AMMO_SERVICE "kiln.ui.ammo"
#define KILN_UI_AMMO_VERSION 1u
#define KUA_CAPACITY 64u
/* Provide may expand flags in managed code; each native call accepts exactly one kind. */
enum { KUA_KIND_ARROW=1, KUA_KIND_SPELL=2 };
enum { KUA_MODE_GAME=0, KUA_MODE_APPEND=1, KUA_MODE_REPLACE=2 };
/* Context diagnostics, separate from KILN_CAP_REASON_* on ui.ammo.list / ui.ammo.pick. */
enum {
    KUA_REASON_NONE=0,
    KUA_REASON_NO_SESSION=1,
    KUA_REASON_NO_CONTEXT=2,
    KUA_REASON_LIMIT=3,       /* Only the first 64 composed rows fit. Truncation is never silent. */
    KUA_REASON_STALE=4,
    KUA_REASON_FAULT=5,
    KUA_REASON_CATALOG=6    /* No owned, validated client item catalog has been observed yet. */
};
#define KUA_ERR_BUSY 200u   /* Another live owner owns this kind. */
typedef struct KilnUiAmmoRow {
    uint32_t item;
    uint32_t count;          /* Shown count; not inventory authority. */
    uint32_t flags;          /* Reserved, must be zero. */
    uint32_t row;            /* Publisher's opaque row key, echoed in picks. */
} KilnUiAmmoRow;
typedef struct KilnUiAmmoPublish {
    uint32_t size,kind,mode,count;
    uint32_t session,reserved;
    uint64_t serial;         /* Must equal the context's game_serial, not a free-running mod counter. */
    const KilnUiAmmoRow* rows;
} KilnUiAmmoPublish;
typedef struct KilnUiAmmoContext {
    uint32_t size,ok,reason,kind;
    uint32_t session,local_player,held_item,game_count;
    uint64_t game_serial;    /* Changes with the held item, vanilla rows/sources/counts, actor or session. */
    KilnUiAmmoRow game_rows[KUA_CAPACITY];
} KilnUiAmmoContext;
typedef struct KilnUiAmmoPick {
    uint32_t size,kind,item,count;
    uint32_t row,session;
    uint64_t step;
} KilnUiAmmoPick;
typedef struct KilnUiAmmoApi {
    KilnServiceHeader header;
    /* Client main/UI thread. Copies all rows before return; publishes an immutable snapshot. One owner per kind.
     * GAME clears the caller's snapshot (session/serial ignored). A set requires a current context and known client
     * item IDs of the requested ammunition kind. No list is published without the qualified pick-suppression seam.
     * INVALID_ARGUMENT: malformed/duplicate/unknown item, bad kind/mode, count >64 or stale serial/session;
     * KUA_ERR_BUSY: another owner; REVOKED: owner retired; UNAVAILABLE: capability off; NOT_FOUND: no context yet. */
    KilnResult(KILN_CALL* publish)(KilnOwner owner,const KilnUiAmmoPublish* value);
    /* Any thread, owned copy, set out->size. OK includes diagnostics (notably LIMIT); NOT_FOUND before a live
     * builder observation. UNAVAILABLE on an unqualified build. Never returns game pointers. */
    KilnResult(KILN_CALL* context)(KilnOwner owner,uint32_t kind,KilnUiAmmoContext* out);
    /* Client main/UI thread. Drains this owner's bounded FIFO, at most capacity<=64; OK with *written=0 when empty.
     * Overflow drops the oldest wish. Only mod rows without an actual backpack stack produce wishes. Kiln suppresses
     * the native equip request for them, performs no server action, and does not treat a wish as authority.
     * Owner retirement/world stop clears snapshots and queued wishes. The caller must discard old-session picks. */
    KilnResult(KILN_CALL* take_picks)(KilnOwner owner,KilnUiAmmoPick* out,uint32_t capacity,uint32_t* written);
} KilnUiAmmoApi;
#ifdef __cplusplus
}
#endif
#endif

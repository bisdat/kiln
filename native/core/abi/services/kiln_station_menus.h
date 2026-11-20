/* SPDX-License-Identifier: MIT
 * kiln.stationmenus v1 (since 0.2.0-dev.31; in kiln.content's DLL, a table of its own): the crafting-menu layouts of mods'
 * own workshops, "station menu tabs" (docs/kiln/modding/world-blocks.md "Menus with tabs of their own"; design:
 * dev31/out/16a-station-tabs/DESIGN.md, chapter 16a).
 *
 * A layout is one normalized record (format 1, canonical JSON; the shape is in the docs and DESIGN.md section 2): the own
 * workshop it lays out (origin), the definition it belongs to (definition.id), the menu it is like (baseWorkshopId) and
 * its tabs, sets and recipe ids. One parser and one serializer are used by define, export and remote_replace: a record
 * reads back byte for byte. Unknown members, an unknown format, a workshop id that is not FNV-1a-32 of the workshop GUID,
 * duplicate ids, a first set with a heading, more than 15 tabs / 28 sets per tab / 512 recipes per tab, a text map with
 * more than 35 languages or a text outside 1..4096 UTF-8 bytes are refused (KILN_ERR_INVALID_ARGUMENT, report says why).
 *
 * Layers. One per (owner, definition id): define by the same owner with the same definition id replaces it (identical
 * canonical bytes: nothing changes, the same handle and generation come back). Per workshop the layer accepted last wins;
 * removing it shows the one before, else the game's own layout. A remote collection (a joined client: the server's
 * snapshot, remote_replace) is above every local layer, per workshop, until remote_clear. Limits: 256 local layers,
 * 1 MiB per record, 64 MiB of records in all. Layers of a revoked owner go (checked at every call and every ~1 s).
 *
 * Generation: one u64 per process for the local layers, never 0 once something was defined; +1 per effective change
 * (a new or changed layer, a removal), never for an identical define. The remote collection keeps the server's own
 * generation (info.remote_generation).
 *
 * Drawing (client, capability stationmenus.render = kiln.content's content.recipe_menu hook plus the layout check): the
 * crafting menu of a workshop with an effective layout is built from it in the existing crafting-menu hook. Icons are the
 * icon of tab iconFromTab of the base workshop's original tree (out of range: refused, IconSourceMissing, the last good
 * layout stays); labels are Kiln loca tags answering the record's texts. While set_pending(1), a mod's own workshop lists
 * nothing. Each materialization (or refusal) is queued for drain_applied: never an event from inside the hook.
 *
 * Every reader checks header.size; the table is append-only. Threads: any (internally locked); the managed facade
 * serializes ownership on the tick thread. */
#ifndef KILN_STATION_MENUS_H
#define KILN_STATION_MENUS_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_STATION_MENUS_SERVICE "kiln.stationmenus"
#define KILN_STATION_MENUS_VERSION 1u

typedef struct KilnStationMenusInfo {
    uint32_t size;              /* in: sizeof(KilnStationMenusInfo) */
    uint32_t store;             /* 1: layers can be stored here (stationmenus.store) */
    uint32_t render;            /* 1: this process draws layouts (stationmenus.render): a game client with the menu hook */
    uint32_t render_reason;     /* KILN_CAP_REASON_* when render is 0 */
    uint64_t generation;        /* the local collection's generation (0: nothing defined yet) */
    uint64_t remote_generation; /* the remote collection's (server's) generation; 0: none */
    uint32_t layers;            /* local layers stored, inactive ones too */
    uint32_t effective;         /* workshops with an effective layout (local and remote) */
    uint32_t remote;            /* 1: a remote collection is in place (it may be empty: the tombstone) */
    uint32_t pending;           /* 1: set_pending(1): own workshops list nothing */
    uint32_t materialized;      /* distinct menus built so far (all workshops) */
    uint32_t reserved;
    char detail[160];           /* render's reason in words, NUL-terminated */
} KilnStationMenusInfo;

typedef struct KilnStationMenusApi {
    KilnServiceHeader header; /* { sizeof(KilnStationMenusApi), KILN_STATION_MENUS_VERSION }; append-only */

    KilnResult(KILN_CALL* info)(KilnStationMenusInfo* out);
    /* Stores `record` as owner's layer for its definition id. *out: the layer's handle (the same for a replacement);
     * *out_generation: the local generation after the call. report (optional): JSON {"ok", "state": "defined" |
     * "replaced" | "unchanged", "error"?} (NUL-terminated, cut at cap). INVALID_ARGUMENT: a bad record, owner 0, or a
     * limit (256 layers, 64 MiB; the report says which); nothing changed. REVOKED: owner revoked. */
    KilnResult(KILN_CALL* define)(KilnOwner owner, KilnStr record, KilnHandle* out, uint64_t* out_generation,
                                  char* report, size_t cap, size_t* out_len);
    /* Removes a layer. NOT_FOUND: no such handle (a second remove); INVALID_ARGUMENT: another owner's layer. */
    KilnResult(KILN_CALL* remove)(KilnOwner caller, KilnHandle handle, uint64_t* out_generation);
    /* The effective local layouts: {"format":1,"generation":N,"menus":[records sorted by workshopId]}. *out_len excludes
     * the NUL; capacity must be at least *out_len + 1 (out NULL / cap 0: a sizing query, BUFFER_TOO_SMALL with the length). */
    KilnResult(KILN_CALL* export_layouts)(char* out, size_t cap, size_t* out_len);
    /* Replaces the whole remote collection with `snapshot` ({"format":1,"generation":N,"menus":[records]}): every record
     * is checked first; a duplicate definition id or workshop refuses the whole snapshot (nothing changes). menus: [] is
     * the tombstone. remote_owner: the service's own owner for the server's menus. */
    KilnResult(KILN_CALL* remote_replace)(KilnOwner remote_owner, KilnStr snapshot, char* report, size_t cap, size_t* out_len);
    /* No remote collection any more (left the server): local layers show again. */
    KilnResult(KILN_CALL* remote_clear)(KilnOwner remote_owner);
    /* on 1: a mod's own workshops list nothing (waiting for the server's menus); 0: as their layouts say. */
    KilnResult(KILN_CALL* set_pending)(uint32_t on);
    /* What the menu hook built or refused since the last call, oldest first: a JSON array of {"definition": guid,
     * "workshop": u32, "generation": u64, "fromServer": bool, "ok": bool, "reason": text}. At most 256 are kept. */
    KilnResult(KILN_CALL* drain_applied)(char* out, size_t cap, size_t* out_len);
} KilnStationMenusApi;

#ifdef __cplusplus
}
#endif
#endif /* KILN_STATION_MENUS_H */

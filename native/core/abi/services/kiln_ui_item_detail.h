/* SPDX-License-Identifier: MIT
 * kiln.ui.item_detail v1 (client only, dev.30.5): copies of the text/values built by the game's item-detail panel.
 * No offscreen game rendering. Capture starts on first copy, only while ItemDetail runs. No borrowed game pointers.
 * Capability ui.item_detail. Cache: last 64 instances/variants in this client session, updated on each panel build.
 * Text is in the language used when captured. Reinspect after a language or item-state change.
 */
#ifndef KILN_UI_ITEM_DETAIL_H
#define KILN_UI_ITEM_DETAIL_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif
#define KILN_UI_ITEM_DETAIL_SERVICE "kiln.ui.item_detail"
#define KILN_UI_ITEM_DETAIL_VERSION 1u
#define KILN_UI_ITEM_DETAIL_MAX_LINES 64u
#define KILN_UI_ITEM_DETAIL_TEXT_BYTES 4096u
#define KILN_UI_ITEM_DETAIL_VALUE 1u
/* Unknown level/rarity = UINT32_MAX. entity != 0 matches only that entity, never another of the same type.
 * entity == 0 matches item (+ optional level/rarity); unspecified variant = most recently inspected variant.
 * item == 0 is allowed only with entity != 0. size is mandatory on every record. */
typedef struct KilnUiItemDetailQuery {
    uint32_t size, entity, item, level, rarity;
} KilnUiItemDetailQuery;
typedef struct KilnUiItemDetailLine {
    uint32_t size, flags;
    float value; /* computed stat value (including tooltip value providers), present with VALUE; not gameplay authority */
    uint32_t text_bytes;
    char text[KILN_UI_ITEM_DETAIL_TEXT_BYTES]; /* length-delimited UTF-8, no terminating NUL promised */
} KilnUiItemDetailLine;
typedef struct KilnUiItemDetailSnapshot {
    uint32_t size, ok, count, item, entity, level, rarity, reserved;
    uint64_t session, frame, capture_ms;
} KilnUiItemDetailSnapshot;
typedef struct KilnUiItemDetailApi {
    KilnServiceHeader header;
    /* Any thread. Complete immutable copy; no partial rows. OK + ok=0: not inspected (or rejected budget).
     * BUFFER_TOO_SMALL: count is required capacity. UNAVAILABLE: server, unknown layout, disabled row, hook failure.
     * REVOKED: retired owner. INVALID_ARGUMENT: owner=0, invalid/short records, empty query, null rows with capacity.
     * A panel over 64 rows / 4096 bytes per row / 32768 total bytes is rejected, not truncated.
     * copy makes no gameplay writes and requires no change event. */
    KilnResult(KILN_CALL* copy)(KilnOwner owner, const KilnUiItemDetailQuery* query,
        KilnUiItemDetailSnapshot* snapshot, KilnUiItemDetailLine* lines, uint32_t capacity);
} KilnUiItemDetailApi;
#ifdef __cplusplus
}
static_assert(sizeof(KilnUiItemDetailQuery) == 20);
static_assert(sizeof(KilnUiItemDetailLine) == 4112);
static_assert(sizeof(KilnUiItemDetailSnapshot) == 56);
static_assert(offsetof(KilnUiItemDetailApi, copy) == sizeof(KilnServiceHeader));
#endif
#endif

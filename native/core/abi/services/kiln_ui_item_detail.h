/* SPDX-License-Identifier: MIT
 * kiln.ui.item_detail v1 (client only). Since 0.2.0-dev.30.5.
 * Passive copies of text/values constructed in the game's item-detail panel, on the client's main/UI thread.
 * No on-demand computation, offscreen rendering, drawn Kiln panel, or borrowed game pointers. First copy starts capture.
 * Capability ui.item_detail. Any-thread reads copy immutable records under a mutex, never live item/world memory.
 * Cache: last 64 instances/variants (including refusals), in capture order; reads do not promote entries.
 * A completed panel replaces the same entity, or the same item+level+rarity for entity=0, including rejected panels.
 * World/session change, missing client state, or no local player invalidates captures when next read/captured.
 * Text is in the language used when captured. No automatic item-state/language invalidation; reinspect to refresh.
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
/* snapshot.reserved carries the refusal code when ok=0 (keeps the v1 record layout). */
#define KILN_UI_ITEM_DETAIL_NOT_CAPTURED 0u
#define KILN_UI_ITEM_DETAIL_NO_SESSION 1u
#define KILN_UI_ITEM_DETAIL_REJECTED 2u
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
    /* Any thread. Complete immutable copy; no partial rows. OK + ok=0: reserved explains the miss (codes above).
     * entity is a CLIENT presentation id, never an arbitrary server inventory entity. Item-only queries select an
     * observed variant, which can include that instance's gems/perks; they do not compute a pristine type's stats.
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

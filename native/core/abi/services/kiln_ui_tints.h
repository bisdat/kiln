/* SPDX-License-Identifier: MIT
 * kiln.ui.tints v1, client only, since 0.2.17. Native slot BACKGROUND colour rules, not overlays.
 * A rule sees a copied item view on the game's UI thread and edits canonical RRGGBBAA. The game's own
 * colour strength, rarity art, focus, input and icon are unchanged. No game pointers escape.
 * Rules run in registration order; the last change wins. Empty slots are included (item=0).
 * Subscribe/unsubscribe: any thread. 128 registrations/mod shared with kiln.ui.values; owner retirement
 * removes them. A removed rule is not called again; an already-running callback may finish. Five consecutive
 * faults remove a rule. A nested slot draw passes through unchanged. No gameplay writes or network traffic.
 */
#ifndef KILN_UI_TINTS_H
#define KILN_UI_TINTS_H
#include "kiln_ui_values.h"
#ifdef __cplusplus
extern "C" {
#endif
#define KILN_UI_TINTS_SERVICE "kiln.ui.tints"
#define KILN_UI_TINTS_VERSION 1u
/* OK: use *rgba (leave unchanged to pass). INTERNAL/exception: a fault; other errors: pass through. */
typedef KilnResult(KILN_CALL* KilnUiTintFn)(const KilnUiItemView* view, uint32_t* rgba, void* user);
typedef struct KilnUiTintsApi {
    KilnServiceHeader header;
    /* item_filter=0: all slots. OK+out.ok=0: registration budget; UNAVAILABLE: capability/layout/hook.
     * INVALID_ARGUMENT: owner=0, missing callback/record; REVOKED: retired owner. */
    KilnResult(KILN_CALL* subscribe)(KilnOwner owner, uint32_t item_filter, KilnUiTintFn fn, void* user,
                                   KilnUiRegistration* out);
    KilnResult(KILN_CALL* unsubscribe)(KilnHandle handle);
} KilnUiTintsApi;
#ifdef __cplusplus
}
static_assert(sizeof(KilnUiTintsApi) == 24);
static_assert(offsetof(KilnUiTintsApi, subscribe) == 8);
static_assert(offsetof(KilnUiTintsApi, unsubscribe) == 16);
#endif
#endif

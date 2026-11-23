/* SPDX-License-Identifier: MIT
 * kiln.ui.chest v1, client only, since 0.2.17. Native buttons in the storage chest's own action row.
 * No overlay, vanilla action id, game pointer or gameplay authority. The native entry renderer owns input/focus.
 * Subscribe/unsubscribe any thread; activation callback on the game's UI thread, after the chest builder returns.
 * Four active buttons total; labels unique among these buttons, 1..128 UTF-8 bytes, no NUL.
 * Registrations also share ui.values' 128/mod limit; labels use its permanent, bounded text store.
 * Owner retirement removes registrations. An in-flight callback may finish; five consecutive faults remove it.
 */
#ifndef KILN_UI_CHEST_H
#define KILN_UI_CHEST_H
#include "kiln_ui_values.h"
#ifdef __cplusplus
extern "C" {
#endif
#define KILN_UI_CHEST_SERVICE "kiln.ui.chest"
#define KILN_UI_CHEST_VERSION 1u
typedef struct KilnUiChestPress {
    uint32_t size, container, local_player, reserved;
    uint64_t session, frame;
} KilnUiChestPress;
typedef KilnResult(KILN_CALL* KilnUiChestFn)(const KilnUiChestPress* press, void* user);
typedef struct KilnUiChestApi {
    KilnServiceHeader header;
    /* OK+out.ok=0: budget (REGISTRATION_BUDGET/TEXT_BUDGET) or duplicate caption (REJECTED).
     * UNAVAILABLE: server, unknown layout, row disabled or hook failure. REVOKED: retired owner.
     * INVALID_ARGUMENT: owner=0, missing callback/record, invalid label. Literal caption is copied. */
    KilnResult(KILN_CALL* subscribe)(KilnOwner owner, KilnStr caption, KilnUiChestFn fn, void* user,
                                   KilnUiRegistration* out);
    KilnResult(KILN_CALL* unsubscribe)(KilnHandle handle);
} KilnUiChestApi;
#ifdef __cplusplus
}
static_assert(sizeof(KilnUiChestPress) == 32);
static_assert(sizeof(KilnUiChestApi) == 24);
static_assert(offsetof(KilnUiChestApi, subscribe) == 8);
static_assert(offsetof(KilnUiChestApi, unsubscribe) == 16);
#endif
#endif

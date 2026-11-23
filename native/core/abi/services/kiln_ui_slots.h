/* SPDX-License-Identifier: MIT
 * kiln.ui.slots v1 (client, dev.30.5): negotiated expanded slot subscription. Capability ui.slots.inspect.
 * kiln.ui.values.subscribe_slots and KilnUiVisibleSlot stay frozen at 112 bytes forever.
 * copy_slots remains size-aware: for V2 storage pass the first record's size=120 and cast to the legacy pointer.
 */
#ifndef KILN_UI_SLOTS_H
#define KILN_UI_SLOTS_H
#include "kiln_ui_values.h"
#define KILN_UI_SLOTS_SERVICE "kiln.ui.slots"
#define KILN_UI_SLOTS_VERSION 1u
#ifdef __cplusplus
extern "C" {
#endif
typedef struct KilnUiSlotsApi {
    KilnServiceHeader header;
    /* record_size must be sizeof(KilnUiVisibleSlotV2) = 120. Unknown sizes refused, never silently changed.
     * Same owner, limits, UI-thread delivery and lifetime as subscribe_slots. Handles use kiln.ui.values.unsubscribe.
     * No extension of the existing kiln.ui.values table. */
    KilnResult(KILN_CALL* subscribe)(KilnOwner owner, uint32_t record_size, KilnUiSlotsFnV2 fn,
                                    void* user, KilnUiRegistration* out);
} KilnUiSlotsApi;
#ifdef __cplusplus
}
static_assert(sizeof(KilnUiSlotsApi) == 16);
#endif
#endif

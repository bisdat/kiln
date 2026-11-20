/* SPDX-License-Identifier: MIT
 * kiln.ui.inventory v1 (in-core service, client only, 0.2.0-dev.30): the game's own inventory slots as something a mod
 * can act on. Built on kiln.ui.values' slot inspection (kiln_ui_values.h copy_slots: the slots the game drew in each
 * completed UI frame) and kiln.ui's presses (kiln_ui.h subscribe_pointer_press). docs/kiln/ui.md "The game's inventory
 * slots" is the author's page; /mnt/Dongus/kiln-wave/dev30/out/ui-hooks/DESIGN.md the design.
 *
 * Capability: ui.slots.click (a sigdb feature of the client exe: the slot sites of ui.slots.inspect and the play data's
 * HUD screen). Its hooks go in with the first click subscriber. Off: subscribe_clicks answers KILN_ERR_UNAVAILABLE and
 * every press reaches the game as without Kiln. slot_at and mapping need ui.slots.inspect only.
 *
 * Coordinates. The game lays its UI out in units; pixel = viewport + unit * pixels_per_unit on its canvas (chapter 05a),
 * and Kiln scales the canvas to the target (the window's client area, or the overlay's display) when their sizes differ.
 * KUI_MAP_VERIFIED is never set in v1: the mapping waits for the client check ui-hooks.c1 (05a-c5).
 *
 * Clicks (fail closed). A left or right press over the game window, while Kiln does not capture input and the cursor is
 * not over a Kiln panel, hits a slot when: the latest slot frame is of this client session and at most 250 ms old, has
 * a usable viewport, was drawn on a screen with a pointer (HUD screen 4 in-game menu, 8 player crafting, 9 backpack,
 * 22 storage chest, 25 station, 27 NPC crafting, 28 factory, 41 chest-style transfer; never 1, the play view, where the
 * hotbar is drawn and a click attacks), and the cursor lies in a slot's outer rectangle (the last drawn wins). The
 * subscribers whose button and exact modifiers match get the click; if any of them set KUI_CLICK_TAKE the game never
 * sees the press (its release still reaches it). Anything missing: no event, the game gets the press.
 * Delivery: fn(click) on the client's main (UI) thread right after the next completed UI frame, in registration order,
 * under a structured-exception guard (five faults in a row remove the subscription). Picture and wish only: the slot's
 * identity is what this game drew; a mod's server must check the item (holder, slot, type) before acting on it.
 *
 * Identity. slot.client_entity is the item's pide in this game's world. For an item the server replicated, that is the
 * server's entity id (createEntityWithId keeps the wire id: docs/re/network.md, chapter 05a §6); slot.server_entity
 * stays 0 here (KUI_SERVER_ENTITY clear): the managed side names it ItemEntity and the server checks it.
 *
 * Registrations share kiln.ui.values' budget (128 per stable mod id) and handles; unsubscribe here or there.
 * Every reader checks header.size and each record's size. The table is append-only. */
#ifndef KILN_UI_INVENTORY_H
#define KILN_UI_INVENTORY_H
#include "../kiln_api.h"
#include "kiln_ui.h"
#include "kiln_ui_values.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_UI_INVENTORY_SERVICE "kiln.ui.inventory"
#define KILN_UI_INVENTORY_VERSION 1u

/* KilnUiSlotClickFilter.flags */
enum { KUI_CLICK_TAKE = 1u };
/* KilnUiSlotMapping.flags */
enum { KUI_MAP_SCALED = 1u, KUI_MAP_VERIFIED = 2u };
/* reasons beyond KilnUiValueReason, for slot_at and mapping (ok = 0) */
enum { KUI_NO_SLOT = 32, KUI_NO_POINTER_SCREEN = 33, KUI_NO_VIEWPORT = 34 };

typedef struct KilnUiSlotClickFilter {
    uint32_t size;   /* sizeof(KilnUiSlotClickFilter) */
    uint32_t button; /* 1 left, 3 right */
    uint32_t mods;   /* KILN_UI_MOD_* that must be held, exactly (0 = none of Ctrl, Shift, Alt) */
    uint32_t flags;  /* KUI_CLICK_TAKE: keep the press from the game */
} KilnUiSlotClickFilter;

typedef struct KilnUiSlotClick {
    uint32_t size, button, mods, taken; /* taken: the game did not get the press (some subscriber asked for it) */
    float x, y;                         /* the press in client pixels */
    uint64_t session, frame;            /* the slot frame the press hit */
    uint64_t press_ms;                  /* GetTickCount64 at the press */
    KilnUiScreen screen;                /* that frame's screen */
    KilnUiVisibleSlot slot;             /* the slot (a copy) */
} KilnUiSlotClick;
typedef void(KILN_CALL* KilnUiSlotClickFn)(const KilnUiSlotClick* click, void* user);

/* pixel = offset + unit * scale, in the target's pixels, for the latest frame. */
typedef struct KilnUiSlotMapping {
    uint32_t size, ok, reason, flags;
    float scale_x, scale_y, offset_x, offset_y;
    uint64_t session, frame;
    KilnUiScreen screen;
} KilnUiSlotMapping;

typedef struct KilnUiSlotHit {
    uint32_t size, ok, reason, pointer_screen; /* pointer_screen: the frame's screen takes clicks (the list above) */
    uint64_t session, frame;
    KilnUiVisibleSlot slot;
} KilnUiSlotHit;

typedef struct KilnUiInventoryApi {
    KilnServiceHeader header; /* { sizeof(KilnUiInventoryApi), KILN_UI_INVENTORY_VERSION }; append-only */
    /* fn(click) for presses matching filter. Any thread. KILN_OK with out->ok = 0: KUI_REGISTRATION_BUDGET.
     * KILN_ERR_UNAVAILABLE: a server, ui.slots.click off, kiln.ui without subscribe_pointer_press, or the hooks failed.
     * INVALID_ARGUMENT: owner 0, no fn, a filter of the wrong size, a button other than 1/3, mods outside Ctrl|Shift|Alt,
     * an unknown flag. */
    KilnResult(KILN_CALL* subscribe_clicks)(KilnOwner owner, const KilnUiSlotClickFilter* filter, KilnUiSlotClickFn fn,
                                            void* user, KilnUiRegistration* out);
    /* The slot at (x, y) in a target of target_width x target_height pixels (the window's client area or the overlay's
     * display) in the latest frame; x < 0 = the cursor (client pixels; the target size is then the client area's).
     * KILN_OK with ok = 0 and a reason (KUI_NO_FRAME, KUI_STALE_FRAME, KUI_NO_VIEWPORT, KUI_NO_SLOT). Any thread. */
    KilnResult(KILN_CALL* slot_at)(KilnOwner owner, float x, float y, float target_width, float target_height,
                                   KilnUiSlotHit* out);
    /* The latest frame's units-to-pixels mapping for a target of that size. Any thread. */
    KilnResult(KILN_CALL* mapping)(KilnOwner owner, float target_width, float target_height, KilnUiSlotMapping* out);
    /* Removes a click subscription (any kiln.ui.values handle). */
    KilnResult(KILN_CALL* unsubscribe)(KilnHandle handle);
} KilnUiInventoryApi;

#ifdef __cplusplus
}
#endif
#endif /* KILN_UI_INVENTORY_H */

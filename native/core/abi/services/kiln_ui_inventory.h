/* SPDX-License-Identifier: MIT
 * kiln.ui.inventory v2 (in-core service, client only, 0.2.0-dev.30): the game's own inventory slots as something a mod
 * can act on, and (v2) the game's backpack and chest screens as a place for a mod's panel. Built on kiln.ui.values' slot inspection (kiln_ui_values.h copy_slots: the slots the game drew in each
 * completed UI frame) and kiln.ui's presses (kiln_ui.h subscribe_pointer_press). docs/kiln/ui.md "The game's inventory
 * slots" is the author's page; /mnt/Dongus/kiln-wave/dev30/out/ui-hooks/DESIGN.md the design.
 *
 * Capability: ui.slots.click (a sigdb feature of the client exe: the slot sites of ui.slots.inspect and the play data's
 * HUD screen). Its hooks go in with the first click subscriber. Off: subscribe_clicks answers KILN_ERR_UNAVAILABLE and
 * every press reaches the game as without Kiln. slot_at and mapping need ui.slots.inspect only.
 *
 * Coordinates. The game lays its UI out in units; pixel = viewport + unit * pixels_per_unit on its canvas (chapter 05a).
 * The canvas is in units (1920x1080 times the UI scale, grown along the longer side), the viewport in pixels: the game's
 * picture is the viewport's size (w + 2x by h + 2y, a box taken as centred; x and y are 0 in the game today), and Kiln
 * scales that picture to the target (the window's client area, or the overlay's display) when their sizes differ
 * (KUI_MAP_SCALED / KUI_VIEW_SCALED). Target 0 x 0 = the picture's own pixels. Through 0.2.0-dev.30.2 Kiln scaled by the
 * canvas's size in units instead: everything landed at ppu times its place whenever ppu was not 1 (issue 107).
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
 * Identity. slot.client_entity is the item's pide in this game's world; 0 (KUI_CLIENT_ENTITY clear) for a plain stack,
 * which the game keeps without an item entity (materials, consumables). For an item the server replicated, that is the
 * server's entity id (createEntityWithId keeps the wire id: docs/re/network.md, chapter 05a §6). Since 0.2.0-dev.31
 * slot.server_entity says so (KUI_SERVER_ENTITY) for the slots of a proven holder: the local player, or the container of
 * an open chest, chest-style or factory screen (kiln_ui_values.h KilnUiVisibleSlot); the server still checks the item.
 * slot.level / slot.rarity (KUI_ITEM_VALUES): the item's as drawn. The slot record grew to 120 bytes in dev.31:
 * slot_at accepts a KilnUiSlotHit whose slot ends at level (the 112-byte record) and writes only that much.
 *
 * Screen view (v2, capability ui.inventory.panels: ui.slots.inspect's slot sites plus the play data's HUD screen and
 * play.target). screen_view answers, for the latest slot frame, whether it shows one of the game's inventory screens a
 * panel may sit beside (KUI_VIEW_BACKPACK: HUD 9, or HUD 4 the in-game menu while no crafting menu is built and slots are
 * drawn; KUI_VIEW_CHEST: HUD 22 storage chest or 41 the chest-style transfer screen's mode 1), the container's entity on a
 * chest screen (kiln.ui's screen_target: play.target, the entity the chest-style transfer screen 0xb9d0f0 finds the other
 * inventory by; for a replicated chest the server's id), and the rectangles of the drawn slots in the target's pixels: all
 * of them, the local player's (holder = local player) and the container's (holder = container; KUI_VIEW_HOLDER_MATCH when
 * at least one slot's holder is the container, so the entity is seen twice). Every other screen, a stale frame, no
 * viewport: ok = 0 with a reason; Kiln.Ui then draws no panel (fail closed). A picture: the server checks the container
 * (the player may use it, is near it) before a mod acts on it.
 *
 * Registrations share kiln.ui.values' budget (128 per stable mod id) and handles; unsubscribe here or there.
 * Every reader checks header.size and each record's size. The table is append-only: v1 is 40 bytes (through
 * unsubscribe), v2 48 (screen_view); a v1 reader of a v2 table works unchanged, a v2 reader checks header.size >=
 * offsetof(KilnUiInventoryApi, screen_view) + sizeof(void*) before it calls screen_view. */
#ifndef KILN_UI_INVENTORY_H
#define KILN_UI_INVENTORY_H
#include "../kiln_api.h"
#include "kiln_ui.h"
#include "kiln_ui_values.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_UI_INVENTORY_SERVICE "kiln.ui.inventory"
#define KILN_UI_INVENTORY_VERSION 2u

/* KilnUiSlotClickFilter.flags */
enum { KUI_CLICK_TAKE = 1u };
/* KilnUiSlotMapping.flags */
enum { KUI_MAP_SCALED = 1u, KUI_MAP_VERIFIED = 2u };
/* reasons beyond KilnUiValueReason, for slot_at, mapping and screen_view (ok = 0) */
enum { KUI_NO_SLOT = 32, KUI_NO_POINTER_SCREEN = 33, KUI_NO_VIEWPORT = 34, KUI_NO_PANEL_SCREEN = 35 };
/* KilnUiScreenView.kind (v2) */
enum { KUI_VIEW_NONE = 0, KUI_VIEW_BACKPACK = 1, KUI_VIEW_CHEST = 2 };
/* KilnUiScreenView.flags (v2) */
enum { KUI_VIEW_HOLDER_MATCH = 1u, KUI_VIEW_SCALED = 2u };

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

/* v2: the inventory screen of the latest slot frame (see "Screen view" above). Rectangles are in the target's pixels
 * (x, y, width, height); a rectangle whose count is 0 is all zero. kind, hud_screen, container, local_player, session and
 * frame are filled whenever a frame was usable, also with ok = 0 for KUI_NO_VIEWPORT or KUI_NO_SLOT. */
typedef struct KilnUiScreenView {
    uint32_t size, ok, reason, kind;
    uint32_t hud_screen, container, local_player, flags; /* container: 0 on the backpack or when play.target is not read */
    uint64_t session, frame;
    uint32_t slot_count, player_slots, container_slots, reserved;
    KilnUiRect all_px, player_px, container_px;
} KilnUiScreenView;

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
    /* v2 (size-guarded): the inventory screen of the latest frame for a target of that size (the overlay's display or the
     * window's client area). KILN_OK with out->ok = 0 and a reason: KUI_NO_FRAME, KUI_STALE_FRAME, KUI_NO_PANEL_SCREEN
     * (any other screen), KUI_NO_VIEWPORT, KUI_NO_SLOT (no slot drawn; on HUD 4 no slot of the local player).
     * KILN_ERR_UNAVAILABLE: a server, ui.inventory.panels off, or the slot hooks failed. INVALID_ARGUMENT: owner 0, out
     * NULL or short. REVOKED: a retired owner. Any thread. */
    KilnResult(KILN_CALL* screen_view)(KilnOwner owner, float target_width, float target_height, KilnUiScreenView* out);
} KilnUiInventoryApi;

#ifdef __cplusplus
}
#endif
#endif /* KILN_UI_INVENTORY_H */

/* SPDX-License-Identifier: MIT
 * kiln.ui.input v1 (native/services/ui, client only; since 0.2.0-dev.31.13, mod API 0.2.22): input a plugin owns from
 * its press to its release, and a read-only view of the game controllers. A table of its own next to kiln.ui (whose
 * tail other work appends to). Guide: docs/kiln/ui.md "Owning a press until its release"; the rules:
 * native/services/ui/input_own.h; evidence: docs/re/input-ownership.md.
 *
 * Owning a press (subscribe_owned). kiln.ui hands an owning subscriber each new press of its sources first, before
 * kiln.ui's other key subscribers (subscribe_key, subscribe_key_ex), Kiln.Ui's key bindings and the pointer subscribers
 * (subscribe_pointer_press), and after the Kiln menu's own key and Escape. Owning subscribers are asked in the order they
 * subscribed; the first that answers KILN_UI_OWN_TAKE owns that press:
 *   - nobody else gets that press, its repeats or its release (not the game, another owner, a key subscriber or a key
 *     binding). The owner gets them: KILN_UI_OWNED_REPEAT (keyboard keys) and KILN_UI_OWNED_RELEASE, which ends it.
 *   - the game never sees the press go down, so it never sees the release either: the window message and the raw input
 *     record (or the record's flag, through the game's GetRawInputData import) of the press, its repeats and its release
 *     are kept from it; a mouse button stays out of the held-button bits (MK_*) of the game's mouse messages while held;
 *     a controller's button reads as not pressed (a trigger as not pulled) until its release.
 *   - stuck-press guard: when the game sees the press go down after all (a raw mouse record that holds other input and
 *     kiln.ui could not hook the game's GetRawInputData import; Alt+F4, which always reaches the game), the owner keeps
 *     ownership but the release and repeats reach the game too (KILN_UI_OWNED_LEAKED on them), so the game never holds
 *     a button.
 *   - ownership ends without the release (KILN_UI_OWNED_END, `end` says why): the game window lost the focus (the release
 *     will go to another window; one that comes back later reaches the game, which never saw the press, so nothing is
 *     held), the client world started or stopped, or the input went down again without its release (lost). After a world
 *     change, an unsubscribe or a disown the press is nobody's: its repeats and release reach nobody and are still kept
 *     from the game. Unsubscribe and disown make no callback.
 * A subscriber that passes (KILN_UI_OWN_PASS) saw the press and gets nothing more of it: the next owner, then Kiln's
 * other handlers, are asked as if it had not been. The wheel cannot be owned (a notch has no release): only key
 * subscribers get it (KILN_UI_KEYS_MOUSE).
 * The left and right buttons (KILN_UI_OWN_POINTER) come as kiln.ui's pointer presses do: over the game's own view, while
 * Kiln does not capture input and the cursor is not over a Kiln panel; their release comes wherever the cursor is then.
 * The other sources come also while Kiln captures input (captured 1) and while a Kiln text field has the keyboard
 * (text_input 1), as key subscribers get them. A controller's buttons are XInput's (an Xbox pad, or any pad Steam Input
 * presents as one); the game reads other pads through DirectInput, which kiln.ui does not see (docs/re/input-ownership.md).
 *
 * Threads: keyboard and mouse callbacks run on the game window's thread inside Kiln's window procedure, controller ones
 * on the thread the game polls the controller from, KILN_UI_OWNED_END for a world change on the game's UI thread. Decide
 * at once: no waiting, no game calls. A structured exception in fn removes the subscription (logged); its presses become
 * nobody's.
 *
 * Controllers (pad_state): what the game's last XInputGetState poll of controller 0..3 returned, before kiln.ui hid a kept
 * or owned button from it, normalized: sticks -1..1 with XInput's dead zones taken out (left 7849, right 8689 of 32767,
 * radially; 0 inside), triggers 0..1, above 0 from XInput's threshold (a pull of 30 of 255, when a trigger counts as
 * pressed). Read only: it never changes what the game reads.
 * Any thread.
 *
 * Readers check header.size against the end of each slot they call; the table is append-only. */
#ifndef KILN_UI_INPUT_H
#define KILN_UI_INPUT_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_UI_INPUT_SERVICE "kiln.ui.input"
#define KILN_UI_INPUT_VERSION 1u

/* subscribe_owned sources (any combination, at least one) */
enum {
    KILN_UI_OWN_KEYS = 1u,    /* keyboard keys (KilnUiKey.vk 0x07..0xFF) */
    KILN_UI_OWN_MOUSE = 2u,   /* the middle and side mouse buttons (0x04, 0x05, 0x06) */
    KILN_UI_OWN_POINTER = 4u, /* the left and right mouse buttons (0x01, 0x02) over the game's own view */
    KILN_UI_OWN_PAD = 8u,     /* a game controller's buttons and triggers (KILN_UI_VK_PAD_*, kiln_ui.h) */
};

/* KilnUiOwnedInput.kind */
enum {
    KILN_UI_OWNED_PRESS = 1u,   /* a new press, offered: answer KILN_UI_OWN_TAKE to own it */
    KILN_UI_OWNED_REPEAT = 2u,  /* the auto-repeat of a keyboard key you own */
    KILN_UI_OWNED_RELEASE = 3u, /* the release of a press you own; ownership ends */
    KILN_UI_OWNED_END = 4u,     /* ownership ended without the release: `end` */
};

/* KilnUiOwnedInput.end */
enum {
    KILN_UI_OWNED_END_NONE = 0u,
    KILN_UI_OWNED_END_FOCUS = 1u, /* the game window lost the keyboard focus */
    KILN_UI_OWNED_END_WORLD = 2u, /* the client world started or stopped (kiln.client generation) */
    KILN_UI_OWNED_END_LOST = 3u,  /* the input went down again before its release reached Kiln */
};

/* KilnUiOwnedInput.flags */
enum { KILN_UI_OWNED_LEAKED = 1u /* the game saw this press go down after all: its repeats and release reach it too */ };

/* the answer to KILN_UI_OWNED_PRESS (other kinds: ignored) */
enum { KILN_UI_OWN_PASS = 0u, KILN_UI_OWN_TAKE = 1u };

typedef struct KilnUiOwnedInput {
    uint32_t size;          /* sizeof(KilnUiOwnedInput) */
    uint32_t kind;          /* KILN_UI_OWNED_* */
    uint32_t source;        /* one KILN_UI_OWN_* bit */
    uint32_t vk;            /* KilnUiKey.vk: a key's virtual key, 0x01 left, 0x02 right, 0x04..0x06 the middle and side
                               buttons, KILN_UI_VK_PAD_* a controller's button */
    uint32_t scan;          /* a keyboard key's scan code (extended 0x100), 0 otherwise */
    uint32_t mods;          /* KILN_UI_MOD_* held now; a controller's: KILN_UI_MOD_PAD_* */
    uint32_t controller;    /* a controller's XInput user index 0..3; 0 otherwise */
    uint32_t end;           /* KILN_UI_OWNED_END_* (kind END), 0 otherwise */
    uint32_t time;          /* the window message's time (GetMessageTime); 0 for a controller and for END */
    uint32_t flags;         /* KILN_UI_OWNED_* flags */
    float x, y;             /* the pointer: the cursor in the window's client pixels at the press and the release */
    float client_width;     /* the pointer: the client area's size in pixels */
    float client_height;
    uint8_t text_input;     /* a Kiln text field has the keyboard */
    uint8_t captured;       /* Kiln captures input right now */
    uint8_t reserved[6];
} KilnUiOwnedInput;         /* 64 bytes */
typedef uint32_t(KILN_CALL* KilnUiOwnedFn)(const KilnUiOwnedInput* input, void* user);

/* pad_state */
typedef struct KilnUiPadState {
    uint32_t size;          /* set by the caller: sizeof(KilnUiPadState) */
    uint32_t controller;    /* 0..3 */
    uint8_t connected;      /* the game's last poll of it succeeded */
    uint8_t reserved0[3];
    uint32_t buttons;       /* held: bit (code - KILN_UI_VK_PAD_UP) of each KILN_UI_VK_PAD_* (LT bit 16, RT bit 17, from a
                               pull of 30) */
    uint32_t owned;         /* the buttons a subscribe_owned subscriber owns now (the same bits) */
    uint32_t hidden;        /* the buttons hidden from the game now: owned, nobody's, or kept by a key subscriber */
    float left_x, left_y;   /* -1..1 (up and right positive), the dead zone taken out */
    float right_x, right_y;
    float left_trigger;     /* 0..1 */
    float right_trigger;
    int16_t raw_left_x, raw_left_y, raw_right_x, raw_right_y; /* XInput's values */
    uint8_t raw_left_trigger, raw_right_trigger;
    uint8_t reserved1[2];
    uint32_t packet;        /* XInput's dwPacketNumber */
    uint64_t poll_ms;       /* GetTickCount64 at the game's last poll; 0: never polled */
    uint64_t polls;         /* the game's polls of this controller since kiln.ui started */
    uint32_t reserved2[2];
} KilnUiPadState;           /* 88 bytes */

typedef struct KilnUiInputApi {
    KilnServiceHeader header; /* KILN_UI_INPUT_VERSION; append-only */
    /* fn(input) for every new press of `sources` (KILN_UI_OWN_*), and the repeats, release and end of the presses it
     * takes (above). INVALID_ARGUMENT: no fn, no source or an unknown bit. Remove it with unsubscribe. Any thread. */
    KilnResult(KILN_CALL* subscribe_owned)(KilnOwner owner, uint32_t sources, KilnUiOwnedFn fn, void* user, KilnHandle* out);
    /* Ends a subscription (KILN_OK: a call in flight has ended; from inside a kiln.ui callback it may answer
     * KILN_ERR_WRONG_THREAD: the subscription is off, retry from elsewhere to wait). Its presses become nobody's, without
     * a callback. NOT_FOUND: not a subscribe_owned handle. */
    KilnResult(KILN_CALL* unsubscribe)(KilnHandle subscription);
    /* Gives up the subscription's press of `vk` on `controller` (vk 0: every press it owns) without a callback: its
     * repeats and release reach nobody and stay kept from the game. Returns how many it gave up. Any thread, also
     * inside the callback. */
    uint32_t(KILN_CALL* disown)(KilnHandle subscription, uint32_t vk, uint32_t controller);
    /* The subscription owns the press of `vk` on `controller` now. Any thread. */
    bool(KILN_CALL* owns)(KilnHandle subscription, uint32_t vk, uint32_t controller);
    /* Controller 0..3 as the game's last poll read it (above; set out->size first). UNAVAILABLE: kiln.ui never saw the
     * game load XInput (out->connected 0). INVALID_ARGUMENT: NULL, size too small or controller > 3. Any thread. */
    KilnResult(KILN_CALL* pad_state)(uint32_t controller, KilnUiPadState* out);
} KilnUiInputApi;

#ifdef __cplusplus
}
#endif
#endif

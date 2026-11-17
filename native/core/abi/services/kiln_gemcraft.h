/* SPDX-License-Identifier: MIT
 * kiln.gemcraft v1: a Shroud Forge's four gem actions (Slot, Unslot, Upgrade, Recycle) for code, and the players'
 * own forge actions before (hold, cancel) and after (what changed, what was paid). Published by kiln-items.dll
 * (native/services/items/gems.cpp) on the server world: a dedicated server, or a game client that hosts. Capability
 * gems.craft (sigdb feature, server.d/gems-forge.json; the client's hosted copy composes the same patterns).
 *
 * The game's action: equipment_actions' per-player weapon-gem handler (server 26430253 0x144bb0, client 26430625
 * 0x385880; docs/re/gems-forge.md). It reads PlayerInput.weaponGemAction (ChangeWeaponGemAction: weapon pide, the gem's
 * InventorySlotId, forge entity, material item, version, action), resolves the forge's ShroudForge template, the gem and
 * its Level, pays the cost in the material from the player on one inventory transaction, moves or destroys the gem
 * (or raises its Level by one), commits and sends GemCraftingEvent (the forge plays its action sequence).
 *
 *  - Code actions (submit): kiln.items runs the same handler inside its program kiln_items_gems, an entity program over
 *    the player characters, with a request of its own instead of the client's input (the client's input and its
 *    consumed version are never touched). Every check, cost and payment of the game's action applies; Kiln checks
 *    before it that the carrier and the gem are where the request says (the player's inventories, or the holder the
 *    caller names), that the socket is there and empty (Slot) or holds a gem (Unslot), that the source is a gem item
 *    (category WeaponGem), and afterwards that the state changed as the action says. KILN_GEM_FREE and KILN_GEM_COST
 *    replace what the player pays (Recycle: what it gets) without changing the game's transaction: Kiln gives the
 *    player the game's cost right before the action and takes the replacement (or nothing), on the same step, and
 *    undoes both if the action does not happen.
 *  - Players' actions: while hold() is renewed, a player's new forge action is held for one step (the game's handler
 *    is not run for it) and reported by held() in the next Pre; decide() allows it (it runs in the next step) or
 *    cancels it (the game consumes it as a request without a forge: nothing is paid, nothing moves). An action not
 *    decided within 8 steps runs. A decision stands once given, also after holding stops; an action released before
 *    held() handed it out (hold(0), or the lease lapsed) is not handed out; a version that ran and comes back
 *    unconsumed (the game's handler leaves it so when the gem's Level cannot be read) runs again but is never held or
 *    handed out twice. While watch() is on, each player's action that ran (allowed, or never held) is
 *    reported by crafted() with what changed and what was paid.
 *  - Code actions refuse what the game's handler would take without asking: an item that is not a gem (category
 *    WeaponGem) for Slot, Upgrade and Recycle, a KILN_GEM_COST item with an entity of its own (gear, a gem). After a
 *    free or replaced cost Kiln takes back what the game did not take of its gift (counted, not assumed).
 *
 * Threads: submit, cancel, status: any thread. hold, held, decide, watch, crafted: the tick thread (Pre). Callbacks run
 * on the tick thread in Pre, one step after the step the action ran in. The table only grows at the end.
 *
 * Since 0.2.0-dev.29.15 (appended, GitHub issue 61):
 *  - Separate holders: KilnGemRequest.gem_holder / carrier_holder name where the gem and the carrier are (each defaults
 *    to `holder`, then to the player). The game's handler resolves both in its one transaction: the gem's slot names its
 *    page entity, the carrier its item entity; a refusal or a rollback leaves both where they were.
 *  - Payers and the return: pay_from (KILN_GEM_PAY_*) names who pays the game's cost, return_to where an unslotted gem
 *    and Recycle's essence go. Kiln moves the handler's own payment (its removeItems on the action's transaction) to the
 *    payers and its own unslot move / addItems to return_to, through mid hooks on those three calls inside the handler
 *    (sigdb feature gems.payers); the game still decides the cost, and a payer short of it makes the game roll back.
 *  - Gem sources (offer, withdraw, sources): a short-lived offer of one gem in a container to one player, with a token
 *    Kiln mints. A Kiln client lists the offered gems in the game's own GEMS grid (kiln.ui.values gemforge_sources), and
 *    the player's forge action for one arrives as the game's own request naming the gem's real slot. The handler hook
 *    finds the offer by player and slot, consumes the request without running it, and runs the action as a code action
 *    with the offer's holder, payers and return; Kiln checks the slot, the gem's entity, item and level again when it
 *    runs. While offers were made in a world, a player's action naming a gem outside the player's own inventories
 *    without a live offer is refused (consumed, nothing moves). */
#ifndef KILN_GEMCRAFT_H
#define KILN_GEMCRAFT_H
#include "../kiln_api.h"
#include "kiln_items.h" /* KilnItemId, KilnSlotId, KILN_SLOT_ID */
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_GEMCRAFT_SERVICE "kiln.gemcraft"
#define KILN_GEMCRAFT_VERSION 1u

/* keen::WeaponGemActionType */
enum {
    KILN_GEM_ACTION_SLOT = 1,    /* the gem goes into the carrier's socket */
    KILN_GEM_ACTION_UNSLOT = 2,  /* the gem in the socket goes to the player's backpack */
    KILN_GEM_ACTION_UPGRADE = 3, /* the gem's level + 1, paid in the forge's material */
    KILN_GEM_ACTION_RECYCLE = 4, /* the gem is destroyed for the forge's essence */
};

/* KilnGemRequest.flags */
enum {
    KILN_GEM_FREE = 0x0001u, /* the player pays nothing (Recycle: gets nothing) */
    KILN_GEM_COST = 0x0002u, /* the player pays cost_count x cost_item instead (Recycle: gets it instead of the essence) */
    KILN_GEM_RETURN = 0x0004u, /* KILN_GEM_ACTION_UNSLOT only, alone, with forge 0 and holder 0: the gem in the carrier's
                                  socket goes into the player's own inventory through the game's own unslot move (the one
                                  the forge's Unslot makes) on a transaction Kiln sets up as the forge's handler does,
                                  without a forge and free. The carrier must be in the player's inventories (worn
                                  included). For taking gems out of sockets the forge does not list (Gems.ReturnSocketedGems) */
};

/* KilnGemRequest.pay_from (since 0.2.0-dev.29.15): who pays the game's cost (Slot, Unslot, Upgrade). Recycle pays
 * nothing; KILN_GEM_FREE pays nothing; KILN_GEM_COST is paid by the player (with another pay_from it is refused). */
enum {
    KILN_GEM_PAY_PLAYER = 0,           /* the player's own inventories, as the game does */
    KILN_GEM_PAY_HOLDER = 1,           /* pay_holder's inventories only (a container the caller vouches for) */
    KILN_GEM_PAY_BASE = 2,             /* the crafting-stock containers (Magic chests) of the base the player stands in, in
                                          the game's order; the player needs the server's CanAccessInventories right, as the
                                          game's crafting from base storage does (else, or outside a base: NOT_ENOUGH) */
    KILN_GEM_PAY_PLAYER_THEN_BASE = 3, /* the player's inventories first, then the base's containers (crafting's order) */
};

#define KILN_GEM_REQUEST_V1_SIZE 56u /* a request of this size (0.2.0-dev.29.11..14) reads the appended fields as 0 */

typedef struct KilnGemRequest {
    uint32_t size;          /* sizeof(KilnGemRequest), or KILN_GEM_REQUEST_V1_SIZE */
    uint32_t action;        /* KILN_GEM_ACTION_* */
    uint32_t flags;         /* KILN_GEM_FREE or KILN_GEM_COST, or 0: the game's cost */
    KilnEntity player;      /* the player character the action runs for: pays (unless pay_from), and gets an unslotted
                               gem or the essence (unless return_to) */
    KilnEntity holder;      /* where the carrier and the gem are: 0 = the player's own inventories, else an entity with
                               an InventorySetup the caller vouches for (a chest) */
    KilnEntity carrier;     /* the carrier's item entity (Slot, Unslot; Upgrade / Recycle of a socketed gem); 0: a loose
                               gem (Upgrade / Recycle only) */
    KilnSlotId gem_slot;    /* Slot: the gem's inventory slot; Upgrade / Recycle of a loose gem: its slot; else 0 */
    KilnEntity gem;         /* the gem's item entity the request expects (0 = whatever is there) */
    KilnEntity forge;       /* a placed Shroud Forge (its template gives the cost, the level cap and the essence) */
    KilnItemId cost_item;   /* with KILN_GEM_COST: the item paid (Recycle: given); 0 with count 0 = nothing */
    uint32_t cost_count;    /* with KILN_GEM_COST: how many (0..100000) */
    uint32_t reserved[2];   /* 0 */
    /* ---- appended in 0.2.0-dev.29.15 (read when size covers them; KILN_GEM_RETURN needs them 0) ---- */
    KilnEntity gem_holder;     /* where the gem is (Slot; a loose gem for Upgrade / Recycle): 0 = holder (else the player) */
    KilnEntity carrier_holder; /* where the carrier is: 0 = holder (else the player) */
    uint32_t pay_from;         /* KILN_GEM_PAY_*; other than KILN_GEM_PAY_PLAYER needs the sigdb feature gems.payers */
    KilnEntity pay_holder;     /* KILN_GEM_PAY_HOLDER: the container that pays; else 0 */
    KilnEntity return_to;      /* where an unslotted gem (Unslot) and Recycle's essence go: 0 = the player; else a
                                  container the caller vouches for (needs gems.payers) */
    uint32_t reserved2;        /* 0 */
} KilnGemRequest;

/* KilnGemCraft.status */
enum {
    KILN_GEM_DONE = 0,        /* the action happened (the state after it checked out) */
    KILN_GEM_REFUSED = 1,     /* the game's action did not change anything (its own checks refused it) */
    KILN_GEM_NOT_ENOUGH = 2,  /* the player cannot pay (the game's cost or the KILN_GEM_COST replacement) */
    KILN_GEM_NO_FORGE = 3,    /* the forge entity is no Shroud Forge (no template, or maxGemLevel 0) */
    KILN_GEM_NOT_FOUND = 4,   /* the carrier, the socket or the gem is not where the request says, or not a gem */
    KILN_GEM_NO_ROOM = 5,     /* Unslot: no free backpack slot; KILN_GEM_FREE / COST: no room for the payment */
    KILN_GEM_MAX_LEVEL = 6,   /* Upgrade: the gem is at its own cap or the forge's maxGemLevel */
    KILN_GEM_CANCELED = 7,    /* a player's action: a handler canceled it (nothing paid, nothing moved) */
    KILN_GEM_UNAVAILABLE = 8, /* gems.craft went off after the request was accepted */
    KILN_GEM_DROPPED = 9,     /* not run: the player left, the world closed, or the player's program did not run in 10 s */
    KILN_GEM_OCCUPIED = 10,   /* Slot: the socket holds a gem already; Unslot: the socket is empty */
};

/* KilnGemCraft.flags */
enum {
    KILN_GEMCRAFT_BY_CODE = 0x0001u,   /* a submit(), not a player's own forge action */
    KILN_GEMCRAFT_FREE = 0x0002u,      /* run with KILN_GEM_FREE */
    KILN_GEMCRAFT_COST = 0x0004u,      /* run with KILN_GEM_COST */
    KILN_GEMCRAFT_PRICED = 0x0008u,    /* cost_* / yield_* were computed by the game's cost functions (held) or
                                          counted from the inventories (crafted) */
    KILN_GEMCRAFT_SOURCE = 0x0010u,    /* (0.2.0-dev.29.15) a player's forge action on an offered gem (gem sources),
                                          run by Kiln: `ticket` is the offer's token, `version` the player's input
                                          version. crafted() reports these whatever their status */
    KILN_GEMCRAFT_ROUTED = 0x0020u,    /* (0.2.0-dev.29.15) payers or a return other than the player (cost_* counted from
                                          the payers, yield_* in return_to) */
};

/* KilnGemcraftApi.extras flags (0.2.0-dev.29.15) */
enum {
    KILN_GEMCRAFT_HAS_PAYERS = 0x0001u,  /* requests with pay_from / return_to run (gems.payers) */
    KILN_GEMCRAFT_HAS_SOURCES = 0x0002u, /* offer / withdraw / sources run, and the handler hook takes offered gems */
};

/* One offered gem (gem sources, 0.2.0-dev.29.15). The caller fills player .. lifetime_steps; offer() fills token and
 * expires_step. The offer is the caller's decision that `player` may use the gem in `holder` at a forge: Kiln checks
 * that the slot is one of holder's pages and still holds this gem entity, item and level when the player acts. */
typedef struct KilnGemSource {
    uint32_t size;           /* sizeof(KilnGemSource) */
    KilnEntity player;       /* the player character it is offered to */
    KilnEntity holder;       /* the container that holds it (an entity with an InventorySetup; not a player) */
    KilnSlotId slot;         /* its slot: KILN_SLOT_ID(page entity, index), a page of holder's */
    KilnEntity gem;          /* its item entity */
    KilnItemId item;         /* its item */
    uint32_t level;          /* its Level now (an action on another level is refused: the gem changed) */
    uint32_t pay_from;       /* KILN_GEM_PAY_* for actions on it (Slot, Upgrade) */
    KilnEntity pay_holder;   /* KILN_GEM_PAY_HOLDER: the container that pays */
    KilnEntity return_to;    /* where Recycle's essence goes: 0 = the player */
    uint32_t lifetime_steps; /* 1..3000 (about 2 minutes on a dedicated server) */
    uint32_t rarity;         /* its rarity byte (what the client draws) */
    uint32_t reserved;       /* 0 */
    uint64_t token;          /* out: Kiln's random, unguessable token (never 0) */
    uint64_t expires_step;   /* out: the last sim step it is good for */
} KilnGemSource;

/* One action: what a held action would do (held: before the game runs it), or what one that ran did (crafted, and a
 * code action's result). */
typedef struct KilnGemCraft {
    uint32_t size;          /* sizeof(KilnGemCraft) */
    uint32_t action;        /* KILN_GEM_ACTION_* */
    int32_t status;         /* KILN_GEM_*: held: KILN_GEM_DONE */
    uint32_t flags;         /* KILN_GEMCRAFT_* */
    KilnEntity player;
    KilnEntity forge;
    KilnEntity carrier;     /* 0: a loose gem */
    KilnItemId carrier_item;
    KilnEntity gem;         /* the gem's item entity */
    KilnItemId gem_item;
    KilnSlotId gem_slot;    /* where the gem was: an inventory slot, or the socket (KILN_SLOT_ID(carrier, 0)) */
    uint32_t level_before, level_after; /* the gem's Level (after: 0 when it was recycled; held: level_before) */
    uint32_t level_cap;     /* the gem's own cap */
    uint32_t forge_max_level;
    KilnItemId cost_item;   /* held: what the game will charge; else what the player paid (0 x 0: nothing) */
    uint32_t cost_count;
    KilnItemId yield_item;  /* Recycle: what the player got (held: the game's essence and yield) */
    uint32_t yield_count;
    uint32_t version;       /* a player's action: its input version; 0 for code */
    uint32_t reserved;
    uint64_t ticket;        /* a code action's ticket; 0 for a player's */
    uint64_t step;          /* the sim step it was held in or ran in */
    char message[128];      /* why, in words, for anything but DONE; NUL-terminated */
} KilnGemCraft;

typedef void(KILN_CALL* KilnGemDoneFn)(const KilnGemCraft* result, void* user);

typedef struct KilnGemcraftStatus {
    uint32_t size;          /* set to sizeof(KilnGemcraftStatus) before calling */
    uint8_t available;      /* 1: submit runs (feature, hook, program and layout all check out) */
    uint8_t hook;           /* 1: the handler hook is installed */
    uint8_t program;        /* KILN_ITEMS_PROGRAM_* of kiln_items_gems */
    uint8_t holding;        /* 1: players' actions are held now (a hold() lease runs) */
    uint32_t held;          /* players' actions held so far */
    uint32_t canceled;      /* of them, canceled */
    uint32_t crafted;       /* actions that ran and were reported (players' and code's) */
    uint32_t pending;       /* code actions waiting */
    uint32_t reserved;
    char detail[160];       /* why something is off, in words; "" when all works */
} KilnGemcraftStatus;

typedef struct KilnGemcraftApi {
    KilnServiceHeader header; /* { sizeof(KilnGemcraftApi), KILN_GEMCRAFT_VERSION } */

    KilnResult(KILN_CALL* status)(KilnGemcraftStatus* out);
    /* Queues a code action. KILN_ERR_INVALID_ARGUMENT: bad size, action, flags or reserved fields, player 0, Slot or
     * Unslot without a carrier, Slot without gem_slot, Upgrade / Recycle with neither carrier nor gem_slot, KILN_GEM_COST
     * with a count over 100000 or a count without an item. KILN_ERR_UNAVAILABLE: gems.craft is off (status says why).
     * KILN_ERR_BUFFER_TOO_SMALL: 256 actions wait (a refusal for now). KILN_OK: `done` (may be NULL) runs once. */
    KilnResult(KILN_CALL* submit)(KilnOwner owner, const KilnGemRequest* request, KilnGemDoneFn done, void* user,
                                  uint64_t* out_ticket);
    /* Drops a code action's callback (the action may still run). KILN_ERR_NOT_FOUND: no such ticket waiting. */
    KilnResult(KILN_CALL* cancel)(uint64_t ticket);
    /* Holds players' new forge actions for the next `steps` steps (1..250; 0 stops at once: what is held runs).
     * Renew it every Pre while a handler listens. */
    KilnResult(KILN_CALL* hold)(uint32_t steps);
    /* The actions held since the last call (each once). *out_count = how many were written; KILN_ERR_BUFFER_TOO_SMALL:
     * more wait (call again). */
    KilnResult(KILN_CALL* held)(KilnGemCraft* out, uint32_t capacity, uint32_t* out_count);
    /* The answer for a held action (player, version): cancel 1 = refuse it, 0 = let it run. KILN_ERR_NOT_FOUND: no such
     * held action (it timed out and ran, or the player sent a newer one). */
    KilnResult(KILN_CALL* decide)(KilnEntity player, uint32_t version, uint32_t cancel);
    /* 1: players' actions that ran are reported by crafted() (code actions always are). */
    KilnResult(KILN_CALL* watch)(uint32_t on);
    /* The actions that ran since the last call, players' (while watched) and code's (DONE only), and every action on an
     * offered gem (KILN_GEMCRAFT_SOURCE, any status). */
    KilnResult(KILN_CALL* crafted)(KilnGemCraft* out, uint32_t capacity, uint32_t* out_count);

    /* ---- appended in 0.2.0-dev.29.15 (header.size covers them) ---- */
    /* Which appended parts run here: KILN_GEMCRAFT_HAS_*; `why` (may be NULL) says why one is off. Any thread. */
    KilnResult(KILN_CALL* extras)(uint32_t* out_flags, char* why, uint32_t why_cap);
    /* Offers gems to players (any number of players and holders in one call): each gets a new token and its expiry. An
     * offer of the same player and slot replaces the older one. KILN_ERR_INVALID_ARGUMENT: a bad entry (size, player,
     * holder, slot, gem, item, pay_from / pay_holder, lifetime, reserved); nothing was offered. KILN_ERR_BUFFER_TOO_SMALL:
     * a player would hold more than 256 live offers; nothing was offered. KILN_ERR_UNAVAILABLE: sources are off. Tick
     * thread. */
    KilnResult(KILN_CALL* offer)(KilnOwner owner, KilnGemSource* sources, uint32_t count);
    /* Withdraws the owner's offers (owner 0: every owner's) to `player` (0: every player) in `holder` (0: every
     * holder). Tick thread. */
    KilnResult(KILN_CALL* withdraw)(KilnOwner owner, KilnEntity player, KilnEntity holder, uint32_t* out_count);
    /* The live offers to `player` (every owner's), in offer order. KILN_ERR_BUFFER_TOO_SMALL: *out_count = how many
     * there are. Tick thread. */
    KilnResult(KILN_CALL* sources)(KilnEntity player, KilnGemSource* out, uint32_t capacity, uint32_t* out_count);
    /* Who would pay for `player` with pay_from / pay_holder now, in order (what a request resolves in the step it runs:
     * KILN_GEM_PAY_PLAYER: nobody listed, the player pays as the game does; base storage: the base the player stands
     * in, with the CanAccessInventories right). KILN_ERR_BUFFER_TOO_SMALL: *out_count = how many. Tick thread. */
    KilnResult(KILN_CALL* payers)(KilnEntity player, uint32_t pay_from, KilnEntity pay_holder, KilnEntity* out, uint32_t capacity,
                                  uint32_t* out_count);
} KilnGemcraftApi;

#ifdef __cplusplus
}
#endif
#endif

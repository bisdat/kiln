/* SPDX-License-Identifier: MIT
 * kiln.stations v1 (since 0.2.0-dev.29): the production stations of a base and the game's own station actions, for code.
 * Published by kiln-items.dll (native/services/items/stations.cpp) on the server world: a dedicated server, or a game client
 * that hosts. A joined client has no server world: status() says so and every call answers KILN_ERR_UNAVAILABLE.
 * Capabilities (sigdb features, server.d/stations.json): stations.read, stations.fill, stations.control, stations.events.
 * Evidence: docs/re/stations.md. Guide: docs/kiln/services/stations.md.
 *
 *  - A station is any entity with the game's FactoryStation component (kilns, smelters, looms, grinders, seed beds: 31
 *    templates). It runs the recipes of its Crafting.workshopId on its own, from one inventory that holds the inputs and
 *    the outputs. The selected recipe stays set and repeats; inputs leave when a cycle starts, outputs arrive when it ends.
 *  - A base is a record of the game's PlayerBasesSystemServer: the union of overlapping flame-altar zones (at most 16),
 *    with two lists the game rebuilds every server frame: its crafting-stock containers (the Magic storage chests and the
 *    player-planted refilling fruit trees: the template component InventoryCraftingStock) and its water sources (wells and
 *    barrels that hold water). Ordinary chests are never in it: the game's fill button and crafting from base storage
 *    never use them, and neither does this service. A base's id is the smallest BaseIdComponent.baseId of its altars (the
 *    16 bytes compared in memory order); it changes when an overlapping altar is added or removed.
 *  - Fill runs the game's own "fill from chests" (AutoTransferRecipe: every input topped up to "complete crafts loaded +
 *    n", taken from the base's stock containers in the game's list order, water from its water sources; one inventory
 *    transaction, all or nothing) with no player: nothing is taken from a player's bag unless the request names one.
 *    Kiln chooses n so the call does not fail for a short input (unless KILN_STATION_FILL_EXACT).
 *  - Stop, resume and select write the bytes the game's own buttons write (PlayerCraftingAction types 4 and 2) through the
 *    game's tracked write, and tell ServerFactoryInfo so open station screens refresh. Stopping aborts the running cycle
 *    and the game puts its inputs back into the station (progress is lost); KILN_STATION_AFTER_CRAFT waits for the cycle's
 *    end first. Select refuses a recipe no one on the server has unlocked (the world or a player in game, through
 *    kiln.knowledge) unless KILN_STATION_ALLOW_LOCKED.
 *  - A finished cycle is kiln.gameplay's KILN_GAMEPLAY_CRAFTED with actor == target == the station (once per cycle since
 *    0.2.0-dev.29). This table adds state, placed, removed and base-list events by polling (watch, events).
 *
 * Threads: status, submit, cancel, watch, last_seq: any thread. Every read (base_*, stations, station_read, recipe*,
 * category*, workshop_recipes, plan, events): the tick thread, Pre or Post (KILN_ERR_WRONG_THREAD otherwise). Actions run
 * inside kiln.items' program kiln_items_stations (registered through kiln.world, after the game's programs, so a player's
 * own action in the same step runs first); their callbacks come on the tick thread in Pre, one step after they ran.
 * The table only grows at the end. */
#ifndef KILN_STATIONS_H
#define KILN_STATIONS_H
#include "../kiln_api.h"
#include "kiln_items.h" /* KilnItemId, KilnSlotId, KilnItemsMoveChange */
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_STATIONS_SERVICE "kiln.stations"
#define KILN_STATIONS_VERSION 1u

#define KILN_STATIONS_MAX_BASES 16u   /* the game's own limit */
#define KILN_STATIONS_MAX_ALTARS 16u  /* altars listed per base (a base merges at most 16 zones) */
#define KILN_STATIONS_MAX_INPUTS 32u  /* recipe inputs (the game's data has at most 32) */
#define KILN_STATIONS_MAX_OUTPUTS 8u
#define KILN_STATIONS_MAX_CHANGES 32u /* ledger entries in a result */
#define KILN_STATIONS_MAX_KEEP 64u    /* reserves in a fill request */

/* A base's id: the 16 bytes of the game's BaseId (a GUID, saved with the altar). All zero: no altar was found. */
typedef struct KilnBaseId {
    uint8_t guid[16];
} KilnBaseId;

/* One category choice of a recipe (the game's RecipeInputCategorySelection, 8 bytes): for the input "any item of
 * category X", the option at `index` of that category. */
typedef struct KilnStationChoice {
    uint32_t category;
    uint8_t index;
    uint8_t reserved[3]; /* 0 */
} KilnStationChoice;

typedef struct KilnStationStack {
    KilnItemId item;
    uint32_t count;
} KilnStationStack;

/* KilnStation.state: the game's FactoryStationState */
enum {
    KILN_STATION_STATE_NO_RECIPE = 0,
    KILN_STATION_STATE_MISSING_MATERIALS = 1,
    KILN_STATION_STATE_PAUSED = 2,   /* no water under it / not enough energy (the game pauses the cycle) */
    KILN_STATION_STATE_STOPPED = 3,  /* the stop button (isStoppedByPlayer), or Kiln's stop */
    KILN_STATION_STATE_RUNNING = 4,
    KILN_STATION_STATE_OUTPUT_FULL = 5, /* the cycle ended and its outputs do not fit: retried every step */
};

/* KilnStation.needs: the game's missingRequirements (bit = FactoryStationRequirement) */
enum {
    KILN_STATION_NEEDS_RECIPE = 0x01u,
    KILN_STATION_NEEDS_ENERGY_SOURCE = 0x02u,
    KILN_STATION_NEEDS_ENERGY = 0x04u,
    KILN_STATION_NEEDS_INPUTS = 0x08u,
    KILN_STATION_NEEDS_OUTPUT_SPACE = 0x10u,
    KILN_STATION_NEEDS_ABOVE_WATER = 0x20u,
};

typedef struct KilnStationBase {
    uint32_t size;          /* set to sizeof(KilnStationBase) before calling */
    uint32_t index;         /* the game's record index this session (0..15): changes when the bases are rebuilt */
    KilnBaseId id;          /* the smallest altar baseId of the record */
    uint32_t altar_count;   /* altars whose zones make this base (more than one when their zones overlap) */
    KilnEntity altars[KILN_STATIONS_MAX_ALTARS];   /* their entities (this session) */
    KilnBaseId altar_ids[KILN_STATIONS_MAX_ALTARS]; /* their own baseId, kept in the save: a stable key per altar */
    uint32_t storage_count; /* crafting-stock containers the game lists for the base now */
    uint32_t water_count;   /* water sources that hold water */
    uint32_t zone_count;    /* altar zone boxes merged into the base */
    uint32_t station_count; /* production stations whose position is in the base */
    uint32_t version;       /* Kiln's count of base rebuilds this session (it moves when the records change) */
    KilnVec3 min, max;      /* the record's box, metres */
    uint32_t reserved[4];
} KilnStationBase;

typedef struct KilnStation {
    uint32_t size;          /* set to sizeof(KilnStation) before calling */
    KilnEntity entity;
    uint32_t workshop;      /* Crafting.workshopId: which recipes it runs */
    int32_t base_index;     /* -1: in no base */
    KilnBaseId base;        /* zero: in no base */
    KilnVec3 position;
    uint8_t state;          /* KILN_STATION_STATE_* */
    uint8_t stopped_by_player; /* FactoryStation.isStoppedByPlayer */
    uint8_t use_all_options;   /* FactoryStation.useAllAvailableCategoryIngredients */
    uint8_t needs;          /* KILN_STATION_NEEDS_* */
    uint32_t running_recipe;  /* the cycle in progress (0: none) */
    uint32_t selected_recipe; /* the recipe the station repeats (FactoryStation.nextRecipe; 0: none) */
    KilnStationChoice running_choices[4], selected_choices[4];
    uint32_t crafts_loaded; /* complete crafts of the selected recipe its inputs hold now (the game's count) */
    int64_t recipe_start_ns; /* the running cycle's start, world program time */
    int64_t pause_ns;        /* FactoryStation.recipePauseDuration */
    int64_t now_ns;          /* the world's program time of the last step (0 before kiln_items_stations ran) */
    int64_t duration_ns;     /* the cycle in progress' duration (running_recipe != 0), else the selected recipe's; scaled by
                                factoryProductionSpeedFactor */
    float progress;          /* 0..1 of the cycle in progress (while paused: where it paused), 0 when none runs */
    int32_t water, water_max; /* the station's water (Fuel units, 64 per charge; 0, 0 for a station without water) */
    uint32_t flags;          /* KILN_STATION_FLAG_* */
    uint32_t reserved[4];
} KilnStation;

/* KilnStation.flags */
enum {
    KILN_STATION_FLAG_STOP_PENDING = 0x1u, /* a STOP with KILN_STATION_AFTER_CRAFT waits for the cycle's end (not saved) */
};

/* One recipe input as the data has it: an item (item != 0) or "any item of a category" (category != 0). */
typedef struct KilnStationInput {
    KilnItemId item;
    uint32_t count;
    uint32_t category;
    uint32_t category_count;
} KilnStationInput;

typedef struct KilnStationRecipe {
    uint32_t size;          /* set to sizeof(KilnStationRecipe) before calling */
    uint32_t id;
    uint32_t workshop;
    uint32_t input_count;   /* entries in inputs (at most 32) */
    uint32_t output_count;  /* entries in outputs (at most 8 copied) */
    uint32_t water_charges; /* RecipeInfo.waterCharges */
    int64_t duration_ns;    /* RecipeInfo.craftingDuration (not scaled) */
    uint32_t knowledge_id;  /* RecipeInfo.knowledgeRequirement (the unlock; id 0: always unlocked) */
    uint32_t knowledge_value;
    uint8_t knowledge_compare, knowledge_type, knowledge_player_only, reserved0;
    uint32_t reserved1;
    KilnStationInput inputs[KILN_STATIONS_MAX_INPUTS];
    KilnStationStack outputs[KILN_STATIONS_MAX_OUTPUTS];
} KilnStationRecipe;

/* KilnStationRequest.action */
enum {
    KILN_STATION_FILL = 1,   /* the game's fill from the base's stock containers */
    KILN_STATION_STOP = 2,   /* the stop button */
    KILN_STATION_RESUME = 3, /* clears the stop (the station starts a new cycle) */
    KILN_STATION_SELECT = 4, /* the recipe the station repeats, with its category choices */
};

/* KilnStationRequest.flags */
enum {
    KILN_STATION_FILL_ADD = 0x0001u,     /* FILL: crafts = this many more complete crafts than are loaded (the game's
                                            meaning); default: crafts = how many to have loaded in all */
    KILN_STATION_FILL_EXACT = 0x0002u,   /* FILL: all of it or nothing (no smaller amount) */
    KILN_STATION_DRY_RUN = 0x0004u,      /* FILL: run the game's function and roll back: says what would move */
    KILN_STATION_NO_WATER = 0x0008u,     /* FILL: pour no water (waterCharges 0) */
    KILN_STATION_AFTER_CRAFT = 0x0010u,  /* STOP: when the running cycle has ended (nothing is refunded or lost) */
    KILN_STATION_KEEP_STOPPED = 0x0020u, /* SELECT: change the recipe, leave a stopped station stopped */
    KILN_STATION_ALLOW_LOCKED = 0x0040u, /* SELECT: also a recipe no one on the server has unlocked */
};

typedef struct KilnStationRequest {
    uint32_t size;          /* sizeof(KilnStationRequest) */
    uint32_t action;        /* KILN_STATION_FILL .. SELECT */
    uint32_t flags;         /* KILN_STATION_* request flags */
    KilnEntity station;
    uint32_t crafts;        /* FILL: 1..1000 */
    uint32_t recipe;        /* SELECT: the recipe; FILL: 0 = the station's selected recipe (else that one) */
    KilnStationChoice choices[4]; /* SELECT, FILL with a recipe: the category choices; all zero = the station's own */
    KilnEntity player;      /* FILL: also take from this player's inventories first, as the game's button does (a player
                               entity, 1..1023); 0 = never */
    uint32_t keep_count;    /* FILL: entries in keep (0..64) */
    const KilnStationStack* keep; /* FILL: leave at least this many of each item in the base's stock (copied by submit) */
    uint32_t reserved[4];   /* 0 */
} KilnStationRequest;

/* KilnStationResult.status (also KilnStationPlan.status) */
enum {
    KILN_STATION_DONE = 0,          /* it happened */
    KILN_STATION_PARTIAL = 1,       /* FILL: fewer crafts than asked (the stock or the station's room) */
    KILN_STATION_NOT_ENOUGH = 2,    /* FILL: the base's stock (after the reserves) covers not even one craft */
    KILN_STATION_NO_ROOM = 3,       /* FILL: the station has no room for one more craft's inputs */
    KILN_STATION_NOT_A_STATION = 4, /* the entity has no FactoryStation (gone, or never one) */
    KILN_STATION_NO_BASE = 5,       /* FILL: the station stands in no base */
    KILN_STATION_NO_RECIPE = 6,     /* FILL: no recipe selected (or the recipe is unknown) */
    KILN_STATION_WRONG_WORKSHOP = 7,/* the recipe is not one of this station's workshop */
    KILN_STATION_UNCHANGED = 8,     /* nothing to do: already stopped / running / selected / loaded */
    KILN_STATION_LOCKED = 9,        /* SELECT: no one on the server has unlocked the recipe (message says how it was checked) */
    KILN_STATION_REFUSED = 10,      /* the game's function refused it (game_error) */
    KILN_STATION_UNAVAILABLE = 11,  /* the capability went off after the request was accepted */
    KILN_STATION_DROPPED = 12,      /* not run: the world closed, or kiln_items_stations did not run it in 10 s */
    KILN_STATION_TIMED_OUT = 13,    /* STOP after craft: the cycle did not end in time (about an hour of steps); nothing written */
    KILN_STATION_QUEUE_FULL = 14,   /* not taken: 1024 actions wait (submit's KILN_ERR_BUFFER_TOO_SMALL; a managed result) */
    KILN_STATION_CANCELED = 15,     /* STOP after craft: withdrawn before it was written (a later resume, select or stop on
                                       the station, Kiln's or a player's; the message says which); nothing written */
};

/* KilnStationResult.limited_by / KilnStationPlan.limited_by: what kept a FILL below what was asked */
enum {
    KILN_STATION_LIMIT_NONE = 0,
    KILN_STATION_LIMIT_STOCK = 1,   /* the base's stock (short_item) */
    KILN_STATION_LIMIT_RESERVE = 2, /* the request's reserve of short_item */
    KILN_STATION_LIMIT_ROOM = 3,    /* the station's room */
    KILN_STATION_LIMIT_WATER = 4,   /* water: the base's water sources, or the station's water capacity */
};

typedef struct KilnStationResult {
    uint32_t size;          /* sizeof(KilnStationResult) of kiln.stations */
    int32_t status;         /* KILN_STATION_DONE .. DROPPED */
    uint32_t action;
    uint32_t flags;         /* the request's flags */
    KilnEntity station;
    uint32_t recipe;        /* FILL / SELECT: the recipe */
    uint32_t crafts_requested; /* FILL: what the request asked (after the UpTo arithmetic: crafts to add) */
    uint32_t crafts_before, crafts_after; /* FILL: complete crafts loaded before and after */
    uint32_t crafts_added;  /* FILL: the n the game's function ran with */
    int32_t game_error;     /* FILL: AutoTransferRecipe's code of the last attempt (1 no inputs or water, 2 no room in the
                               station, 4 missing materials, 5 no station inventory), 0 */
    uint32_t change_count;  /* entries in changes */
    uint8_t state_before, state_after, stopped_before, stopped_after;
    uint8_t limited_by;     /* FILL, not DONE: KILN_STATION_LIMIT_* */
    uint8_t reserved8[3];
    KilnItemId short_item;  /* FILL, limited by STOCK or RESERVE: the item; 0 otherwise */
    uint32_t reserved;
    uint64_t ticket, step;
    KilnItemsMoveChange changes[KILN_STATIONS_MAX_CHANGES]; /* FILL: the transaction's ledger (a take: from_slot set, to_slot
                               0; a put into the station: to_slot set, from_slot 0) */
    char message[128];      /* why, in words, for anything but DONE; NUL-terminated */
} KilnStationResult;

typedef void(KILN_CALL* KilnStationDoneFn)(const KilnStationResult* result, void* user);

/* What a fill would do now (plan(), tick thread; no game function runs). */
typedef struct KilnStationPlan {
    uint32_t size;          /* set to sizeof(KilnStationPlan) before calling */
    int32_t status;         /* KILN_STATION_DONE (crafts_planned > 0), UNCHANGED, NOT_ENOUGH, NO_BASE, NO_RECIPE, ... */
    KilnEntity station;
    uint32_t recipe;
    int32_t base_index;
    uint32_t crafts_loaded;     /* complete crafts the station holds now */
    uint32_t crafts_affordable; /* more crafts the base's stock covers (after the reserves) */
    uint32_t crafts_planned;    /* what a fill with this request would ask the game for */
    uint32_t input_count;       /* resolved inputs (the category choices applied) */
    KilnItemId short_item;      /* limited by STOCK or RESERVE: the item; 0 otherwise */
    uint8_t limited_by;         /* KILN_STATION_LIMIT_* when crafts_planned is below what was asked */
    uint8_t use_all_options;    /* the station has "use all ingredient options" on: it may consume other options */
    uint8_t reserved8[2];
    uint32_t water_per_craft;   /* Fuel units one craft needs (0: none, or NO_WATER) */
    uint32_t water_have;        /* in the station now */
    uint32_t water_stock;       /* in the base's water sources */
    KilnStationStack inputs[KILN_STATIONS_MAX_INPUTS]; /* per craft */
    uint32_t have[KILN_STATIONS_MAX_INPUTS];           /* in the station now */
    uint32_t stock[KILN_STATIONS_MAX_INPUTS];          /* in the base's stock containers, after the reserves (plus the
                                                          named player's inventories) */
    char message[128];
} KilnStationPlan;

/* KilnStationEvent.kind */
enum {
    KILN_STATION_EV_STATE = 1,   /* state, stop flag, selected or running recipe changed (seen in Pre) */
    KILN_STATION_EV_PLACED = 2,  /* a station appeared (placed, or loaded) */
    KILN_STATION_EV_REMOVED = 3, /* a station is gone */
    KILN_STATION_EV_BASES = 4,   /* the game rebuilt its bases (an altar placed, removed or upgraded) */
};

typedef struct KilnStationEvent {
    uint32_t size;
    uint32_t kind;          /* KILN_STATION_EV_* */
    uint64_t seq;           /* increasing; events(after = the last seq you saw) */
    uint64_t step;
    KilnEntity station;
    uint32_t workshop;
    uint8_t state_before, state_after, stopped_before, stopped_after;
    uint32_t selected_before, selected_after;
    uint32_t running_before, running_after;
    int32_t base_index;     /* this session's record index; -1: in no base */
    uint32_t reserved0;
    KilnOwner by;           /* STATE: the owner whose kiln.stations request made exactly this change (its stop flag or its
                               selection) in the step before; 0 = the game, a player, or a change the game made after it */
    KilnVec3 position;
    KilnBaseId base;        /* the base's id (zero: in no base) */
} KilnStationEvent;

typedef struct KilnStationsStatus {
    uint32_t size;          /* set to sizeof(KilnStationsStatus) before calling */
    uint8_t read;           /* 1: reads work here (stations.read and a server world) */
    uint8_t fill;           /* 1: FILL runs (stations.fill, the program verified) */
    uint8_t control;        /* 1: STOP / RESUME / SELECT run (stations.control, the program verified) */
    uint8_t events;         /* 1: watch() works */
    uint8_t program;        /* KILN_ITEMS_PROGRAM_* of kiln_items_stations */
    uint8_t watching;       /* 1: a watch is on */
    uint8_t knowledge;      /* 1: kiln.knowledge is here (SELECT's unlock check) */
    uint8_t scanned;        /* 1: the first scan of the world's stations is done (stations() is complete from then on) */
    uint32_t bases;         /* the game's bases now */
    uint32_t stations;      /* stations seen at the last scan */
    uint32_t pending;       /* requests waiting */
    uint32_t done;          /* requests answered so far */
    char detail[160];       /* why something is off, in words; "" when all works */
} KilnStationsStatus;

typedef struct KilnStationsApi {
    KilnServiceHeader header; /* { sizeof(KilnStationsApi), KILN_STATIONS_VERSION } */

    KilnResult(KILN_CALL* status)(KilnStationsStatus* out);
    /* The game's bases. base_at: index 0..count-1. base_of: the base the entity stands in (KILN_ERR_NOT_FOUND: none). */
    KilnResult(KILN_CALL* base_count)(uint32_t* out);
    KilnResult(KILN_CALL* base_at)(uint32_t index, KilnStationBase* out);
    KilnResult(KILN_CALL* base_of)(KilnEntity entity, KilnStationBase* out);
    KilnResult(KILN_CALL* base_at_position)(const KilnVec3* position, KilnStationBase* out);
    /* A base's crafting-stock containers / water sources in the game's order; its stock summed per item (stock
     * containers only). KILN_ERR_BUFFER_TOO_SMALL: *out_count = what is needed. */
    KilnResult(KILN_CALL* base_storage)(uint32_t index, KilnEntity* out, uint32_t capacity, uint32_t* out_count);
    KilnResult(KILN_CALL* base_water)(uint32_t index, KilnEntity* out, uint32_t capacity, uint32_t* out_count);
    KilnResult(KILN_CALL* base_stock)(uint32_t index, KilnStationStack* out, uint32_t capacity, uint32_t* out_count);
    /* Production stations: base_index -1 = all, else those in that base. A scan of the world's entities at most every 32
     * steps (or on demand when older). */
    KilnResult(KILN_CALL* stations)(int32_t base_index, KilnEntity* out, uint32_t capacity, uint32_t* out_count);
    /* KILN_ERR_NOT_FOUND: the entity has no FactoryStation. */
    KilnResult(KILN_CALL* station_read)(KilnEntity station, KilnStation* out);
    /* Recipes: KILN_ERR_NOT_FOUND for an unknown id. recipe_inputs: the game's own resolution with choices (NULL or all
     * zero: index 0 of every category), counts scaled as the game scales them (ceil(count x option.scaling)). */
    KilnResult(KILN_CALL* recipe)(uint32_t id, KilnStationRecipe* out);
    KilnResult(KILN_CALL* recipe_inputs)(uint32_t recipe, const KilnStationChoice* choices4, KilnStationStack* out,
                                         uint32_t capacity, uint32_t* out_count);
    /* Ingredient categories (22 in the game's data): the options of one (item, scaling); category_at walks them. */
    KilnResult(KILN_CALL* category)(uint32_t id, KilnItemId* items, float* scaling, uint32_t capacity, uint32_t* out_count);
    KilnResult(KILN_CALL* category_at)(uint32_t index, uint32_t* out_id);
    /* The recipes of a workshop (a station's Crafting.workshopId). */
    KilnResult(KILN_CALL* workshop_recipes)(uint32_t workshop, uint32_t* out, uint32_t capacity, uint32_t* out_count);
    /* What a FILL request would do now (no game function runs). */
    KilnResult(KILN_CALL* plan)(const KilnStationRequest* request, KilnStationPlan* out);
    /* Queues an action. KILN_ERR_INVALID_ARGUMENT: bad size, action, flags or reserved fields, station 0, crafts outside
     * 1..1000, keep_count over 64. KILN_ERR_UNAVAILABLE: the capability is off or no server world here.
     * KILN_ERR_BUFFER_TOO_SMALL: 1024 actions wait. KILN_OK: `done` (may be NULL) runs once. */
    KilnResult(KILN_CALL* submit)(KilnOwner owner, const KilnStationRequest* request, KilnStationDoneFn done, void* user,
                                  uint64_t* out_ticket);
    /* Drops an action's callback (the action may still run). KILN_ERR_NOT_FOUND: no such ticket waiting. */
    KilnResult(KILN_CALL* cancel)(uint64_t ticket);
    /* Events are collected while any watch is on (each on() needs an off()); they cost a scan every 32 steps and a read
     * of every station each step. */
    KilnResult(KILN_CALL* watch)(uint32_t on);
    /* Events with seq > after, oldest first (the last 1024 are kept). KILN_ERR_BUFFER_TOO_SMALL: more wait. */
    KilnResult(KILN_CALL* events)(uint64_t after, KilnStationEvent* out, uint32_t capacity, uint32_t* out_count);
    uint64_t(KILN_CALL* last_seq)(void);
} KilnStationsApi;

#ifdef __cplusplus
}
#endif
#endif

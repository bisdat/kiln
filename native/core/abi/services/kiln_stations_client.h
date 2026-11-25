/* SPDX-License-Identifier: MIT
 * kiln.stations.client v1 (since 0.2.0-dev.31.13, mod API 0.2.22): a read-only view of the production stations a game
 * client knows from its own world, for a player who joined someone else's world (where kiln.stations has no server world
 * and answers nothing) and for a hosting player's own client. Served by kiln-items.dll on the game client. A table of its
 * own: kiln.stations does not grow for it. Capability: stations.client_view (sigdb client.d/stations-client-view.json).
 * Managed face: Kiln.Blocks' ClientStations and ClientStationInfo. Guide: docs/kiln/services/stations.md ("On a joined
 * client: the client's own station view"); evidence: docs/re/stations.md section 14.
 *
 * Where it comes from. The server's factory_station_info program keeps one row per station in ServerFactoryInfo and sends
 * changed rows to the players; the client keeps them in its world's ClientFactoryInfo resource (the cache the game's own
 * station HUD and station screen read), at most 1000 rows, grouped by the base the client puts the station in. Only
 * stations standing in a base (a flame altar's zone) that the client's own base list knows are kept. Which stations the
 * server sends to which player, and how far from them, was not traced: report what the cache holds.
 *
 * What a row has: the station, its workshop and selected recipe with its category choices, "use all options", its state
 * and what it lacks, the complete crafts its inputs still hold, its water, the start of the cycle in progress, the base
 * it is grouped under and its position (0.25 m steps). Not in a row: the running recipe (when a player changed the
 * selection during a cycle, the cycle in progress is of the earlier recipe), the pause time, the stop flag of a stop that
 * waits for the cycle's end. A row is as fresh as the last update the server sent for it.
 *
 * Computed on the client from that row, the game's own way (its HUD's remaining-time helper, which Kiln re-implements and
 * sigdb qualifies): one craft's duration (the selected recipe's craftingDuration in the client's RecipeRegistry, scaled by
 * the client's factory speed through the game's FactorySpeed), the cycle in progress' remaining time and progress, and the
 * time until every loaded craft is done. Only while the row's state is Running (as the game's HUD), on the client's clock.
 * Outputs: the finished stacks of the selected recipe's outputs (an item that is no input of it) in the client world's
 * copy of the station's inventory, read every 15th copy; absent when the client has no copy of that inventory.
 *
 * Threads: every function on any thread. The view is a copy the client world's thread takes after its step (Post),
 * while some caller read it in the last KILN_STATIONS_CLIENT_DEMAND_MS milliseconds: the first read after a pause starts
 * the copying and answers from the previous copy (or none: KILN_OK with no rows). Nothing here writes game memory or
 * sends anything to the server.
 *
 *   status    what works here, the copy's step and age. KILN_OK always (size checked).
 *   stations  every station of the copy, in the cache's order. *out_count = how many. More than capacity: the first
 *             capacity rows are written and KILN_ERR_BUFFER_TOO_SMALL. capacity 0 with out NULL asks for the count.
 *             KILN_ERR_UNAVAILABLE: stations.client_view is off, or this is no game client.
 *   station   one station of the copy; KILN_ERR_NOT_FOUND when the copy has none of that id.
 * KILN_ERR_INVALID_ARGUMENT: a NULL pointer where a result goes, out NULL with a capacity, a row whose size is not
 * sizeof(KilnStationClientView). */
#ifndef KILN_STATIONS_CLIENT_H
#define KILN_STATIONS_CLIENT_H
#include "../kiln_api.h"
#include "kiln_stations.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_STATIONS_CLIENT_SERVICE "kiln.stations.client"
#define KILN_STATIONS_CLIENT_VERSION 1u
#define KILN_STATIONS_CLIENT_MAX_OUTPUTS 8u
#define KILN_STATIONS_CLIENT_MAX_ROWS 1000u /* the game's ClientFactoryInfo holds at most this many */
#define KILN_STATIONS_CLIENT_DEMAND_MS 5000u

/* KilnStationClientView.flags */
enum {
    KILN_STATION_CLIENT_RECIPE = 0x1u,  /* the selected recipe is in the client's registry: craft_ns is set */
    KILN_STATION_CLIENT_TIME = 0x2u,    /* state Running with a finite time: craft_remaining_ns, remaining_ns, progress */
    KILN_STATION_CLIENT_OUTPUTS = 0x4u, /* outputs were read from the client world's copy of the station's inventory */
    KILN_STATION_CLIENT_ENERGY = 0x8u,  /* the station has a rotational-energy consumer (it needs a power source) */
};

typedef struct KilnStationClientView {
    uint32_t size;                 /* 0: sizeof(KilnStationClientView), set by the caller */
    KilnEntity entity;             /* 4: the station (the server's entity id; the client uses the same ids) */
    uint32_t workshop;             /* 8: Crafting.workshopId */
    uint32_t selected_recipe;      /* 12: the recipe it repeats (FactoryStation.nextRecipe); 0: none */
    KilnBaseId base;               /* 16: the base the client grouped it under (its altar's BaseId) */
    float position[3];             /* 32: metres, as sent (0.25 m steps) */
    uint8_t state;                 /* 44: KILN_STATION_STATE_* as sent */
    uint8_t needs;                 /* 45: KILN_STATION_NEEDS_* (FactoryStation.missingRequirements) */
    uint8_t use_all_options;       /* 46 */
    uint8_t reserved0;             /* 47 */
    KilnStationChoice selected_choices[4]; /* 48 */
    uint32_t crafts_loaded;        /* 80: complete crafts its inputs still hold (the server's count) */
    int32_t water, water_max;      /* 84, 88: Fuel units (64 per charge), 0, 0 without water */
    uint32_t flags;                /* 92: KILN_STATION_CLIENT_* */
    int64_t recipe_start_ns;       /* 96: the cycle's start, on the game clock the client shows */
    int64_t now_ns;                /* 104: that clock at the copy */
    int64_t craft_ns;              /* 112: one craft of the selected recipe, scaled; 0 when unknown */
    int64_t craft_remaining_ns;    /* 120: the cycle in progress (KILN_STATION_CLIENT_TIME), at least 0 */
    int64_t remaining_ns;          /* 128: until every loaded craft is done, the game's HUD value (TIME), at least 0 */
    float progress;                /* 136: 0..1 of the cycle in progress (TIME), else 0 */
    uint32_t output_count;         /* 140 */
    KilnStationStack outputs[KILN_STATIONS_CLIENT_MAX_OUTPUTS]; /* 144 */
    uint64_t step;                 /* 208: the client world's step of the copy */
    uint32_t reserved[4];          /* 216 */
} KilnStationClientView;           /* 232 */

typedef struct KilnStationsClientStatus {
    uint32_t size;      /* 0: sizeof(KilnStationsClientStatus), set by the caller */
    uint8_t on;         /* 4: stations.client_view resolved on this client and nothing faulted */
    uint8_t world;      /* 5: a client world runs now */
    uint8_t copying;    /* 6: a caller read in the last KILN_STATIONS_CLIENT_DEMAND_MS */
    uint8_t reserved0;  /* 7 */
    uint32_t rows;      /* 8: stations in the copy */
    uint32_t reserved1; /* 12 */
    uint64_t step;      /* 16: the client world's step of the copy (0: none yet) */
    int64_t age_ms;     /* 24: since the copy was taken (-1: none) */
    char detail[160];   /* 32: why it is off, in words; "" when it works */
} KilnStationsClientStatus; /* 192 */

typedef struct KilnStationsClientApi {
    KilnServiceHeader header; /* KILN_STATIONS_CLIENT_VERSION; append-only */
    KilnResult(KILN_CALL* status)(KilnStationsClientStatus* out);
    KilnResult(KILN_CALL* stations)(KilnStationClientView* out, uint32_t capacity, uint32_t* out_count);
    KilnResult(KILN_CALL* station)(KilnEntity station, KilnStationClientView* out);
} KilnStationsClientApi;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
static_assert(sizeof(KilnStationClientView) == 232, "KilnStationClientView is 232 bytes");
static_assert(offsetof(KilnStationClientView, base) == 16 && offsetof(KilnStationClientView, state) == 44, "view head");
static_assert(offsetof(KilnStationClientView, selected_choices) == 48 && offsetof(KilnStationClientView, flags) == 92, "view middle");
static_assert(offsetof(KilnStationClientView, recipe_start_ns) == 96 && offsetof(KilnStationClientView, outputs) == 144, "view times");
static_assert(offsetof(KilnStationClientView, step) == 208, "view tail");
static_assert(sizeof(KilnStationsClientStatus) == 192 && offsetof(KilnStationsClientStatus, detail) == 32, "status");
static_assert(sizeof(KilnStationsClientApi) == 32, "KilnStationsClientApi is 32 bytes on x64");
#endif

#endif /* KILN_STATIONS_CLIENT_H */

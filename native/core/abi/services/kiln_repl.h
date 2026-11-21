/* SPDX-License-Identifier: MIT
 * kiln.repl v1 (since 0.2.0-dev.28): the game's ECS replication, read only. What the server sends each peer (per
 * machine and channel counters, the component bytes of one entity as each peer is sent them), what a Kiln client
 * receives, and on lab servers a loopback harness that runs the game's own replication for fake players.
 * Engine facts: docs/re/network.md (the dev.28 replication notes); the dossier's chapter 11.
 *
 * Machines and peers
 *  - The game replicates per machine (0..16, the GameServer's machine slots). An event or a stats row names the peer by
 *    its full handle (generation | machine), the handle in that machine's slot when the frame ran: a reused slot is a
 *    new peer. In a hosted world machine 0 is the local client; a dedicated server's machine 0 is the server itself and
 *    gets no replication.
 *  - A loopback fake (lab only, below) has no handle: it shows as KILN_REPL_FAKE_PEER | machine, unless a kiln.net lab
 *    peer was bound to its machine (kiln_net.h, KILN_NET_SELFTEST_BIND_MACHINE): then it shows as that lab peer.
 * Channels: 4 = client channel, 5 = prediction channel (the game's wire channel numbers).
 * Threads: the game's replication jobs run on the world's worker threads, in parallel per (machine, channel), after the
 * sim step; kiln.repl's hooks there only copy into memory of their own (machine, channel). Everything a caller gets
 * arrives on the tick thread (server) / main thread (client) in the next Pre.
 * Faults: kiln.hooks does not guard raw detours, so kiln.repl guards its own code in each of its detours: the game's
 * function runs exactly once and its result is untouched; a fault in kiln.repl's part disables that hook for the
 * session (it passes through) and logs "repl: observer <name> faulted at <where>; disabled" once.
 *
 * The table only grows at its end; readers check header.size. */
#ifndef KILN_REPL_H
#define KILN_REPL_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_REPL_SERVICE "kiln.repl"
#define KILN_REPL_VERSION 1u

/* capabilities(): what this table can do in this process (the bits are frozen). A bit is set when its sigdb feature
 * resolved and its code is there; the capability rows (repl.observe, repl.client, repl.loopback) say why one is not. */
enum {
    KILN_REPL_CAP_OBSERVE = 1u << 0,  /* sigdb feature repl.observe (server side, also a hosted world) */
    KILN_REPL_CAP_CLIENT = 1u << 1,   /* repl.client (a Kiln client's receive side) */
    KILN_REPL_CAP_LOOPBACK = 1u << 2, /* repl.loopback: the lab harness; only with policy "any" and a kiln.net lab port */
    KILN_REPL_CAP_FILTER = 1u << 3,   /* reserved (snapshot filters) */
    KILN_REPL_CAP_TOUCH = 1u << 4,    /* reserved */
    KILN_REPL_CAP_MESSAGES = 1u << 5, /* reserved */
    KILN_REPL_CAP_SCHEMA = 1u << 6,   /* reserved */
};

#define KILN_REPL_ALL_MACHINES 0xFFFFFFFFu
#define KILN_REPL_FAKE_PEER 0x80000000u /* a loopback fake without a bound lab peer: KILN_REPL_FAKE_PEER | machine */
#define KILN_REPL_MACHINES 17u          /* machine indices 0..16 */

enum { KILN_REPL_SEND = 1, KILN_REPL_RECEIVE = 2 };

/* KilnReplPeerStats.flags */
enum {
    KILN_REPL_PEER_FAKE = 1u,  /* a loopback fake machine (lab) */
    KILN_REPL_PEER_BOUND = 2u, /* ... with a kiln.net lab peer bound to it: `peer` is that lab peer */
    KILN_REPL_PEER_LOCAL = 4u, /* machine 0 of a hosted world: this process's own client (since 0.2.0-dev.31) */
    KILN_REPL_PEER_RECEIVED = 8u, /* a game client's receive counters: what this client received from its server
                                     (`peer` 0; stats_enable / peer_stats on the client world's thread; since 0.2.0-dev.31) */
};

/* Per machine and channel, since the first stats_enable of any owner (the counters, `dropped` too, start at 0 then). A
 * machine's rows start over when the peer in its slot changes (a reused slot is a new peer) and when the world stops;
 * the peer change is seen at the next Pre, so a slot taken over within one frame has that frame counted for neither
 * peer. Counted by the stats hooks on the game's job threads, each (machine, channel) in memory of its own; read in Pre. */
typedef struct KilnReplPeerStats {
    uint32_t size;                 /* set by the caller: sizeof(KilnReplPeerStats) */
    uint32_t peer;                 /* full peer handle (generation | machine); see "Machines and peers" */
    uint8_t channel;               /* 4 client, 5 prediction */
    uint8_t flags;                 /* KILN_REPL_PEER_* */
    uint8_t pad[6];
    uint64_t frames;               /* send callbacks (one batch per machine, channel and frame) */
    uint64_t messages, bytes;      /* messages the session accepted and their bytes (every message type) */
    uint64_t refused;              /* messages the session refused at allocation (batch count - batch sent) */
    uint64_t creates, updates, destroy_lists, destroyed_ids; /* of the accepted messages; ids of both lists */
    uint64_t unchanged;            /* writer skips with nothing changed */
    uint64_t too_big;              /* writer skips because the message was over the channel's maximum */
    uint64_t acks, resets, dropped; /* ack messages, reset requests, records the rings dropped */
} KilnReplPeerStats;

/* One component of a watched entity, as a peer is sent it (send) or as this client decoded it (receive). */
typedef struct KilnReplComponent {
    uint16_t index;                /* the component's index in the world's registry */
    uint16_t size;                 /* bytes at `data` */
    uint32_t hash;                 /* component hash (kiln_world.h component_hash) */
    uint8_t custom_codec;          /* 1: the game packs it with its own codec; a client sees it quantized */
    uint8_t pad[7];
    const uint8_t* data;
} KilnReplComponent;

/* KilnReplWatchEvent.kind */
enum {
    KILN_REPL_KIND_CREATE = 2,    /* a create message: the components as the peer will create the entity */
    KILN_REPL_KIND_UPDATE = 3,    /* an update message: the components' full values (never a delta) */
    KILN_REPL_KIND_DESTROYED = 4, /* a destroy list named the entity as destroyed (its list A); no components */
    KILN_REPL_KIND_ENDED = 5,     /* the watch is over (`reason`); the last event of a watch; no components */
};
/* KilnReplWatchEvent.reason (kind KILN_REPL_KIND_ENDED) */
enum {
    KILN_REPL_END_DESTROYED = 1,     /* the entity was destroyed (after its KILN_REPL_KIND_DESTROYED events) */
    KILN_REPL_END_ID_REUSED = 2,     /* the id now belongs to an entity of another template */
    KILN_REPL_END_PEER_LEFT = 3,     /* the one peer the watch was for left (its machine slot changed) */
    KILN_REPL_END_WORLD_STOPPED = 4, /* the server world stopped */
};

typedef struct KilnReplWatchEvent { /* tick thread (send) / main thread (receive), next Pre; valid during the call */
    uint32_t size;
    uint32_t peer;                 /* 0 for KILN_REPL_KIND_ENDED */
    uint8_t direction, channel, kind /* KILN_REPL_KIND_* */, gap /* 1: records were dropped before it */;
    uint32_t entity, tick, seq;
    uint32_t baseline_tick;        /* update: the tick of the state it was coded against (0: the template) */
    uint32_t payload_bytes;        /* create, update: the message's payload bytes (after its header) */
    uint16_t count;                /* components at `components` */
    uint8_t peer_flags;            /* KILN_REPL_PEER_* of `peer` (was pad before dev.31) */
    uint8_t pad;
    uint32_t reason;               /* KILN_REPL_END_* for KILN_REPL_KIND_ENDED, else 0 (was pad2 before dev.31) */
    const KilnReplComponent* components;
} KilnReplWatchEvent;

typedef void(KILN_CALL* KilnReplWatchFn)(const KilnReplWatchEvent* e, void* user);

/* KilnReplComponentInfo.flags */
enum {
    KILN_REPL_COMPONENT_REPLICATED = 1u,  /* a tracked write of it queues the entity for replication */
    KILN_REPL_COMPONENT_VERSIONED = 2u,   /* a version byte travels with it */
    KILN_REPL_COMPONENT_CUSTOM_CODEC = 4u, /* the game packs it with its own codec */
    KILN_REPL_COMPONENT_CLIENT = 8u,      /* may be on the client channel */
    KILN_REPL_COMPONENT_PREDICTION = 16u, /* may be on the prediction channel */
};

typedef struct KilnReplComponentInfo {
    uint32_t size;                 /* set by the caller: sizeof(KilnReplComponentInfo) */
    uint32_t hash;
    uint16_t index;                /* registry index */
    uint16_t data_size;            /* bytes of one component */
    uint32_t flags;                /* KILN_REPL_COMPONENT_* */
} KilnReplComponentInfo;

/* ---- lab: the loopback harness (KILN_REPL_CAP_LOOPBACK) ----
 * The game's own replication runs for "fake players" on free machine indices of the server world: the game activates a
 * replication object per fake, runs relevance, snapshot, writer, delta and send-callback jobs for it on its workers
 * every frame, and the fake's messages are captured at the send callback instead of a socket. In the next Pre the
 * server exe's own copy of the client decoder decodes them (against the template's state or the harness's earlier
 * decoded state, as a client does), each decoded state is compared component by component with the snapshot the
 * server coded, and the fake acknowledges every message through the game's own ack handler, so baselines advance as
 * with a real client. Nothing is relevant to a fake by itself (it has no player entity): lab_force_relevant names the
 * entities. What it does not give: a client world (merge, apply), a player entity, a session (bandwidth, receive).
 * Every lab function answers KILN_ERR_UNAVAILABLE unless kiln.json's policy is "any" AND kiln.net's lab transport is
 * open (labPort), and KILN_ERR_WRONG_THREAD off the tick thread. */
typedef struct KilnReplFake {
    uint32_t machine;              /* machine index (16, 15, ... the highest free ones) */
    uint32_t slot;                 /* player slot the game replicates it as (15, 14, ...) */
} KilnReplFake;

typedef struct KilnReplLoopbackStats { /* since the fakes were added (counters start over at an add from none) */
    uint32_t size;                 /* set by the caller: sizeof(KilnReplLoopbackStats) */
    uint32_t fakes;                /* bit per machine: the fakes active now */
    uint64_t messages;             /* messages captured from the fakes' batches (every type) */
    uint64_t decoded;              /* entity messages decoded */
    uint64_t components_equal;     /* generic components equal to the server's snapshot */
    uint64_t components_differ;    /* generic components that differ, or a decoded component set that differs */
    uint64_t custom_equal, custom_differ; /* components with the game's own codec (lossy: a difference is expected) */
    uint64_t decode_faults;        /* the decoder or the template state faulted or returned nothing */
    uint64_t no_baseline;          /* an update whose baseline the harness did not have (or an unknown template) */
    uint64_t acked;                /* message seqs acknowledged back */
    uint64_t dropped;              /* messages or snapshots that did not fit the capture buffers */
    uint64_t destroy_lists;        /* destroy-list messages captured */
    uint32_t active_objects;       /* bit per machine: the game's replication object is active now (also real players) */
    uint32_t slot_mask;            /* the world's mask of player slots being replicated, now */
} KilnReplLoopbackStats;

/* kiln.repl.lab hook_cost: kiln.repl's observer code (everything its hooks do besides calling the game) in nanoseconds, by hook,
 * over a window that starts at a call with reset = true; the game's send step and frame interval over the same window
 * (the cost's yardsticks); heap allocations made inside observer code (operator new in kiln-repl.dll; 0 by design). */
typedef struct KilnReplHookCost {
    uint32_t size;                 /* set by the caller: sizeof(KilnReplHookCost) */
    uint32_t frames;               /* send steps in the window */
    uint64_t window_ns;
    uint64_t send_step_mean_ns, send_step_p50_ns, frame_interval_mean_ns, frame_interval_p50_ns;
    uint64_t callback_calls, callback_ns; /* the stats/watch SendCallback hook, every machine and channel */
    uint64_t writer_calls, writer_ns;     /* the EntityWriter hook */
    uint64_t delta_calls, delta_ns;       /* the DeltaWriter hook (copies only a watched entity) */
    uint64_t tick_calls, tick_ns;         /* the AckReceive and EcsReset hooks (tick thread) */
    uint64_t allocations;                 /* heap allocations inside observer code (all hooks, also the harness's) */
    uint64_t watch_records, watch_dropped; /* watch records written / dropped by full rings */
} KilnReplHookCost;

typedef struct KilnReplApi {
    KilnServiceHeader header;      /* { sizeof(KilnReplApi), KILN_REPL_VERSION } */
    uint32_t(KILN_CALL* capabilities)(void); /* KILN_REPL_CAP_*; any thread */
    /* Reference-counted per owner (an owner holds one reference; enabling twice is once): the first enable of any owner
     * installs the stats hooks (SendCallback, EntityWriter, AckReceive, EcsReset) and zeroes the counters, the last
     * disable (or the owner's revoke) removes them. On the server world's thread (the tick thread; on a hosting game client,
     * the thread its server world steps on; KILN_ERR_WRONG_THREAD elsewhere).
     * On a game client's main thread (the client world's thread; KILN_REPL_CAP_CLIENT) the same two functions hold and read
     * the client's own receive counters instead: every message of both channels the client's session hands the game, in
     * rows flagged KILN_REPL_PEER_RECEIVED (frames = server ticks seen, messages, bytes, creates, updates, destroy lists and
     * ids; unchanged, too_big, refused, acks and resets stay 0; dropped = the receive watches' ring drops). Separate holders,
     * separate counters (from 0 at the first holder and at each new client world); the hook is the receive watches' one
     * (ClientStore). KILN_ERR_UNAVAILABLE there: repl.client is off, or kiln.hooks refused the hook.
     * KILN_ERR_UNAVAILABLE: repl.observe is off, or kiln.hooks refused a hook (the owner then holds nothing);
     * KILN_ERR_INVALID_ARGUMENT: owner 0, or 64 owners hold it already. */
    KilnResult(KILN_CALL* stats_enable)(KilnOwner owner, bool on);
    /* One row per machine and channel with anything counted; *count = the rows (also with KILN_ERR_BUFFER_TOO_SMALL,
     * when `cap` is too small; `out` may then be NULL). Each out[i].size is set by the caller. On the server world's
     * thread, as stats_enable. KILN_ERR_UNAVAILABLE: repl.observe is off. */
    KilnResult(KILN_CALL* peer_stats)(KilnReplPeerStats* out, uint32_t cap, uint32_t* count); /* KILN_ERR_BUFFER_TOO_SMALL */
    /* What `peer_or_all` (KILN_REPL_ALL_MACHINES, or a peer handle as peer_stats names it, a fake's too) is sent of
     * entity `e` (direction KILN_REPL_SEND, a server or a hosted world): `fn` gets one event per message that carries
     * the entity, with its full component values as the game coded them (the snapshot, not the delta), then
     * KILN_REPL_KIND_DESTROYED for a destroy list that names it, and KILN_REPL_KIND_ENDED as its last event; after ENDED
     * the watch's slot is given back by itself (remove() is not needed then, and answers KILN_ERR_NOT_FOUND once the
     * slot is gone). Events come on the server world's thread in the next Pre. While a watch exists the watch hooks are installed (EntityWriter,
     * DeltaWriter, SendCallback); a watch copies only its own entity's components. At most 64 watches. Tick thread.
     * KILN_REPL_RECEIVE (a game client, since 0.2.0-dev.31; KILN_REPL_CAP_CLIENT): what this client receives of `e`, from
     * the game's own decode of the client channel; `peer_or_all` is ignored; events on the game's main thread at the
     * client world's next Pre; at most 16; call it on the main thread (KILN_ERR_WRONG_THREAD otherwise).
     * KILN_ERR_INVALID_ARGUMENT: entity 0, no fn, a bad direction, a peer that is not connected, or no free watch;
     * KILN_ERR_UNAVAILABLE: repl.observe (send) or repl.client (receive) is off, or its hooks could not be installed. */
    KilnResult(KILN_CALL* watch)(KilnOwner owner, KilnEntity e, uint32_t peer_or_all, uint32_t direction,
                                 KilnReplWatchFn fn, void* user, KilnHandle* out);
    /* Any handle this service returned; any thread. A removed watch gets no event that starts after the call.
     * KILN_ERR_INVALID_ARGUMENT: h is 0; KILN_ERR_NOT_FOUND: not a live handle of this service (an ended watch's slot
     * may already be gone). */
    KilnResult(KILN_CALL* remove)(KilnHandle h);
    /* The registry's facts about a component, from the live world (cached at its first Pre). Any thread.
     * KILN_ERR_NOT_FOUND: no component of that hash; KILN_ERR_UNAVAILABLE: no world seen yet. */
    KilnResult(KILN_CALL* component_info)(uint32_t hash, KilnReplComponentInfo* out);
    /* Since 0.2.0-dev.31: writes everything `peer` is sent for `seconds` (1..600) as JSON lines (tools/replinspect/README.md)
     * into the file `path` names in kiln/logs: a plain file name (letters, digits, '.', '_', '-'; empty: one is made up,
     * repl-<peer>-<utc>.jsonl), never a path, never over a file that is there; at most 256 MiB. Ends early when `owner`
     * goes, the peer leaves or the world stops. One capture at a time. On the server world's thread.
     * KILN_ERR_INVALID_ARGUMENT: owner 0, a bad name, peer or length, a peer that is not connected, a file that cannot be
     * made or is there, or a capture runs already; KILN_ERR_UNAVAILABLE: repl.observe is off, no world yet, or the hooks
     * could not go in (then no file is made). */
    KilnResult(KILN_CALL* capture)(KilnOwner owner, uint32_t peer, uint32_t seconds, KilnStr path);

    /* ---- lab: the loopback harness; since 0.2.0-dev.28; check header.size ---- */
    /* Adds `count` (1..16) fakes on the highest free machine indices and free player slots; `out` gets up to `cap` of
     * them, *added how many. They replicate from the next frame on. KILN_ERR_INVALID_ARGUMENT: a bad count, or fakes are
     * active already (remove them first); KILN_ERR_NOT_FOUND: not that many free machines or slots; KILN_ERR_UNAVAILABLE:
     * no server world yet (or no send step seen), or the harness is off. */
    KilnResult(KILN_CALL* lab_add_fakes)(uint32_t count, KilnReplFake* out, uint32_t cap, uint32_t* added);
    /* Makes `entity` relevant to fake `machine` on both channels, from the next frame on (at most 64 per fake).
     * KILN_ERR_INVALID_ARGUMENT: not a fake machine, or entity 0. */
    KilnResult(KILN_CALL* lab_force_relevant)(uint32_t machine, KilnEntity entity);
    /* Stops the fakes and lets the game free their replication objects (the world's slot bits go). Their counters stay
     * for lab_stats. KILN_OK also when there were none. */
    KilnResult(KILN_CALL* lab_remove_fakes)(void);
    KilnResult(KILN_CALL* lab_stats)(KilnReplLoopbackStats* out);
    /* The newest state the harness decoded for `entity` on fake `machine`'s client channel: the bytes of component
     * `hash` (*len = their count; copied up to `cap`; out may be NULL to ask the size) and that message's tick.
     * KILN_ERR_NOT_FOUND: nothing decoded for it, or that state has no such component; KILN_ERR_BUFFER_TOO_SMALL. */
    KilnResult(KILN_CALL* lab_decoded)(uint32_t machine, KilnEntity entity, uint32_t hash, uint8_t* out, uint32_t cap,
                                       uint32_t* len, uint32_t* tick);
} KilnReplApi;

/* ---- kiln.repl.lab (since 0.2.0-dev.31): more of the lab harness, in a table of its own ----
 * Published by the same DLL as kiln.repl, on servers. Every function answers KILN_ERR_UNAVAILABLE unless the harness
 * may run (kiln.json's policy "any" and kiln.net's lab port, as kiln.repl's lab functions), and KILN_ERR_WRONG_THREAD
 * off the tick thread. The table only grows at its end; readers check header.size. */
#define KILN_REPL_LAB_SERVICE "kiln.repl.lab"
#define KILN_REPL_LAB_VERSION 1u

typedef struct KilnReplLabApi {
    KilnServiceHeader header;      /* { sizeof(KilnReplLabApi), KILN_REPL_LAB_VERSION } */
    /* The payload (the bytes after the message header) of the newest message of `kind` (KILN_REPL_KIND_CREATE or
     * _UPDATE) that fake `machine` was sent for `entity` on its client channel, and that message's tick. Same buffer
     * rules as kiln.repl's lab_decoded. KILN_ERR_NOT_FOUND: no such message since the fakes were added. */
    KilnResult(KILN_CALL* payload)(uint32_t machine, KilnEntity entity, uint32_t kind, uint8_t* out, uint32_t cap,
                                   uint32_t* len, uint32_t* tick);
    /* What kiln.repl's own hook code costs (measured on lab servers only), since the last call with reset = true;
     * the first call starts the window. KILN_ERR_INVALID_ARGUMENT: out is NULL or out->size is too small. */
    KilnResult(KILN_CALL* hook_cost)(KilnReplHookCost* out, bool reset);
} KilnReplLabApi;

/* ---- kiln.repl.net (since 0.2.0-dev.31): what mods add to the game's replication, in a table of its own ----
 * Published by the same DLL as kiln.repl (servers, hosted worlds and game clients; capabilities() says what works in
 * this process). Sidecar tracking (which entities each peer was sent and lost, for Kiln's own channel 12), per-peer
 * snapshot filters and the item remap (the bytes one peer is sent of a component), touch (re-send an entity without
 * changing it), message hooks on the game's other channels and the game's message codec. The dossier's chapter 15.
 * Threads: registration on the tick thread (server) / main thread (client); filter functions run on the game's
 * replication job threads (any thread, in parallel per machine and channel, no allocation, no tick-thread API).
 * The table only grows at its end; readers check header.size. */
#define KILN_REPL_NET_SERVICE "kiln.repl.net"
#define KILN_REPL_NET_VERSION 1u

/* kiln.repl.net capabilities() (the bits are frozen) */
enum {
    KILN_REPL_NET_CAP_TRACK = 1u << 0,            /* track(KILN_REPL_SEND), peer_entities: server or hosted world */
    KILN_REPL_NET_CAP_CLIENT_TRACK = 1u << 1,     /* track(KILN_REPL_RECEIVE): a game client's client channel */
    KILN_REPL_NET_CAP_FILTER = 1u << 2,           /* filter_add (sigdb feature repl.filter) */
    KILN_REPL_NET_CAP_TOUCH = 1u << 3,            /* touch (repl.touch + kiln.world's write_other) */
    KILN_REPL_NET_CAP_REMAP = 1u << 4,            /* remap_set (repl.filter) */
    KILN_REPL_NET_CAP_MESSAGES_SEND = 1u << 5,    /* message_hook(KILN_REPL_SEND), message_inject */
    KILN_REPL_NET_CAP_MESSAGES_RECEIVE = 1u << 6, /* message_hook(KILN_REPL_RECEIVE) (repl.messages.server / .client) */
    KILN_REPL_NET_CAP_SCHEMA = 1u << 7,           /* schema_* (repl.schema) */
};

/* ---- tracking (sidecars) ---- */
enum {
    KILN_REPL_TRACK_CREATED = 1,   /* the peer was sent a create of the entity; tick = its generation for that peer */
    KILN_REPL_TRACK_DESTROYED = 2, /* a destroy list named it as destroyed (list A) */
    KILN_REPL_TRACK_LEFT = 3,      /* a destroy list named it as out of the peer's view (list B) */
    KILN_REPL_TRACK_PEER_GONE = 4, /* the machine's peer changed or the world stopped: forget everything of that peer */
    KILN_REPL_TRACK_GAP = 5,       /* records of that peer were lost (a full ring): re-read peer_entities */
};
typedef struct KilnReplTrackEvent {
    uint32_t peer;                 /* full peer handle (kiln_repl.h "Machines and peers"); 0 on a game client (the server) */
    uint32_t entity;               /* 0 for PEER_GONE and GAP */
    uint32_t tick;                 /* the message's server tick (CREATED: the create's tick = the entity's generation) */
    uint8_t kind;                  /* KILN_REPL_TRACK_* */
    uint8_t peer_flags;            /* KILN_REPL_PEER_* */
    uint8_t machine;               /* machine index (server); 0 on a client */
    uint8_t pad;
} KilnReplTrackEvent;
/* One call per Pre with the events of the last frame, oldest first (tick thread / client main thread); valid during it. */
typedef void(KILN_CALL* KilnReplTrackFn)(const KilnReplTrackEvent* events, uint32_t count, void* user);

typedef struct KilnReplPeerEntity {
    uint32_t entity;
    uint32_t create_tick;          /* the generation tracking saw; 0 = created before tracking began */
    uint8_t acked;                 /* the peer acknowledged its create */
    uint8_t pad[3];
} KilnReplPeerEntity;

/* ---- filters ---- */
enum { KILN_REPL_CLIENT_CHANNEL = 1u, KILN_REPL_PREDICTION_CHANNEL = 2u }; /* filter_add channels */
typedef struct KilnReplFilterCtx {
    uint32_t size;                 /* sizeof(KilnReplFilterCtx) */
    uint32_t peer;                 /* full peer handle (a fake's KILN_REPL_FAKE_PEER | machine) */
    uint8_t channel;               /* 4 or 5 */
    uint8_t peer_flags;            /* KILN_REPL_PEER_* */
    uint8_t machine;
    uint8_t pad;
    uint32_t entity;
    uint32_t tick;                 /* the server tick of the snapshot */
    uint32_t component_hash;       /* the component the scratch holds */
    /* Another component of any entity, read-only, as the world holds it now (job-safe; the world is quiet during the
     * send step). NULL when the entity or the component is absent; *out_size its bytes. Valid during the call. */
    const void*(KILN_CALL* read)(const struct KilnReplFilterCtx* self, uint32_t entity, uint32_t component_hash, uint32_t* out_size);
    /* Set *failed to 1 to say the filter failed (a managed filter that threw): as a fault, the component's edits of this
     * snapshot are discarded and the filter is removed. */
    uint32_t* failed;
} KilnReplFilterCtx;
/* Edits scratch (size bytes: this peer's copy of the component in its snapshot). Job threads, in parallel. A fault
 * discards the component's edits for this snapshot (the game's bytes go out), removes the filter and logs once. */
typedef void(KILN_CALL* KilnReplFilterFn)(uint8_t* scratch, uint32_t size, const KilnReplFilterCtx* ctx, void* user);

/* touch() out_status */
enum { KILN_REPL_TOUCH_QUEUED = 0, KILN_REPL_TOUCH_NO_CARRIER = 1, KILN_REPL_TOUCH_GONE = 2 };

/* ---- the item remap ---- */
typedef struct KilnReplItemSite {
    uint32_t component_hash;
    uint16_t offset;               /* byte offset of the first item id in the component */
    uint16_t count;                /* ids at offset, offset + stride, ... (1 for a single field) */
    uint16_t stride;               /* 0 when count is 1 */
    uint16_t pad;
} KilnReplItemSite;
typedef struct KilnReplItemMap {
    uint32_t from;                 /* a Kiln-only item id */
    uint32_t to;                   /* its look-alike, a game item id; 0 = the empty item */
} KilnReplItemMap;
typedef struct KilnReplRemapStats {
    uint32_t size;                 /* set by the caller: sizeof(KilnReplRemapStats) */
    uint32_t sites;                /* sites in the table */
    uint64_t snapshots;            /* snapshots of a site component sent to a remapped peer */
    uint64_t remapped;             /* ids replaced by their look-alike */
    uint64_t zeroed;               /* ids replaced by 0 (no look-alike, or neither a game item nor mapped) */
    uint64_t faults;               /* remap faults (every site of that component was zeroed) */
} KilnReplRemapStats;

/* ---- message hooks ---- */
enum { KILN_REPL_PASS = 0, KILN_REPL_DROP = 1, KILN_REPL_REWRITE = 2 };
typedef struct KilnReplMessage {
    uint32_t size;                 /* sizeof(KilnReplMessage) */
    uint32_t peer;                 /* server: the machine's or player's peer handle; client: 0 = the server */
    uint8_t channel;               /* the game's channel (0..24) */
    uint8_t direction;             /* KILN_REPL_SEND / KILN_REPL_RECEIVE */
    uint8_t player_socket;         /* 1: the player socket's loop, 0: the machine socket's */
    uint8_t pad;
    uint32_t length;               /* bytes at data */
    const uint8_t* data;           /* valid during the call */
    uint8_t* rewrite;              /* REWRITE: write the new bytes here (rewrite_cap = length) */
    uint32_t rewrite_cap;
    uint32_t pad2;
    uint32_t* rewrite_length;      /* REWRITE: the new length (at most rewrite_cap) */
} KilnReplMessage;
/* Returns KILN_REPL_PASS, _DROP or _REWRITE. Server: tick thread; client: main thread. */
typedef int32_t(KILN_CALL* KilnReplMessageFn)(const KilnReplMessage* m, void* user);

/* ---- the game's message codec ---- */
typedef struct KilnReplSchemaField {
    KilnStr name;
    uint32_t type;                 /* the game's type code */
    uint32_t element;              /* the element parameter (+0xc) */
    uint32_t shape;                /* 0 optional, 1 scalar, 2 dynamic array */
    uint32_t offset;               /* byte offset in the message struct */
    uint32_t count_offset;         /* optional: its present flag; dynamic array: its u16 count */
    uint32_t fixed_count;          /* inline array length (1 for a scalar) */
    uint64_t nested;               /* a nested schema (schema_fields takes it), or 0 */
} KilnReplSchemaField;

typedef struct KilnReplNetApi {
    KilnServiceHeader header;      /* { sizeof(KilnReplNetApi), KILN_REPL_NET_VERSION } */
    uint32_t(KILN_CALL* capabilities)(void); /* KILN_REPL_NET_CAP_*; any thread */
    /* Any handle this table returned (track, filter, message hook); any thread; takes effect before the next send step
     * (a filter or hook is never called after the Pre that follows). KILN_ERR_NOT_FOUND: not a live handle. */
    KilnResult(KILN_CALL* remove)(KilnHandle h);

    /* Tracking. KILN_REPL_SEND (server or hosted world): fn gets, in every Pre, the client-channel creates, destroy
     * lists and peer changes of the last frame for every machine, as the session accepted them. KILN_REPL_RECEIVE (a
     * game client): what this client stored on its client channel. Installs its hooks while any track exists. Tick
     * thread / client main thread. KILN_ERR_UNAVAILABLE: KILN_REPL_NET_CAP_TRACK / _CLIENT_TRACK is off. */
    KilnResult(KILN_CALL* track)(KilnOwner owner, uint32_t direction, KilnReplTrackFn fn, void* user, KilnHandle* out);
    /* The entities in the peer's client-channel state map (relevant to it now). *count = how many (also with
     * KILN_ERR_BUFFER_TOO_SMALL). Tick thread. KILN_ERR_INVALID_ARGUMENT: not a connected peer. */
    KilnResult(KILN_CALL* peer_entities)(uint32_t peer, KilnReplPeerEntity* out, uint32_t cap, uint32_t* count);

    /* Filters: fn edits component component_hash of every snapshot sent to peers (peer_count 0 = every peer) on
     * channels (KILN_REPL_*_CHANNEL); higher priority first, then registration order; the item remap runs after all.
     * sources (components of the same entity): a change of their bytes touches the entity so the client channel
     * re-snapshots it. Tick thread. KILN_ERR_UNAVAILABLE: repl.filter off (or repl.touch off with sources on the
     * client channel); KILN_ERR_INVALID_ARGUMENT: no fn, an unknown component, a component not on those channels. */
    KilnResult(KILN_CALL* filter_add)(KilnOwner owner, uint32_t component_hash, const uint32_t* peers, uint32_t peer_count,
                                      uint32_t channels, int32_t priority, const uint32_t* sources, uint32_t source_count,
                                      KilnReplFilterFn fn, void* user, KilnHandle* out);
    /* Another peer set for a filter; touches the entities whose output changes. Tick thread. */
    KilnResult(KILN_CALL* filter_set_peers)(KilnHandle filter, const uint32_t* peers, uint32_t peer_count);
    /* Marks e for re-sending on the client channel without changing a byte (the touch program runs in the next sim
     * step). *out_status = KILN_REPL_TOUCH_*. Tick thread. KILN_ERR_UNAVAILABLE: repl.touch is off. */
    KilnResult(KILN_CALL* touch)(KilnEntity e, uint32_t* out_status);

    /* The item remap. Replaces the whole table: item-id sites, the game's own item ids (game_ids, sorted), and the
     * Kiln-only ids' look-alikes (map, sorted by from). For a remapped peer every id at a site that is not a game
     * item becomes its look-alike or 0. Touches what changes. Tick thread. */
    KilnResult(KILN_CALL* remap_set)(const KilnReplItemSite* sites, uint32_t site_count, const uint32_t* game_ids,
                                     uint32_t game_id_count, const KilnReplItemMap* map, uint32_t map_count);
    /* The peers the remap applies to (vanilla peers, Kiln peers without the content); touches what changes. Tick thread. */
    KilnResult(KILN_CALL* remap_peers)(const uint32_t* peers, uint32_t peer_count);
    KilnResult(KILN_CALL* remap_stats)(KilnReplRemapStats* out);

    /* Message hooks on the game's channels (not 4, 5, 12, 14, 17): directions KILN_REPL_SEND | KILN_REPL_RECEIVE.
     * Tick thread (server) / main thread (client). KILN_ERR_UNAVAILABLE: that direction is off here. */
    KilnResult(KILN_CALL* message_hook)(KilnOwner owner, uint8_t channel, uint32_t directions, KilnReplMessageFn fn, void* user,
                                        KilnHandle* out);
    /* Sends data to peer on channel as a game message (schema channels 1, 15, 19, 20, 21 only). Tick thread.
     * KILN_ERR_INVALID_ARGUMENT: another channel, a peer that is not connected; KILN_ERR_UNAVAILABLE: the session refused. */
    KilnResult(KILN_CALL* message_inject)(uint32_t peer, uint8_t channel, const void* data, uint32_t length);

    /* The game's message codec. schema_find: a message schema by its name (*out_size its struct's bytes).
     * KILN_ERR_NOT_FOUND: no such schema; KILN_ERR_UNAVAILABLE: repl.schema off, or the schema uses a type Kiln does
     * not know (it is refused, not guessed). Any thread. */
    KilnResult(KILN_CALL* schema_find)(KilnStr name, uint32_t* out_size, uint64_t* out_schema);
    KilnResult(KILN_CALL* schema_fields)(uint64_t schema, KilnReplSchemaField* out, uint32_t cap, uint32_t* count);
    /* Decodes length bytes into out (zeroed first; out_size at least the schema's size). KILN_ERR_INVALID_ARGUMENT:
     * the game's decoder refused the bytes. Any thread. */
    KilnResult(KILN_CALL* schema_decode)(uint64_t schema, const void* bytes, uint32_t length, void* out, uint32_t out_size);
    KilnResult(KILN_CALL* schema_encode)(uint64_t schema, const void* msg, uint32_t msg_size, void* out, uint32_t cap,
                                         uint32_t* out_length);
    /* Each machine's peer as the last Pre saw it (kiln_repl.h "Machines and peers"): out_peers[m] the peer handle (0: none),
     * out_flags[m] its KILN_REPL_PEER_* flags, for m < min(cap, KILN_REPL_MACHINES). Tick thread (server) / main thread
     * (client: all 0). *count = KILN_REPL_MACHINES. */
    KilnResult(KILN_CALL* peers)(uint32_t* out_peers, uint8_t* out_flags, uint32_t cap, uint32_t* count);
} KilnReplNetApi;

#ifdef __cplusplus
static_assert(sizeof(KilnReplTrackEvent) == 16, "KilnReplTrackEvent is 16 bytes");
static_assert(sizeof(KilnReplPeerEntity) == 12, "KilnReplPeerEntity is 12 bytes");
static_assert(sizeof(KilnReplFilterCtx) == 40 && offsetof(KilnReplFilterCtx, read) == 24, "KilnReplFilterCtx is 40 bytes");
static_assert(sizeof(KilnReplItemSite) == 12 && sizeof(KilnReplItemMap) == 8, "remap records");
static_assert(sizeof(KilnReplRemapStats) == 40, "KilnReplRemapStats is 40 bytes");
static_assert(sizeof(KilnReplMessage) == 48 && offsetof(KilnReplMessage, data) == 16 && offsetof(KilnReplMessage, rewrite_length) == 40,
              "KilnReplMessage is 48 bytes");
static_assert(sizeof(KilnReplSchemaField) == 48, "KilnReplSchemaField is 48 bytes");
static_assert(sizeof(KilnReplNetApi) == 8 + 17 * sizeof(void*), "KilnReplNetApi v1: 17 members");
#endif

#ifdef __cplusplus
}
#endif
#endif /* KILN_REPL_H */

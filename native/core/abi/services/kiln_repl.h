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

#ifdef __cplusplus
}
#endif
#endif /* KILN_REPL_H */

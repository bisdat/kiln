/* SPDX-License-Identifier: MIT
 * kiln.component_changes v1 (server side, kiln-decisions.dll): net changes of named components of watched entities.
 * Built in 0.2.0-dev.29 (chapter 07 S7); the record and table prefixes are those landed with S1, the members marked
 * "appended" were added when the service was built. Readers check header.size before an appended member.
 *
 * Watches. A watch names up to 128 root entities (scope 0) or "every player in the game" (scope 1) and a set of
 * components: Health (1), Stamina (2), Mana (3), Fuel (4), Inventory (5), SlotSelection (6); the mask bit of index i
 * is 1 << (i - 1). Expansion 1 also watches Inventory on every 8-slot inventory linked from the root's InventorySetup
 * (Virtual links followed in link order, depth 3, at most 32 inventories and 64 setups) and Fuel on the item entities
 * in those inventories; the other components stay on the root. Every watch is on the server world (world_kind 0, the
 * only one v1 watches) and on one world session: when that world stops, the watch is Disposed and never resumed.
 *
 * Budgets: 32 watches (suspended ones count); 4096 distinct present (entity life, component) pairs over all watches;
 * 128 roots for any one pair. They are the process's, shared by every owner: no per-mod share or reservation, first
 * come first served (the earlier watch keeps its pairs when watches grow); status() and the service's row show what is
 * held. Absent pairs are not counted but are read every window. A watch that would exceed them is NOT created: watch()
 * answers KILN_OK with watch_id 0 and the reason (status 1 budget, 2 metadata). A watch whose expansion grows past them
 * later (a player joins, an inventory is linked, an absent component appears during the sim) is suspended whole before
 * anything of that step is emitted for it (status 1), as is a watch whose root cannot be followed any more (status 2:
 * gone, another life of its id, also when the id is reused inside the sim, an inventory tree Kiln cannot walk
 * completely). It never covers less than it says. resume() rebuilds it from scratch with a fresh baseline and no
 * catch-up change.
 *
 * Windows. The core calls window_begin after the last Pre subscriber and window_end after WaitWorldJobs, before the
 * Post subscribers (kiln_window_clock.h phases 2 and 4), and only in steps where window_wanted() is nonzero (a watch
 * exists, or a stop is to be carried out). window_begin refreshes the watched pairs (roots, links, players) and reads
 * the baseline of pairs that are new; window_end reads every pair and queues one record per pair whose bytes or
 * presence differ from its baseline, which is the pair's bytes at the previous window_end (or at the window_begin that
 * added it). So the interval of a record is from one compare to the next: the game's sim step and the Post and Pre
 * around it, where Kiln's own tick-thread writers (Combat.SetHealth, a decision's survival health) run. It is a net
 * change: A -> B -> A inside one interval gives no record; it is not a feed of every store. A watch that joins a pair
 * other watches cover splits its interval at window_begin (a record for the older watches if it changed since their
 * baseline): with a change after the split too, one pair has two records of the same step. Bytes are compared up to
 * 256: a larger component is absent (empty bytes), so one that grows past 256 looks like it went.
 *
 * Origin. Native writers that change a watched component call record_write with the bytes before and after, inside
 * the interval and while they hold the component. A record's origin is Kiln (2) when every token of the interval for
 * that pair names the same source and a chain of tokens, in the order they were recorded, leads from the baseline to
 * the final bytes; Mixed (3) when there are tokens but more than one source, no such chain, more than 32 tokens or a
 * token was lost; Unknown (0) when there is no token or a token's source is not valid. Game (1) is never emitted by v1:
 * a change without a token may be the game's, another loader's mod or raw memory, and Kiln cannot tell them apart.
 *
 * Events are read with freeze / read / ack batches. An ended watch is removed even from a frozen batch: it can shrink
 * between begin and read; use read_events' required count after success. The batch id remains valid until ack.
 * The queue holds
 * at most two frames (a frame: the records of one window_end, including interval splits and presence exchanges).
 * The present-pair budget is not a per-frame record limit. When a third frame would be needed (the
 * consumer did not read), the frame is dropped, every ready watch is suspended (status 1, LastCompleteStep the last
 * queued step) and counters().queue_overflows counts it: a change is never dropped silently.
 *
 * Threads: record_write and counters on any thread (record_write takes a bounded lock and never waits for ever; with
 * no watch it returns at once). Everything else: the world thread in a world-safe phase (kiln_window_clock.h),
 * KILN_ERR_WRONG_THREAD otherwise. window_begin, window_end, window_wanted, window_faulted: the core only.
 * Nothing in this service calls managed code: the managed host reads the batches in Pre.
 */
#pragma once
#include "kiln_decisions.h"
#pragma pack(push, 8)
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_COMPONENT_CHANGES_SERVICE "kiln.component_changes"
#define KILN_COMPONENT_CHANGES_VERSION 1

/* component index (KilnComponentChangeV1.component, KilnKnownWriteV1.component); mask bit = 1 << (index - 1) */
#define KILN_WATCH_HEALTH 1u
#define KILN_WATCH_STAMINA 2u
#define KILN_WATCH_MANA 3u
#define KILN_WATCH_FUEL 4u
#define KILN_WATCH_INVENTORY 5u
#define KILN_WATCH_SLOT_SELECTION 6u
#define KILN_WATCH_COMPONENTS_ALL 63u

#define KILN_WATCH_MAX_WATCHES 32u
#define KILN_WATCH_MAX_PAIRS 4096u
#define KILN_WATCH_MAX_ROOTS 128u
#define KILN_WATCH_MAX_TOKENS 32u   /* per pair and interval */
#define KILN_WATCH_MAX_BYTES 256u   /* a watched component's bytes */
#define KILN_WATCH_MAX_FRAMES 2u
#define KILN_WATCH_FRAME_RECORDS 4096u

/* KilnComponentWatchRequestV1.scope / .expansion */
#define KILN_WATCH_SCOPE_ENTITIES 0u
#define KILN_WATCH_SCOPE_PLAYERS 1u
#define KILN_WATCH_EXPAND_NONE 0u
#define KILN_WATCH_EXPAND_INVENTORY 1u

/* KilnComponentWatchStatusV1.status. For a watch() that created nothing (watch_id 0) it says why: 1 budget, 2 metadata. */
#define KILN_WATCH_READY 0u
#define KILN_WATCH_SUSPENDED_BUDGET 1u
#define KILN_WATCH_SUSPENDED_METADATA 2u
#define KILN_WATCH_DISPOSED 3u

/* KilnComponentChangeV1.origin */
#define KILN_CHANGE_ORIGIN_UNKNOWN 0u
#define KILN_CHANGE_ORIGIN_GAME 1u /* reserved: v1 never emits it */
#define KILN_CHANGE_ORIGIN_KILN 2u
#define KILN_CHANGE_ORIGIN_MIXED 3u

/* KilnComponentChangeV1.flags */
#define KILN_CHANGE_BEFORE_PRESENT 1u
#define KILN_CHANGE_AFTER_PRESENT 2u
#define KILN_CHANGE_TOKENS_TRUNCATED 4u
#define KILN_CHANGE_TOKENS_LOST 8u /* a record_write of the interval could not be taken (its lock was busy, the token store full) */

/* KilnComponentChangeV1.attribute: the typed values are valid (Health, Stamina, Mana from kiln.combat's pairs, Fuel from
 * kiln.items), read at the compares in a world-safe phase. 0: no typed values for this record. */
#define KILN_CHANGE_ATTRIBUTE_NONE 0u
#define KILN_CHANGE_ATTRIBUTE_HEALTH 1u
#define KILN_CHANGE_ATTRIBUTE_STAMINA 2u
#define KILN_CHANGE_ATTRIBUTE_MANA 3u
#define KILN_CHANGE_ATTRIBUTE_FUEL 4u

/* KilnEventEntityKey of an entity reached through a root's links (an inventory, an item entity): flags 0, life_serial
 * a serial of its own given when it entered the tree (a new one when it left every tree and came back, or when the item
 * id in its slot changed). SpawnTime and player sessions are the roots' (kiln.decisions' metadata). */

typedef struct KilnComponentWatchRequestV1 {
 uint32_t size,scope,components,expansion; /* scope 0 Entities / 1 live Players; expansion 1 linked inventory/items */
 const uint32_t *entities; uint32_t entity_count,reserved; /* copied, max128 explicit roots */
} KilnComponentWatchRequestV1;
typedef struct KilnComponentWatchStatusV1 {
 uint32_t size,status,pairs,limit; /* status 0 Ready,1 SuspendedBudget,2 SuspendedMetadata,3 Disposed */
 uint64_t watch_id,last_complete_step; char reason[192];
} KilnComponentWatchStatusV1;
typedef struct KilnComponentChangeV1 {
 uint32_t size,version,kind,component; /* kind1; component 1..6 in mask order */
 uint64_t world_epoch,step; KilnEventEntityKey entity;
 uint32_t before_bytes,after_bytes,origin,flags; /* flags1 before-present,2 after-present,4 token-truncated */
 uint32_t root_count,watch_count,token_count,reserved;
 uint32_t roots[128]; uint64_t watches[32],tokens[32];
 uint8_t before[256],after[256]; char source_mod[128];
 /* ---- appended (0.2.0-dev.29; sizeof is the record the service writes: read with stride >= it) ---- */
 uint32_t world_kind;      /* KILN_WORLD_SERVER (0) */
 uint32_t attribute;       /* KILN_CHANGE_ATTRIBUTE_*: the four values below hold */
 int32_t value_before, value_after, maximum_before, maximum_after;
 uint64_t since_step;      /* the step of the pair's baseline (the previous compare, or the window_begin that added it) */
 uint64_t world_session;   /* kiln.world's game_world_session(KILN_WORLD_SERVER) of the watch's world; 0 when not known */
} KilnComponentChangeV1;
typedef struct KilnKnownWriteV1 {
 uint32_t size,component; uint64_t world_epoch,step,operation_token;
 KilnEventEntityKey entity; const uint8_t *before,*after;
 uint32_t bytes,reserved; KilnStr source_mod; /* copied; >127 UTF8 bytes -> Unknown provenance */
} KilnKnownWriteV1;
/* record_write's KilnKnownWriteV1: component 1..6; entity.entity the entity (the other key fields 0, or the life the
 * writer knows: a write for another life is not counted); before/after the whole component (bytes 1..256, the
 * component's size), copied during the call; world_epoch 0 (now) or the window clock's epoch (another: refused);
 * step and operation_token for diagnostics (0: the service numbers it); source_mod the writing mod's id, or the
 * Kiln service's name when the writer does not know the mod (kiln.combat), 1..127 bytes of UTF-8 (else the token
 * is not valid: Unknown). */

/* counters(): what the service did (any thread). */
typedef struct KilnComponentChangesCountersV1 {
 uint32_t size, state;              /* state 0 off, 1 on, 2 stopped after a fault */
 uint32_t watches, pairs, present_pairs, queued_records, queued_frames, reserved;
 uint64_t world_epoch, last_complete_step, windows, changes, queue_overflows, frames_dropped;
 uint64_t tokens_recorded, tokens_unwatched, tokens_refused, tokens_lost, tokens_truncated, tokens_off_world_thread;
 uint64_t begin_ns_p50, begin_ns_p99, end_ns_p50, end_ns_p99, samples; /* the last <= 1024 windows that had pairs */
 uint64_t begin_ns_max, end_ns_max;
} KilnComponentChangesCountersV1;

typedef struct KilnComponentChangesApiV1 {
 KilnServiceHeader header;
 KilnResult(KILN_CALL *attach)(KilnOwner dispatcher);
 KilnResult(KILN_CALL *detach)(KilnOwner dispatcher);
 KilnResult(KILN_CALL *watch)(KilnOwner,const KilnComponentWatchRequestV1*,KilnComponentWatchStatusV1*);
 KilnResult(KILN_CALL *resume)(KilnOwner,uint64_t watch_id,KilnComponentWatchStatusV1*);
 KilnResult(KILN_CALL *status)(uint64_t watch_id,KilnComponentWatchStatusV1*);
 KilnResult(KILN_CALL *remove)(KilnOwner,uint64_t watch_id);
 KilnResult(KILN_CALL *release_owner)(KilnOwner);
 KilnResult(KILN_CALL *begin_events)(KilnOwner dispatcher,uint64_t *batch,uint32_t *records);
 KilnResult(KILN_CALL *read_events)(KilnOwner,uint64_t batch,KilnComponentChangeV1*,uint32_t capacity,uint32_t stride,uint32_t *required);
 KilnResult(KILN_CALL *ack_events)(KilnOwner,uint64_t batch);
 /* native service owners only; data valid during call, copied; any legal component writer thread */
 KilnResult(KILN_CALL *record_write)(KilnOwner,const KilnKnownWriteV1*);
 /* core-internal only: after all Pre / after WaitWorldJobs before ordinary Post */
 void(KILN_CALL *window_begin)(uint64_t world_epoch,uint64_t step);
 void(KILN_CALL *window_end)(uint64_t world_epoch,uint64_t step);
 /* ======== appended (0.2.0-dev.29); check header.size covers a member before calling it ========
  * window_wanted: the core asks it after the last Pre subscriber: nonzero when this step needs both window calls (a
  * watch exists). Zero: neither call, and no wait for the world's jobs on this account. Any thread, lock-free: a native
  * writer may ask it (kiln_component_watching) before it copies a component for record_write.
  * window_faulted: core only: a window call faulted; the core makes no further one. The service stops: every watch is
  * Disposed, record_write returns at once, the rows are lowered.
  * counters: any thread.
  * watched: any thread, lock-free: the components (mask, bit i - 1 for index i) that may be watched on `entity` in this
  * interval. A filter: it may name a component that is not watched, never leaves out one that is (a pair a Ready watch
  * covers from its baseline on). 0: a writer copies nothing and tells nothing for that entity. kiln_component_watched
  * asks it. */
 uint32_t(KILN_CALL *window_wanted)(void);
 void(KILN_CALL *window_faulted)(uint32_t code);
 KilnResult(KILN_CALL *counters)(KilnComponentChangesCountersV1* out);
 uint32_t(KILN_CALL *watched)(uint32_t entity);
} KilnComponentChangesApiV1;

#ifdef __cplusplus
}
static_assert(sizeof(KilnComponentChangeV1) <= 2048, "KilnComponentChangeV1 fits 2048 bytes");
static_assert(sizeof(KilnComponentChangeV1) == 1808, "KilnComponentChangeV1 is 1808 bytes");
static_assert(offsetof(KilnComponentChangeV1, world_kind) == 1768, "the appended part starts after source_mod");
static_assert(sizeof(KilnKnownWriteV1) == 112, "KilnKnownWriteV1 is 112 bytes");
static_assert(sizeof(KilnComponentChangesApiV1) == 8 + 17 * sizeof(void*), "13 members, then 4 appended");
static_assert(sizeof(KilnComponentChangesCountersV1) == 184, "KilnComponentChangesCountersV1 is 184 bytes");

/* A native writer's call (C++): after a successful write of a watched component, with the whole component's bytes
 * before and after. `t` is the kiln.component_changes table the service found at its on_ready (null: no-op). Any
 * thread; cheap when nothing is watched. Example (kiln.combat's set_health):
 *   kiln_component_write(g_changes, g_owner, KILN_WATCH_HEALTH, entity, before, after, size, 0, "kiln.combat"); */
static inline void kiln_component_write(const KilnComponentChangesApiV1* t, KilnOwner owner, uint32_t component, uint32_t entity,
                                        const void* before, const void* after, uint32_t bytes, uint64_t token,
                                        const char* source) {
    if (!t || !t->record_write || !before || !after || bytes == 0 || bytes > KILN_WATCH_MAX_BYTES) return;
    KilnKnownWriteV1 w{};
    w.size = sizeof w;
    w.component = component;
    w.operation_token = token;
    w.entity.entity = entity;
    w.before = static_cast<const uint8_t*>(before);
    w.after = static_cast<const uint8_t*>(after);
    w.bytes = bytes;
    size_t n = 0;
    while (source && source[n] && n < 200) ++n;
    w.source_mod = KilnStr{source, n};
    t->record_write(owner, &w);
}
/* Whether anything is watched now (lock-free): a writer copies the bytes before its write only then. */
static inline bool kiln_component_watching(const KilnComponentChangesApiV1* t) {
    return t && t->header.size >= offsetof(KilnComponentChangesApiV1, window_wanted) + sizeof(void*) && t->window_wanted &&
           t->window_wanted() != 0;
}
/* Whether `component` (1..6) of `entity` may be watched now (lock-free, any thread): a writer asks it before it copies
 * the component for kiln_component_write, so an entity nothing watches costs it no copy. A table without `watched`
 * answers as kiln_component_watching does. */
static inline bool kiln_component_watched(const KilnComponentChangesApiV1* t, uint32_t entity, uint32_t component) {
    if (!kiln_component_watching(t) || component < 1 || component > 6) return false;
    if (t->header.size < offsetof(KilnComponentChangesApiV1, watched) + sizeof(void*) || !t->watched) return true;
    return (t->watched(entity) & (1u << (component - 1))) != 0;
}
/* The table a writer keeps (call at on_ready; null when kiln-decisions.dll is not loaded). */
static inline const KilnComponentChangesApiV1* kiln_component_changes_find(const KilnApi* api) {
    if (!api || !api->service) return nullptr;
    const auto* t = static_cast<const KilnComponentChangesApiV1*>(
        api->service(KilnStr{KILN_COMPONENT_CHANGES_SERVICE, sizeof(KILN_COMPONENT_CHANGES_SERVICE) - 1}, KILN_COMPONENT_CHANGES_VERSION));
    return t && t->header.size >= offsetof(KilnComponentChangesApiV1, record_write) + sizeof(void*) ? t : nullptr;
}
/* The component index of a component's short name ("Health"), 0 for one that is not watched. */
static inline uint32_t kiln_component_index(const char* name, size_t len) {
    static const char* const k[] = {"Health", "Stamina", "Mana", "Fuel", "Inventory", "SlotSelection"};
    for (uint32_t i = 0; i < 6; ++i) {
        size_t n = 0;
        while (k[i][n]) ++n;
        if (n != len) continue;
        size_t j = 0;
        while (j < n && k[i][j] == name[j]) ++j;
        if (j == n) return i + 1;
    }
    return 0;
}
#endif
#pragma pack(pop)

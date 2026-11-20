/* SPDX-License-Identifier: MIT */
/* kiln.routines (kiln-routines.dll), since 0.2.0-dev.28: a villager follows a day plan.
 *
 * A plan is data: windows of the game day, each with one of the game's own villager tasks (idle, roam, sit, inspect,
 * sleep, or "vanilla": the game's own choice), and a fallback. The service hooks the villager worker and its task
 * chooser on the server: when a villager with an assigned plan reaches a decision, the plan's window of the game
 * minute says which task it takes, and the game's own code walks, sits, finds the bed and plays the animations. A
 * villager without a plan, a suspended one, and every call the service does not own run the game's code unchanged.
 * Managed face: Kiln.Blocks (Routines, RoutineBlock). Docs: docs/kiln/services/routines.md.
 *
 * Capability: the sigdb feature routines.decisions (the worker, the chooser, the setters and the context offsets
 * resolved on this build; since 0.2.0-dev.31 also SleepCheck's keep-awake prefix and MarkWritten). Nothing holds it
 * (its game test, 16g-cc16g, is a guided check, not a gate). While it is off no hook is installed and assign / remove /
 * resume answer KILN_ERR_UNAVAILABLE.
 *
 * The plan (assign's plan_json) is exact UTF-8 JSON, format 1:
 *   {"format":1,"key":"<mod>:<name>","planVersion":1,"contentHash":"<sha256 hex>",
 *    "windows":[{"startMinute":360,"endMinute":720,"allDay":false,"priority":0,"task":{"kind":"idle"}}],
 *    "fallback":{"kind":"vanilla"}}
 * Every property once, none unknown, in any order; integers; at most KILN_ROUTINES_MAX_WINDOWS windows; minutes 0..1439
 * with start != end, or allDay with 0 and 0; planVersion >= 1; the fallback is vanilla, idle or roam. contentHash is the
 * SHA-256 (lower-case hex) of the canonical document
 *   {"fallback":{"kind":"…"},"format":1,"planVersion":N,"windows":[{"allDay":…,"endMinute":…,"priority":…,
 *    "startMinute":…,"task":{"kind":"…"}}]}
 * (properties in byte order, no whitespace): a document whose hash is not that of its content is refused.
 * At a decision the window with the highest priority that covers the game minute wins, the first written among equals;
 * end-exclusive; a start after the end runs over midnight; no window: the fallback, as a task.
 *
 * Owners. One routine per villager (NpcId, the FNV-1a-32 of its template GUID): the first accepted assign reserves it;
 * the same owner may replace its plan; another owner's assign, remove or resume completes with ok = 0 and the reason
 * "OwnedBy:<mod>". The mod of an owner is the part of the plan's key before ':': an owner's first assign binds it to
 * that mod, a later plan of another mod from the same owner is KILN_ERR_INVALID_ARGUMENT, and so is a mod another live
 * owner is bound to. The binding ends with revoke_owner.
 *
 * One completion per change. assign, remove and resume check their arguments, queue the change and give a ticket; a
 * refusal at once (a result other than KILN_OK) makes no ticket and never calls `done`. Queued changes are committed
 * at the server world's next Pre, in order, and each `done` is called exactly once on that step's Post, on the tick
 * thread. ok = 1 says the assignment changed (or already was as asked: then the revision is the old one and no event
 * follows), also when the villager is paused, not in the world, or its first task cannot start: what tasks do later
 * (no seat, no bed, NativeSleepNotCurrentlyAllowed) comes as task events only, never as a second completion.
 * At most KILN_ROUTINES_MAX_QUEUED changes wait at a time; one more is refused with KILN_ERR_BUFFER_TOO_SMALL before
 * anything changed. A world that stops completes what is queued with ok = 0, "WorldStopped"; an owner that is revoked
 * before the commit gets ok = 0, "Revoked". The reason string is valid for the callback only.
 *
 * Revisions count the changes of all assignments of the running world, from 1; an assignment's revision is that of its
 * last change.
 *
 * Events (subscribe_events) are JSON documents, called on the tick thread on Post, one per change. Assignment events
 * are never dropped. Task events are buffered per villager, 16 a step: when one villager's task changes more often in
 * one step, the newest change is kept and the ones in between are dropped (counted, and logged once per new count):
 *   {"type":"assignment","before":<snapshot or null>,"after":<snapshot>,"reason":"Assigned","teardown":false}
 *   {"type":"task","npcId":1,"key":"mod:day","planVersion":1,"revision":1,"invocation":7,"windowIndex":0,
 *    "task":"sit","from":"Chosen","to":"Started","reason":null,"teardown":false}
 * A snapshot (also what inspect writes):
 *   {"npcId":1,"owner":"mod","key":"mod:day","planVersion":1,"revision":1,"assignment":"Assigned","windowIndex":0,
 *    "invocation":7,"task":"sit","taskState":"Started","nativeState":5,"lastReason":null,"paused":false}
 * assignment: Assigned, ManualSuspended, PendingSpawn (the villager is not in the world now), Removed.
 * taskState: None, Chosen, Started, Completed, Refused, Suspended. nativeState: keen::active_npc::State (0..17).
 * When the world stops, each assignment gets one event with "teardown":true and after.assignment PendingSpawn, a
 * running task one with to "Suspended" and the reason "WorldStopped"; then the service forgets the world's assignments
 * (they are the caller's to keep and to assign again in the next world).
 *
 * Manual commands (since 0.2.0-dev.31): a Reset or GoTo command the game's villager worker takes for an assigned,
 * active villager (from any source: Kiln's Villager.GoTo/Teleport/Stay/Follow, a native plugin, another loader)
 * suspends its routine: the task in progress ends "Suspended" with the reason "ManualSuspended", and on that step's
 * Post the assignment becomes "ManualSuspended" at a new revision (event reason "ManualSuspended"). The game decides
 * for it until the owner's resume. A command the game ignores suspends nothing.
 *
 * Threads: assign, remove, resume, inspect, subscribe_events and unsubscribe_events on the tick thread
 * (KILN_ERR_WRONG_THREAD otherwise); revoke_owner on any thread (it takes effect at the next Pre).
 * The table grows at its end only; check header.size before a member added later.
 */
#ifndef KILN_ROUTINES_H
#define KILN_ROUTINES_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_ROUTINES_SERVICE "kiln.routines"
#define KILN_ROUTINES_VERSION 1u

#define KILN_ROUTINES_FEATURE "routines.decisions"
#define KILN_ROUTINES_MAX_WINDOWS 64u
#define KILN_ROUTINES_MAX_QUEUED 256u
#define KILN_ROUTINES_MAX_PLAN_BYTES 16384u

/* KilnRoutineResult.reason_code */
enum {
    KILN_ROUTINE_OK = 0,
    KILN_ROUTINE_OWNED_BY = 1,      /* "OwnedBy:<mod>" */
    KILN_ROUTINE_WORLD_STOPPED = 2, /* "WorldStopped" */
    KILN_ROUTINE_REVOKED = 3,       /* "Revoked" */
    KILN_ROUTINE_NOT_ASSIGNED = 4,  /* "NotAssigned": resume of a villager without a routine */
};

/* restore's flags (since 0.2.0-dev.31) */
#define KILN_ROUTINE_RESTORE_SUSPENDED 1u /* the saved assignment was suspended by a manual command */

typedef struct KilnRoutineResult {
    uint32_t size; /* sizeof(KilnRoutineResult) of the service */
    uint32_t ok;
    uint64_t ticket;
    uint64_t revision; /* the assignment's revision after the change; 0 when it has none */
    uint32_t npc_id;
    uint32_t reason_code; /* KILN_ROUTINE_* */
    KilnStr reason;       /* callback lifetime; empty when ok */
} KilnRoutineResult;

typedef void(KILN_CALL* KilnRoutineDone)(const KilnRoutineResult* result, void* user);
typedef void(KILN_CALL* KilnRoutineEventFn)(KilnStr json, void* user);

typedef struct KilnRoutinesApi {
    KilnServiceHeader header; /* { sizeof(KilnRoutinesApi), KILN_ROUTINES_VERSION } */

    /* Queues "bind this plan to the villager". KILN_ERR_INVALID_ARGUMENT: npc_id 0, no `done`, a plan that is not a
     * sound format-1 document, or a plan of another mod than the owner's; KILN_ERR_BUFFER_TOO_SMALL: the queue is
     * full; KILN_ERR_UNAVAILABLE: routines.decisions is off or held, or no server world runs. */
    KilnResult(KILN_CALL* assign)(KilnOwner owner, uint32_t npc_id, KilnStr plan_json, KilnRoutineDone done,
                                  void* user, uint64_t* ticket);
    /* Queues "take the routine off the villager". No routine: ok = 1, revision 0, no event. */
    KilnResult(KILN_CALL* remove)(KilnOwner owner, uint32_t npc_id, KilnRoutineDone done, void* user,
                                  uint64_t* ticket);
    /* Queues "end the manual suspension". Not suspended: ok = 1, the old revision, no event. */
    KilnResult(KILN_CALL* resume)(KilnOwner owner, uint32_t npc_id, KilnRoutineDone done, void* user,
                                  uint64_t* ticket);
    /* The villager's snapshot as JSON (not NUL-terminated). KILN_ERR_NOT_FOUND: no routine. *out_len = the length;
     * KILN_ERR_BUFFER_TOO_SMALL when it is more than cap (nothing is written). */
    KilnResult(KILN_CALL* inspect)(uint32_t npc_id, char* json, size_t cap, size_t* out_len);
    /* The owner is going (its mod unloads): at the next Pre its queued changes complete with "Revoked" and its
     * assignments are removed (reason "OwnerUnloaded"), each with its event. Any thread. */
    void(KILN_CALL* revoke_owner)(KilnOwner owner);

    /* Framework only (Kiln.Blocks): the events above. At most 8 subscribers (KILN_ERR_UNAVAILABLE beyond). The
     * subscription goes with `owner`, or with unsubscribe_events. */
    KilnResult(KILN_CALL* subscribe_events)(KilnOwner owner, KilnRoutineEventFn fn, void* user, KilnHandle* out);
    KilnResult(KILN_CALL* unsubscribe_events)(KilnHandle handle);

    /* ---- since 0.2.0-dev.31 (check header.size) ---- */

    /* Framework only (Kiln.Blocks, when a world is ready after a restart): queues "bind this saved assignment again"
     * at the saved revision, not a new one, with the saved manual suspension (flags). An empty reason: a plain rebind,
     * no event; a reason (e.g. "Migrated", the caller gave a new revision): an assignment event with that reason.
     * Refused with OwnedBy like assign when another mod's routine holds the villager. The world's revision counter is
     * raised to at least the revision. Same threads and errors as assign; revision 0 is KILN_ERR_INVALID_ARGUMENT. */
    KilnResult(KILN_CALL* restore)(KilnOwner owner, uint32_t npc_id, KilnStr plan_json, uint64_t revision, uint32_t flags,
                                   KilnStr reason, KilnRoutineDone done, void* user, uint64_t* ticket);
    /* Framework only: the next revision the world gives is above floor (the highest revision a saved world ever
     * gave, also of assignments since removed). Tick thread; KILN_ERR_UNAVAILABLE when no world runs. */
    KilnResult(KILN_CALL* set_revision_floor)(uint64_t floor);
} KilnRoutinesApi;

#if defined(__cplusplus) && (defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__))
static_assert(sizeof(KilnRoutineResult) == 48, "KilnRoutineResult");
static_assert(offsetof(KilnRoutinesApi, assign) == sizeof(KilnServiceHeader), "kiln.routines v1 starts after the header");
static_assert(offsetof(KilnRoutinesApi, unsubscribe_events) == sizeof(KilnServiceHeader) + 6 * sizeof(void*), "kiln.routines v1");
static_assert(offsetof(KilnRoutinesApi, restore) == sizeof(KilnServiceHeader) + 7 * sizeof(void*), "kiln.routines dev.31 tail");
static_assert(sizeof(KilnRoutinesApi) == sizeof(KilnServiceHeader) + 9 * sizeof(void*), "kiln.routines dev.31 tail");
#endif

#ifdef __cplusplus
}
#endif
#endif /* KILN_ROUTINES_H */

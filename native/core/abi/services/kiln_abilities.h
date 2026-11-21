/* SPDX-License-Identifier: MIT
 * kiln.abilities v1: the game's Impact VM for mods. Observe impact programs (instances, native calls), tweak them per
 * cast (config values before an instance's first run, wrappers around natives), add Kiln natives (C++ built-ins and
 * managed ones) and register new Impact programs at run time. Evidence: docs/re/abilities.md; guide:
 * docs/kiln/services/abilities.md (also the program/native contract with tools/impact).
 *
 * Words:
 *  - program: a keen::ImpactProgram (bytecode + variables), id = FNV-1a-32 of its GUID bytes. 571 ship with the game.
 *  - instance: a running program bound to one entity (ImpactSystem slot): impact id, program id, self entity, data
 *    (the program's variables, config values copied in by the game when the instance is created).
 *  - native: a C++ function programs call by name hash (ECALL). The game registers 107; Kiln adds its own.
 *  - config id: the HashKey32 that binds a program variable to a value of a template, sequence event, item, buff or
 *    skill (Damage 0x72864524, NoiseRange 0xa19409b2, AttackMod 0x854ea8fa, ...).
 *
 * Threads: functions marked "tick" run on the server world's thread in Pre or Post (posted tasks count as Pre) and
 * return KILN_ERR_WRONG_THREAD elsewhere. The listener runs in Pre. Natives and their managed sync wrappers run on
 * the game's job threads inside the sim step.
 * Fail soft: a missing symbol turns the part that needs it off (status() says which); the service stays published.
 */
#ifndef KILN_ABILITIES_H
#define KILN_ABILITIES_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_ABILITIES_SERVICE "kiln.abilities"
#define KILN_ABILITIES_VERSION 1u

/* Engine value types of program variables (FNV-1a of the reflection "impact name"). Others exist (structs such as
 * DamageSetup 0x4af58ef7, entity ids, float3, templates): read them by size. */
#define KILN_IMPACT_TYPE_UINT32 0x32940becu
#define KILN_IMPACT_TYPE_SINT32 0x9859f31eu
#define KILN_IMPACT_TYPE_FLOAT32 0xe89f7410u
#define KILN_IMPACT_TYPE_BOOL 0xc894953du
#define KILN_IMPACT_TYPE_TIME 0xdfe4e404u /* int64 nanoseconds */

/* Well-known native hashes (FNV-1a-32 of the node name). */
#define KILN_NATIVE_APPLY_DAMAGE 0xb04d0e7au /* argc 25 */
#define KILN_NATIVE_APPLY_BUFF 0x91f30b26u   /* argc 5 */
#define KILN_NATIVE_SPAWN_ENTITY 0x61b11b03u /* argc 13 */

/* status() */
typedef struct KilnAbilitiesStatus {
    uint32_t size;         /* set by the caller */
    uint8_t natives;       /* the native registry was captured and Kiln's natives/wrappers are registered */
    uint8_t programs;      /* the ImpactRegistry is reachable (program lookup, registration) */
    uint8_t instances;     /* instance creation and first-run hooks are on (observe, overrides, hold) */
    uint8_t attributes;    /* attr_get / attr_add work */
    uint32_t vanilla_natives;   /* natives the game registered (107 on svn 1285601) */
    uint32_t native_capacity;   /* the registry's capacity (256) */
    uint32_t kiln_natives;      /* live Kiln natives (built-ins + managed) */
    uint32_t programs_total;    /* programs in the ImpactRegistry */
    uint32_t programs_kiln;     /* programs Kiln inserted */
    uint32_t held_now;          /* instances held for the listener right now */
    uint32_t interest;          /* the listener's current interest mask */
    uint64_t instances_created, instances_held, overrides_applied, overrides_mismatched, runs, native_calls,
        native_calls_observed, signals, managed_calls, managed_suspends, events_delivered, events_dropped,
        wrap_calls, programs_refused;
    uint64_t step;
} KilnAbilitiesStatus;

/* KilnAbilityEvent.kind; the listener's interest mask uses (1 << kind). */
enum {
    KILN_ABILITY_EV_IMPACT_STARTED = 1,  /* an instance was created (configs applied). With HOLD interest it is held:
                                            its first run waits until the listener returned (one step). Writable:
                                            `canceled` (destroy the entity; the program never runs). Read/write its
                                            variables with instance_* during the call. */
    KILN_ABILITY_EV_IMPACT_ENDED = 2,    /* an instance reported earlier no longer exists (checked every Pre) */
    KILN_ABILITY_EV_NATIVE = 3,          /* a wrapped vanilla native ran (observe; one step late): hash, args, result */
    KILN_ABILITY_EV_SIGNAL = 4,          /* kiln.Signal ran: tag + value */
    KILN_ABILITY_EV_MANAGED_CALL = 5,    /* a managed Kiln native was called: answer it with native_complete() during
                                            the listener call (else it completes with value 0). The ECALL resumes in
                                            the next step with that value. */
};
#define KILN_ABILITY_INTEREST_HOLD (1u << 31) /* hold new instances (see IMPACT_STARTED); hold_filter narrows it */

/* One event for the listener (640 bytes in v1). Payload fields depend on the kind (unused ones are 0). */
typedef struct KilnAbilityEvent {
    uint32_t size;       /* sizeof(KilnAbilityEvent) */
    uint32_t kind;       /* KILN_ABILITY_EV_* */
    uint64_t step;       /* the step it happened in (the listener runs in the next Pre) */
    uint32_t impact_id;  /* the instance */
    uint32_t program_id;
    KilnEntity self;     /* the instance's entity */
    KilnEntity root;     /* ImpactHierarchy root of self (the caster for player abilities), 0 if none or gone */
    uint32_t native_hash;/* NATIVE / MANAGED_CALL: the native */
    uint32_t site;       /* NATIVE / SIGNAL / MANAGED_CALL: the ECALL's call-site id */
    uint32_t argc;       /* NATIVE / MANAGED_CALL: args[] used */
    uint8_t result;      /* NATIVE: the value the native pushed */
    uint8_t error;       /* NATIVE: its error byte (0 ok, 1-3 suspended) */
    uint8_t held;        /* IMPACT_STARTED: the instance is held for this call */
    uint8_t canceled;    /* IMPACT_STARTED (held): set by the listener to destroy the entity before the first run */
    uint32_t args[31];   /* NATIVE / MANAGED_CALL: the raw argument values (handles 0xFFFF0000|index, 0, raw u32) */
    uint32_t tag;        /* SIGNAL: the tag */
    uint32_t arg_types[31]; /* NATIVE: the type hash of each handle's variable (0 = raw value or not connected) */
    uint32_t value_type; /* SIGNAL: the value's variable type (0 = raw u32 in value[0..3]) */
    uint32_t value_size; /* SIGNAL: bytes in value */
    uint32_t arg_values[31]; /* NATIVE: after the call, the first 4 bytes of each handle's variable (or the raw value) */
    uint8_t arg_offsets[31]; /* NATIVE: where each arg's variable bytes start in value[] (0xFF = not captured) */
    uint8_t arg_sizes[31];   /* NATIVE: how many bytes of it value[] holds */
    uint8_t reserved0[2];
    uint64_t call_id;    /* MANAGED_CALL: pass to native_complete */
    uint8_t value[136];  /* SIGNAL: the value bytes. NATIVE: the variable bytes of the arguments (arg_offsets) */
} KilnAbilityEvent;

/* Listener: called in Pre on the tick thread with the events of the previous step. Returns the interest mask for the
 * next step (bits 1 << KILN_ABILITY_EV_*, plus KILN_ABILITY_INTEREST_HOLD). One listener (the managed Kiln.Abilities). */
typedef uint32_t(KILN_CALL* KilnAbilityListenerFn)(KilnAbilityEvent* events, uint32_t count, void* user);

/* A program variable (instance_vars / program_vars). */
typedef struct KilnImpactVar { /* 88 bytes, fixed in v1 */
    uint32_t index;      /* the variable's index (handle 0xFFFF0000|index) */
    uint32_t name_hash;
    uint32_t config_id;  /* 0 = not bound to a config value */
    uint32_t type;       /* KILN_IMPACT_TYPE_* or another type hash */
    uint16_t value_size; /* bytes */
    uint16_t offset;     /* in the instance data */
    uint32_t reserved;
    char name[64];       /* debug name ("NoiseRange::value"), NUL-terminated, cut at 63 */
} KilnImpactVar;

/* A config override for entity_overrides(): applied to the entity's next instance right after the game copied its
 * configs, before its first run. */
typedef struct KilnConfigOverride {
    uint32_t match;      /* KILN_OVERRIDE_BY_CONFIG: key = config id (every variable bound to it);
                            KILN_OVERRIDE_BY_NAME: key = variable name hash; KILN_OVERRIDE_BY_INDEX: key = index */
    uint32_t key;
    uint32_t size;       /* bytes in data; must equal the variable's size (else it is skipped and counted) */
    uint32_t reserved;
    uint8_t data[64];
} KilnConfigOverride;
enum { KILN_OVERRIDE_BY_CONFIG = 0, KILN_OVERRIDE_BY_NAME = 1, KILN_OVERRIDE_BY_INDEX = 2 };

/* A native in the registry (native_at). */
typedef struct KilnNativeInfo {
    uint32_t size;      /* set by the caller */
    uint32_t hash;
    uint32_t argc;
    uint32_t kind;      /* KILN_NATIVE_KIND_* */
    uintptr_t fn;       /* the game's function (vanilla) or Kiln's thunk */
    char name[64];      /* display name */
} KilnNativeInfo;
enum {
    KILN_NATIVE_KIND_VANILLA = 0, /* registered by the game, untouched */
    KILN_NATIVE_KIND_WRAPPED = 1, /* a vanilla native Kiln wraps (observe / managed wrappers); the game's fn below */
    KILN_NATIVE_KIND_BUILTIN = 2, /* kiln.Log, kiln.Signal */
    KILN_NATIVE_KIND_MANAGED = 3, /* registered through native_register (answers in the next step) */
    KILN_NATIVE_KIND_SYNC = 4,    /* since 0.2.0-dev.29: registered through native_register_sync (answers inside the call) */
};

/* A program in the ImpactRegistry (program_info). Named KilnImpactProgramInfo since 0.2.0-dev.29: kiln_world.h had a
 * struct of the same name (a Kiln ECS program, KilnWorldProgramInfo), so no plugin could include both headers. The old
 * name stays as an alias of the struct of the header included first (KILN_PROGRAM_INFO_ALIAS): write the new names in
 * code that includes both. Layout unchanged. */
typedef struct KilnImpactProgramInfo {
    uint32_t size;        /* set by the caller */
    uint32_t id;
    uint8_t guid[16];     /* game byte order */
    uint32_t code_words, shutdown_words, variables, data_bytes;
    uint16_t stack_size;
    uint8_t streams;      /* usedStreams bits: OnCollision 1, OnHit 2, OnParry 4, EntityInRange 8 */
    uint8_t kiln;         /* 1 = inserted by Kiln */
} KilnImpactProgramInfo;
#ifndef KILN_PROGRAM_INFO_ALIAS
#define KILN_PROGRAM_INFO_ALIAS 2 /* 1: KilnProgramInfo is kiln_world.h's, 2: kiln_abilities.h's */
typedef KilnImpactProgramInfo KilnProgramInfo;
/* Preserve both the old typedef spelling and `struct KilnProgramInfo` source compatibility. */
#define KilnProgramInfo KilnImpactProgramInfo
#endif

/* A managed native call or a sync wrapper call (job thread): the native context, typed access through the
 * instance's variables. `out_error`/`out_value` are what the native returns to the interpreter. */
typedef struct KilnNativeCall {
    uint32_t size;
    uint32_t hash;
    uint32_t argc;
    uint32_t reserved0;
    const uint32_t* args;       /* args[0..argc): handles 0xFFFF0000|index, 0, or raw values */
    const void* layout;         /* the program's ImpactVariable array (24 B each: name, configId, type, u16 size,
                                   u16 offsetInBytes, BlobString dbgName) */
    uint64_t layout_count;
    uint8_t* data;              /* the instance's variables */
    uint64_t data_size;
    uint32_t impact_id, program_id, site;
    KilnEntity self;
    uint8_t out_error;          /* 0 ok, 1-3 suspend (the ECALL re-runs next step), >= 4 error */
    uint8_t out_value;          /* pushed when out_error == 0 */
    uint8_t original_called;    /* set by call_original */
    uint8_t reserved1;
    uint32_t reserved2;
    void* engine[3];            /* opaque: the engine's result, arg slice and context */
} KilnNativeCall;

/* Sync wrapper dispatcher (managed Kiln.Abilities): runs on the job thread that runs the native. It may change
 * variables, call call_original(call) (at most once), and sets out_error/out_value. Returns nothing. */
typedef void(KILN_CALL* KilnNativeWrapFn)(KilnNativeCall* call, void* user);

/* since 0.2.0-dev.29: why native_register_sync (and native_attach) said no */
enum {
    KILN_NATIVE_REFUSED_NONE = 0,
    KILN_NATIVE_REFUSED_REGISTRY_FULL = 1, /* the server world's native registry has no room: the game takes 128 keys
                                              (half its capacity of 256), 107 of them its own; a wrap needs none */
    KILN_NATIVE_REFUSED_NO_SLOT = 2,       /* Kiln's 160 native thunks are in use (wraps, built-ins, mods' natives) */
    KILN_NATIVE_REFUSED_TAKEN = 3,         /* a live Kiln native has this name */
    KILN_NATIVE_REFUSED_VANILLA = 4,       /* the name's hash is one of the game's natives */
    KILN_NATIVE_REFUSED_PROGRAM_NOT_LOADED = 5, /* native_attach: the filter's program is not in the ImpactRegistry */
    KILN_NATIVE_REFUSED_SITE_NOT_FOUND = 6,     /* native_attach: that program does not call the native (at that site) */
    KILN_NATIVE_REFUSED_SECOND_INSTEAD = 7,     /* native_attach: an Instead for this native, program and site exists */
    KILN_NATIVE_REFUSED_NOT_A_NATIVE = 8,       /* native_attach: not a native the game registered */
};

/* since 0.2.0-dev.29: which calls an attachment gets (0 = any). The thunk compares it before any handler runs. */
typedef struct KilnNativeFilter {
    uint32_t size;       /* sizeof(KilnNativeFilter) */
    uint32_t program_id; /* the calling program (FNV-1a-32 of its GUID); 0 = every program */
    uint32_t site;       /* the ECALL's call-site id in that program; 0 = every site */
    uint32_t reserved;   /* 0 */
} KilnNativeFilter;
enum {
    KILN_NATIVE_BEFORE = 0,  /* before the game's native: may change arguments, call_original, or answer (out_error) */
    KILN_NATIVE_INSTEAD = 1, /* runs when no Before answered; one per (native, program, site); answers, else the game's
                                native runs */
    KILN_NATIVE_AFTER = 2,   /* after the native ran (or a handler answered): may read outputs and change the answer;
                                call_original does nothing here */
};

/* since 0.2.0-dev.29: one argument of a typed native declaration (native_declare_typed). args[0] is the value pushed
 * last before the ECALL. */
typedef struct KilnNativeArg {
    uint32_t type;  /* the variable's type hash (FNV-1a-32 of the impact type name: KILN_IMPACT_TYPE_*, ecs.EntityId
                       0x737c8f86, WorldTransform 0x9493a8c5, ...); 0 = any type */
    uint16_t size;  /* the variable's size in bytes; 0 = any size */
    uint8_t dir;    /* KILN_NATIVE_ARG_IN / OUT / INOUT (what the native does with it; checked alike) */
    uint8_t flags;  /* KILN_NATIVE_ARG_OPTIONAL: may be not connected (0); KILN_NATIVE_ARG_RAW: takes a raw integer */
} KilnNativeArg;
enum { KILN_NATIVE_ARG_IN = 0, KILN_NATIVE_ARG_OUT = 1, KILN_NATIVE_ARG_INOUT = 2 };
enum { KILN_NATIVE_ARG_OPTIONAL = 1u << 0, KILN_NATIVE_ARG_RAW = 1u << 1 };
/* native_state() */
enum {
    KILN_NATIVE_STATE_NONE = 0,    /* no Kiln native of this hash */
    KILN_NATIVE_STATE_WAITING = 1, /* registered with Kiln; no server world registry yet */
    KILN_NATIVE_STATE_LIVE = 2,    /* in the server world's native registry */
    KILN_NATIVE_STATE_REFUSED = 3, /* the server world's registry had no room for it (the world's setup refused it) */
    KILN_NATIVE_STATE_DEAD = 4,    /* taken off (its hash answers 0x1F) */
};
/* native_attach_state() */
enum {
    KILN_NATIVE_ATTACH_GONE = 0,    /* no such attachment (detached, or its owner released) */
    KILN_NATIVE_ATTACH_PENDING = 1, /* its program and site are checked when the world's programs are in */
    KILN_NATIVE_ATTACH_ACTIVE = 2,
    KILN_NATIVE_ATTACH_OFF = 3,     /* its program is not loaded or does not call the native (at that site) */
};
/* KilnNativeCounters.ecs_state (since 0.2.0-dev.29): the ECS bridge of Kiln natives' calls (native_ecs_read/write) */
enum {
    KILN_NATIVE_ECS_PENDING = 0, /* no server world's view yet, or its first call has not been checked */
    KILN_NATIVE_ECS_ON = 1,      /* checked in this world: the calls' iterator is impact_simulation's as kiln.world sees it */
    KILN_NATIVE_ECS_OFF = 2,     /* off in this world: ecs_reason says why (natives still run) */
};
enum {
    KILN_NATIVE_ECS_REASON_NONE = 0,
    KILN_NATIVE_ECS_REASON_NO_WORLD_API = 1, /* no kiln.world with foreign views (0.2.0-dev.29) */
    KILN_NATIVE_ECS_REASON_LAYOUT = 2,       /* kiln.world could not compute impact_simulation's view (kiln.log) */
    KILN_NATIVE_ECS_REASON_COUNTS = 3,       /* the job's group counts are not the view's */
    KILN_NATIVE_ECS_REASON_REGISTRY = 4,     /* the iterator's impact::ImpactSystem qword is not the captured registry */
    KILN_NATIVE_ECS_REASON_FAULT = 5,        /* foreign access faulted or its live view guard failed */
    KILN_NATIVE_ECS_REASON_NOT_SERIAL = 6,   /* impact_simulation does not run serially on the world's thread in this world:
                                                the world's pin of CHUNKED programs to it is clear, or a call came on another
                                                thread (two slices could touch one entity at once) */
    KILN_NATIVE_ECS_REASON_PIN_UNKNOWN = 7,  /* the pin cannot be read on this build (sigdb abilities.ecs_pin unresolved) */
};

/* since 0.2.0-dev.29 (native_counters). Reads every field the caller's size covers. */
typedef struct KilnNativeCounters {
    uint32_t size;            /* set by the caller */
    uint32_t inflight;        /* calls inside a Kiln native thunk right now (0 in Pre and Post) */
    uint32_t flags;           /* KILN_NATIVE_COUNTERS_* */
    uint32_t reserved;
    uint64_t sync_calls;      /* sync native calls that reached their fn */
    uint64_t sync_faults;     /* of those, fn faulted (answered 0x11) */
    uint64_t sync_slow;       /* of those, fn took longer than the 0.2 ms budget (logged, not cut off) */
    uint64_t dead_calls;      /* calls of a Kiln native that was taken off (answered 0x1F) */
    uint64_t attach_entered;  /* attachment callbacks entered (their filter matched) */
    uint64_t attach_skipped;  /* attachment filters that did not match a call: no callback, no managed code */
    uint64_t typed_refused;   /* calls a typed declaration refused with 0x2C (type, size, a raw value, bounds) */
    uint64_t typed_unconnected; /* calls it refused with 0x18 (a required argument not connected) */
    uint64_t retire_deferred; /* owner retirements that found calls still inside and finished in a later Pre */
    uint64_t watchdog_reports; /* handlers named by the watchdog: inside for more than a second */
    uint32_t retire_pending;  /* owner retirements waiting for their calls to leave right now */
    uint32_t attach_pending;  /* attachments now (KILN_NATIVE_ATTACH_*): program and site not checked yet, */
    uint32_t attach_active;   /* checked and taking calls, */
    uint32_t attach_off;      /* off: program not loaded or not calling the native there (checked again on new programs) */
    uint64_t lock_bypass;     /* hook visits (createImpactInstance, runImpact, managed calls, events) that gave up on
                                 Kiln's lock after its bound and took the path that needs none */
    uint32_t ecs_state;       /* KILN_NATIVE_ECS_* of the live server world */
    uint32_t ecs_reason;      /* KILN_NATIVE_ECS_REASON_* when OFF */
} KilnNativeCounters;
enum {
    KILN_NATIVE_COUNTERS_NATIVES = 1u << 0, /* the RegisterNative hook is in: Kiln natives can be registered */
};

/* item_use() (since 0.2.0-dev.30.3): one use of an item from a player's slot. */
typedef struct KilnItemUse {
    uint32_t size;          /* in: sizeof(KilnItemUse) */
    KilnEntity entity;      /* in: the player character */
    uint64_t slot;          /* in: InventorySlotId as kiln.items names it: inventory entity | index << 32 */
    uint32_t item;          /* in: the item expected in the slot */
    uint32_t sequence;      /* in: the item's own sequence for the use (FNV-1a of its ActorSequenceResource GUID) */
    uint32_t flags;         /* in: 0 */
    uint32_t status;        /* out: KILN_ITEM_USE_* */
    uint32_t count;         /* out: the stack in the slot at the call (before the use) */
    uint32_t reserved;
    int64_t trigger_time;   /* out, STARTED: the start's ECS time (sequence_state's trigger_time once it runs) */
} KilnItemUse;

/* KilnItemUse.status */
enum {
    KILN_ITEM_USE_STARTED = 0,      /* the sequence is triggered; the game starts it in this step */
    KILN_ITEM_USE_NOT_OWN_SLOT = 1, /* the slot is not in the entity's own inventories (or it has none) */
    KILN_ITEM_USE_EMPTY_SLOT = 2,   /* the slot holds nothing */
    KILN_ITEM_USE_OTHER_ITEM = 3,   /* the slot holds another item than use->item */
    KILN_ITEM_USE_NOT_A_PLAYER = 4, /* the entity's inventories are not a player's, or it has no Actor / ActorInput */
    KILN_ITEM_USE_DEAD = 5,         /* StateFlag.Dead */
    KILN_ITEM_USE_NOT_READY = 6,    /* not StateFlag.IsReadyForGameplay (joining, loading, respawning) */
    KILN_ITEM_USE_BUSY = 7,         /* a sequence plays (an attack, another use, an emote), a trigger already waits for this
                                       step (another item_use or sequence_play), or a state no use starts from */
};

typedef struct KilnAbilitiesApi {
    KilnServiceHeader header; /* { sizeof(KilnAbilitiesApi), KILN_ABILITIES_VERSION } */

    KilnResult(KILN_CALL* status)(KilnAbilitiesStatus* out);                 /* any thread */
    KilnResult(KILN_CALL* set_listener)(KilnAbilityListenerFn fn, void* user); /* NULL removes it */
    /* Narrows HOLD to instances of these programs (count 0 = hold every new instance while HOLD interest is set). */
    KilnResult(KILN_CALL* hold_filter)(const uint32_t* program_ids, uint32_t count);

    /* ---- instances (tick) ---- */
    /* Variables of a live instance (count = all; writes up to cap). NOT_FOUND: no such instance. */
    KilnResult(KILN_CALL* instance_vars)(uint32_t impact_id, KilnImpactVar* out, uint32_t cap, uint32_t* count);
    KilnResult(KILN_CALL* instance_read)(uint32_t impact_id, uint32_t var_index, void* dst, uint32_t cap,
                                         uint32_t* len);
    KilnResult(KILN_CALL* instance_write)(uint32_t impact_id, uint32_t var_index, const void* src, uint32_t len);
    /* The instance's self entity, program and flags (Initialized 1, Executed 2, Halt 8, Error 0x10, Destroy 0x40). */
    KilnResult(KILN_CALL* instance_info)(uint32_t impact_id, KilnEntity* self, uint32_t* program_id, uint32_t* flags);
    /* Config overrides for the next instance created on `entity` (e.g. one Kiln just spawned). count 0 clears.
     * Replaces earlier overrides of that entity. Expire after `steps` steps (0 = 50). */
    KilnResult(KILN_CALL* entity_overrides)(KilnEntity entity, const KilnConfigOverride* list, uint32_t count,
                                            uint32_t steps);

    /* ---- programs ---- */
    uint32_t(KILN_CALL* program_count)(void); /* tick */
    KilnResult(KILN_CALL* program_info)(uint32_t program_id, KilnImpactProgramInfo* out);        /* tick */
    KilnResult(KILN_CALL* program_vars)(uint32_t program_id, KilnImpactVar* out, uint32_t cap, uint32_t* count); /* tick */
    /* tick. Verifies and inserts a keen::ImpactProgram blob (copied). *out_id = its id. INVALID_ARGUMENT: the
     * verifier refused it (error says why, NUL-terminated), the GUID is a game program's, or this world's registry
     * holds the GUID with other bytes; UNAVAILABLE: the registry is not reachable yet (before the first step).
     * Kiln keeps registered programs across worlds and puts them back into each new world's registry; registering
     * the same bytes again is KILN_OK (in a new world before that happened, it puts the program back), so a mod may
     * register at every world start. Other bytes under a GUID go in only in a world that does not hold it yet. */
    KilnResult(KILN_CALL* program_register)(KilnOwner owner, const void* blob, size_t len, uint32_t* out_id,
                                            char* error, size_t error_cap);
    /* tick. Re-lists keen::ImpactProgram resources of kiln.assets and registers the new ones. *added = count. */
    KilnResult(KILN_CALL* program_refresh)(uint32_t* added);

    /* ---- natives ---- */
    uint32_t(KILN_CALL* native_count)(void);                               /* any thread */
    KilnResult(KILN_CALL* native_at)(uint32_t index, KilnNativeInfo* out); /* any thread */
    KilnResult(KILN_CALL* native_find)(uint32_t hash, KilnNativeInfo* out);
    /* any thread. Declares a Kiln native (from a mod's impact/natives.json) so programs that call it pass the
     * verifier before it is registered (a program waiting for an undeclared native is retried on every
     * native_declare / native_register / program_refresh). */
    KilnResult(KILN_CALL* native_declare)(uint32_t hash, uint32_t argc);
    /* tick. A managed native `name` (full "<modid>.<Name>", hash = FNV-1a-32 of it) with argc 0..31. Answers come
     * through MANAGED_CALL events. Collisions with a vanilla or a live Kiln native: INVALID_ARGUMENT. */
    KilnResult(KILN_CALL* native_register)(KilnOwner owner, KilnStr name, uint32_t argc, uint32_t* out_hash);
    /* tick. Takes a managed native off (programs that call it error out from then on). */
    KilnResult(KILN_CALL* native_unregister)(uint32_t hash);
    /* tick (listener). Answers a MANAGED_CALL: error 0 + value, or an error >= 4. */
    KilnResult(KILN_CALL* native_complete)(uint64_t call_id, uint8_t error, uint8_t value);
    /* any thread. Routes calls of a vanilla native through the sync dispatcher (on = 1) or not. Observation
     * (NATIVE events) needs no wrap: interest bit KILN_ABILITY_EV_NATIVE covers damage, buff and spawn; native_observe
     * adds other hashes. */
    KilnResult(KILN_CALL* native_wrap)(uint32_t hash, uint32_t on);
    KilnResult(KILN_CALL* native_observe)(uint32_t hash, uint32_t on);
    KilnResult(KILN_CALL* set_wrap_dispatcher)(KilnNativeWrapFn fn, void* user);
    /* job thread, from the dispatcher only: runs the game's native with the current args; sets out_error/out_value. */
    void(KILN_CALL* call_original)(KilnNativeCall* call);
    /* tick, outside a native call. Closes owner admission and drops its natives and attachments.
     * Waits at most 2 ms; KILN_OK does not mean an earlier callback returned. Caller-owned fn/user resources must
     * remain valid until its in-flight callbacks return (deferred retirements are reported by native_counters). */
    KilnResult(KILN_CALL* release_owner)(KilnOwner owner);

    /* ---- attributes (tick) ----
     * The current value of attribute `attr` (e.g. Mana 0x6aa2574e) in component `component` (FNV-1a of
     * "keen::ecs::Mana", 0x4d7229a8) of an entity, and adding to it through the game's attribute code (formulas
     * re-run). NOT_FOUND: no such component or attribute on the entity. */
    KilnResult(KILN_CALL* attr_get)(KilnEntity entity, uint32_t component, uint32_t attr, int32_t* value);
    KilnResult(KILN_CALL* attr_add)(KilnEntity entity, uint32_t component, uint32_t attr, int32_t delta);

    /* ---- sequences (tick) ----
     * The actor's current sequence (Actor.sequenceRuntimeId): sequence id (FNV-1a of the ActorSequenceResource GUID,
     * 0 = none), trigger time (ECS ns) and call index. NOT_FOUND: the entity has no Actor. */
    KilnResult(KILN_CALL* sequence_state)(KilnEntity entity, uint32_t* sequence_id, int64_t* trigger_time,
                                          uint32_t* call_index);
    /* tick. Starts an action sequence (animation, its timed impacts, costs, locks) on an actor the way the game's own
     * programs do (the helper death, fall damage and player actions call: ActorInput.triggerContext). sequence_id =
     * FNV-1a of an ActorSequenceResource GUID the game has; trigger = KILN_SEQUENCE_TRIGGER_*. The actor's sequence
     * system takes it in its next step. NOT_FOUND: no ActorInput on the entity, or the sequence is unknown.
     * trigger may carry KILN_SEQUENCE_FLAG_HOLD; older services answer INVALID_ARGUMENT. */
    KilnResult(KILN_CALL* sequence_play)(KilnEntity entity, uint32_t sequence_id, uint32_t trigger);

    /* ---- since 0.2.0-dev.29: synchronous natives (check header.size before use) ----
     * tick, and not from inside a native call (WRONG_THREAD there). A Kiln native `name` (full "<modid>.<Name>", hash =
     * FNV-1a-32 of it, argc 0..31) answered inside the program's ECALL, in the same simulation step: `fn` runs on the
     * thread that runs impact_simulation (the server world's thread on a dedicated server) with a KilnNativeCall that
     * has no original (call_original does nothing); its out_error/out_value are the result (out_error left at 0xFF:
     * error 0, value 0). A fault in `fn` answers 0x11 (the instance stops with Error, the game goes on). `fn` must not
     * block and must not call tick-only functions. native_unregister / release_owner take it off like a managed native:
     * from then on the hash answers 0x1F. `fn` and `user` must stay valid until every in-flight call returns, including after removal.
     * KILN_OK: *out_hash set. INVALID_ARGUMENT: a bad name, argc or fn (*out_refusal 0), or a refusal (*out_refusal is
     * KILN_NATIVE_REFUSED_*). UNAVAILABLE: no natives on this build. */
    KilnResult(KILN_CALL* native_register_sync)(KilnOwner owner, KilnStr name, uint32_t argc, KilnNativeWrapFn fn,
                                                void* user, uint32_t* out_hash, uint32_t* out_refusal);
    /* any thread. Counters of the native thunks (KilnNativeCounters; set out->size first). */
    KilnResult(KILN_CALL* native_counters)(KilnNativeCounters* out);

    /* ---- since 0.2.0-dev.29: attachments (check header.size before use) ----
     * tick, not from inside a native call. Attaches `fn` to the calls of the game's native `hash` that pass `filter`
     * (NULL = every call), at `stage` (KILN_NATIVE_*) and `priority` (Before and Instead high first, After low first;
     * ties in registration order). The program is the game's own: nothing is cloned or rewritten. `fn` runs inside the
     * call on the thread that runs impact_simulation, with the call record every stage of this call shares (a Before
     * may answer or call_original; the game's native runs at most once). A fault in Kiln's part runs the game's native
     * if it has not run yet. With a program in the filter and the world's programs loaded, the program must call the
     * native (at the site): else a refusal. Before the world has its natives or programs, the attachment waits; one
     * that does not fit them then is logged and stays off. *out: the handle for native_detach; release_owner drops
     * an owner's. INVALID_ARGUMENT: a bad stage, filter or fn, or a refusal (*out_refusal: KILN_NATIVE_REFUSED_*).
     * UNAVAILABLE: no natives on this build. */
    KilnResult(KILN_CALL* native_attach)(KilnOwner owner, uint32_t hash, const KilnNativeFilter* filter, int32_t stage,
                                         int32_t priority, KilnNativeWrapFn fn, void* user, KilnHandle* out,
                                         uint32_t* out_refusal);
    /* tick, not from inside a native call. From the next call on `fn` is not called; `fn`/`user` may go once this
     * returns (no call is in flight on the tick thread outside the step). NOT_FOUND: no such attachment. */
    KilnResult(KILN_CALL* native_detach)(KilnHandle attachment);

    /* ---- since 0.2.0-dev.29: typed declarations (check header.size before use) ----
     * any thread. Declares Kiln native `hash` with `argc` arguments and the type and size of each (args[0..argc), see
     * KilnNativeArg), replacing an earlier declaration. kiln.abilities reads the "sync" entries of every mod's
     * impact/natives.json this way when it starts. The verifier refuses a program that pushes a variable of another
     * type or size, a raw value for a variable argument, or nothing for a required one (E_ARGTYPE). Every call of a
     * Kiln native (sync or managed) with a typed declaration is checked before its handler runs: 0x2C for another
     * type or size, a raw value or an index out of bounds, 0x18 for a required argument not connected.
     * INVALID_ARGUMENT: argc over 31, args NULL with argc > 0, or a vanilla native's hash. */
    KilnResult(KILN_CALL* native_declare_typed)(uint32_t hash, uint32_t argc, const KilnNativeArg* args);

    /* ---- since 0.2.0-dev.29: states (check header.size before use) ----
     * any thread. Where a Kiln native stands (KILN_NATIVE_STATE_*): one registered before the world exists waits for its
     * registry, and the world's setup may still refuse it for room (REFUSED). An attachment (KILN_NATIVE_ATTACH_*):
     * PENDING until its program and site were checked against the world's programs (also for a program registered
     * later: checked in the Pre after it goes in), then ACTIVE or OFF. */
    KilnResult(KILN_CALL* native_state)(uint32_t hash, uint32_t* out_state);
    KilnResult(KILN_CALL* native_attach_state)(KilnHandle attachment, uint32_t* out_state);

    /* ---- since 0.2.0-dev.29: the ECS bridge (chapter 06c 9.1), check header.size ----
     * Inside a sync native's or an attachment's callback (`call` as it got it): component `component` (short name:
     * "Health") of entity `entity`, through impact_simulation's own other_read / other_write accessors (kiln.world's
     * foreign view; the job's storage and tracker). native_ecs_read copies the component's first `size` bytes (at
     * most its size) into `out`. KILN_ERR_NOT_FOUND: the program does not declare that component for other entities, or
     * the entity has none; KILN_ERR_INVALID_ARGUMENT: `size` is over the component's (for a write: not its size); KILN_ERR_UNAVAILABLE: the bridge is not on in this
     * world (KilnNativeCounters.ecs_state), or not inside a call. */
    KilnResult(KILN_CALL* native_ecs_read)(const KilnNativeCall* call, KilnStr component, uint32_t entity, void* out,
                                           uint32_t size);
    /* The same through the other_write accessor with the job's tracker (saved and replicated like the game's own
     * writes): `before` (optional, size bytes) gets the bytes before, then `value` is written; with
     * KILN_NATIVE_ECS_ATTRIBUTE in `flags` the game's attribute formulas run after it (an attribute block with values);
     * `after` (optional) gets the bytes after. KILN_ERR_NOT_FOUND also for a component only in other_read. */
    KilnResult(KILN_CALL* native_ecs_write)(KilnNativeCall* call, KilnStr component, uint32_t entity, const void* value,
                                            uint32_t size, uint32_t flags, void* before, void* after);

    /* ---- since 0.2.0-dev.30.3 (mod API 0.2.13): an item used from a player's slot, check header.size (a table
     * without it ends at 312) ----
     * tick, in Pre or Post, in a process with a server world (KILN_ERR_WRONG_THREAD on a joined client, before a world
     * started, or inside a Kiln native call: a sync native, an attachment or a wrap runs in the world's step, beside
     * the game's programs on other threads). What the game does when a player uses an item from the
     * hotbar: the item's own sequence starts through the helper player actions use (ActorInput.triggerContext, trigger
     * ActionSequenceTrigger.Primary), with the slot and the item as the trigger context's used item. From then on the
     * game's own code runs as for a hand use: the sequence's consume event takes one item from that slot in its
     * transaction (actor_apply_buff, the spawn path), the buff is applied with the used item (so with the item's own
     * impactValues and time), and NetworkActor and the buff entity replicate as they do for a hand use. Nothing is
     * consumed here: the item goes when the sequence's consume event runs (0.05 to 0.6 s later for the game's
     * consumables), and anything that stops a hand use (a cancel, a rule, death) stops this one.
     * Checked first, in this order (use->status; KILN_OK either way): the slot is in the entity's own inventories
     * (kiln.items read: NOT_OWN_SLOT), holds something (EMPTY_SLOT) and holds use->item (OTHER_ITEM); the inventories
     * are a player's (InventorySetup IsPlayer) and the entity has an Actor and an ActorInput (NOT_A_PLAYER); the player is not dead (StateFlag.Dead: DEAD), is ready
     * for gameplay (StateFlag.IsReadyForGameplay: NOT_READY), and runs no sequence, has no trigger waiting for this
     * step (ActorInput.triggerContext: a second item_use or a sequence_play in the same step, or one the game queued),
     * and is in none of the states a hand use does not start from (sleeping, sitting, climbing, gliding and others:
     * BUSY). The game has no cooldown for consumables: what plays as one is the sequence itself, reported as BUSY.
     * use->sequence must be the item's own sequence for this use (ItemInfo.sequences[] with triggeredBy Primary;
     * FNV-1a of its GUID): Kiln.Abilities reads it from the game's data and checks that it consumes the item. A
     * sequence whose consume event runs with no item in the trigger context takes the server down
     * (docs/re/abilities.md), so the item is checked against the slot here, at the call, and a trigger that faults
     * before it has its used item is taken back (the context cleared, KILN_ERR_INTERNAL).
     * KILN_ERR_INVALID_ARGUMENT: NULL, size, entity, slot, item or sequence 0, flags not 0. KILN_ERR_NOT_FOUND: a
     * sequence the game does not have. KILN_ERR_UNAVAILABLE: sequences off on this build (abilities.sequences), the
     * trigger context (its used item, its runtime id), Actor.sequenceRuntimeId or the state flags not in this build's
     * reflection (kiln.types), or kiln.items not here (kiln.log says which, once). */
    KilnResult(KILN_CALL* item_use)(KilnItemUse* use);
} KilnAbilitiesApi;
enum { KILN_NATIVE_ECS_ATTRIBUTE = 1u << 0 };

/* sequence_play() trigger (keen::actor::ActionSequenceTrigger) */
enum {
    KILN_SEQUENCE_TRIGGER_EXTERNAL = 22,       /* ExternalTrigger */
    KILN_SEQUENCE_TRIGGER_ABORT_OVERRIDE = 24, /* ExternalAbortAndOverride: aborts the current sequence (fall damage uses it) */
    KILN_SEQUENCE_FLAG_HOLD = 0x10000,         /* OR into trigger: hold the sequence's keep-alive states (held input) */
};

#ifdef __cplusplus
}
#endif
#endif

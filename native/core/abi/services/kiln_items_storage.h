/* SPDX-License-Identifier: MIT
 * kiln.items.storage v1: per-player storage that mods register (quivers, spellbooks, extra bags). Since
 * 0.2.0-dev.30.7 (mod API 0.2.16). Served by kiln-items.dll. Usage: docs/kiln/services/player-storage.md. Managed face:
 * Kiln.Items (Items.RegisterPlayerStorage, PlayerStorage, Items.PlayerStorages); the persistence part: Kiln.Saves.
 *
 * What a storage is. One registration (a full key "<mod id>:<name>", a slot count, a filter and a death rule) gives
 * every player of the server world one storage of that key. In memory a storage is a Kiln-owned inventory holder per
 * player and storage: an entity with its own InventorySetup whose links are real 8-slot Inventory pages (Generic),
 * with real stacks and real item entities, built the way the game builds a player's bag pages. The holder is not linked
 * into the player's InventorySetup, so the game never draws it, saves it with the character, crafts from it or buries
 * it. kiln.items' own rows (give, take, move, move all, equip, read) work on a holder like on a chest; every add into a
 * holder page is checked against the storage's filter (status KILN_ITEMS_FILTERED). A client's
 * InventoryTransferAction to or from a holder or its pages is refused (only the server moves storage items).
 * Holders are never returned by kiln.items' find_inventories unless KILN_ITEMS_FIND_PLAYER_STORAGE is set.
 *
 * On disk Kiln keeps the contents (Kiln.Saves: the world's Kiln store per character, and the character's sidecar that
 * travels with it), as the game's own item records, paired with a witness: the xxh3-64 of the character's inventory
 * exactly as the game captured it in the same save. The game's character save is never written by Kiln.
 *
 * Two parts:
 *  - public rows, for mods and plugins: register_storage, holder, state, set_ref, status, list, next_event,
 *    return_parked;
 *  - persist rows (capability items.storage.persist), for Kiln.Saves only, not for mods: persist_bind, take_frozen,
 *    release_frozen, witness_of, restore, park, unpark. Exactly one owner (the persistence owner) holds
 *    them at a time: persist_bind claims them, and every other persist row answers KILN_ERR_UNAVAILABLE for any other
 *    owner, and for everyone while nobody is bound. Holders are only ever built by a restore, so without a persistence
 *    owner no item can enter a storage that would not be saved.
 *
 * A player's storages, through a session:
 *  1. Join. The game reads the character (characters/Read, the struct reader on the join path). Kiln computes W_g, the
 *     witness of what it read, starts a new join for that slot and posts KIS_EVENT_JOINED; witness_of(slot) answers
 *     from then on (the player entity is 0 until the join apply spawns it).
 *  2. Decide and restore. Kiln.Saves picks the copy (the duplication rule) and calls restore(slot, join, document)
 *     once for this join, with an empty document (KIS_SOURCE_NEW) for a character without storage. restore is valid at
 *     any time after the read of step 1; it copies the document and returns. At the end of the game's join apply
 *     (characters/JoinApply: the player's own pages exist) Kiln builds a holder for every registered key and rebuilds
 *     its records; a restore that comes after the join apply is built in the next sim step. Records that fail
 *     validation are quarantined (kept, with a reason); records of a key no loaded mod registered, or that do not fit,
 *     stay parked unchanged. KIS_EVENT_RESTORED / _PARKED / _QUARANTINED follow.
 *  3. Play. Mods move items in and out with kiln.items' rows. A registration made while the player is online restores
 *     the key's parked records into a new holder (KIS_EVENT_RESTORED with KIS_EVENT_LIVE). When the registering owner
 *     is revoked, its storages' contents are parked (KIS_PARK_UNREGISTERED) and their holders removed, in the next sim
 *     step.
 *  4. World save. The game's capture of a connected character (characters/Capture) reads the live world. In its
 *     post-hook, on the server thread at that same instant, Kiln freezes the character's storages with the game's own
 *     record converter and writer, computes W of the struct the game captured, and adds a KilnStorageCapture to the
 *     frozen batch of that save (KIS_EVENT_CAPTURED). No managed code runs in that hook. The game's per-player save
 *     step then ends: after every capture of the save and before the game starts its world-write worker or publishes
 *     the captured characters to their clients (or writes the hosting player's character file), Kiln marks the batch
 *     complete, posts KIS_EVENT_BATCH and calls the persistence owner's stage function synchronously on the server
 *     thread: the captures-done stage, in the pattern of kiln.saves' SNAPSHOT stage (a pump, its kind a hint, the events
 *     read with next_event). There Kiln.Saves takes the batch (take_frozen) and queues the storage part of the pending
 *     generation for its writer; kiln.saves' stage barrier holds the world write, and the publication to clients after
 *     it, until that part is durable, so the pair is on disk before the world file and before the character leaves.
 *     It releases the batch (release_frozen) once it has it. A leave save runs the same way. On a build without the
 *     captures-done hook items.storage.persist is unavailable ("no captures-done hook on this build") and persist_bind
 *     refuses: no holder is ever built there.
 *  5. Death. Per storage: KIS_DEATH_KEEP, _GRAVE (everything into the player's tombstone, when the game made one), or
 *     _FOLLOW_GAME (the game's tombstoneMode: Everything takes all, AddBackpackMaterials only Materials and Collectibles,
 *     NoTombstone nothing). Kiln never makes a tombstone itself. KIS_EVENT_DEATH.
 *  6. Leave. The leave save captures the storages (4). Then the holders, their pages and their item entities are removed
 *     (KIS_EVENT_LEFT). A player who leaves without a save keeps the last committed copy.
 *
 * Threads
 *  - Every row: any thread. Values are as of the last sim step. Writes (set_ref, return_parked, restore, park, unpark)
 *    are copied before the call returns and run in the next sim step (a restore before the join apply: at its end),
 *    inside kiln.items' program.
 *  - KilnStorageStageFn: the server world's thread, inside the game's save, after the world's jobs finished and after
 *    the save's captures (game state may be read as in Post). It must not block on the disk, wait for another thread
 *    or call kiln.items' write rows: it copies the batch and only queues the pending write; kiln.saves' stage barrier
 *    (stage_progress) then holds the world write, and with it the publication to clients, until that write is
 *    durable. A stage function that faults is removed (logged); the batch stays for take_frozen.
 *  - KilnStorageReturnFn: the tick thread, Pre of the step after the return ran.
 *
 * Fail soft: on a game build where a row of sigdb feature items.storage does not resolve, register_storage still
 * records registrations (so a later build or a restart can use them), but no holder is built, persist_bind answers
 * KILN_ERR_UNAVAILABLE and status.storage is 0 with the reason. Evidence: docs/re/player-storage.md. */
#ifndef KILN_ITEMS_STORAGE_H
#define KILN_ITEMS_STORAGE_H
#include "../kiln_api.h"
#ifdef __cplusplus
extern "C" {
#endif

#define KILN_ITEMS_STORAGE_SERVICE "kiln.items.storage"
#define KILN_ITEMS_STORAGE_VERSION 1u

/* ------------------------------------------------------------------------------------------------ limits ------- */

#define KIS_MAX_SLOTS 64u          /* slots of one storage: 8 pages */
#define KIS_MAX_PER_MOD 8u         /* registrations of one owner */
#define KIS_MAX_SLOTS_PLAYER 256u  /* slots of all registered storages together (every player has each): 32 pages */
#define KIS_MAX_FILTER_IDS 1024u   /* KilnStorageFilter.id_count */
#define KIS_MAX_STORAGES 32u       /* distinct keys one character carries: registered and parked */
#define KIS_MAX_ENTRIES 128u       /* entries of one capture or document */
#define KIS_MAX_RECORDS 2048u      /* records of one capture or document */
#define KIS_KEY_MAX 128u           /* bytes of a full key's buffer, NUL included */
#define KIS_RECORD_MAX 64u         /* bytes of one item record (the game's writes at most 36 today) */

/* Keys. A full key is "<mod id>:<name>": the mod id as in kiln.mod.json (^[a-z0-9][a-z0-9._-]{0,63}$), a colon, and
 * a name of ^[a-z0-9][a-z0-9._-]{0,31}$. The managed layer prefixes the calling mod's id itself; a native plugin passes
 * its own id. A full key is registered by one live registration at a time. */

/* ------------------------------------------------------------------------------------------------ enums -------- */

/* KilnStorageDef.death, KilnStorageState.death, KilnStorageEvent.reason of KIS_EVENT_DEATH */
enum {
    KIS_DEATH_KEEP = 0,        /* the storage keeps its items when the player dies (default) */
    KIS_DEATH_GRAVE = 1,       /* everything goes into the player's tombstone, when the game made one */
    KIS_DEATH_FOLLOW_GAME = 2, /* the game's own rule (GameSettings.tombstoneMode) decides which stacks go */
};

/* KilnStorageFilter.flags */
enum {
    KIS_FILTER_AMMO_ARROW = 1u, /* items the game equips into the arrow slot */
    KIS_FILTER_AMMO_SPELL = 2u, /* items the game equips into the spell slot */
    KIS_FILTER_NO_PIDE = 4u,    /* only items without an item entity (plain stackables, no gear) */
};

/* KilnStorageState.status */
enum {
    KIS_STATE_NONE = 0,    /* this player has no storage of that key: not registered and nothing parked */
    KIS_STATE_WAITING = 1, /* registered; the player's storages are not restored yet (join in progress, or no
                              persistence owner) */
    KIS_STATE_READY = 2,   /* the holder is built: holder() answers, kiln.items' rows work on it */
    KIS_STATE_PARKED = 3,  /* not registered (or held): its records are parked unchanged */
    KIS_STATE_LOCKED = 4,  /* this character's storage is read-only this session (KIS_DOC_LOCKED): nothing is built */
};

/* KilnStorageState.source, KilnStorageDoc.source, KilnStorageEvent.source */
enum {
    KIS_SOURCE_NONE = 0,   /* not restored */
    KIS_SOURCE_NEW = 1,    /* no copy anywhere: the storages start empty */
    KIS_SOURCE_SERVER = 2, /* the world's Kiln store */
    KIS_SOURCE_CLIENT = 3, /* the character's carried copy (the client's sidecar, kiln:character) */
};

/* KilnStorageEntry.kind */
enum {
    KIS_ENTRY_LIVE = 1,       /* a registered storage: its holder's contents (restore: what was live when saved) */
    KIS_ENTRY_PARKED = 2,     /* records kept unchanged, not in any holder (reason says why) */
    KIS_ENTRY_QUARANTINE = 3, /* records that failed validation; each record's reason says why (captures only) */
};

/* KilnStorageEntry.reason of a KIS_ENTRY_PARKED entry; KilnStorageEvent.reason of KIS_EVENT_PARKED; park(reason).
 * Below KIS_PARK_HELD Kiln restores the records by itself when they can go back (the key registers, the storage has
 * room, the filter takes them). From KIS_PARK_HELD on they stay parked until unpark() or return_parked(). */
enum {
    KIS_PARK_UNREGISTERED = 1, /* no loaded mod registered the key (or its owner went away while the player was on) */
    KIS_PARK_OVERFLOW = 2,     /* the slot is beyond the registration's slot count, or the cell was taken */
    KIS_PARK_FILTER = 3,       /* the registration's filter does not take the item */
    KIS_PARK_HELD = 16,        /* first held reason */
    KIS_PARK_REQUESTED = 16,   /* park() without a more specific reason */
    KIS_PARK_STALE = 17,       /* changed elsewhere since this server's copy (the server does not trust the client's
                                  copy, which is newer): kept, never instantiated, until an admin releases it */
    KIS_PARK_LOCKED = 18,      /* the document is from a newer Kiln: kept as it is */
};

/* KilnStorageRecord.reason of a record in a KIS_ENTRY_QUARANTINE entry */
enum {
    KIS_QUARANTINE_UNKNOWN_ITEM = 1, /* the item id is not in the game's item registry */
    KIS_QUARANTINE_COUNT = 2,        /* count 0, or over the item's max stack */
    KIS_QUARANTINE_SLOT = 3,         /* slot out of range for the record format, or two records for one slot */
    KIS_QUARANTINE_DECODE = 4,       /* the game's record reader refused the bytes */
    KIS_QUARANTINE_REBUILD = 5,      /* the game refused to create the item from the record */
};

/* KilnStorageEntry.refs / KilnStorageState.refs: a player's four reference slots (keen::EquipmentSlot RangedWeapon 12,
 * Arrow 14, Spell 15, FishingBait 16). The game's character save keeps a reference only when it points into one of the
 * player's own Generic pages, so Kiln keeps a reference into a storage itself and writes SlotSelection again after the
 * restore (only while that slot points nowhere). */
enum { KIS_REF_RANGED = 0, KIS_REF_ARROW = 1, KIS_REF_SPELL = 2, KIS_REF_BAIT = 3, KIS_REF_COUNT = 4 };
#define KIS_REF_NONE 0xFFFFu       /* refs[i]: no reference */
#define KIS_REF_CLEAR 0xFFFFFFFFu  /* set_ref(index): clear the reference */

/* KilnStorageRecord.flags: the record's field flags, as the game's record writes them */
enum {
    KIS_RECORD_LEVEL = 0x01u,      /* u8 level */
    KIS_RECORD_DURABILITY = 0x02u, /* u32 durability, u32 max */
    KIS_RECORD_LIQUID = 0x04u,     /* u32 liquid or fuel, u32 max */
    KIS_RECORD_DYE = 0x08u,        /* u32 appearance */
    KIS_RECORD_UPGRADES = 0x10u,   /* u8 max upgrades, u8 applied */
    KIS_RECORD_GEM = 0x20u,        /* u32 + u8 socket */
};

/* KilnStorageDoc.flags */
enum {
    KIS_DOC_PAIRED = 1u, /* the chosen copy's witness equals W_g (for logs, state and events) */
    KIS_DOC_LOCKED = 2u, /* read-only this session: build nothing, keep every entry as given, captures carry
                            KIS_CAPTURE_LOCKED (a document from a newer Kiln) */
};

/* KilnStorageCapture.flags */
enum {
    KIS_CAPTURE_UNRESTORED = 1u, /* no restore was accepted for this join yet: no entries, they say nothing about the
                                    storage; keep your copy (only the witness is new) */
    KIS_CAPTURE_LOCKED = 2u,     /* the restore was KIS_DOC_LOCKED: do not write this character's storage */
    KIS_CAPTURE_REFUSED = 4u,    /* the game's capture ran but Kiln could not freeze (reason: KILN_CAP_REASON_*): no
                                    entries; stage nothing for this character and never pair an older copy with this
                                    witness (it may be 0) */
    KIS_CAPTURE_LEFT = 8u,       /* the player left after this capture (its holders are gone) */
};

/* KilnStorageFrozenBatch.flags */
enum {
    KIS_BATCH_COMPLETE = 1u, /* the save's captures are all in (the captures-done stage ran) */
    KIS_BATCH_REFUSED = 2u,  /* at least one capture carries KIS_CAPTURE_REFUSED */
};

/* KilnStorageWitness.flags */
enum {
    KIS_WITNESS_READ = 1u,     /* the game read the character at this join: witness is W_g */
    KIS_WITNESS_APPLIED = 2u,  /* the game's join apply made the player's own pages */
    KIS_WITNESS_RESTORED = 4u, /* restore was accepted for this join */
    KIS_WITNESS_LEFT = 8u,     /* the player left (the values stay until the slot is taken again) */
};

/* KilnStorageEvent.kind */
enum {
    KIS_EVENT_JOINED = 1,      /* the game read a joining character: witness_of answers (for the persistence owner) */
    KIS_EVENT_CAPTURED = 2,    /* a capture was frozen into batch `tag` (for the persistence owner) */
    KIS_EVENT_RESTORED = 3,    /* a key's holder was built: stacks rebuilt, source, revision, KIS_EVENT_PAIRED */
    KIS_EVENT_PARKED = 4,      /* records of a key were parked: stacks, reason KIS_PARK_* */
    KIS_EVENT_QUARANTINED = 5, /* records of a key were quarantined: stacks, reason of the first KIS_QUARANTINE_* */
    KIS_EVENT_DEATH = 6,       /* the player died: stacks/items moved to target (0: kept), reason = the rule applied */
    KIS_EVENT_LEFT = 7,        /* the player left: its holders were removed */
    KIS_EVENT_RETURNED = 8,    /* return_parked ran: stacks/items to target, handle = 0 */
    KIS_EVENT_BATCH = 9,       /* batch `tag` is complete: the captures-done stage (for the persistence owner) */
};

/* KilnStorageEvent.flags */
enum {
    KIS_EVENT_PAIRED = 1u, /* RESTORED: the restored copy was paired */
    KIS_EVENT_LIVE = 2u,   /* RESTORED: a registration or unpark while the player was online */
};

/* kiln.items' find_inventories flag for holders: holders only come back when it is set. Flags 0 ("all") and the other
 * KILN_ITEMS_FIND_* bits never return a holder (managed: InventoryKinds.PlayerStorage, not part of All). */
#define KILN_ITEMS_FIND_PLAYER_STORAGE 0x10u

/* kiln.items' result status (KilnItemsResult.status, KilnItemsMoveResult.status): an add into a holder page that the
 * storage's filter does not take; nothing changed. */
#define KILN_ITEMS_FILTERED 10

/* ------------------------------------------------------------------------------------------------ structs ------ */

/* What items a storage takes. Every condition that is set must hold (AND). */
typedef struct KilnStorageFilter {
    uint32_t size;             /* sizeof(KilnStorageFilter) */
    uint32_t categories;       /* bit (1u << ItemCategory) per category taken; 0 = any category */
    uint32_t flags;            /* KIS_FILTER_*: with AMMO_ARROW and/or AMMO_SPELL only that ammunition */
    uint32_t id_count;         /* 0 = no id list; else 1..KIS_MAX_FILTER_IDS */
    const uint32_t* ids;       /* sorted ascending, no duplicates: the items taken (copied by register_storage) */
} KilnStorageFilter;

typedef struct KilnStorageDef {
    uint32_t size;             /* sizeof(KilnStorageDef) */
    uint32_t slots;            /* 1..KIS_MAX_SLOTS; pages = slots rounded up to 8 */
    KilnStr key;               /* full key "<mod id>:<name>" (copied) */
    uint32_t death;            /* KIS_DEATH_* */
    uint32_t flags;            /* 0 */
    KilnStorageFilter filter;
} KilnStorageDef;

/* One key of one player (state() for a registration, list() for every key of the player). */
typedef struct KilnStorageState {
    uint32_t size;             /* set by the caller: sizeof(KilnStorageState) */
    uint32_t status;           /* KIS_STATE_* */
    char key[KIS_KEY_MAX];     /* full key, NUL-terminated */
    uint32_t handle;           /* the registration; 0 for a key no live registration has */
    uint32_t slots;            /* the registration's slot count (0 when unregistered) */
    uint32_t pages;            /* the holder's pages (0 until READY) */
    KilnEntity holder;         /* 0 until READY */
    uint32_t stacks;           /* non-empty cells in the holder */
    uint32_t parked;           /* parked records of this key */
    uint32_t quarantined;      /* quarantined records of this key */
    uint32_t source;           /* KIS_SOURCE_* of this join's restore */
    uint32_t paired;           /* 1: this join's restore was paired */
    uint32_t death;            /* KIS_DEATH_* of the registration */
    uint16_t refs[KIS_REF_COUNT]; /* storage index per KIS_REF_*, KIS_REF_NONE */
    uint64_t revision;         /* the document revision this join's restore took */
} KilnStorageState;

/* One item record: the game's own record bytes plus a decoded mirror for tools. */
typedef struct KilnStorageRecord {
    uint16_t slot;             /* index in the storage, 0..KIS_MAX_SLOTS-1 (page * 8 + cell) */
    uint8_t length;            /* bytes used in data, 8..KIS_RECORD_MAX */
    uint8_t flags;             /* KIS_RECORD_*: the record's field flags */
    uint32_t item;             /* mirror: the item id */
    uint32_t count;            /* mirror: the stack count */
    uint8_t level;             /* mirror: the level with KIS_RECORD_LEVEL, else 0 */
    uint8_t reason;            /* KIS_QUARANTINE_* in a QUARANTINE entry, else 0 */
    uint16_t reserved;         /* 0 */
    uint8_t data[KIS_RECORD_MAX]; /* the record as the game's record writer writes it (its u8 slot first) */
} KilnStorageRecord;

/* One storage in a capture or a document: its records are records[first .. first + count). */
typedef struct KilnStorageEntry {
    uint32_t size;             /* sizeof(KilnStorageEntry) */
    uint32_t kind;             /* KIS_ENTRY_* */
    char key[KIS_KEY_MAX];     /* full key, NUL-terminated; a key has at most one LIVE entry */
    uint32_t slots;            /* LIVE: the registration's slots; PARKED: the slots it had when it was parked */
    uint32_t first;            /* index of its first record */
    uint32_t count;            /* records */
    uint32_t reason;           /* PARKED: KIS_PARK_* (a key may have one PARKED entry per reason); else 0 */
    uint16_t refs[KIS_REF_COUNT]; /* LIVE: storage index per KIS_REF_*, KIS_REF_NONE */
} KilnStorageEntry;

/* What restore takes: the chosen copy of one character. Every pointer is read before restore returns. */
typedef struct KilnStorageDoc {
    uint32_t size;             /* sizeof(KilnStorageDoc) */
    uint32_t flags;            /* KIS_DOC_* */
    uint32_t source;           /* KIS_SOURCE_* */
    uint32_t entry_count;      /* 0..KIS_MAX_ENTRIES; LIVE and PARKED entries only */
    uint32_t record_count;     /* 0..KIS_MAX_RECORDS */
    uint32_t reserved;         /* 0 */
    uint64_t revision;         /* the document's revision */
    uint64_t witness;          /* the chosen pair's witness (0: none); for logs */
    uint64_t account;          /* the character it belongs to: must be the joined player's (witness_of) */
    uint8_t character[16];     /* the game's CharacterId (Windows byte order, as .NET Guid(byte[])) */
    const KilnStorageEntry* entries;
    const KilnStorageRecord* records;
} KilnStorageDoc;

/* One character in a frozen batch: every LIVE, PARKED and QUARANTINE entry Kiln held for it at the game's capture.
 * Its entries are entries[first_entry .. first_entry + entry_count) of the batch. */
typedef struct KilnStorageCapture {
    uint32_t size;             /* set by the caller (captures[0].size is enough): sizeof(KilnStorageCapture) */
    uint32_t flags;            /* KIS_CAPTURE_* */
    uint64_t seq;              /* 1, 2, 3 ... per run of the game */
    uint64_t join;             /* the join it belongs to (KilnStorageWitness.join) */
    uint64_t step;             /* server-world step of the capture */
    uint64_t witness;          /* W of the struct the game captured (section "The witness", docs/re/player-storage.md) */
    uint64_t revision;         /* the document revision this join's restore took (0: none) */
    uint64_t changes;          /* content changes since that restore (0: unchanged); equal values, equal contents */
    uint64_t account;          /* the character (the game's account key and CharacterId) */
    uint8_t character[16];     /* Windows byte order, as .NET Guid(byte[]) */
    KilnEntity player;
    uint32_t slot;             /* the game's player slot */
    uint32_t first_entry;
    uint32_t entry_count;
    uint32_t reason;           /* KIS_CAPTURE_REFUSED: KILN_CAP_REASON_*; else 0 */
    uint32_t reserved;
} KilnStorageCapture;

/* One save's frozen captures. A batch never mixes saves: a failed save's batch stays apart from the retry's. */
typedef struct KilnStorageFrozenBatch {
    uint32_t size;             /* set by the caller: sizeof(KilnStorageFrozenBatch) */
    uint32_t flags;            /* KIS_BATCH_* */
    uint64_t tag;              /* Kiln's id of the batch: 1, 2, 3 ... per run of the game */
    uint64_t saves_seq;        /* kiln.saves' SAVING event of that save (KilnSaveEvent.seq; WRITTEN/FAILED carry it as
                                  save_seq); 0: unknown */
    uint32_t revision;         /* the world revision that save will get (the SAVING event's); 0: unknown */
    uint32_t capture_count;
    uint32_t entry_count;
    uint32_t record_count;
    uint64_t first_step;       /* server-world steps of its first and last capture */
    uint64_t last_step;
    int64_t unix_ms;           /* UTC milliseconds of its first capture */
    uint8_t world_guid[16];    /* the world it belongs to (kiln.saves' world GUID; zero: unknown) */
} KilnStorageFrozenBatch;

/* A joining character, as the game read it. */
typedef struct KilnStorageWitness {
    uint32_t size;             /* set by the caller: sizeof(KilnStorageWitness) */
    uint32_t flags;            /* KIS_WITNESS_* */
    uint64_t join;             /* 1, 2, 3 ... per run of the game, one per join */
    uint64_t witness;          /* KIS_WITNESS_READ: W_g, of the struct the game read at this join */
    uint64_t step;             /* server-world step of the read */
    uint64_t captured;         /* W of this join's newest game capture (0: none yet) */
    uint64_t capture_seq;      /* its seq */
    uint64_t account;
    uint8_t character[16];
    KilnEntity player;         /* 0 until the join apply spawned the player */
    uint32_t slot;
} KilnStorageWitness;

typedef struct KilnStorageEvent {
    uint32_t size;             /* set by the caller: sizeof(KilnStorageEvent) */
    uint32_t kind;             /* KIS_EVENT_* */
    uint64_t seq;              /* 1, 2, 3 ... per run of the game */
    uint64_t step;
    KilnEntity player;
    uint32_t handle;           /* the registration (0: none, a parked key, the persistence kinds) */
    uint32_t flags;            /* KIS_EVENT_PAIRED, KIS_EVENT_LIVE */
    uint32_t reason;           /* PARKED: KIS_PARK_*; QUARANTINED: KIS_QUARANTINE_*; DEATH: KIS_DEATH_* applied */
    uint32_t stacks;
    uint32_t items;            /* the stacks' counts together */
    KilnEntity target;         /* DEATH: the tombstone (0: kept); RETURNED: where they went */
    uint32_t source;           /* RESTORED: KIS_SOURCE_* */
    uint32_t slot;             /* the game's player slot */
    uint32_t reserved;
    uint64_t revision;         /* RESTORED: the document revision */
    uint64_t join;             /* the join (every kind about a player) */
    uint64_t tag;              /* CAPTURED, BATCH: the batch */
    uint64_t account;
    uint8_t character[16];
    char key[KIS_KEY_MAX];     /* full key; empty for JOINED, CAPTURED, LEFT, BATCH */
} KilnStorageEvent;

typedef struct KilnItemsStorageStatus {
    uint32_t size;             /* set by the caller: sizeof(KilnItemsStorageStatus) */
    uint8_t storage;           /* 1: feature items.storage is on: holders can be built, filled, frozen, restored */
    uint8_t persist;           /* 1: a persistence owner is bound */
    uint8_t session;           /* 1: a server world runs in this process */
    uint8_t stage_point;       /* 1: this build has the captures-done hook (the stage function is called) */
    uint32_t reason;           /* KILN_CAP_REASON_* when storage is 0, else NONE */
    uint32_t registrations;
    uint32_t holders;          /* built holders, all players */
    uint32_t parked;           /* parked records, connected characters */
    uint32_t quarantined;      /* quarantined records, connected characters */
    uint32_t batches;          /* frozen batches held */
    uint64_t captures;         /* game captures frozen this run */
    uint64_t restores;         /* restores accepted this run */
    char detail[160];          /* why something is off; NUL-terminated */
} KilnItemsStorageStatus;

/* return_parked's result. */
typedef struct KilnStorageReturnResult {
    uint32_t size;             /* sizeof(KilnStorageReturnResult) */
    uint32_t status;           /* KILN_ITEMS_DONE / PARTIAL / NO_ROOM / NO_INVENTORY / UNAVAILABLE / DROPPED */
    uint64_t ticket;
    KilnEntity player;
    KilnEntity target;
    uint32_t requested;        /* items in the parked records */
    uint32_t done;             /* items rebuilt into target */
    uint32_t stacks;           /* records rebuilt */
    uint32_t kept;             /* records still parked (no room) */
    uint32_t steps;            /* sim steps it took */
    uint32_t reserved;
    char message[160];         /* NUL-terminated */
} KilnStorageReturnResult;

typedef void(KILN_CALL* KilnStorageReturnFn)(const KilnStorageReturnResult* result, void* user);
/* The captures-done stage: `kind` is the kind of the newest queued event (a hint: KIS_EVENT_BATCH; read them with
 * next_event). */
typedef void(KILN_CALL* KilnStorageStageFn)(uint32_t kind, void* user);

/* ------------------------------------------------------------------------------------------------ table -------- */

typedef struct KilnItemsStorageApi {
    KilnServiceHeader header; /* { sizeof(KilnItemsStorageApi), KILN_ITEMS_STORAGE_VERSION } */

    /* ---- public rows ---- */

    /* Registers one storage for every player. Copies the definition (key, filter ids). One live registration per full
     * key; an owner holds at most KIS_MAX_PER_MOD, and all registrations together at most KIS_MAX_SLOTS_PLAYER slots.
     * Players already restored get the key's holder in the next sim step (their parked records of the key go in).
     * INVALID_ARGUMENT: a bad size, key, slot count, death rule, flags or id list (unsorted, duplicates, too many), or the
     * key is registered already; UNAVAILABLE: a quota is full; REVOKED: the owner is gone. *out_handle != 0 on OK. */
    KilnResult(KILN_CALL* register_storage)(KilnOwner owner, const KilnStorageDef* def, uint32_t* out_handle);
    /* The player's holder of a registration. NOT_FOUND until READY, for a player without storages, or a handle that is
     * not the owner's. */
    KilnResult(KILN_CALL* holder)(KilnOwner owner, uint32_t handle, KilnEntity player, KilnEntity* out);
    /* The player's state of a registration (any status). NOT_FOUND: a handle that is not the owner's. */
    KilnResult(KILN_CALL* state)(KilnOwner owner, uint32_t handle, KilnEntity player, KilnStorageState* out);
    /* Points the player's reference slot equip_slot (keen::EquipmentSlot 12, 14, 15 or 16) at the storage's cell
     * `index`, or clears it (KIS_REF_CLEAR), in the next sim step; state().refs shows it afterwards, and captures carry
     * it. NOT_FOUND until READY; INVALID_ARGUMENT: another slot, or index >= slots. */
    KilnResult(KILN_CALL* set_ref)(KilnOwner owner, uint32_t handle, KilnEntity player, uint32_t equip_slot,
                                   uint32_t index);
    /* The service's state. Set out->size first. */
    KilnResult(KILN_CALL* status)(KilnItemsStorageStatus* out);
    /* Every key of the player: its registrations (any status) and its parked keys, sorted by key. BUFFER_TOO_SMALL with
     * *out_count = needed; NOT_FOUND: no such connected player. Set out[i].size first (out[0].size is enough). */
    KilnResult(KILN_CALL* list)(KilnEntity player, KilnStorageState* out, uint32_t capacity, uint32_t* out_count);
    /* The oldest event with seq > after_seq (the queue keeps the newest 256). NOT_FOUND: none. Set out->size first. */
    KilnResult(KILN_CALL* next_event)(uint64_t after_seq, KilnStorageEvent* out);
    /* Rebuilds every parked record of `key` (any reason) into `target`, in the next sim step: the player itself (its
     * backpack: Generic pages and the worn backpack's pages, never equipment or action bar) or an inventory owner (a
     * chest). What does not fit stays parked. done runs once, also for DROPPED; KIS_EVENT_RETURNED follows.
     * NOT_FOUND: the player has no parked records of that key; INVALID_ARGUMENT: bad key or target. */
    KilnResult(KILN_CALL* return_parked)(KilnOwner owner, KilnEntity player, KilnStr key, KilnEntity target,
                                         KilnStorageReturnFn done, void* user, uint64_t* out_ticket);

    /* ---- persist rows (items.storage.persist; Kiln.Saves only) ---- */

    /* Claims the persist rows for `owner` and sets its stage function (fn = NULL keeps the claim without one; the
     * owner's revoke releases it). Owner 0 (a Kiln service without an owner of its own, as kiln.saves' pump) is taken
     * too: it is never revoked, so its claim lasts for the run. Returns the newest event seq in *last_seq (events up
     * to it are history).
     * UNAVAILABLE: items.storage.persist is off (no storage, or no captures-done hook), or another owner holds them. */
    KilnResult(KILN_CALL* persist_bind)(KilnOwner owner, KilnStorageStageFn fn, void* user, uint64_t* last_seq);
    /* Copies batch `tag`: its header, captures, entries and records. Two calls: with too little room
     * BUFFER_TOO_SMALL, *out filled with the counts needed and the arrays left alone. The batch stays until
     * release_frozen; it may still grow while KIS_BATCH_COMPLETE is not set. Kiln keeps the newest 16 batches.
     * NOT_FOUND: no such batch. */
    KilnResult(KILN_CALL* take_frozen)(KilnOwner owner, uint64_t tag, KilnStorageFrozenBatch* out,
                                       KilnStorageCapture* captures, uint32_t capture_capacity,
                                       KilnStorageEntry* entries, uint32_t entry_capacity,
                                       KilnStorageRecord* records, uint32_t record_capacity);
    /* Drops batch `tag`: Kiln.Saves has it, committed or staged on its own. NOT_FOUND: no such batch. */
    KilnResult(KILN_CALL* release_frozen)(KilnOwner owner, uint64_t tag);
    /* The current join of the game's player slot `slot`: W_g and the character the game read. NOT_FOUND before the game
     * read a character for that slot. */
    KilnResult(KILN_CALL* witness_of)(KilnOwner owner, uint32_t slot, KilnStorageWitness* out);
    /* Restores the chosen document for join `join` of slot `slot` (once per join, at any time after the game read the
     * character). Copies it, checks its form, returns. Kiln builds the holders at the end of the game's join apply, or
     * in the next sim step when the join apply is past.
     * Entries of registered keys are rebuilt (LIVE, or PARKED below KIS_PARK_HELD; records keep their slot), the rest
     * stays parked with its reason, records that fail validation are quarantined. Until the holders exist a capture
     * carries the given entries unchanged. INVALID_ARGUMENT: a bad size, flags, source, counts or ranges, a key, a
     * QUARANTINE entry, two LIVE entries of one key, more than KIS_MAX_STORAGES keys, a character that is not the one
     * the game read, or a second restore for this join; NOT_FOUND: `join` is not the slot's current join. */
    KilnResult(KILN_CALL* restore)(KilnOwner owner, uint32_t slot, uint64_t join, const KilnStorageDoc* doc);
    /* Parks the player's storage `key` in the next sim step: its holder's contents are frozen into a PARKED entry with
     * `reason` (KIS_PARK_HELD or above) and the holder is removed; parked records of the key below KIS_PARK_HELD take
     * that reason too. NOT_FOUND: the player has no such key; INVALID_ARGUMENT: a reason below KIS_PARK_HELD. */
    KilnResult(KILN_CALL* park)(KilnOwner owner, KilnEntity player, KilnStr key, uint32_t reason);
    /* Puts the player's parked records of `key` (any reason) back into its holder in the next sim step: each into its
     * own slot when free, else the first free slot; what does not fit stays parked (KIS_PARK_OVERFLOW). NOT_FOUND: no
     * parked records of that key; UNAVAILABLE: the key is not registered or the player is not restored. */
    KilnResult(KILN_CALL* unpark)(KilnOwner owner, KilnEntity player, KilnStr key);
    /* Append new rows here. */
} KilnItemsStorageApi;

#ifdef __cplusplus
}
#endif
#endif /* KILN_ITEMS_STORAGE_H */

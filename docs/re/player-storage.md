# Player storage: engine and persistence contracts

Since 0.2.0-dev.30.7 (mod API 0.2.16). This is static reverse-engineering evidence, not a game-test report. No game, dedicated server or lab was started for this work.

## Targets and evidence

Both executables are svn 1287369, image base `0x140000000`. Every address below is an RVA.

| Target | Steam build | SHA-256 |
|---|---|---|
| Dedicated server | 26430253 | `f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2` |
| Client, including hosted authority | 26430625 | `5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77` |

Evidence corpus: `/mnt/Dongus/kiln-wave/dev30/out/player-storage/`. `decomp/` contains Ghidra decompiles over read-only executable copies, without whole-image auto-analysis. Supplemental byte scans, full instruction listings, decoded template metadata and test logs are under `/mnt/Dongus/kiln-wave/dev30/out/ps-re/`. A decompiler's guessed return type or omitted forwarded arguments is not a callable prototype; the contracts below also follow callers and instructions. The separate REA import attempt timed out during whole-image analysis and contributed no successful decompile evidence.

## Qualified save and record rows

`sigdb/symbols/server.d/player-storage.json` and its composed client part provide these rows. The build-record generator preserves pre-existing verification levels; these additions enter at **static**, never lab or seen.

| Symbol | Server | Client |
|---|---:|---:|
| `characters/Capture` | `0x72b440` | `0xe19e60` |
| `characters/Read` | `0x723a30` | `0xe0f4f0` |
| `characters/JoinApply` | `0x7a93a0` | `0xe4d850` |
| `characters/Write` | `0x743d00` | `0xe37300` |
| `items/RecordFromStack` | `0x72a9b0` | `0xe193d0` |
| `items/RecordWrite` | `0x741dc0` | `0xe353c0` |
| `items/RecordRead` | `0x71bd50` | `0xe071b0` |
| `items/PideFromRecord` | `0x7abd60` | `0xe50160` |
| `items/CreateEntity` | `0x620ce0` | `0x984e50` |
| `items/TombstoneCheck` | `0x9471c0` | `0xe9ff80` |
| `saves/EcsSnapshot` | `0x62df70` | `0x9920e0` |
| `saves/CapturesDone` | `0x71afd0` | `0xe06430` |
| `saves/PublishCharacters` | `0x73b750` | `0xe2d920` |

All entry patterns match uniquely on **each** image, not merely by assuming that same-svn code was unchanged. `items.storage` requires every declared storage entry, field probe and constant. A missing probe, insufficient votes or disagreeing captures disables the feature.

### Character layout

Each of these constants is captured independently from the character reader and writer, with at least two agreeing votes.

| Constant | Both builds | Meaning |
|---|---:|---|
| `characters/struct.main_pages` | `0x1e38` | main page range |
| `characters/struct.extra_pages` | `0x1e44` | backpack page range |
| `characters/struct.list` | `0x1e50` | item-record vector |
| `characters/struct.equip0` | `0x1a68` | first owned equipment optional |
| `characters/struct.equip1` | `0x1aa4` | second owned equipment optional |
| `characters/struct.equip_last` | `0x1dec` | sixteenth owned equipment optional |
| `characters/struct.refs` | `0x1e28` | four reference optionals |
| `items/record.stride` | `0x38` | unwrapped item record |

The equipment reader/writer are **unrolled**. There is no equipment-stride immediate to capture honestly. Derive `stride = equip1 - equip0`, require it to be positive, and require `equip_last == equip0 + 15*stride`; otherwise disable storage with a capability reason naming the inconsistent row. The derived stride is `0x3c` on both targets. Do not ship a fixed `equip_stride` row.

The vector contains a data pointer at +0, length at +8 and capacity at +0x10. A page range contains u8 pages at +0, u32 first-record index at +4 and u32 record count at +8. References are four 4-byte optionals `{u16 value, u8 present, padding}`. Each owned equipment optional has a Record56 payload and presence byte at payload +0x38.

Record56 is `{count:u16 +0, level:u8 +2/present +3, slot:u8 +4, upgrades:max:u8 +5/applied:u8 +6/present +7, item:u32 +8, durability:u32 current +0xc/max +0x10/present +0x14, liquid:u32 current +0x18/max +0x1c/present +0x20, appearance:u32 +0x24/present +0x28, socket:u32 item +0x2c/u8 level +0x30/present +0x34}`. Padding is not persistent data.

Evidence: `decomp/s-char-reader-writer-723a30-743d00.c:252-340`; complete record writer/reader in `decomp/s-record-context.c:1690-1944`. The reader accepts at most 16 pages per range and 248 records across both ranges. The 16 equipment slots are owned records; ranged weapon, arrow, spell and fishing bait are references into saved Generic pages, not additional records.

## R3: the capture reads live ECS

The save machine retains its original context (`decomp/s-save-context.c:197`), calls the snapshot with the live storage and stores its result separately at :324, then passes the **original** storage to the per-player step at :677. That step constructs an entity reference containing its `param_2`, the live storage, and calls Capture (:100-113). It does not read the returned snapshot object. The independently decompiled hosted path is identical (`decomp/c-save-context.c:197,324,677`, capture at :99).

Freeze storage in the successful `characters/Capture` post-hook, on the authority thread with jobs quiescent, and compute the witness from that same captured character. The save path iterates reserved player entity IDs 1..16; capture entity id minus one is the zero-based save slot. Do not derive the freeze instant from the world snapshot's object lifetime.

### Durability ordering

On both targets the order for one save is:

1. ECS snapshot (`saves/EcsSnapshot`).
2. All per-player captures, inside `saves/CapturesDone`.
3. Worker launch: server `0x6fedf0` signals its Save2 worker.
4. World write and completion.
5. `saves/PublishCharacters` transfers the completed character blob to the replication object; a remote client's eventual character-file write follows its receipt.

Evidence: `decomp/s-save-context.c:324,677,1152-1153,221`; `decomp/s-save-order.c:119-144,336-350`. The publisher requires worker state +0xd8 to indicate completion and the saved result +0xe4 to indicate success before publishing. Client machine and publisher are independently decompiled in `decomp/c-save-context.c`.

A **post-hook of CapturesDone** is after every capture but before the worker launch and either persistence publication path. Use it for a synchronous durability barrier over all frozen storage/witness pairs. A failed durable write must not be treated as successful staging. There is no pre-existing managed callback at this post-capture point: the existing synchronous EvSnapshot is pumped in `native/services/saves/saves.cpp:783-805`, in final Post **before** the game's snapshot and captures. `managed/services/Kiln.Saves/Runtime.cs:422-429` stages the store there, before this save's witness exists.

A leave requests the same save pipeline. The publisher carries the final-request flag from replication object +0x140 bit 1 to outgoing +0x14 (`decomp/s-save-order.c:400-412`). It is not a separate earlier character capture. This establishes publication ordering, not a real crash/power-loss experiment or the client's filesystem flush semantics.

## R1: world-save inclusion is not determined by OwnerRelationship alone

The game's page builder uses template `cddf716d-de7c-4a73-8f5a-3deff2de6782`, whose raw GUID bytes are `6d71dfcd7cde734a8f5a3deff2de6782`. It is **not a zero/blank GUID**. Kiln's resource decoder identifies it as `DefaultInventory`, `family: None`, `components: []` (`out/ps-re/page-template.json:2-10`). Reflection identifies `TemplateFamily.None` as 0.

The saved-entity type is read from the entity's template descriptor +0x20 by server `0x65bb80` (`decomp/s-storage-save-class.c:4-13`). The saved-entity dispatcher `0x72a1c0` has cases 1..13, not family 0 (`decomp/s-tombstone-link.c:94-139`). Normal persistent container/tombstone roots serialize only their explicitly linked Generic pages through `0x72a830`/`0x72a520` (`decomp/s-storage-save-class.c:354-428`; `decomp/s-page-world-filter.c:46-147`). The character capture separately walks the player's Generic/Virtual links.

**Static conclusion:** a holder and pages created with this same family-None template are not standalone persistent world roots. Adding InventorySetup and OwnerRelationship does not change the authored TemplateFamily. Unlinked storage is not reached by a persistent container's page walk either. This is the relevant filter for the player's own extra pages and it covers storage **only if the same DefaultInventory template is used**. A Container or Tombstone template is not equivalent.

This is not a world-save/restart observation; the full snapshot/persistence lifecycle still needs the isolated restart experiment. Do not implement an orphan sweep that deletes arbitrary disconnected inventory owners: ownership does not identify a Kiln holder. Keep an exact tracked set of created holder, page and PIDE IDs for cleanup; any restart cleanup would require an explicit, unambiguous persistent tag.

## R4: direct selected-slot debit has no link-membership requirement

The spawn/consume path passes the selected `InventorySlotId` to `takeItems` (`decomp/s-ammo-consumption.c:2509-2515`, server `0x43dab`). The usage-cost path passes its selected item slot as the preferred slot to `InventoryRemoveItems` (:289-306, server `0x41250`). Preferred-slot removal first reads that exact inventory through `0x1b5fa0`, then debits it through `0x1b1070`; only an unsatisfied remainder falls back to traversing the player's links (:2698-2709,2771-2780).

The direct inventory lookup is by entity/type, without checking that the entity is one of the player's InventorySetup links (`decomp/s-storage-save-class.c:435-500`). The touch path `0x1b6b80` follows OwnerRelationship to mark parent empty-state information, rather than rejecting an external page (:504-590).

**Static conclusion:** consumption can debit an external page when the action's selected slot actually names that page. This does not prove that every client/authority selection-validation path keeps an external SlotSelection reference, that the page is replicated, or that the HUD fires successfully. Keep the bag-cell activation fallback until a real selection/fire test establishes the whole path.

## R5: rebuilding records and structural mutation safety

`PideFromRecord` reads only context +0 (EntityStorage), +0x40 (ItemRegistry) and +0x48 (attribute registry). It does not read a join message, connection or character slot (`decomp/s-record-context.c:6-400`, context reads :177-186,268,316-327,360,396). Its other arguments are the record, inventory owner and ItemInfo. ItemInfo supplies the PIDE template GUID at +0x54c/+0x554. It handles all six optional record fields, including a separately created gem entity.

Thus the call is reusable **outside join**, but it directly calls CreateEntity. It must run with structural mutation safe: at authority Post with no world jobs, not merely inside a program that declares writes to Inventory. `native/core/abi/services/kiln_world.h:25` and the existing spawn service's structural gate explicitly distinguish these cases. For a worker-only restoration, use the queued creation path and preserve every record field; simply giving an item ID/count is not equivalent. PIDE stacks have count 1; stackables preserve their count. Set the final contained slot/owner when inserting; creation alone is not a completed inventory attachment.

## Tombstone to player association

`player_items_loot` runs on the **already created tombstone**. Its 16-byte job row names the current tombstone at +0 and the source player at +4. `CurrentEntityId` reads +0; the source getter server `0x65eec0` reads +4 (`decomp/s-tombstone-link.c:4-13`; `decomp/s-owner-and-ammo.c:4-13`). The program reads Actor, SlotSelection and InventorySetup for that source ID, not for the tombstone (`decomp/s-player_items_loot-1aa7a0.c:100-131`). State 1 calls the transfer helper with `(tx, sourcePlayer, currentTombstone, mode)` (:243-252), commits (:254), then changes state to 2 or 3 (:269-291).

Reflection: CreateItemLoot is one byte, states Start=0, WaitForInventorySetup=1, WaitForPlayerRevive=2, LootCreated=3. The authored tombstone template is `3a0df3ef-817c-46c0-975f-bd6b86be1f5f`, family Tombstone, with CreateItemLoot, InventorySetup, InventorySetupInit and ForceSave (`out/ps-re/grave-template.json`). Do not assume a persistent OwnerRelationship component on the grave; the program's source row is the concrete association.

At Start the game checks whether eligible vanilla contents exist. If not, it queues removal rather than creating a storage-only grave. At WaitForInventorySetup it waits for the destination inventory before transferring. Storage death moves belong **after** that successful transfer/commit and state transition, once, not before the eligibility check. If no usable tombstone exists, keep storage; do not create or retain an extra grave just for storage.

## Witness encoding

This is the shared byte contract for the native implementation and the launcher. Hash with **XXH3-64, default seed 0**, and format the numeric unsigned result as `xxh3:` followed by **16 lowercase hexadecimal digits**. This is numeric hash text, not a little-endian digest-byte string.

Encode, in order:

1. Four ASCII bytes `KPS1` (`4b505331`).
2. u32 little-endian equipment mask, restricted to bits 0..19. Bits 0..15 are the owned records; bits 16..19 are ranged weapon, arrow, spell and fishing bait references. Exclude pre-version-3 legacy bits 20..23.
3. For each of the 16 owned equipment slots in bit order: u8 presence 0/1, followed by a canonical record only when present.
4. For the four references, in ranged/arrow/spell/bait order: u8 presence 0/1, followed by u16 little-endian saved-list index only when present.
5. u8 main pages, u8 main count, u8 backpack pages, u8 backpack count.
6. Canonical records in captured list order: main first, backpack second. Do not sort.

A canonical record is u8 slot, u32 little-endian item ID, u16 little-endian count, then these six options in the game's flag order. **Each option has its own u8 presence 0/1**, followed by its values only when present; there is no packed flags byte in the witness record.

| Option | Present values |
|---|---|
| level | u8 |
| durability | u32 little-endian current, u32 little-endian maximum |
| liquid/fuel | u32 little-endian current, u32 little-endian maximum |
| appearance | u32 little-endian |
| upgrades | u8 maximum, u8 applied |
| socket | u32 little-endian item, u8 level |

Presence is optional **has-value**, not nonzero. A present zero level or zero durability is different from absence. Ignore stale bytes in absent optionals. Exclude all padding, pointers, actionbar/material selection, buffs, character appearance, skills, character level/XP, save version, metadata, revisions and storage contents.

Shared synthetic vectors: `tests/fixtures/player-storage/witness-vectors.json`. The fixture's appearance hex is four **raw little-endian bytes**; socket hex is four raw little-endian item bytes followed by one level byte. They are not integer text requiring a second endian conversion. The vectors cover each option, present zero, a durability-only change, record order, empty inventory and the 16+16-page/248-record limit. Real character files are never committed as fixtures.

## Remaining runtime evidence

- Save/restart an isolated world with only tracked family-None holders/pages and PIDE contents; compare world entities and encoded world contents before and after restart, before storage restoration. This settles world-copy duplication, rather than trusting the static classification alone.
- At the capture barrier, move storage to bag and bag to storage immediately before a save, interrupt after character publication and after world completion separately, and inspect pairing on restart. This settles crash ordering in execution.
- Establish an arrow/spell reference to a page outside the player's links, trigger one real use, and inspect both inventories and selection validity. This settles the full external-ammo path, including client replication.
- Rebuild every optional record field, including a gem PIDE, in a quiescent Post; compare a subsequent game-written record with the input. This settles actual callable ABI and initialization effects.
- Observe one eligible grave and one death with no eligible vanilla contents; confirm the source row, initialized-state transition and exactly-once storage move. No extra tombstone is expected.

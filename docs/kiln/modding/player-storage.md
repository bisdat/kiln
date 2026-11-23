# A persistent bag, quiver, or spellbook

Since 0.2.0-dev.30.7 (mod API 0.2.16), register a player storage with `Kiln.Items`. It stays separate from the backpack,
travels through Kiln's character sidecar, and waits parked when its mod is off. Thanks to **Fuzen777** for
[#128](https://github.com/frostdev-ops/kiln/issues/128).

Add `Items` to the mod's service references. Register once in `OnLoad`, keeping the key stable between versions:

```csharp
private PlayerStorage _quiver = null!;

public override void OnLoad()
{
    _quiver = Items.RegisterPlayerStorage("quiver", 18,
        StorageFilter.Ammunition(AmmoKinds.Arrow).NoItemEntities(),
        StorageDeath.FollowGame);
}
```

Kiln prefixes the key with your mod id: a mod named `example-quiver` gets `example-quiver:quiver`. A key has 1–32
lowercase letters, digits, `.`, `_`, or `-`, starting with a letter or digit. Changing the full key makes the old key an
orphan, not an empty replacement for the same save. A storage has 1–64 logical slots; a mod may register 8 storages,
with a 256-slot page-rounded budget across all registrations per player: each registration counts its slots rounded
up to 8, for at most 32 physical pages. Its logical slot count stays unchanged.

Registration can happen on any thread. It does not mean a player's storage is ready yet: wait for
`PlayerStorageRestoredEvent`, or check readiness on each operation. A registration after a player joined can restore
parked records live. Unloading the mod parks its records again.

## Read and move on the tick thread

```csharp
if (_quiver.IsReady(player))
{
    var contents = _quiver.Read(player);
    var state = _quiver.Status(player);
    Log.Info($"quiver: {state.Stacks} stack(s), {state.Parked} parked, paired={state.Paired}");
    var holder = _quiver.EntityOf(player);
    // holder is an inventory owner accepted by Items.Read/Give/Take/Move/MoveAll.
}
```

In your **server-side, tick-thread** command handler, after validating that the selected slot belongs to the acting
player and the action is allowed:

```csharp
var stored = await _quiver.Store(player, selectedSlot);       // count 0: whole stack
if (!stored.Succeeded) Log.Info(stored.Message);
```

Retrieving a known logical index, likewise from a tick-thread handler:

```csharp
var retrieved = await _quiver.Retrieve(player, index: 0, count: 5);  // backpack
if (!retrieved.Succeeded) Log.Info(retrieved.Message);
```

These are ordinary inventory moves (`items.move`), not a take followed by a give. Gear keeps its item entity on a move.
Tasks finish after the queued simulation operation; re-enter the tick thread before any subsequent inventory call if
your handler's continuation is no longer there. Resolve `PlayerStorage.Slot` again when needed; do not cache page ids
across a leave or rebuild. `ItemsStatus.NoInventory` means storage is not ready; `ItemsStatus.Filtered` means its filter
refused the item. Other refusal/partial outcomes follow [inventory.md](inventory.md).

`Items.Read(player)` and crafting do not see the quiver. Inventory scans omit storage unless explicitly passed
`InventoryKinds.PlayerStorage`; `InventoryKinds.All` deliberately excludes it. Storage never pays crafting costs in
this release. A server mod controls all storage moves; do not trust a client-supplied player or slot id.

## Choose the filter and death policy

Use `StorageFilter.Any`, exact ids with `StorageFilter.Items`, categories with `StorageFilter.Categories`, or
`StorageFilter.Ammunition(AmmoKinds.Arrow | AmmoKinds.Spell)`. Intersect with `StorageFilter.And`;
`StorageFilter.NoItemEntities` restricts to plain stackables. `StorageFilter.Where` is a **one-time catalog query**, not a
per-player callback. Keep it pure. It waits for the catalog if needed and compiles to at most 1024 ids; an empty or
oversized result is refused. Items added later are not automatically included.

`StorageDeath.Keep` keeps everything. `StorageDeath.Grave` takes everything to the game's tombstone when usable.
`StorageDeath.FollowGame` follows the backpack's death setting. No usable tombstone means items stay in storage; Kiln
never creates an extra grave just for it. `PlayerStorageDeathEvent` reports the actual moved stacks and destination.

A filter or slot-count change never deletes saved records: rejects and overflow wait parked. Bad/unknown records are
quarantined. `PlayerStorageParkedEvent` and `PlayerStorageStatus` explain the outcome. Parking does not authorize
silently retargeting a saved ammo selection after rebuilding; the public storage object has no selection setter.
External-cell ammo firing/debit/cycling must be verified separately from the storage of ammunition.

## Orphans, saves, and trust

An admin command can enumerate `Items.PlayerStorages.Of(player)` and explicitly call
`Items.PlayerStorages.ReturnOrphans(player, fullKey, OrphanTarget.Backpack)`, or target a permitted container with
`OrphanTarget.Into`. Return affects parked records, not the ready holder's live items. What does not fit stays parked;
check the result. Enumeration is one ordinary row per key, not the native temporary return inventories.
A `PlayerStorageParkReason.ReturnedOutsideKiln` hold means the server suspects those items were already returned
outside Kiln; an admin should release that copy through the orphan return path only if that suspicion is wrong.

Do not implement a second save file for these item records. Kiln.Saves pairs storage with the character inventory from
the same capture, and selects a matching pair at join before considering newer unmatched copies. When no witness
matches, the newest copy is used and `PlayerStorageStatus.Paired` is false. Warn rather than promising exact restoration
from a very old backup or an edited character. Histories are bounded (8 carried/32 server pairs, also 64 KiB/256 KiB).
Save failures or timeouts can leave an older durable copy; readiness does not prove the latest save reached disk.

Servers accept the carried copy by default, with the same trust as the character save. Server owners can set
`storage.trustClient` to false in `kiln/config/characters.json`; then this server ignores carried storage. Do not market
this as anti-cheat storage. Arbitrary mod InstanceData remains world-only, even though game item record fields travel.

With your mod disabled, records wait under the same full key. With Kiln removed, the world and character copies remain
on disk but vanilla cannot access them. Players can use the launcher's separate offline return tool with the game
closed and a backup; it only returns eligible base-game records within the character format's limits. No automatic
import from Quiver & Spellbook is included. Full rules: [player storage service](../services/player-storage.md).

## Test without the game

`KilnHarness.PlayerStorage` scripts join, restore, apply, capture, captures-done, leave, and death for your tests.
Holders are TestKit fake containers: read/give/take and filtering work. The fake does **not** implement storage moves,
real item entities, the full game record codec, native temporary-return holders/tracker notices, or real character
hooks. Its join/restore calls are test inputs, not proof that the game saved them. See [unit-tests.md](unit-tests.md).

The client mirror API shape is present but unavailable in this implementation: check `PlayerStorage.Client.Available`;
`PlayerStorage.Client.Rows` throws and `PlayerStorageClientChangedEvent` does not fire. Do not assume server storage is
readable from a joined client or appears in the game's backpack UI.

Real-game unverified: holder snapshot/restart exclusion; record rebuild/capture and travel; live return metadata;
death transfers; external-cell firing/debit/cycling; client replication.

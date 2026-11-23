# Player storage

Since 0.2.0-dev.30.7 (mod API 0.2.16), `Kiln.Items` lets a mod register a persistent storage for every player: a
quiver, a spellbook, or an extra bag. Thanks to **Fuzen777** for
[#128](https://github.com/frostdev-ops/kiln/issues/128).

A storage has real inventory pages, but is **not part of the player's backpack**. The game does not draw it, include it
in crafting costs, or write it into the character save. Kiln saves its game-format item records separately, paired with
the character inventory captured in the same save. Removing the mod parks its records; removing Kiln leaves the Kiln
copies on disk. Neither makes those items available in vanilla inventories automatically.

Start with the [modding guide](../modding/player-storage.md). Ordinary inventory operations are documented in
[items.md](items.md) and [inventory.md](../modding/inventory.md).

| Layer | Contract |
|---|---|
| Managed | `managed/services/Kiln.Items/PlayerStorage.cs`, namespace `Kiln` |
| Native | `native/core/abi/services/kiln_items_storage.h`, service `kiln.items.storage` v1, served by kiln-items.dll |
| Persistence | Kiln.Saves; world generation store and the existing character sidecar/channel |
| Client copy | channel `kiln:storage` v1 (`managed/services/Kiln.Items/PlayerStorageMirror.cs`); HUD rows through Kiln.Ui's `Ui.AmmoList` (`PlayerStorageAmmoUi.cs`) |
| Offline tests | `managed/tests/Kiln.Items.Tests`, TestKit's scripted player storage fake |

## Register and use

Call `Items.RegisterPlayerStorage(key, slots, filter, death)` once per key per mod load, preferably in `OnLoad`.
The returned `PlayerStorage` belongs to that mod. `Key` is the local name; `FullKey` is `<mod id>:<key>`. Keep both the
mod id and key stable: saved records belong to that full key, not to the current load's handle or entity ids.

- A key is 1–32 lowercase letters, digits, `.`, `_`, or `-`, starting with a letter or digit.
- A storage has 1–64 slots, backed by 8-slot pages. The final page's padding is not extra public storage capacity.
- Each mod may register at most 8 storages; all registrations together have a 256-slot page-rounded budget per player:
  each counts its slots rounded up to 8, so at most 32 physical pages. Sixteen 9-slot storages fill that budget even
  though they expose only 144 logical slots; a seventeenth is refused. Logical storage capacity does not change.
- A character document has at most 32 distinct storage keys, including keys parked from absent mods.
- A filter's explicit or compiled allow list has at most 1024 distinct item ids.
- Registration is allowed on any thread. A `StorageFilter.Where` needing an unloaded catalog waits for it, then compiles
  on the tick thread. A late registration restores eligible parked records for players already present.
- Unloading a mod ends its registrations and parks the online players' records. Reloading the same full key can restore
  them. Reducing slots or narrowing the filter preserves overflow and rejected records as parked records.

Read `PlayerStorage.IsReady` or `PlayerStorage.Status` before use. `PlayerStorage.EntityOf` gives the inventory owner,
not a page; use it with `Items.Read`, `Items.Give`, `Items.Take`, `Items.Move`, and `Items.MoveAll`. `PlayerStorage.Read`
is the same read, or null when not ready. `PlayerStorage.Slot(player, index)` resolves a logical storage slot to the
current page and cell. Never persist these entities or slot ids across a leave or rebuild.

`PlayerStorage.Store(player, from, count)` moves from the player's inventory into storage. `PlayerStorage.Retrieve`
moves from a logical storage index into the backpack, or another inventory owner supplied as `to`. Count 0 means the
whole stack. These use the existing move service, preserving the item entity during a move, and require `items.move` as
well as ready storage. A not-ready storage returns `ItemsStatus.NoInventory`; a filter refusal returns
`ItemsStatus.Filtered`. Other move outcomes and the delayed task completion follow the ordinary inventory contract.

Reads, slot resolution, moves, enumeration, and returning parked items require the tick thread. Status/readiness and
capability queries are any-thread snapshots. Native server authority performs mutations; a joined client cannot mutate
storage by forging a drag/drop or another explicit storage-entity inventory action. A mod must still validate the
player, requested item, and access policy on every client-originated command.

`Items.Read(player)` does not include storage. `Items.FindInventories()` and `InventoryKinds.All` omit it; explicitly
include `InventoryKinds.PlayerStorage` to discover storage holders. Storage does not pay crafting costs in this release.

## Filters

`StorageFilter.Any` accepts any item. `StorageFilter.Categories`, `StorageFilter.Items`, and `StorageFilter.Ammunition`
restrict categories, exact item ids, or items the game equips into arrow/spell slots. `AmmoKinds.Arrow | AmmoKinds.Spell`
accepts either ammunition kind. `StorageFilter.And` intersects restrictions; `StorageFilter.NoItemEntities` excludes
gear/item entities, leaving plain stackables.

`StorageFilter.Where` runs **once** over the loaded item catalog to compile a sorted allow list. It is not a callback
on every transfer: the native inventory transaction never calls mod code. Use a pure predicate of the catalog item,
not player state, time, or inventory contents. Items added later are not added to that list automatically. An empty
compiled result, contradictory restrictions, or a list larger than 1024 is rejected. A deferred compilation failure is
logged and the registration stays unusable; it does not create an unfiltered storage.

The native check applies to every add into storage, including other mods' gives and moves. At restore, an item that no
longer passes the filter is parked, not discarded. Invalid/unknown records are quarantined separately.

## Status, events, and parked items

`PlayerStorageStatus.State` is `None`, `Waiting`, `Ready`, `Parked`, or `Locked`. `Restored` is true only for `Ready`.
`Paired`, `Source`, and `Revision` describe the copy selected at join. `Stacks` counts current stacks; `Parked` and
`Quarantined` count records that remain preserved outside the live holder. `PlayerStorageSource` distinguishes a new
empty storage, the server copy, and the carried client copy.

`Items.PlayerStorages.Of(player)` returns each ordinary storage key once, sorted by full key: registrations in any
state and preserved keys from absent mods. `PlayerStorageInfo` includes whether it is registered, its registered slot
count (0 if unregistered), its holder when ready, and its status. Unknown players return an empty list. Internal
parked-return holders are not public storages and do not appear as extra keys or holders in this view.

Tick-thread events are:

- `PlayerStorageRestoredEvent`: a key became ready at join or after a live registration; includes source, pairing,
  revision, and whether the restoration was live.
- `PlayerStorageParkedEvent`: records were parked, with `PlayerStorageParkReason`.
- `PlayerStorageDeathEvent`: the death rule's outcome, including stacks/items moved and the tombstone, or zero/none when
  kept.

Unregistered, overflow, and filter parking may restore automatically when the key becomes usable. Requested, stale,
and locked parking is held back; registration alone does not release it. `PlayerStorageParkReason.ReturnedOutsideKiln`
means the server held its copy because character items were likely returned outside Kiln; an admin should release it
only if that is wrong, through `Items.PlayerStorages.ReturnOrphans`. Records from a newer document version remain
read-only. Unknown items, invalid counts/slots, and failed decodes/rebuilds remain quarantined, with their bytes.

`Items.PlayerStorages.ReturnOrphans(player, fullKey, OrphanTarget.Backpack)` returns parked records into the backpack;
`OrphanTarget.Into(container)` targets another inventory owner. It never silently consumes the live storage's contents.
It rebuilds the records and moves what fits, leaving the remainder parked. The result is a `MoveResult`: `Done`,
`Partial`, `NoRoom`, or an explained failure such as `NotEnough` when nothing is parked. Return is an explicit server
operation, not a permission check: an admin command or mod UI must authorize it before calling.

## Saving, travel, removal, and trust

Kiln.Saves stores records in the world's `player-storage/<character key>` generation scope. The carried copy is
`mods["kiln-items"]["storage"]` of the existing character sidecar, sent through `kiln:character`; mods do not write either
copy directly. The item record codec preserves the same item fields as the game's backpack codec, including present
level, durability, liquid, appearance, upgrades, and gem options. Arbitrary mod `InstanceData` is **world-only**: do not
promise that metadata travels to a different world with the item record.

Each copy contains a witness of the character's captured inventory. At join, a matching witness wins over any
nonmatching copy; among matches the highest revision wins. If nothing matches, the newest revision wins and
`PlayerStorageStatus.Paired` is false. This fallback cannot guarantee no duplication or loss after an old character
backup outside retained history, or an offline edit combined with later storage moves. Surface the unpaired state
rather than treating it as proof of an exact save match.

If the join witness signals more main Generic pages than the game itself creates for a character (native defines that
baseline), an unpaired arrival without a client copy resolving the uncertainty parks the server copy: a likely return
outside Kiln is a conservative hold, never a delete.

History retains at most 8 carried pairs and 32 server pairs, also bounded by encoded UTF-8 size: 64 KiB carried and
256 KiB server. Equal contents can share witness aliases within those bounds. Byte trimming protects current contents,
parked records, quarantine, and the newest history entry. Irreducible excess preserves the previous durable copy and
reports degraded persistence rather than deleting items to fit.

There is one parked bag per key, holding at most 64 records in this document version. If a key has both live storage
and a held bag when it is unregistered or shrunk, the held records and stricter reason win. Live records are renumbered
into free parked slots; records beyond 64 are preserved byte-for-byte in quarantine with reason `parked storage full`,
counted as quarantined and logged with the player and key. The native reason is `KIS_QUARANTINE_PARKED_FULL = 6`.
These bytes are kept, not deleted, but `/playerstorage release` cannot release quarantine in 30.7. A future multi-bag
format is needed to make that excess directly releasable.

Server configuration is `kiln/config/characters.json`:

```json
{ "storage": { "trustClient": true } }
```

`storage.trustClient` defaults to true, trusting the carried copy as the game trusts the character save. Setting it to
false ignores client storage candidates on that server; storage does not travel there from the client copy. This is
independent of `importProgress`. Unreadable configuration disables both imports. A paired server copy still wins over a
newer unpaired carried copy when client copies are allowed.

A missing persistence owner or required native hook means holders are not built. A failed durable stage or bounded save
barrier timeout does **not** stop the game's own save indefinitely: persistence reports a fault and retries. A crash
before recovery may leave only an older/unpaired fallback. Keep backups; readiness alone is not proof the latest pair
is on disk. Future document versions are locked and preserved. Malformed sidecar files are preserved as
`.corrupt-<stamp>` before replacement; bad individual records are isolated.

With the mod off, its records remain parked under their original full key. With Kiln removed, the copies remain in
`kiln/data/characters` and `kiln/data/worlds`, but vanilla cannot use them. The launcher's offline return tool is a
separate, explicit operation with the game closed and a backup: eligible base-game records go into new main pages after
the character's existing ones (never into empty cells, so the page count always rises), within the game's page/record
limits. Unsupported/custom records and quarantine remain stored. It refuses a character that pairs with no storage copy
("this character changed since Kiln last saw its storage") and changes nothing. Its sidecar is published first with the
reduced new pair, the actual old character's full historical pair and a `returns` marker (`revision`, `fromWitness`,
`toWitness`; newest first, at most 8, never trimmed), then the character is written atomically, so interruption before
that write can still select the old pair. Servers read the marker to hold their own copy instead of handing returned
items back. Only the storage value and the sidecar's revision change: other mods' data keeps its exact bytes. Returning
is not an automatic migration of another mod's data; no Quiver & Spellbook import is included.

## Death and references

`StorageDeath.Keep` is the default. `StorageDeath.Grave` moves all stacks to the game's usable tombstone when one exists.
`StorageDeath.FollowGame` follows the game's rule: everything, only materials/collectibles, or nothing. Kiln does not
spawn an extra tombstone for storage; without a usable grave it keeps the items. Reviving without death moves nothing.

Native reference selections cover ranged weapon, arrow, spell, and bait cells. The public storage object does not
expose a `SetRef` method in this API. Parked records are preserved, but restoring a registration must not silently
retarget a saved selection to a different cell: selection needs explicit re-establishment through `Items.Equip` with a
storage slot, `PlayerStorage.Activate`, or `PlayerStorage.Client.ProvideAmmo` (below). Automatic external-cell firing,
debit, and cycling behavior still need real-game verification.

`Items.Equip(player, storage.Slot(player, i), EquipmentSlot.Arrow)` equips from the storage cell itself: native
kiln.items accepts a storage slot only of the acting player's own storage, and checks the storage filter against what
the equip puts back. `PlayerStorage.Activate(player, index)` is the fallback for a game that does not fire from a cell
outside the backpack: it moves the arrow or spell stack into the backpack (`Items.Move`), equips it there, and moves the
stack it activated before for that slot back into the storage (one step each; tick thread; `items.move` and
`items.equip`). One runs at a time per player and slot (another is `Refused` while it does). When the equip fails after
the move, what the move put into the backpack goes back into the storage, no more than moved (a stack it topped up keeps
the player's own items). A stack that cannot go back (storage full, filter) stays in the backpack, logged. The result is
the equip's; `NoInventory` (not ready), `SlotChanged` (empty slot), `Refused` (not an arrow or spell, or one under way) or
a failed move's status otherwise. Kiln remembers the activated cell per player and slot for the player's session only (it
forgets it when the player leaves or the world stops), and checks it still holds that item before moving it back. Each step runs on the tick thread as the storage's mod, also when the
call came from Kiln itself (a HUD pick): a mod that unloads meanwhile drops the rest.

## Capability and client boundaries

Check `PlayerStorage.Available` or `Capabilities.Why(PlayerStorage.Capability)`. Required missing hooks/layout facts,
wrong side, missing service, or a fault refuse storage instead of guessing engine state. See [caps.md](caps.md).
The native `items.storage.persist` rows are exclusively for Kiln.Saves, not mod persistence APIs.

Storage needs its client inventory guard: a link in Kiln's hook registry after `gameplay/DeliverEvents` that refuses
every client action of `PlayerInput.fromClient` naming a storage holder, page or item (found through the game's
reflection; the nine inventory-addressed actions must all be there). Without it `items.storage` is unavailable
(`symbol-missing`) and the text says which check failed: the sigdb feature `players.client_action_hold`, the symbol, the
live code, the reflected action or field that did not resolve, or the hook registry's result. A `DeliverEvents` that
another Kiln component already linked is fine: the guard joins that chain. One that another module hooked or patched
outside Kiln's registry (before Kiln started, or under Kiln's own detour) is refused: the text names the module its jump
leads to (through a hook's relay), says to remove that mod or loader and restart, and ends with the bytes Kiln saw.
shroudlink's own core (`steam_api64.dll` on a server, `shroudlink_core.dll` next to a client) hooks `DeliverEvents`
itself; Kiln runs shroudlink mods without it ([shroudlink](../engines/shroudlink.md)).

Server storage availability does not imply a joined client can read it: the client copy below is a separate row.

## The client copy and the HUD's ammunition lists

The owner's game holds a read-only copy of that player's storages: `PlayerStorage.Client.Rows(fullKey)` (one
`PlayerStorageRow` per slot that holds something: key, index, item, count, level, and flags: the item's `AmmoKinds` by
the server's item catalog, 1 arrows, 2 a spell, 0 anything else; a game that joined another's world has no catalog of its
own, `Items.Get` answers null there) and `PlayerStorageClientChangedEvent` (one per storage whose rows changed, also when
it went or the game left the world; tick thread: the main thread of a joined game, the server thread of a hosting one). Capability `items.storage.client`:
available where Kiln's channel is open (both sides), unavailable (`service-missing`) without kiln.net. It does not
depend on `items.storage`: a game that joined another's world has no storage of its own but gets its copy.

Channel `kiln:storage` v1, opened by Kiln on both sides, not required (a game without it, or without Kiln, joins as
before and is sent nothing):

| Message | Direction | Fields |
|---|---|---|
| 1 Update | server → client | view (random per player session, nonzero), seq, then per storage: full key, slots, rows (slot, item, count, level, flags: the item's `AmmoKinds` by the server's catalog) |
| 2 Ack | client → server | view, seq of the update the game applied |
| 3 Pick | client → server | full key, kind (1 arrow, 2 spell), item |

- **Owner only, whole set, on change.** About four times a second the server builds each player's rows from their
  ready storages (any mod's, `Items.PlayerStorages.Of` + `Items.Read`) and sends that player the whole set when it
  differs from the last one sent. A game without the channel is skipped before anything is read. A hosting game's own
  player gets the rows straight into its copy; when its world stops (`ServerStoppingEvent`) that copy empties, and the
  views, the picks its HUD queued and the pick rates go with the world.
- **Delivery.** An update not acknowledged after 5 s is sent again, each wait doubling up to 60 s. The game drops an
  update of its view whose seq is not newer than the one it applied, and starts over when the view changes.
- **Limits, checked on the game before anything is applied** (a broken update is dropped whole): at most 32 storages,
  1-64 slots each, 256 rows in all, keys of the `<mod id>:<name>` form (128 bytes at most), rows in slot order inside
  the slots, no item 0, no count 0, nothing after the message but kiln.net's padding. An update is at most 8,185 bytes.
- **Server-authoritative.** No message from a game changes a storage. An Ack only stops resends. A Pick is a wish: it
  is read only for a storage whose own mod called `ProvideAmmo` on the server, at most 4 a second per player (8 at
  once), and only when the item is in the sender's own storage and is ammunition of the picked kind; anything else is
  dropped and logged (at most once a minute per reason, whatever item or key the game sent). A repeat of a pick that is
  under way is ignored; a pick's outcome is logged on the tick thread.

`PlayerStorage.Client.ProvideAmmo(fullKey, kinds, options)` returns a `PlayerStorageAmmoProvider` and does both halves
of a quiver or spellbook, made in `OnLoad` on both sides:

- **Game client:** the storage's items the server marked as arrows or spells (its catalog's equipment slot type 6 or 7)
  become rows of `Ui.AmmoList` (`ui.ammo.list`), one per item with the counts of its slots added up, in slot order,
  Append unless `PlayerStorageAmmoOptions.Replace`. They are asked for again when the copy changes. A pick (`ui.ammo.pick`) of an
  item of that storage goes to the server; a hosting game queues it for its own server tick. Without the HUD lists only
  the server half runs; `Shown` and `WhyNotShown` say so.
- **Server:** after the checks above, the first slot holding the item is equipped with `Items.Equip` as the storage's
  mod, or swapped in with `PlayerStorage.Activate` when `PlayerStorageAmmoOptions.Activate` is set. The outcome is
  logged (`player-storage: <key> pick by <player>: ...`).
- A mod provides only its own storages (`<its id>:...`), one provider per storage at a time. Disposing ends both
  halves; unloading the mod does too.

## Internal list contract and offline evidence

The raw ABI list can contain an **extra row with the original key** while returning parked records: `status=PARKED`,
`handle=0`, a nonzero temporary holder, 64 slots/8 pages. It can coexist with the normal `READY` or permanent `PARKED`
row; permanent parked rows have holder 0. Only one temporary holder exists per player. It remains discoverable through
the transaction and tracker notices until the next safe cleanup stage. Kiln.Saves uses the duplicate raw rows for
world-only InstanceData tracking, with separate live and temporary slot bands. The managed public enumeration filters
only the temporary row; it does not change the ABI layout or hide it from internal persistence code.

The TestKit fake covers registration, filters, ordinary holders, scripted restores/captures, parking, returns, and
death outcomes. It delivers returns directly and does not model temporary native return holders, tracker notices,
real item entities, the full record codec, actual character hooks, or the client mirror. Raw-table regression tests
cover the public enumeration boundary; Saves' separate fake tests cover live/temporary metadata bands. These are not
proof of the engine's transaction or restart behavior.

The client copy's tests (`PlayerStorageMirrorTests`) put a loopback in place of kiln.net: the real encoder, decoder,
copy, server passes and pick checks, and the HUD half on the kit's fake `kiln.ui.ammo`. The kit itself has no network.

Real-game unverified: holder exclusion from world snapshots/restarts; exact capture/rebuild and travel pairing; live
parked-return metadata tracking/cleanup; death transfers; external-cell ammunition debit and cycling; the client copy
over a real connection; HUD rows from storage and their picks; `Activate` in a running game.

## Changelog

- **0.2.0-dev.30.8:** storage no longer switches itself off at start ("client inventory guard is unavailable"): the
  guard's reading of `PlayerInput.fromClient` stopped at its first member that is not an action (an enum), on dedicated
  servers and hosted worlds alike. The capability text now names the check that failed, and for a game function another
  mod hooked first, which module and what to do. Thanks Fuzen777 (kiln#135).

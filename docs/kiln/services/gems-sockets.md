# Sockets on other gear (rings, armour, shields)

Part of the Gems guide (`docs/kiln/services/gems.md`; this page is its "Sockets on other gear" section until the two are
merged). Capability row `gems.sockets`.

In the game only weapons get a gem socket, and only weapons show up in the Shroud Forge. Everything else about gems does
not care what holds them: the game runs the gem of the item in **any** of a player's 11 equipment impact slots (held item,
shield, the five armour pieces, both rings, arrows, spell), and the save keeps a socket on any item. Kiln lifts the three
places that stop other gear:

| What stops a ring | Where | What Kiln does |
|---|---|---|
| a new item only gets a socket when it is a weapon | the game's createPide (server) | kiln.items' createPide hook gives other gear one, when a mod allows it |
| an existing item cannot get a component added | the game | `Gems.AddSocket` re-creates the item with a socket |
| the forge lists weapons only | the client's Gem Forge lists | the lists also show gear that has a socket, on Kiln clients |
| the vanilla forge cannot take a gem out of a ring | the client | `Gems.ReturnSocketedGems` (the uninstall path) |

**Vanilla stays vanilla.** Nothing changes until a mod calls `Gems.AllowSockets`, or a data pack sets `gemSlotOverride` on
one of its own items. No game item outside the weapons sets `gemSlotOverride` (checked on game build 26430253 / client
26430625 over every ItemInfo: 40 items set it, all weapons).

## For mods (C#)

```csharp
// on both sides (a mod with "side": "both"): server = sockets and rolls, client = the forge lists
var setup = Gems.AllowSockets(GemSocketKinds.Rings | GemSocketKinds.Armour, rollChance: 0.05);
if (!setup.Server && Mods.Side != ModSide.Client) Log.Warn(setup.ServerReason!);

// a new ring with an empty socket
await Items.Give(player, ringId, 1, new Gear(20, ItemRarity.Epic) { GemSlot = true });

// an existing ring without one (from a backpack, an action bar or a chest; not worn)
var r = await Gems.AddSocket(ringRef);   // r.After is the new item; r.Before is gone

// before a server stops using sockets on other gear: every gem back into the owner's inventory
var back = await Gems.ReturnSocketedGems(player);
```

- `Gems.AllowSockets(kinds, rollChance = 0)` lets gear of these kinds hold a gem while the calling mod is loaded (a later
  call replaces this mod's; `GemSocketKinds.None` ends it; mods add up). Call it in `OnLoad` or on the tick thread: that
  is where Kiln knows the calling mod (from another thread the allowance would be Kiln's own and outlive the mod).
  Kinds: `Rings`, `Armour` (head, chest, hands, legs, feet), `Shields`, `OtherGear` (tools, backpacks, gliders,
  instruments: a gem there runs only while the item sits in an impact slot, a tool in the hand), `All`.
  - Server: `Gear.GemSlot` and `AddSocket` work on these kinds. With `rollChance` above 0 every new item of these kinds the
    game creates (loot, crafting, a give) rolls an empty socket with that chance; the highest chance any mod asks for
    wins. Weapons keep the game's own roll (a chance per rarity from its balancing data).
  - Client: the Gem Forge's WEAPONS list and its socketed rows also show gear of these kinds **that has a socket** (see
    "The forge").
  - Returns `GemSocketsSetup { Kinds, Server, ServerReason, ForgeLists, ForgeReason }`: what works on this side and why
    not. On a dedicated server `ForgeLists` is false (there is no forge screen); on a game client in its menu or joined
    to another server `Server` is false.
- `Gems.SocketKinds`: the kinds any mod allows now.
- `Gear.GemSlot` (Kiln.Items): a weapon gets an empty socket when the game's roll gave none; other gear when its kind is
  allowed and the server part works here (`GemSocketsSetup.Server`); otherwise the item is given without one, as
  before. Nothing happens to an item that already has one.
- `Gems.AddSocket(item)` (server, tick thread): the game cannot add a component to an existing item entity, so Kiln
  re-creates the item **in the same slot, in one inventory transaction** (the old item entity is taken and the new one
  created: both or neither). Kept: the item, its level, rarity, unlocked upgrades, durability (as a fraction of its max:
  a broken item stays broken) and dye. Not kept: the item entity (follow `GemSocketResult.After`; nothing keyed to the
  old one moves: Kiln.Saves' instance data on it counts as destroyed, so a mod that keeps instance data on gear reads it
  before and sets it on `After`), water in a can. Weapons are always accepted; other gear needs its kind allowed.
  Refusals: `AlreadyHasSocket`, `KindNotAllowed` (with the call that allows it), `Equipped` (worn or held in the hand:
  unequip it first), `NotFound`, `NotGear` (gems, dyes and customization items never hold a gem), `Refused` (the game
  said no, or the item changed before the step it ran: moved, put on, taken in hand; kiln.items checks worn, held and
  socketed again on that step), `Unavailable`.
- `Gems.ReturnSocketedGems(owner)` (server, tick thread): the uninstall path below.

## For data packs

An item of your own (or one you change) gets a socket whenever the game creates it when its ItemInfo sets
`gemSlotOverride` (the item field kiln.content and kiln.gamedata name `gemSlotOverride`, a bool; the game's struct
`GemSlotOverride { bool isSet }`), on any kind of gear, without code:

```json
{ "format": 1,
  "items": { "add": [ { "guid": "6b1f0c2e-5a7d-4e19-9c3b-2f8e1d4a7c60", "name": "MyMod_Ring_Of_Sparks", "base": "Ring_T3_Ring_of_Light",
                        "fields": { "gemSlotOverride": true },
                        "texts": { "en": { "name": "Ring of Sparks" } } } ] } }
```

This is the game's own flag: on weapons the game honours it, on other gear kiln.items' createPide hook does
(`gems.sockets`). It does not open the client's forge lists: the mod (or another one) still calls
`Gems.AllowSockets(GemSocketKinds.Rings)` on clients for players to use the forge on it.

## The forge (game clients with Kiln)

The forge's two lists come from the local player's backpack pages and action bars only, and only weapons pass their
category filter. On a Kiln client, `Gems.AllowSockets` (called on the client by a mod with side `both`) adds the
categories of the allowed kinds to both filters (kiln.ui.values `gemforge_sockets`, two mid hooks in the forge's list
builders, `ui/GemForgeWeaponsMask` client `0xcafc89` and `ui/GemForgeCarrierMask` `0xd27d80`):

- WEAPONS lists gear of those kinds that **has a socket** (the game's own socket-flag filter still applies, so a ring
  without a socket never shows up);
- GEMS shows a ring that holds a gem as a carrier row, like a socketed weapon (upgrade, recycle).

So players socket, unsocket, upgrade and recycle a ring's gem at the forge with the game's own costs; the server's
socket action never checks what kind of item the carrier is. **Worn items are not listed** (the lists read only the
backpack and action bars, as for weapons): take the ring off first. The hooks go in at the first mask that adds a
category and do nothing while no mod widens, so a player without such a mod has the game's code untouched. Clients
without Kiln (or without the mod) see vanilla lists: their players cannot use the forge on other gear, but the gems
still work for them (the effect runs on the server). The Gem Forge snapshot (`ui.gemforge.inspect`) accepts the widened
carrier rows.

## Tooltips

The item tooltip and the item details draw the gem row (the gem, or an empty socket) for any item whose socket flag is
set, without a category test (client: builder `0xb1ec72..0xb1ed61` copies the flag and the socket's gem view, renderer
`0xc13b5f..0xc13c01` draws it; the delete confirmation `0xb8ce00` also warns about a socketed gem). So a ring with a
socket should show its gem like a weapon does, on every client; no Kiln change is needed. Static evidence only: the
real-client check is item 8 below.

## Uninstall: getting the gems back

A socket on a ring stays in the save and keeps working in a game without Kiln (the game's own code runs it). But the
vanilla forge only lists weapons, so without Kiln **nobody can take that gem out again**. Before a server drops a mod that
puts gems into other gear, run `Gems.ReturnSocketedGems(player)` for each player (or let the mod do it from a command):
every gem in a non-weapon socket of that player's inventories (worn items included) goes into the player's inventory,
free of charge; the result lists each gem with `Returned` and a reason when it did not fit. It runs the forge actions'
unsocket (`Gems.Unsocket`, capability `gems.craft`), so it needs a Kiln with the forge actions; without them every entry
says so. Items in chests: move them into the player's inventory first.

## Balance

A gem program reacts to every hit of its wearer, not only to hits of the item that holds it. A ring's Gem of Bleeding
works with any weapon, and with sockets on everything a player can run up to 11 gems at once (held item, shield, five
armour pieces, two rings, arrows, spell); two copies of one gem trigger twice. That is the mod's design decision, which
is why nothing happens until a mod asks.

## C (kiln.items v7)

`native/core/abi/services/kiln_items.h` "gem sockets": `KILN_ITEMS_GEM_SLOT_ANY` (with `KILN_ITEMS_GEM_SLOT`: any gear),
`KILN_ITEMS_REPLACE` (a give that first takes the item entity `pide` out of `slot` on the same transaction; refused,
nothing changed, when that item is worn or held, has a gem slot, or has no readable ItemState),
`KILN_ITEMS_DYE` / `KILN_ITEMS_DURABILITY` with `KilnItemsRequestEx` (the re-created item's dye, and its durability as
`DurabilityPercentageInit`, which the game's `item_durability_init` reads), and `socket_roll(owner, KilnItemsSocketRoll*)`
(chances per kind: rings, armour, shields, other; `NULL` ends the owner's; `KILN_ERR_UNAVAILABLE` is checked before the
arguments, so an invalid roll is a probe that changes nothing). The forge lists: kiln.ui.values'
`gemforge_sockets(owner, category_mask)` (`kiln_ui_values.h`).

## How it works (evidence)

- createPide (server 26430253 `0x1b1920`) appends `WeaponGemSlot` (12 zero bytes, an empty `ItemStack`) only when
  `ItemInfo.category == 2` and the BalancingRegistry roll wins or `ItemInfo.gemSlotOverride.isSet` (`0x1b1cf5..0x1b1e14`;
  the flag's offset `0x798` is captured by `@items/CreatePide.gemOverride`, sigdb/symbols/server.d/gems-sockets.json).
  kiln.items' existing mid hook on its spawn call (`items/PideSpawn`) appends the same entry for other gear: gives with
  `GEM_SLOT_ANY`, `gemSlotOverride` on a non-weapon, and the rolls. The kind comes from `ItemInfo.category` and
  `ItemInfo.equipment.slot` (`EquipmentSlotType`: 13-17 armour, 18 shield, 37 ring).
- The override list has 8 inline entries; a re-created item adds the socket first, then `DurabilityPercentageInit`, then
  `ColorPaletteItem`, each only while there is room. On server 26430253 createPide's own entries for a re-creation are at
  most five (`UsedItem`, `ItemState`, `OwnerRelationship`, `Level`, `PerkContainerNew`; a give passes no fuel; a weapon
  whose own roll gave a socket has a sixth and gets no second one), so all three fit. An entry the game already has
  is pointed at Kiln's value, never written through. The game's save loader (`0x7abe80..0x7abebd`) builds
  `DurabilityPercentageInit` the same way, as current / max (0..1).
- equipment_impacts (`0x147330`) runs the gem of the item in each of its 11 impact slots (`0x146430`), category-blind;
  the save writer (`0x70894f`) and loader (`0x7ac159`) keep `WeaponGemSlot`, the gem id and the gem's level on any item;
  the server's socket action (`0x144bb0`) checks no carrier category (dossier `dev29/out/gem-system/DOSSIER.md` §6).

## Tests

- Native host tests: `native/services/items/tests/run.sh` (pide_test: kinds, GEM_SLOT_ANY, gemSlotOverride, rolls,
  extras, the full list; move_test `replace`: the re-creating give, its refusals and rollback).
- Managed: `Kiln.Items.Tests/ItemSocketsTests.cs`, `Kiln.Contract.Tests/GemSocketsTests.cs`.
- Lab: `mods/tests/gems-sockets-test` (a spawned 1_Player stand-in; run the lab twice for the save check, on a world
  that has a container in its save: a spawned loot chest is not saved). Lab 2026-11-16 (server 26430253, Wine lab
  `gems-g4`, a copy of the dev26-playtest save): first start 19 PASS 0 FAIL (Gear.GemSlot on a ring, AddSocket keeps
  slot, level, rarity and upgrades and replaces the item entity, the refusals, a roll of 1.0 on shields and its end,
  `gemSlotOverride` set through kiln.content on a chest piece, a Gem of Bleeding written into a ring's socket runs from
  **equipment impact slot 7** with an `ImpactStartedEvent` of its program when the ring is worn); second start after a
  save and restart 3 PASS 0 FAIL (the ring in the container still holds the gem; the gem's entity has its level 7).
  Not shown in the lab: durability carried over (the lab's reads of `Durability` on the items came back empty; the
  `DurabilityPercentageInit` entry is host-tested only).
- What only a real client shows: the checklist in "Client checks" below.

## Client checks

For work-00 with the owner (a Kiln client built from this tree, a server-and-client mod calling
`Gems.AllowSockets(GemSocketKinds.Rings | GemSocketKinds.Armour)`, a ring with a socket and one without):

1. kiln.log at boot: `ui.values: features resolved: ... gems.sockets ...`; `gems.sockets` available in the capabilities.
2. Without any mod widening: the Shroud Forge's WEAPONS and GEMS lists look exactly as vanilla; no
   `gems.sockets hooks installed` line in kiln.log.
3. With the mod: kiln.log `gems.sockets hooks installed (Gem Forge masks at +0x6c and +0x4c)` and
   `Gem Forge lists widened to category mask 0x6`.
4. WEAPONS: the socketed ring in the backpack or on the action bar is listed; the ring without a socket is not; a worn
   socketed ring is not listed until it is taken off.
5. Select the ring in WEAPONS, pick a gem, Insert: the essence is paid, the gem leaves the backpack, the ring holds it
   (server: `Items.Read`, `Gems.Socket(carrier)`).
6. Put the ring on: the gem works (for Bleeding: crits make enemies bleed with any weapon); the server's
   `Gems.Equipped(player)` lists it in slot Ring1/Ring2 (impact slot 7 or 8).
7. GEMS after the insert: the ring appears as a carrier row with the gem's icon and level; `Ui.Values.GemForgeCandidates()`
   stays Ok with that row's carrier = the ring.
8. Tooltip: hover the socketed ring (with a gem, then with an empty socket) in the backpack and while worn: the socket
   row shows as on a weapon. The delete confirmation warns about the gem.
9. Upgrade and Recycle the ring's gem from its GEMS row, and Unslot it: each works and costs as for a weapon.
10. Quit to the menu and join again: the ring still holds the gem (server save).
11. Unload the mod (or `Gems.AllowSockets(GemSocketKinds.None)` on the client): the next forge open shows vanilla lists.
12. A client without Kiln on the same server: the ring's gem still works when worn; its forge lists stay vanilla; its
    tooltip shows the socket.

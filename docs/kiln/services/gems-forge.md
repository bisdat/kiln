# Gems: forge actions

Part of the Gems guide ([gems.md](gems.md), section "Forge actions"): socket, unsocket, upgrade and recycle a gem from
code, and the events of the players' own forge actions (`Kiln.Abilities`, `namespace Kiln`).

Capability: **`gems.craft`** (dedicated server, and a game client while it hosts). Since 0.2.0-dev.29.11.

## What it does
A Shroud Forge has four actions. Kiln runs the game's own action for each, the one a player's click at the forge runs,
with all the game's checks and its costs:

| Call | The game's action | Cost (game data) |
|---|---|---|
| `Gems.Socket(player, carrier, gem, forge)` | Insert: the gem goes into the carrier's empty socket | essence, by the forge's tier |
| `Gems.Unsocket(player, carrier, forge)` | Remove: the gem goes back to the player's backpack | free in this game build |
| `Gems.Upgrade(player, gemOrCarrier, forge)` | Upgrade: the gem's level + 1 | essence, by the gem's level and the forge's tier; refused at the gem's own cap or the forge's maximum level |
| `Gems.Recycle(player, gemOrCarrier, forge)` | Recycle: the gem is destroyed | the player gets the forge's essence, by the gem's level |

- **player**: the player character the action runs for. It pays, and gets an unsocketed gem or the essence.
- **carrier**: the item with the socket: a weapon, or a ring, armour piece, shield or other gear a mod gave a socket
  ([gems-sockets.md](gems-sockets.md)).
- **gem / gemOrCarrier**: a loose gem, or (Upgrade, Recycle) the carrier whose socket holds it.
- **forge**: a placed Shroud Forge. The game prices every action with a forge: its tier sets the cost, the maximum level
  and the essence. Kiln needs one even for a free action. You get a forge's entity from `GemCraftingEvent.Forge`, or from
  the world (`Spawn.FindByName("Shroud_Gem_Forge")`).

Each call returns a `Task<GemCraftResult>` that completes on the tick thread one step after the step it ran in. A refusal is
a result, never an exception: `Ok` is false, `Reason` says why, and nothing changed.

```csharp
var sword = Items.Read(player)!.Slots.First(s => s.Item == swordId);
var gem = Items.Read(player)!.Slots.First(s => s.Item == bleedingId);
GemCraftResult r = await Gems.Socket(player, sword.Ref, gem.Ref, forge);
if (!r.Ok) Log.Info($"not socketed: {r.Reason}");
else Log.Info($"socketed, paid {r.Paid}");

var up = await Gems.Upgrade(player, sword.Ref, forge);          // the gem in the sword's socket
Log.Info($"level {up.LevelBefore} -> {up.LevelAfter}, paid {up.Paid}");
```

### Where the items may be
The carrier and the gem must be in the player's own inventories (backpack, action bars, worn gear), or in an entity you
name with `GemCraftOptions.Holder` (for example a chest your mod lets the player use). Kiln checks before the action that
they are there, that each ref still names the item it was taken from (an `ItemInstanceRef` whose item was destroyed, or
whose entity id now names another item, is `NotFound`), that the socket is there and empty (Socket) or holds something
(Unsocket), and that the item to socket, upgrade or recycle is a gem. The game itself checks none of that (its action
would recycle a sword, or raise a sword's level, if asked), so a mod should not skip it either: only name a holder the
player may use.

### Free and changed costs
`GemCraftOptions.Cost` changes what the player pays, for creative tools, quest rewards or a mod's own prices:

```csharp
await Gems.Upgrade(player, gemRef, forge, new GemCraftOptions { Cost = GemCost.Free });            // nothing paid
await Gems.Upgrade(player, gemRef, forge, new GemCraftOptions { Cost = new GemCost(goldId, 50) }); // 50 gold instead
await Gems.Recycle(player, gemRef, forge, new GemCraftOptions { Cost = new GemCost(runeId, 1) });  // a rune instead of essence
```
For Recycle the option replaces what the player gets (`GemCost.Free`: nothing). Everything else about the action stays
the game's: the level cap, the forge's maximum level, the socket checks. How it works: Kiln gives the player the game's
price right before the game's action (the game takes it back) and takes your price; afterwards Kiln counts the material
and takes back whatever of its gift the game did not take, and if the action did not happen it gives your price back.
The player needs a free slot for that moment when the game's price is not 0 (otherwise `NoRoom`). A replaced price must
be a plain item (essence, gold, runes): an item with an entity of its own (gear, a gem) throws `ArgumentException`, since
Kiln takes it by item id from anywhere in the inventory and it could be the very carrier or gem of the action. For
Recycle the replacement is given, so any item works there; when the game's essence cannot be swapped (the replacement
does not fit), the player keeps the essence and `Message` says so.

### Results
`GemCraftResult`: `Status`, `Action`, `Player`, `Forge`, `Carrier`, `Gem`, `LevelBefore`, `LevelAfter` (Recycle: 0),
`Paid`, `Received` (Recycle), `Message`. Statuses:

| Status | Meaning |
|---|---|
| `Done` | it happened |
| `Refused` | the game's own checks refused it; also when 256 actions are waiting (try again in a step) |
| `NotEnough` | the player cannot pay |
| `NoForge` | the forge entity is no Shroud Forge |
| `NotFound` | the carrier, its socket or the gem is not where the call says, a ref is stale, or the item is not a gem |
| `Occupied` | Socket: the socket holds a gem already; Unsocket: it is empty |
| `NoRoom` | Unsocket: no free slot on the player's Generic pages (a worn backpack's pages count); a free or changed cost: no room for the game's price |
| `MaxLevel` | Upgrade: at the gem's own cap or the forge's maximum level |
| `Unavailable`, `Dropped` | `gems.craft` went off, or the player left / the world closed before it ran |

## Events
```csharp
Events.On<GemCraftingEvent>(e =>
{
    // a player is about to use a forge: e.Action, e.Carrier, e.Gem, e.GemLevel, e.Forge, e.Cost (Recycle: e.Yield)
    if (e.Action == GemCraftAction.Recycle && IsQuestGem(e.Gem)) e.Cancel();  // nothing is paid, nothing moves
});
Events.On<GemCraftedEvent>(e =>
    Log.Info($"{e.Player} {e.Action} {e.Gem}: level {e.LevelBefore} -> {e.LevelAfter}, paid {e.Paid}, got {e.Received}, code: {e.ByCode}"));
```

- **`GemCraftingEvent`** (cancelable): a player's own forge action, before the game runs it. While a handler listens,
  the game holds each new forge action for one step (about 40 ms on a dedicated server) and Kiln posts it in the next Pre;
  not canceled, it runs in the next step. `Cost` is the game's own price with that forge (the cost function the game then
  charges). Cancel refuses it: the game treats it as a request without a forge, so nothing is paid and nothing moves. The
  player's game is not told; its forge screen shows the inventory as it is at the next update. A held action nobody
  answers within 8 steps runs. A cancel stands even when the handler's mod stops listening before the game saw the
  answer. Each action is posted once: when the last handler goes, what is still held runs without an event, and an
  action the game leaves unconsumed (its own handler does that when the gem's level cannot be read) is not posted again.
  Only players' actions: `Gems`' own calls are never held or posted here.
- **`GemCraftedEvent`**: after an action changed something: a player's (after the game committed it) or a `Gems` call
  (`ByCode`), including the forgeless unsocket of `Gems.ReturnSocketedGems` (`Unsocket`, `ByCode`, `Forge` none, nothing
  paid). `Paid` and `Received` are counted from the player's inventory before and after. A refused or canceled action
  posts nothing.
- Gems that change any other way (a mod moving items, a socket written directly) are not seen here;
  `GemSocketChangedEvent` sees every change of a socket by polling, a few steps later (gems.md, "Which event for what":
  each forge action that changes a socket shows up in both).

## Limits
- Server side only. On a game client that joined someone else's world, `gems.craft` is off.
- The forge screen of the player's game shows Kiln's actions only as the inventory updates (no animation for code
  actions on the player's screen; the forge itself plays its action sequence, as for a player's click).
- Gems on rings and armour: the game's action accepts any carrier with a socket ([gems-sockets.md](gems-sockets.md) adds
  those sockets). Lab: a ring in the backpack socketed and upgraded by code, and a socket into a worn ring.
- A game update that changes the forge's handler turns `gems.craft` off until Kiln is updated (`Capabilities.Why("gems.craft")`).

## For native code
`native/core/abi/services/kiln_gemcraft.h` (`kiln.gemcraft` v1, in kiln-items.dll): `submit`, `cancel`, `hold`, `held`,
`decide`, `watch`, `crafted`, `status`. `KILN_GEM_RETURN` (an Unslot alone, forge 0, no holder) takes a gem out of any
carrier in the player's own inventories without a forge and free: what `Gems.ReturnSocketedGems` uses for sockets the
forge does not list. Evidence and the handler's layout: [docs/re/gems-forge.md](../../re/gems-forge.md).

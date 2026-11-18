# Production stations and bases (kiln.stations)

What Kiln knows about the game's automated production stations (kilns, smelters, looms, seed beds: `FactoryStation`), the
bases the game's crafting code sees (`PlayerBasesSystemServer`), the game's own "fill from chests", stop and select, and
the station events. Read 2026-11-16 on game client Steam 26430625 and dedicated server Steam 26430253 (svn 1287369);
image base `0x140000000`, addresses are RVAs, "C" = client, "S" = server. The API built on it: kiln.stations
(`native/core/abi/services/kiln_stations.h`, `native/services/items/stations.cpp`; C# `Stations` and `Bases` in
Kiln.Blocks), guide [docs/kiln/services/stations.md](../kiln/services/stations.md); symbols `sigdb/symbols/server.d/stations.json`
(shared with the client) and `@gameplay/CraftRecipe.factory_phase` in `server.d/gameplay.json`.

Evidence tags: **[refl]** the game's reflection records (read offline with `tools/kfc`), **[data]** game resources decoded
with `tools/kfc`, **[dis]** disassembly, **[sig]** offline sigdb resolve on both exes, **[cmp]** the client and server
functions compared instruction by instruction (same code, only addresses differ), **[kiln]** already in Kiln's docs/re,
**[inf]** inferred from code, not run, **[hyp]** a guess that needs a lab or client check, **[lab]** seen on a lab
dedicated server (section 7).

## 1. Stations and bases (task item 1)

### 1.1 What a production station is [data][refl]

`kfc find-templates --components` over all 12,269 templates: **31 templates carry `FactoryStation`** (family `Factory`):
`T1_Charcoal_Kiln_Factory`, `T1_Drying_Rack_Factory`, `T1_Farmbed_Factory`, `T1_Forge_Factory`, `T1_HandGrinder_Factory`,
`T1_Seedling_Factory`, `T2_ColoringStation_Factory`, `T2_EctoplasmPress_Factory`, `T2_Fireplace_Factory`,
`T2_HandSpindle_Factory`, `T2_Kiln_Factory`, `T2_Smelter_Factory`, `T2_Table_Saw_Factory`, `T3_Alchemy_Station_Factory`,
`T3_Oven_Factory`, `T3_Tanning_Station_Factory`, `T3_butter_churn_factory`, `T4_Grindstones_Factory`, `T4_Honey_Factory`,
`T4_Laboratory_Factory`, `T4_Loom_Factory`, `T4_SpinningWheel_Factory`, `T5_BlastFurnace_Factory`,
`T5_cardingMachine_Factory`, `T6_Improved_Grinder_Factory` (the one with a rotational-energy consumer),
`T6_Medieval_Grill_Factory`, `T6_coffeemaker_brewer_Factory`, `T6_coffemaker_roaster_Factory`,
`T7_Force_Generator_Factory`, `T7_Infusion_Station_Factory`, and `Base_Craftingstation_Factory`. Six have `Fuel`
(water): oven, honey, laboratory, fireplace, seedling bed, coffee brewer. All have `CheckPlayerInRange`, which
`factory_station` does **not** require (its descriptor has no `with` list).

Kiln.Blocks' `StationKind` table (`managed/services/Kiln.Blocks/World/PropModel.cs`) lists 12 timed kinds; the API must
not use that table to recognise stations: it reads `FactoryStation` and `Crafting.workshopId` from the entity.

Components a station uses (sizes from reflection):

| Component | Size | Fields used |
|---|---|---|
| `FactoryStation` (`0x05599679`) | 104 | `+0x00` u8 `state` (`FactoryStationState`), `+0x04` `runningRecipe`, `+0x08` `runningRecipeInputCategorySelections` (32 B), `+0x28` `nextRecipe` (= the **selected** recipe; it stays set and repeats), `+0x2C` `nextRecipeInputCategorySelections` (32 B), `+0x50` `recipeStart` (Time, ns), `+0x58` `recipePauseDuration`, `+0x60` bool `useAllAvailableCategoryIngredients`, `+0x61` bool `isStoppedByPlayer`, `+0x62` u8 `missingRequirements` (Bitmask8) |
| `Crafting` (`0xad776575`) | 20 | `+0x00` workshop GUID, `+0x10` `workshopId` (which recipes run here) |
| `CraftingInteraction` | 32 | `+0x1C` `workshopId` (which menu opens) |
| `InventorySetup` | 84 | template: 16 generic slots, 16 available, flag `IndestructibleWhileNotEmpty`; inputs and outputs share the slots |
| `CurrentTransform` | 0x38 | position, fixed 32.32 (the game converts with `× 2^-32`, constant C `0x13d265c`) |
| `ServerToggleState` / `NetworkToggleState` | 24 / 16 | the "fire on" mirror, copied from the state by `factory_station_toggle`; what other players see |

`FactoryStationState`: 0 NoRecipe, 1 MissingMaterials, 2 Paused, 3 Stopped, 4 Running, 5 MissingOutputSpace.
`FactoryStationRequirement` (bit index in `missingRequirements`): 0 Recipe, 1 EnergySourceInRange, 2 EnergyAmount,
3 InputMaterials, 4 OutputSpace, 5 AboveWater. `RecipeInputCategorySelections` = 4 × `{u32 categoryId; u8 selectedIndex}`
(8 B each). [refl]

### 1.2 Bases: `PlayerBasesSystemServer` [dis][sig][cmp]

The singleton `PlayerBasesSystemServer` (a sys-read of `player_crafting`, `inventory_actions`, `factory_station_info`,
`handle_crafting_events`, …; its iterator slot in `player_crafting` is `+0x48`) holds the bases the crafting code uses.
Offsets are from the singleton; `i` = base index (u8, 0..15):

| Offset | What | Evidence |
|---|---|---|
| `+0x20` | u32 zone-set version; a change rebuilds the bases | update C `0x473470` / S `0x20be40` |
| `+0x24` | u32 base count (≤ 16) | `BaseAtPosition`, captured `stations/bases.count` |
| `+0x30 + i·0x130` | DynamicArray<u32 entity> **stock containers** `{ptr, count +8, capacity +0x10, grow fn +0x18}` | `BaseStockForPlayer`, `AddToBaseStock`, captured `stock.items` 0x30 / `stock.items_count` 0x38, stride 0x130 (2 captures) |
| `+0x58 + i·0x130` | DynamicArray<u32 entity> **water sources** | captured `stock.water` 0x58 / `water_count` 0x60 |
| `+0x108 + i·0x130` | u64 `CraftingPropMask` of the base (`lock or` by `base_crafting_prop_register`) | C `0x470d30`, `0x471a80` |
| `+0x120 + i·0x130` | float3 AABB min (+pad), `+0x130` max | `BaseAtPosition` (`bases.aabb_y` 0x124 = min.y) |
| `+0x140 + i·0x130` | u32 mask of the zone boxes that make the base | `bases.zone_mask_rel` 0x1C from `+0x124` |
| `+0x144.. / +0x150..` | per-base counters (animals, beds, NPCs) for `ClientBaseIdComponent` | C `0x470700`, `0x470dc0` |
| `+0x1330` / `+0x1338` | pointer / count of the zone boxes, 0x20 B each `{float3 min, pad, float3 max, pad}`, at most 16 | `bases.zones` 0x1330 |

- **`BaseAtPosition(Result* out, PlayerBasesSystem*, const float3* pos)`** C `0x470410`, S `0x208c60`: first base whose
  AABB holds the point (min inclusive, max exclusive) and, when it has more than one zone, one of whose zone boxes holds
  it. `out+0` u8 error (0 found, `0x1F` in no base), `out+8` u8 index. 25 callers on the client. [dis][sig][cmp]
- **`RebuildBases(sys, world)`** C `0x46dda0`, S `0x206510`: clears every base, copies the zone list (`0x9bbfa0`) and
  merges overlapping zone boxes into bases (`AddBase` C `0x46eb30`). So **one record can hold several altars** when
  their zones overlap. [dis]
- **`RebuildBaseStock(sys, world)`** C `0x46fde0`, S `0x208590`: zeroes both lists of every base, enumerates every entity
  with **`InventoryCraftingStock`** (hash read from `.data` C `0x152fc20` = `0xe41a0d1f`) and adds each to list A of the
  base that holds its position (`AddToBaseStock` C `0x471220`, S `0x209b20`; error `0x1F` outside every base, `0x27` list
  full), then every entity with **`FuelCraftingStock`** (`0x560461c1`) whose `NetworkFuel` (`0x15557d63`) is not zero to
  list B. [dis][data]
- **When**: the server's system update `0x23b000` (called from the server frame `0x7bbffc` at `0x7bc585`) calls
  `PlayerBasesSystemServer::update` S `0x20be40` with no player test: zones changed → `RebuildBases`; zones present →
  `RebuildBaseStock`, **every frame**. The same frame function reaches the world step (`SimStep` wrapper `0x6c8e40` from
  `0x7bcb14`) later, on the same thread. So the lists are current and stable during the step and in Kiln's Pre/Post.
  On a hosting client the hosted server's update is C `0x473540` (from `0x4a28bd`); the client's own
  `PlayerBasesSystemClient` is C `0x473470` (from `0x3af099`). [dis] Order inside the frame: **[inf]**, lab check L1.

### 1.3 Which containers are "the base's storage" [data][dis]

Templates with `InventoryCraftingStock` (10): `Prop_Decoration_T1_Storage_24_Magic`, `T2_Storage_32_Magic`,
`T3_Storage_40_Magic`, `T4_Storage_56_Magic`, `T6_Storage_72_Magic`, and five player-planted refilling harvest plants
(`Scatter_Loot_Wetland_Above_Bananaplant_01_e_fruits_harvest_PlayerPlanted_Refill`, coffee tree, hazelnut, coconut
palm, date palm). `InventoryCraftingStock {bool canReceiveAreaDeposit}`, overridable per entity by
`DynamicInventoryCraftingStock` (2 bytes, reflection: `+0 canReceiveAreaDepositOverrideIsSet`, `+1
canReceiveAreaDepositOverride`). By the programs' access lists, `area_deposit_trigger` (S update `0x88a50`) reads the
template's `InventoryCraftingStock` and the entity's `DynamicInventoryCraftingStock`, and `inventory_actions` (S
`0x1a5af0`, a player's inventory input) writes the latter: the per-chest deposit switch is taken to be that write [refl],
not seen in a game. Templates with `FuelCraftingStock` (13):
wells (`Prop_NPC_Water_Well_a/b/c`, desert/wetland wells) and the two water barrels.

Ordinary chests (`Prop_Decoration_T1_Storage_24`, …) have no `InventoryCraftingStock`: **the game's fill never uses them**,
and neither does crafting from base stock (`rules/InventoryPay` S `0x199c80` calls `BaseStockForPlayer` too). This is the
game's own rule ("magic chests").

### 1.4 Base identity

- The flame altar is the one template with `BaseIdComponent` (`Home_Base_FlameAltar`, family `Altar`, also
  `PlayerSpawnPoint`, `TriggerShape`). `BaseIdComponent` (48 B) `+0x00 baseId` (a GUID; made once by the INIT program
  `base_id_generator` from `GuidGenerator`), `+0x10` `buildZoneSize` (not authoritative), `+0x20 endOfTimeout`,
  `+0x28 wasAbandoned`. [refl][kiln build.md §11]
- The game itself maps altar → record by position: `home_base` (C `0x339040`) reads the altar's `BaseIdComponent` and
  `CurrentTransform` and calls `BaseAtPosition` (C `0x46fdb0`) for the record's counters. Kiln does the same.
- **Proposed stable base id** = the `baseId` GUID of the record's altar; when a record holds several altars, the
  numerically smallest GUID, with every altar listed (`Base.Altars`). Survives restarts (the save keeps
  `BaseIdComponent`; save regions and characters' respawn bases are keyed by it, docs/re/build.md §5,
  docs/re/characters.md). The record index is per session and can change when zones change. Name: the altar's
  `CustomPlayerString` (`Items.ContainerName` already reads any entity's).
- Kiln.Build's `Build.Bases` (BuildZoneSystem list 0: altar, centre, half size) stays the per-altar view.

### 1.5 Enumerating stations of a base

Stations are not in any base list. A station belongs to the record that `BaseAtPosition(station position)` returns;
that is the same test the game applies to the stock containers. Kiln finds stations by component (`World.Find` /
kiln.world `find` on `FactoryStation`, or the service's own scan of the entity map) and groups them per record.

---------------------------------------------------------------------------------------------------------------------

## 2. A station's state (task item 2)

### 2.1 The state machine, `factory_station` (C update `0x335e80`, S `0xf5980`, flags `0x200`, every step) [dis][cmp]

Access: read `Crafting`, `CurrentTransform`; read-opt `DynamicIsInWaterCheck`, `DynamicRotationalEnergyConsumer`; write
`FactoryStation`; sys-read `BalancingRegistry`, `ServerPlayers`, `ServerProgressSystem`, `ItemRegistry`, `RecipeRegistry`,
`AttributeRegistry`; sys-write `KnowledgeTriggerQueue`, `ServerFactoryInfo`; other-read `InventorySetup`; other-write
`Fuel, Inventory, InventoryEmptyState, ItemState, NetworkFuel, OwnerRelationship, SlotSelection`; events-out
`CraftRecipeSuccessEvent`. Per station, by state (jump table S `0xf5cf0`):

| State | What the program does |
|---|---|
| NoRecipe | `nextRecipe` set → **StartNext** (C `0x32def0`, S `0xee0a0`) |
| MissingMaterials | `nextRecipe` set → StartNext; else `CancelRunning` |
| Paused | resume check (S `0x110e20`) |
| Stopped | `runningRecipe` set → **CancelRunning** (C `0x32d2b0`, S `0xed460`: refunds the running recipe's inputs into the station through an inventory transaction (`0x19ac50` → `InventoryAddItems`), clears `runningRecipe`/`recipeStart`). Then water/energy → pause; `isStoppedByPlayer == 0` → StartNext; else stays stopped |
| Running | `nextRecipe == 0` → CancelRunning. `isStoppedByPlayer` → state := Stopped. Water/energy → pause. Then `end = recipeStart + craftingDuration × f(factoryProductionSpeedFactor)` (`FactorySpeed` C `0xe903d0` / S `0x937510` reads `BalancingRegistry+0x90` → settings `+0x3C`); `now ≥ end` → **Complete** (C `0x32dab0`, S `0xedc60`) |
| MissingOutputSpace | like Running's completion path, retried |

- **StartNext** checks pause conditions, calls `TryStart` (S `0xe4330`): `CraftRecipe` with context flag **`0x08`**
  (take the inputs only; `| 0x40` when "use all ingredient options" is on: `RecipePayAlt`), failure → state
  MissingMaterials; success → state Running, `runningRecipe = nextRecipe`, running selections := next selections,
  `recipeStart = now`. **Inputs leave at the start.**
- **Complete**: `CraftRecipe` with flag **`0x10`** (outputs only: the recipe function skips payment when `ctx+0x10 & 0x10`,
  S `0x19a25a`); failure → state MissingOutputSpace; success → knowledge push, state NoRecipe, so the next step starts
  the next cycle. **Outputs arrive at the end.** `CraftRecipe` sends `CraftRecipeSuccessEvent {workshopId, recipeId}`
  (16 B, no station id). [dis]
- When the state byte changes, the program calls **`FactoryInfoInvalidate(ServerFactoryInfo*, station)`**
  (C `0x4a6350`, S `0x23ec60`): it clears the station's "sent" bits so players with the station's screen open get it
  again. `player_crafting` calls the same before any station action. [dis]

### 2.2 What the API reports, and how each value is computed

| Field | Source |
|---|---|
| running / stopped (the player's stop button) | `state`, `isStoppedByPlayer` |
| selected recipe(s) and category choices | `nextRecipe`, `nextRecipeInputCategorySelections`, `useAllAvailableCategoryIngredients` |
| the craft in progress | `runningRecipe`, its selections, `recipeStart` |
| progress | `(now − recipeStart) / (craftingDuration × f(speed))`, clamped 0..1, while Running; `now` = the world's program time (`gameplay/ProgramTime`); the speed function read from `FactorySpeed` or kiln.settings (`factoryProductionSpeedFactor`, live) |
| powered / water | `missingRequirements` bits EnergySourceInRange, EnergyAmount, AboveWater; state Paused |
| inputs / fuel / outputs | the station's `Inventory` (Items.Read): **no slot is typed**; Kiln classifies a stack as output when its item is an output and not an input of the running or selected recipe; "loaded crafts" = `min over inputs (have_i / count_i)`, exactly as the fill computes it (§3.2). Water: `Fuel` of the station (the 7 templates with water) |
| loaded? | on a dedicated server every entity simulates; `factory_station` has no player-in-range filter, and the lab already saw a rack cycling with no player (docs/re/building.md §5). Not a v1 field; lab check L6 |

### 2.3 `PlayerCraftingAction` and how the server handles it [refl][dis][cmp][sig]

`PlayerInput.fromClient.playerCraftingAction` at `PlayerInput+0xBC` (68 B): `+0x00 craftingStationId`, `+0x04 recipeId`,
`+0x08 itemId`, `+0x0C u16 amount`, `+0x10 waterAmount`, `+0x14 waterSourceId`, `+0x18` selections (32 B), `+0x38 bool
useAllAvailableIngredientOptions`, `+0x3C` version, `+0x40 u8 type`. Consumed version at
`ServerConsumedPlayerInput+0x44`. Program `player_crafting` (C `0x2e62b0`, S `0xd2250`, flags `0x210` = host only,
chunked) runs each new version; dispatch (jump table C `0x2e6520`):

| Type | Name | Server action | Checks |
|---|---|---|---|
| 0 | CraftRecipe | sends `CraftRecipeEvent` (handled by `handle_crafting_events`) | there |
| 1 | **AutomaticTransfer** | `AutoTransferAction(action, iterator)` C `0x2c6b30` / S `0xb2080` | §3.2 |
| 2 | **SelectCraftingStationRecipe** | `StationWrite` (C `0x2c0c60`, S `0xabf70` = `WriteOther` on the program's `FactoryStation` accessor) → `nextRecipe = recipeId`, next selections = action's, `isStoppedByPlayer = 0` | **none** (no distance, no permission, no workshop or knowledge test; the workshop gate only comes at the start) |
| 3 | SetCraftingStationUseAllIngredientOptions | `useAllAvailableCategoryIngredients = action's` | none |
| 4 | **CraftingStationStopProduction** | `isStoppedByPlayer = 1` | none |
| 5 | TransferWater | C `0x2cbfb0` | not traced |

Before the dispatch it calls `FactoryInfoInvalidate(ServerFactoryInfo, action.craftingStationId)`; after it, the
consumed version is stored. **Resume = type 2 with the station's own selected recipe** (it clears the flag).

---------------------------------------------------------------------------------------------------------------------

## 3. Actions (task item 3)

### 3.1 Fill from chests: the game's path [dis][cmp][sig]

`AutoTransferAction` (C `0x2c6b30`, S `0xb2080`), for the player = the program's current entity:

1. Builds an inventory-transaction setup block (docs/re/items.md §3) from `player_crafting`'s iterator: flags 0, stock mode
   0, the context, ItemRegistry, BalancingRegistry, AttributeRegistry, accessors InventorySetup (read), Inventory,
   SlotSelection, ItemState, OwnerRelationship, InventoryEmptyState, **NetworkFuel**, **Fuel**, the PickupEvent and
   InventoryFull senders, the knowledge queue, **operator = the player**, **`+0xD8` = PlayerBasesSystemServer**,
   **`+0xE0` = the player's position** (float3). `InventoryTxCtor` + `InventoryTxInit`.
2. **`BaseStockForPlayer(out, &stock, 0, PlayerBasesSystemServer*, &position, player)`** (C `0x46fa10`, S `0x2081c0`):
   `PlayerPermissions(player)` (C `0x3b8410`, S `0x163040`: `BasicPlayerInfos[player−1]+0x9D`) must have bit **0x02 =
   `CanAccessInventories`** (else `0x16`); `BaseAtPosition(player position)` (else `0x1F`); `stock` = the record's two
   lists `{A.ptr, A.count, B.ptr, B.count}`. **The player has to stand in the base**.
3. `recipeId != 0`: `RecipeFind`; **`ResolveRecipeInputs(recipe, &out, &selections, RecipeRegistry)`** (C `0xe86340`,
   S `0x92d4c0`) turns every input into a concrete 24-byte item stack (`ResolveInput` C `0xe99df0` / S `0x941030`: a
   category input takes the selected option of `FindInputCategory` (C `0xe99890`, S `0x940ad0`) and its count is
   `ceil(count × option.scaling)`); then **`AutoTransferRecipe(tx, &stock, &inputs, station, u16 amount,
   recipe.waterCharges, player)`**. `itemId != 0`: `AutoTransferItem` (C `0x3f7950`, S `0x198bf0`).
   `waterAmount != 0`: `AutoTransferWater` (C `0x3f8200`, S `0x1994a0`).
4. Result 0 → `InventoryCommit`, else `InventoryRollback`.

**`AutoTransferRecipe`** (C `0x3f7ab0`, S `0x198d50`), `int (InventoryTx*, const BaseStock*, const Slice<ItemStack24>*
inputs, u32 station, u16 crafts, u32 waterCharges, u32 player)`:

```
no inputs                                  -> return 1
station InventorySetup missing (tx[3])     -> return 5
have[i]  = sum of input i over every slot of every link of the station
loaded   = min over inputs of have[i] / count[i]                (0 if any count is 0)
           (with water: also min with station water / (charge units * waterCharges))
for each input i:
    need = clamp((crafts + loaded) * count[i] - have[i], 0, 0xFFFF)
    if need == 0: continue
    if player: take up to need from the player's inventories          (removeItems, preferred slot "none")
    for each container c in base stock list A, in list order, while something is missing:
        take the rest from c
    still missing                          -> return 4  (missing materials)
    addItems(station, item, need)          -> on error or leftover: return 2 (no room in the station)
water: need = charges*(crafts+loaded) - station water; AutoTransferWater from list B -> its code
return 0
```

So the game's `amount` means **"this many more complete crafts than the station holds now"**, every input topped up
to the same level; partial stacks count. **All or nothing**: one transaction, rolled back by the caller on any code.
The client uses the same function as a **dry run** to enable the button: player_ui (C `0x2be77f`) calls it on a scratch
transaction and rolls back (`0x2be78d`), with an amount from the station's `ClientFactoryInfo` (how that amount is
chosen was not traced).

### 3.2 Fill from chests: how Kiln calls it (no player needed)

Kiln does steps 1, 3, 4 itself, **inside its own program**, and replaces step 2:
- the stock = the 4 qwords of the station's own record (`BaseAtPosition(station position)`), read from the singleton; no
  permission test (no player); `player = 0` unless the caller asks to also take from a named player's inventories (the
  button's behaviour, opt-in);
- setup as the game builds it, with operator 0 (a container, as kiln.items gives to chests), no knowledge queue,
  `+0xD8` the singleton and `+0xE0` the station's position;
- **crafts chosen by Kiln**, so that the all-or-nothing call does not fail for a missing stack: Kiln counts the stock
  (list A, per item, with optional reserves the caller keeps back), computes the largest `n ≤ requested` the stock
  covers for every resolved input, calls with `n`, and on code 2 (no room) or a race tries `n/2`, … (each attempt rolled
  back) within the same update, at most 6 attempts (kMaxTries);
- a dry run = the same call, then rollback (exactly what the client's button does);
- after a commit: `FactoryInfoInvalidate(station)`.

`AutoTransferRecipe` never reads the program's current entity (it reads through the transaction's accessors and job
storage only, [dis]), so it is safe to call from a SYSTEM program. The player-taking path, the water path and the
transaction internals are the game's.

Known limits (from the code): the station's **selected** category options are used (Kiln passes the station's
`nextRecipeInputCategorySelections`); with "use all ingredient options" on, the fill still loads only the selected
option ([inf], lab L7). Gear inputs (`HasPIDE`) go through the same `removeItems`/`addItems`.

### 3.3 Stop, resume, select

Kiln writes, inside its program, through `WriteOther` on its own `FactoryStation` other-write accessor (the path
kiln.world's tracked writes and kiln.items' water already use, docs/re/world.md "Tracked writes"), exactly the bytes
the game's type 2 and 4 write, after `FactoryInfoInvalidate(station)`:
- **Stop**: `isStoppedByPlayer = 1`. The running cycle is aborted and refunded next step (§2.1); the station shows
  Stopped to everyone. Option **after the current craft**: Kiln waits for the station's completion (§4) and sets the
  flag then; the cycle that `StartNext` begins in the same instant is aborted and refunded at once, so nothing is lost
  ([inf], lab L4).
- **Resume**: `isStoppedByPlayer = 0` (`nextRecipe` unchanged); the station starts a new cycle next step.
- **Select**: `nextRecipe`, selections, `isStoppedByPlayer = 0` (or keep it stopped on request); `nextRecipe = 0` clears
  the selection (the game then cancels the running cycle).
The server checks nothing for these (§2.3); Kiln checks the entity has `FactoryStation`, and for Select that the
recipe's `workshopId` equals the station's `Crafting.workshopId` (else the game would sit in MissingMaterials). The
save keeps `FactoryStation` (a running rack came back running after a restart, docs/re/building.md §5); that our
written flag survives a restart is lab check L4.

### 3.4 Collect finished items

Already in Kiln: `Items.Move(station, slot, chest)` (the game's `SlotTransfer` S/C `0x1b4550`/`0x4149b0`, keeps item
entities, docs/re/items.md §13). The station's output stacks are classified as in §2.2. Choosing a chest is the mod's
policy; Kiln offers a helper that mirrors quick stack: base stock containers already holding the item, then any with a
free slot. Moving into an ordinary chest is allowed (it is just a move).

### 3.5 Recipes, ingredient categories, stack sizes

| Data | Where | In Kiln today |
|---|---|---|
| recipes (2,542 in the game data) | `RecipeRegistry` map `+0x48` | Kiln.Content `Recipes` (inputs, outputs, duration, `workshopId` field) |
| a workshop's recipes | RecipeRegistry `+0xF8` (workshop → `SmallDynamicArray<RecipeInfo*,512>`) | captured by kiln.content (`content/recipes.workshops`); no public "recipes of workshop" call |
| **input categories (22)** | RecipeRegistry `+0xA0` HashMap: bits `+0xA8`, capacity `+0xB0`, keys `+0xC8`, values `+0xE0` (`RecipeInputCategory*` = `{u32 id; BlobArray<{ItemId, float scaling}>}`), count `+0xF4` | **new** (Kiln.Content knows a category's id and count on a recipe input, not its options) |
| options with scaling ≠ 1 | 4 of the 22 categories | new |
| `RecipeInfo.waterCharges` `+0x98`, `craftingDuration` `+0x38` | reflection | Content fields |
| max stack | `ItemInfo+0x14` | `Items.Get(id).MaxStack` |

The category map offsets are captured from `FindInputCategory` (`@stations/FindInputCategory.fields`); 22 categories and
2,542 recipes counted in `RecipeRegistryResource` (`5e57de0f-…`) with `tools/kfc decode`. [data][sig]

---------------------------------------------------------------------------------------------------------------------

## 4. Events (task item 4)

| Event | Game seam | Proposed source | Confidence |
|---|---|---|---|
| a cycle finished, outputs in the station | `Complete` → `CraftRecipe(ctx, recipe)` with `ctx+0x10 & 0x10` (outputs phase); `ctx+0` operator = `ctx+4` station = the station | kiln.gameplay's existing CraftRecipe hook (`gameplay/CraftRecipe` S `0x210f00` / C `0x478680`), **extended to read the flag byte** and report the phase; the managed side posts `StationCraftedEvent` for phase "outputs" of a FactoryStation entity, next Pre, with the recipe's outputs | [dis] for the flags; double report today [inf from code: the hook keeps successes where operator == station], lab L5 |
| a cycle started (inputs taken) | the same hook, flag `0x08` | `StationStartedEvent` (optional) | same |
| state changed (started, stopped, paused, missing materials, output full) | none: `FactoryStation` written by `factory_station` and `player_crafting` | per-step diff of every known station's `state`, `isStoppedByPlayer`, `nextRecipe` in Pre (`GetComponent`, tens of stations) | [inf]; cost lab |
| who stopped/selected/filled: a player | `PlayerCraftingAction` types 1..4 in `PlayerInput` | the existing `PlayerClientActionEvent` (`playerCraftingAction`, held one step, cancelable, docs/kiln/services/player-state.md) decoded into a typed **`StationPlayerActionEvent`** (station, type, recipe, amount, selections) | [kiln] for the hold; decoding [refl] |
| …: Kiln | Kiln's own writes | the state event names the mod | n/a |
| placed / removed | entity created / destroyed | kiln.gameplay `ENTITY_JOINED`/`ENTITY_LEFT` filtered to `FactoryStation` (template family Factory via kiln.spawn) | [kiln] |
| base list changed | `+0x20` version, `+0x24` count | Pre poll of the version: `BasesChangedEvent` | [dis] |

`CraftRecipeSuccessEvent` (the game's own, `0x7fe02e20`, sent by `CraftRecipe` S `0x211458`) has no station id, so it is
not usable for per-station events. Hooking `Complete` directly (S `0xedc60`) is the fallback if the gameplay change is
refused; the kiln.gameplay route (an existing hook) is preferred, and it is what Kiln does since 0.2.0-dev.29: the
craft core's context has a flags byte (`ctx+0xA0-0x90` from the hook's `ctx`) whose bit `0x10` is set for the completion
call of a factory cycle and clear for its start (`@gameplay/CraftRecipe.factory_phase`, feature `gameplay.craft_phase`).

---------------------------------------------------------------------------------------------------------------------

## 5. Threads and sides (task item 5)

- **Actions** (fill, stop, resume, select): inside a Kiln ECS program of the **server world**, as kiln.items does
  (docs/re/items.md §1: the transaction holds the `ProgramContext`, reads through program accessors, sends events through
  the program's event slots). Proposed program `kiln_items_stations`, flags SYSTEM | HOST_ONLY, registered through
  kiln.world after the game's programs, with exactly the access `player_crafting` declares minus `PlayerInput` /
  `ServerConsumedPlayerInput`: sys-read `BalancingRegistry`, `PlayerBasesSystemServer`, `g38_items::ItemRegistry`,
  `g38_items::RecipeRegistry`, `impact::AttributeRegistry`; sys-write `KnowledgeTriggerQueue` (optional),
  `ServerFactoryInfo`; other-read `CurrentTransform`, `Crafting`; other-write `FactoryStation, Fuel, Inventory,
  InventoryEmptyState, InventorySetup, ItemState, NetworkFuel, OwnerRelationship, SlotSelection`; events-out
  `InventoryFull`, `PickupEvent`. Requests are queued from any thread, run in the next update (≤ 16 per step), and their
  callbacks come in the next Pre on the tick thread (one step, ~41 ms on a dedicated server). Because it registers after
  the game's programs and declares `FactoryStation`/`Inventory` writes, it runs after `player_crafting` and
  `factory_station` in a step ([kiln] world.md "Where the write sits"): a player's own action in the same step runs first.
- **Reads** (bases, stock, stations, categories): the tick thread, Pre or Post, when no job runs. `GetComponent` for
  components; the `PlayerBasesSystemServer` and `RecipeRegistry` singletons read directly. The stock lists are rebuilt in
  the server frame before the step ([dis], order [inf], lab L1); Kiln copies what it hands out.
- **Events**: posted in Pre (next step) on the tick thread, like every Kiln gameplay event.

| | dedicated server | hosting client | joined client |
|---|---|---|---|
| reads | yes | yes (the hosted server world; same functions, [cmp]) | **no in v1**: `FactoryStation` is server-only; the client has its own `PlayerBasesSystemClient` lists (C `0x473470`) and replicated inventories, so a read-only "approximate" view is possible later |
| fill / stop / resume / select / collect | yes | yes | no: the mod sends a request to its own server side (Kiln.Network); the only vanilla route would be writing the local player's `ClientPlayerInput.playerCraftingAction` (acts as the player, needs the player in the base): not proposed |
| events | yes | yes | no (`StationPlayerActionEvent` only where `PlayerClientActionEvent` can hold) |

Other loaders: Kiln adds **no hook** on `player_crafting`, `factory_station` or the fill functions (another loader may
detour `player_crafting`'s update; Kiln stays off it). The only hook involved is kiln.gameplay's existing `CraftRecipe`
hook (already chain-safe).

---------------------------------------------------------------------------------------------------------------------

## 6. Signatures (`sigdb/symbols/server.d/stations.json`) [sig][cmp]

All patterns were checked with `tools/sigport/sigdb_check.py` composed into a copy of the full sigdb tree
(`server.d/stations.json`, shared with the client like every server.d file): **each resolves exactly once on client
26430625 and on server 26430253, every constant captured with the same value on both, all four features on, no name
collides.** Client and server bodies of the main functions compare instruction-for-instruction identical
(only jump-table addresses differ).

| Name | C 26430625 | S 26430253 | How it is found |
|---|---|---|---|
| `stations/PlayerCraftingUpdate` | `0x2e62b0` | `0xd2250` | prologue `40 55 41 57 48 8D 6C 24 D8 48 81 EC 28 01 00 00 41 B8 E0 00 00 00` (22 B; also the descriptor `player_crafting`) |
| `@stations/PlayerCrafting.dispatch` | in update | in update | 3 scoped patterns: captures `input.crafting_action` 0xBC, `consumed.crafting_action` 0x44, `action.version` 0x3C, `action.type` 0x40, `factory.next_recipe` 0x28, `next_selections` 0x2C, `stopped_by_player` 0x61, `use_all` 0x60, `FactoryInfoInvalidate`, `AutoTransferAction`, `StationWrite` (rel32) |
| `stations/AutoTransferAction` | `0x2c6b30` | `0xb2080` | prologue (26 B) + captured from the dispatch |
| `@stations/AutoTransferAction.calls` | | | captures `BaseStockForPlayer`, `ResolveRecipeInputs`, `AutoTransferRecipe`, `recipe.water_charges` 0x98, `action.amount` 0x0C |
| `stations/AutoTransferRecipe` | `0x3f7ab0` | `0x198d50` | prologue `44 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 56 57 41 56 41 57 48 8D AC 24 60 FF FF FF` (35 B) and the capture, which must agree |
| `stations/ResolveRecipeInputs` | `0xe86340` | `0x92d4c0` | prologue (35 B, through `cmp [rcx+0x2C], esi`: the input count) + capture |
| `@stations/ResolveInput.calls` → `stations/FindInputCategory` | `0xe99890` | `0x940ad0` | the call in `ResolveInput` (C `0xe99df0`, S `0x941030`; its prologue has a twin, so it is found by the call site) |
| `@stations/FindInputCategory.fields` | | | `categories.count` 0xF4, `cap` 0xB0, `bits` 0xA8, `keys` 0xC8, `values` 0xE0 |
| `stations/BaseAtPosition` | `0x470410` | `0x208c60` | prologue with `bases.count` 0x24 |
| `@stations/BaseAtPosition.fields` | | | `bases.aabb_y` 0x124, `zone_mask_rel` 0x1C, `zones` 0x1330, `stride` 0x130 |
| `stations/BaseStockForPlayer` | `0x46fa10` | `0x2081c0` | prologue + capture; fields `stock.items` 0x30, `items_count` 0x38, `water` 0x58, `water_count` 0x60, stride (2nd vote), `PlayerPermissions` (C `0x3b8410`, S `0x163040`), `permission.stock` 0x02 |
| `stations/RebuildBaseStock` | `0x46fde0` | `0x208590` | prologue (evidence / optional) |
| `stations/FactoryStationUpdate` | `0x335e80` | `0xf5980` | prologue; `@…timing` captures `FactorySpeed` (C `0xe903d0`, S `0x937510`), `recipe.duration` 0x38, `factory.recipe_start` 0x50 (also fixed) |
| `@stations/FactorySpeed.fields` | | | `balancing.settings` 0x90, `settings.factory_speed` 0x3C |
| `stations/FactoryStart` | `0x32def0` | `0xee0a0` | prologue (events, optional) |
| `stations/FactoryComplete` | `0x32dab0` | `0xedc60` | prologue; `@…craft` captures `craft.outputs_only` 0x10 |
| `stations/FactoryInfoInvalidate` | `0x4a6350` | `0x23ec60` | captured from the dispatch |
| `stations/StationWrite` | `0x2c0c60` | `0xabf70` | captured (evidence; Kiln uses its own `WriteOther`) |

Fixed constants (reflection, not code): `factory.size` 0x68, `state` 0, `running_recipe` 4, `running_selections` 8,
`pause_duration` 0x58, `missing` 0x62, `basesys.record0` 0x30. Features: `stations.read`, `stations.fill`,
`stations.control`, `stations.events`. Existing names reused, not redefined: `items/*` transaction, `world/*`,
`gameplay/CraftRecipe`, `gameplay/ProgramTime`, `content/RecipeFind`.

The build records (`sigdb/builds/{client,server}/<sha>.json`) get the new service at level `static` when the file lands;
`lab` after the lab tests (section 11).

---------------------------------------------------------------------------------------------------------------------


---------------------------------------------------------------------------------------------------------------------

## 7. Lab results [lab]

Lab dedicated server 26430253 (a copy of the standard lab save: one base, altar 1025, no player), 2026-11-16, test mod
`mods/tests/station-actions-test` (kiln.log lines `station-actions-test: PASS|FAIL`). Run 3: 62 PASS, 1 FAIL (the altar's
`EntityData.IsPersistent`, below, since an INFO line); run 4 after a SIGINT stop and a start: 4 PASS, 0 FAIL.

- **Bases** (L1): the running core resolves `stations.json` like the offline check ("stations: reads on, fill on, control
  on, events on"). `PlayerBasesSystemServer`'s list A holds a placed Magic chest and not a plain chest, a few steps after
  the placement, and still after the whole run (some thousand steps, no player). The base's id is its altar's own
  `BaseIdComponent` id. The lab save's base has Magic chests of its own (entity 1063 among them): the game's fill takes
  from them in list order, so counts are the base's, not one chest's. `EntityData.IsPersistent` is **false for the flame
  altar** and true for a placed kiln and Magic chest: per-base data is kept by the altars' ids.
- **Fill** (L2): up to 5 from 0 moves 5 crafts' logs (12 a craft) and dirt (3) from the Magic chests in one
  transaction; the ledger names only stock containers (a chest's take comes from its inventory entity: the chest 2542 took
  from 2543); add 2 makes 7; up to 7 is unchanged; an exact fill beyond the stock moves nothing (the game's code 4); a
  reserve leaves exactly the reserve in the base; a dry run moves nothing; a plain chest's logs are untouched.
- **A stopped station that receives inputs** (a fill on a stopped kiln, MissingMaterials with the stop flag set) starts a
  cycle in its next update (the inputs leave) and the stop aborts it in the update after (the inputs come back): for one
  step the station reads Running, one craft less loaded. The game's own fill button leads to the same.
- **Water** (L3): a well (`Prop_NPC_Water_Well_a`) filled with water is in list B; a seedling bed's water recipe
  (1 charge) filled for 2 crafts pours 128 units (64 a charge). With the items for 2 crafts in and the water taken out,
  the station holds 0 complete crafts (water counts) and a fill up to 2 pours the water.
- **Stop and resume** (L4): stop now mid-cycle aborts it and the inputs are back (11 loaded while running, 12 after);
  stop after the current craft waits through the cycle (120 s here), writes the stop in the step between two cycles
  (state NoRecipe), the next cycle the game starts in that step is aborted at once, and nothing is lost (10 charcoal, one
  craft fewer loaded). A waiting stop is withdrawn by a stop now (`Canceled`, nothing written); a second one is
  `Unchanged`. The stop flag, the selection and 3 crafts' inputs came back after a SIGINT stop and a start.
- **Pause** (from the code, not run): `factory_station`'s resume from Paused sets `recipeStart := now -
  recipePauseDuration` (S `0x110f55`..`0x110fd8`): `recipePauseDuration` holds how long the cycle had run when it paused.
  So a paused cycle's progress is `pause / duration`, and the start moves at a resume (a stop waiting for the cycle's end
  must not take that for a new cycle).
- **Events** (L5): one `StationCraftedEvent` and one successful `ItemCraftedEvent` per cycle (the craft phase flag);
  the state events of Kiln's stop and resume name the mod; `StationPlacedEvent` for a placed kiln; a subscription made in
  `OnLoad` is accepted.
- **Select**: the charcoal recipe was selectable (the world has it); a blast furnace's recipe (0xf362723f) was refused
  with "no one on this server has unlocked" (the world and 0 players checked) and selected with `allowLocked`.
- **No player** (L6): all of the above ran with no player connected. **Use all options** (L7) was not run. **Cost** (L8):
  the managed reads of 14 stations took 0.6 ms (first call, JIT included); the native per-step diff was not timed apart.

---------------------------------------------------------------------------------------------------------------------

## 8. Station outputs as crafting stock (GitHub #68) [dis][cmp][sig]

Read 2026-11-17 on the same builds (server 26430253 = sigdb f0d824be, client 26430625 = sigdb 5d9ef2f1), static.

**How the fill takes (§3.1, in detail).** `AutoTransferRecipe` (S `0x198d50`, C `0x3f7ab0`) walks the resolved input
stacks (24 B each: `+0x10` item, `+0x14` count, read together as one qword at S `0x199240`; stride `0x18` at
S `0x1993ce`). Per input, after `need`: the player first, then every u32 entity of list A in order (loop S
`0x1992f4`..`0x199331`, C `0x3f8054`..), each through **`PayMaterial(out, tx, entity, item, missing)`** (S `0x1b0d60`,
C `0x411330`; the game's `removeItems` with no preferred slot, already `gems/PayMaterial`): it takes only stacks of
the asked item, from every slot of every link of that entity. Still missing: code 4 (a build constant check, S
`0x95b720`, returns false). Then `InventoryAddItems` (S `0x1b1f60`) into the station; water last through
`AutoTransferWater(tx, stock, station, units, player)` (S `0x1994a0`, C `0x3f8200`) from list B. `have[i]` reads the
station's inventory live, so writes earlier in the same transaction count. The caller commits on 0 and rolls the whole
transaction back on any code.

**So a source is per item.** A list-A entity gives only stacks of the item asked. A production station in the list is
safe **for item X** exactly when every stack of X in it is an Output: X an output of its selected or running recipe and
no input of either (all of a category input's options count). Its queued inputs (input items), its leftovers (neither)
and an output that is also an input (an input item) are then out of reach by construction. No hook is needed: Kiln
already passes its own `stock` to the function for `Stations.Fill` (§3.2).

**What Kiln does (since 0.2.0-dev.29.17, feature `stations.output_sources`).** Stations are opted in one by one
(`set_output_source`; managed: `Stations.SetOutputSource`, kept beside the save). In `kiln_items_stations`, at the
transaction, for a fill of n crafts on top of `loaded`:
1. the opted-in stations of the fill's base (the game's own `BaseAtPosition`), the target excluded, are read through the
   program's accessors: their recipes now, their inventories now; for each resolved input item, the stations that lend
   it and how much;
2. `short_i = need_i - (player's bag + chests after the reserves)` where positive (the player and the Magic chests pay
   first);
3. per input with a shortfall: `AutoTransferRecipe(tx, {lending stations of item i, in entity order}, [stack_i with count
   1], target, crafts = short_i, water 0, player 0)`: with count 1 the game's `loaded` is what the target holds of the
   item, so its `need` is exactly `short_i`;
4. the game's own call for the rest: the base's stock, the real inputs, `crafts = n + loaded - loaded'`, the water and the
   player: every input ends where a plain fill of n would put it, the player and the chests paying what they would have;
5. any code: rollback, nothing moved.
A fill whose resolved inputs repeat an item, a fill with `KILN_STATION_FILL_NO_OUTPUTS`, and every fill while nothing is
opted in run exactly as before (one plain call).

**Not chosen.** (a) A hook on `PayMaterial` or the list loop with a per-source filter: possible, but it is a shared
payment function (gem payment and others), and Kiln adds no hook on the fill path (§5). Collect-on-demand (move the
needed outputs into a Magic chest first, then fill): two commits, not all or nothing, and it needs a chest with room.

**The station screen's button.** The server's `player_crafting` handles it with the **player's** base (§3.1). Kiln does
not hook it; Kiln.Blocks follows the held client action (`PlayerClientActionEvent`, docs/kiln/services/player-state.md)
while a station is opted in, and runs a fill the player and the chests cannot cover but the outputs can as its own fill
(same recipe, choices and amount, the player's bag first, all or nothing), canceling the game's.
**Client [hyp]:** the client runs `AutoTransferRecipe` as a dry run on its own view of the base (C `0x2be77f`, rolled
back at C `0x2be78d`) and keeps a status per entry (C `0x2be816`..`0x2bea65`: 1 ok, 2..5 the game's codes). Whether a
"missing materials" status greys the button or stops the client from sending the action was not traced: a real-client
check. If it does, the button reaches the server only when the chests alone cover the fill, and station outputs at the
button need a client half (phase 2).

Symbols: `@stations/AutoTransferRecipe.take` (scoped to `stations/AutoTransferRecipe`, three patterns) captures
`stations/stack.item` = 0x10, `stations/stack.size` = 0x18 and `gems/PayMaterial` from the list loop, on both exes;
feature `stations.output_sources` (`server.d/stations.json`).

**Lab (2026-11-17, server 26430253, `mods/tests/station-outputs-test`, standard lab save, no player): 32 PASS, 0 FAIL,
and 3 PASS on the run after a SIGINT stop and a start.** A T1 forge's recipe (2 charcoal + 2 metal scraps) filled for 2
crafts took its 4 charcoal from an opted-in T1 charcoal kiln's outputs, the kiln's 36 queued logs stayed, the Magic chests'
charcoal (all reserved) was untouched, the ledger names the kiln's slot; with one craft's charcoal let through by the
reserve the chests gave 2 and the kiln 2; with the chests free the kiln gave nothing; an exact fill beyond both moved
nothing; the kiln's leftover (metal scraps) and a kiln with no recipe lent nothing; a kiln in a second base (an altar
placed 565 m away made the game's second base) was never used; a dry run and `UseStationOutputs = false` behaved as
documented; the opt-in came back after the restart. Scanning all 31 production station templates: **no recipe has an
item on both sides** (inputs and category options against outputs), so "an output that is also an input" is a guard
the game's data does not exercise today (host test only). `station-actions-test` on the same build: 64 PASS, 0 FAIL,
then 4 PASS, 0 FAIL after the restart.

## 9. Ordinary chests as a named fill source

Same call as §8, with one more source list. An ordinary chest is a placed storage container in the station's base that
the game's list A does not contain (no `InventoryCraftingStock`: not a Magic chest and not a refilling plant) and that
is not a production station. `PayMaterial` takes stacks of the asked item from whatever entity it is given, so Kiln
passes only those chests, and only for the amount the player and the Magic chests do not cover.

For a fill of n crafts the shares are, per input: the player's bag and the Magic chests (after reserves) first, then
the named chests, then opted-in station outputs. Inside the one transaction Kiln asks `AutoTransferRecipe` for the
chest share (source list = those chests, one stack of count 1, crafts = the share, player 0, water 0), then for the
output share as in §8, then for the remainder with the base's own stock, the real inputs, the water and the player.
Any code rolls the transaction back. A request that names no chests is the §8 fill. The station screen's button does
not name chests, so it is unchanged.

## 10. Station outputs in a craft

GitHub #68 step 2. Same two builds as §8: server **26430253** (`f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2`,
svn 1287369) and client **26430625** (`5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77`). Image base
`0x140000000`; the numbers below are RVAs. Static only. `rules/InventoryPay` was also decompiled with Ghidra 12.1.2
through `rea` 6.7.0 (evidence `ev_750028c865fbccdf76fe105c80640bbd9eb1426d6efffb10c89024d4351839f8`, subject the server
sha256 above). The pseudocode is the decompiler's, not the game's source. The other sites are disassembly.

### 10.1 How InventoryPay builds its sources

A craft that is only short of materials does enter `InventoryPay`. `RecipeApply` returns 4 at S `0x19a208`, before any payment, when `0x91de90` returns 0. That function compares the recipe's workshop (`+0x24`) with the station's, then `requiresSheltered` (recipe `+0xa1` on this build: skipped when that byte is 0, otherwise the context byte `+0x1d` must be set and `+0x1c` must not be 0), then a level (recipe `+0x8c`), then three requirement words (recipe `+0x9c`, `+0x9d`, `+0x9e`: level, comfort, server progress). It does not read an inventory. A props mismatch returns 5 at S `0x19a228`, still before payment. Being short of an item is `InventoryPay`'s own 4, tested at S `0x19a293` and taken to the failure path at S `0x19a41a`. A player's or a request's craft (`handle_crafting_events`) sets the transaction's stock mode to 2 at S `0x8b226`: the owner, then the base's chests. `BaseStockForPlayer` then requires permission bit 0x02 (`CanAccessInventories`) at S `0x2081fb`; without it, it writes error 0x16 at S `0x208209` and Kiln leaves the payment to the game. A Magic chest is not a player, so a dedicated server with nobody joined cannot take from a station this way. A factory's start (`TryStart` S `0xe4330`) sets the inputs-only flag `0x08` at S `0xe43a7` and does not set that mode.

`rules/InventoryPay` is S `0x199c80` (returns at `0x199f05`). `rules/RecipeApply` (S `0x19a1d0`) calls it at S `0x19a287`;
the return address is `rules/RecipePaid` S `0x19a28c` (the sigdb pattern already on both builds; client `0x3f8fec`).
The fifth argument, when non-null, replaces the cost line's item. Each cost line is 0x18 bytes, item at +0x10, count
at +0x14. `tx+0xb1` is the stock mode.

Mode 0 pays the owner only, one `gems/PayMaterial` (S `0x1b0d60`) per line, and returns 4 when a line is short.
Any other mode calls `stations/BaseStockForPlayer` (S `0x2081c0`) **once**, then walks the lines. Mode 2 pays the
owner before the list; mode 1 pays the owner after it. The list is one list for every line. `PayMaterial` takes only
stacks of the asked item, but a station put on that shared list is asked for every line, so a line whose item is also
queued in the station would take those inputs. Fill's option (b) (a different entity list per item, passed into
`AutoTransferRecipe`) is not available here: `InventoryPay` builds the list itself and does not take one.

A take before the original call does not work either. The original still tries to take the full cost from the owner
and the chests. When they can pay, both takes would run. When they cannot, the original returns 4 and `CraftRecipe`
(S `0x210f00`) rolls the transaction back at S `0x2112d4` (`rules/InventoryRollback` S `0x1b8310`), which undoes the
earlier take, so the outputs would never actually pay.

Two more facts of the decompile. `InventoryPay` does not test `BaseStockForPlayer`'s error byte: the stock list is
zeroed before the call, so on 0x16 or 0x1f mode 2 pays from the owner alone. And the test it makes before returning 4
for a short line, `0x95b720`, is `xor al,al; ret` on this build (a compiled-out cheat): a shortfall is always 4.

The permission test inside `BaseStockForPlayer` is `0x163040(out, player, [bases+0x10])` (S `0x2081f6`): it answers 0
for any id outside 1..1023 (`cmp eax,0x3fe; ja`), otherwise the byte at `+0x9d` of the 0xa0-byte record `player-1` of
the table at `[bases+0x10]` (BasicPlayerInfos, `docs/re/gameplay.md`), whose first dword must be the id. So only a
logged-in player can pass it: a character spawned by a lab (an id from 1024 up) and a Magic chest never do.

**What Kiln does.** Only when the return address is `RecipePaid` (not `RecipePayAlt` S `0x19a9b0`, which `RecipeApply`
calls instead when `tx+0xb0` has bit 0x40, and not any other caller), on the server world's own transaction (§10.4), when
the mode is not 0, and `BaseStockForPlayer` succeeds (its error byte stays 0; 0x16 and 0x1f are the permission and the
"no base" the game already returns), and the opted-in outputs cover every line's shortfall after the owner and the
chests are counted in the game's order: Kiln pays the whole recipe with `PayMaterial` inside the same transaction (owner
and chests first, then the lending stations for that line's remainder) and returns 0 or 4 without calling the original.
A fault or a remainder returns 4 so `CraftRecipe` rolls back. When the outputs cannot cover, or a line's item repeats,
or the mode is 0, or nothing is opted in, or the stock list is longer than 4096 entries, the original runs unchanged.
The decision and the loop are `stations_logic.h` (`craft_order`, `craft_lenders`, `craft_pay`) and are host-tested.

Only players' crafts reach this path. A production station's own cycle (`factory_station` -> `TryStart` S `0xe4330`)
pays with stock mode 0, and a request whose operator is not a player fails the permission test above, so neither ever
takes from another station.

The station being crafted at is `CraftContext+4`, noted by kiln.gameplay's `CraftRecipe` hook for the duration of the
call (`native/services/gameplay/craft_phase.h`: a factory crafting into itself has operator == station, and player
entities are 1..1023). That hook group is installed on demand; kiln.stations asks for it (`request_hooks`,
`KILN_GAMEPLAY_HOOK_CRAFT`) once a station is opted in. Until it is in, only a non-player owner is left out. A player
crafts at a crafting station (a Crafting component), and only a station with a FactoryStation lends, so this matters
only for an entity that has both.

### 10.2 How the client counts a recipe

`ui/CraftingMenu` is C `0xb80130`. `rules/client.CraftableAmount` (C `0xb2ddd0`) is a cache read at the game UI data
+0x14f70, keyed by the recipe, the cap and the selections; it returns the cached count or 0 until the cache is filled.
The crafting screen stores the cap-1 count on the selected entry at +8 (the call at C `0xc79652`). The hints path
(C `0xd21120`) receives the cap-10 count. At C `0xd2127a` the screen compares that entry's count with 1 and, when it
is not 1, sets the bit that disables the action. The handler C `0xd13120` (called from the screen at C `0xc79a63`)
queues a craft (`UiQueueCraftRecipe` C `0xd6e460`) only for an action whose activation byte is set: type 0 crafts 1,
type 0x1b crafts 10. So the client does not send the one-craft when its own count is not enough.

The cache is the client's view of the replicated chests and the bag. It does not include a station. Kiln's existing
`CraftableAmount` hook (kiln.rules, installed for `rules.craft_ui` and, now, for `stations.craft_ui`) returns the
greater of the game's count and a count from a snapshot the server pushed. The snapshot is not authority.

The count needs the recipe's resolved inputs (`content/RecipeFind`, `stations/ResolveRecipeInputs`, both on the client
build: `sigdb/symbols/client.d/stations-craft-ui.json`) and a `g38_items::RecipeRegistry`. A joined client has no server
world, so the registry is the client world's own singleton, read on the client world's thread (its Pre) and kept for the
UI thread. The client world registers it: kiln.content's client pass already reads it there on a joined client
(`native/services/content/content.cpp`, `on_client_pre`).

### 10.3 C7, the station screen's fill button

The dry run is inside client `FUN_1402b8430` (Ghidra 12.1.2, `rea` 6.7.0, evidence
`ev_44de36cc918e87a4a86e5e7d08c29ddc33e2debbd5cc1b00b49705dc3857b8b2`). It calls `AutoTransferRecipe`
(`FUN_1403f7ab0`) and then `FUN_140418150` before it breaks out of the case: the disassembly of that site is the call
at C `0x2be77f` and the rollback at C `0x2be78d`. The decompiler does not keep the result through the rest of the
function; the status stores below are from the disassembly. The call at C `0x2be77f` keeps the code in `edi` and always
rolls back at C `0x2be78d`. Status 0 is stored as 1 (ok) at C `0x2bea65`. Codes 2, 3, 4 and 5 are stored as themselves
(C `0x2be836`..`0x2be87e`); 4 is missing materials. The station screen reads that dword at C `0xbda1fd`. At
C `0xbda219` it compares with 1: the byte it then stores at `[rbp+0x520]` (C `0xbda22d`) is 1 only for status 1 (and
two other conditions, `r12b` and `[r13+0x218] == 0`), else 0. That byte is the enabled flag the widget calls get
(C `0xbdaa90`, `0xbdaac8`). At C `0xbdab1c` a status other than 1 selects the missing-materials style (`rbx+0x4b90`).

So the button is **disabled**, not only restyled, when the bag and the chests alone cannot cover the fill: a fill only
station outputs can complete cannot be pressed there, and the server's handling of that press (step 1) is reached only
if something else sends it. The hotkey poll C `0xd68510` does not receive the status; whether the fill hotkey still
sends the press was not shown by the static trace. Kiln does not hook this screen: enabling the button for Kiln clients
would need the fill's own plan (the station's loaded inputs, the amount on the slider) on the client, which the craft
snapshot does not carry. `Stations.Fill` from a mod and crafts use the outputs; the button does not. C7 checks the hotkey.

### 10.4 Which transaction, which thread

`tx+0x68` is the bases system the transaction holds. `InventoryPay` passes it as `BaseStockForPlayer`'s third argument
(r8); there it is kept in `rbp` (S `0x2081d7`), its `+0x10` is the permission table (S `0x2081e9`), it is `BaseAtPosition`'s
system (S `0x20821b`..`0x208234`), and the base records it indexes (`rbp + index*0x130 + 0x30`, S `0x208256`) are
`PlayerBasesSystemServer`'s (stride and offsets as `stations/bases.*`). `handle_crafting_events` puts the bases system it
holds into its setup at S `0x8b23b` (`[rbp+0x148]`, setup+0xD8 for the setup at `rbp+0x70`), which the transaction keeps
at `+0x68`. Kiln acts only when that word is the server world's `PlayerBasesSystemServer` (read by the tick thread every
Pre) and kiln.items' `access_from_tx` accepts the transaction (its job's storage is the server world's: storage kind 0).

The client's own `RecipeApply` dry run (C `0x2be03a`, inside the station screen's status function `FUN_1402b8430`, stock
mode 2 set at C `0x2bdd01`, rolled back at C `0x2be048`) also returns to `RecipePaid` (C `0x3f8fec`). On a hosting Kiln
client with a station opted in, the payment hook sees it. Its transaction is the client world's: its bases word is not the
server's and its storage kind is 2, so Kiln leaves it to the game (counter `other_world`).

Thread. On a dedicated server the game's programs, `handle_crafting_events` among them, run on the tick thread
(docs/kiln/services/world.md). In a hosted game the server world's programs may run on a game worker thread. Kiln does
not rely on either: inside the payment it calls nothing of kiln.world (tick thread only) and not the core's singleton
lookup. It reads inventories through the transaction's own accessors (`ReadOther`, the view `PayMaterial` takes from),
a lender's `FactoryStation` and `CurrentTransform` through the game's `GetComponent` under an exception guard (as
`read_inventory` does), the base through `BaseAtPosition` on the transaction's bases system (what `BaseStockForPlayer`
itself calls), and the recipe registry and the server's bases system from what the tick thread cached. Writes from
other programs at the same time: the craft program holds write access to `Inventory` (its transaction's accessors);
`factory_station` and `player_crafting`, which write `FactoryStation`, also write inventories (a cycle's inputs and
outputs, a fill), and so does Kiln's `kiln_items_stations`, so the scheduler does not run them beside it; a station's
`CurrentTransform` does not change. That last part is reasoning from what those programs do, not a traced access list.

### 10.5 What a Kiln client is sent

While a Kiln client's crafting menu is open (any `ScreenKind.Crafting`: the player's own, a crafting station's, an NPC's,
a production station's screen), it says so on `kiln:craftstock` (`required: false`) every 5 s and when the menu closes.
For such a player, standing in a base where they have `CanAccessInventories` (the test the payment relies on), the
server sends per item the bag, the base's Magic chests and the opted-in stations' Outputs, only for the items of the
recipes that take an item those stations lend (native `craft_items`, an index of every recipe's inputs and category
options from the `RecipeRegistry` map, `content/recipes.*`), at most 1024 rows, again when a count changes (every 30
steps it looks). A vanilla client never says its menu opened and is sent nothing.

### 10.6 Lab (dev29-sc2)

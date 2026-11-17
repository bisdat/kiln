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

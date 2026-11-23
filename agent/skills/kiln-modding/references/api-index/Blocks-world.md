# Blocks (Kiln.Blocks): world

## BaseAltar `0.2.6` — One flame altar of a base: its entity (this session) and its own id (the GUID the game keeps in the save with the altar; it never changes).
- `BaseAltar()`
- `BaseAltar(EntityId Entity, BaseId Id)`
- `Entity { get; init; } -> EntityId` — The altar's entity this session.
- `Id { get; init; } -> BaseId` — The altar's own id: the GUID the game keeps in the save.

## BaseId `0.2.6` — A base's id: the GUID the game gives a flame altar (its `BaseIdComponent`, kept in the save).
- `BaseId()`
- `BaseId(Guid Value)`
- `IsNone { get; } -> bool` — True for `BaseId.None`.
- `static None { get; } -> BaseId` — No base.
- `Value { get; init; } -> Guid` — The GUID.

## BaseIdChange `0.2.6` — A base whose id changed when the game rebuilt its bases: an overlapping altar came or went (`BasesChangedEvent.Changed`).
- `BaseIdChange()`
- `BaseIdChange(BaseId Old, BaseId New)`
- `New { get; init; } -> BaseId` — The base's id after the rebuild.
- `Old { get; init; } -> BaseId` — The base's id before the rebuild.

## BaseInfo `0.2.6` — One of the game's bases as its crafting code sees it: the union of overlapping flame-altar zones, with the crafting-stock containers the game lists for it (the Magic...
- `BaseInfo(BaseId Id, int Index, IReadOnlyList<BaseAltar> Altars, WorldPosition Min, WorldPosition Max, int StorageCount, int WaterCount, int ZoneCount, int StationCount)`
- `Altars { get; init; } -> IReadOnlyList<BaseAltar>` — The flame altars whose zones make the base (more than one when their zones overlap), each with its own id: keep...
- `Id { get; init; } -> BaseId` — The base's id (see `BaseId`).
- `InBox(WorldPosition p) -> bool` — Whether `p` lies in the base's box (the game also checks the altar zones inside it: `Bases.At` answers exactly).
- `Index { get; init; } -> int` — The game's record index this session (0..15): it changes when the bases are rebuilt.
- `Max { get; init; } -> WorldPosition` — The opposite corner.
- `Min { get; init; } -> WorldPosition` — The corner of the base's box with the smallest coordinates.
- `Name { get; } -> string?` — The name a player gave the base's (first) flame altar; null when none.
- `StationCount { get; init; } -> int` — Production stations standing in the base (as of the last scan).
- `StorageCount { get; init; } -> int` — Crafting-stock containers the game lists for the base now.
- `WaterCount { get; init; } -> int` — Water sources holding water.
- `ZoneCount { get; init; } -> int` — Altar zone boxes merged into the base.

## Bases `0.2.6` — The game's bases as its crafting code sees them (since 0.2.0-dev.29.14): each is the union of overlapping flame-altar zones, with the crafting-stock containers the...
- `static All { get; } -> IReadOnlyList<BaseInfo>` — Every base now, in the game's record order.
- `static At(WorldPosition position) -> BaseInfo?` — The base a position lies in (the game's own lookup); null when none.
- `static Available { get; } -> bool` — True when kiln.stations reads here (a server world, stations.read on).
- `const Capability = "stations.read" -> string` — The capability row of the reads.
- `static FindRoomFor(BaseId id, ItemId item) -> EntityId?` — A crafting-stock container of the base that takes deposits (`Bases.Storage` with deposit) for `item`: one that holds...
- `static Get(BaseId id) -> BaseInfo?` — The base with this id; null when none has it now.
- `static Of(EntityId entity) -> BaseInfo?` — The base the entity stands in (the game's own lookup by position); null when none.
- `static Stock(BaseId id) -> IReadOnlyDictionary<ItemId, long>` — What the base's crafting-stock containers hold, per item; empty when no base has this id now (as `Bases.Storage`).
- `static Stock(BaseId id, ItemId item) -> long` — How many of `item` the base's crafting-stock containers hold.
- `static StockWithStations(BaseId id) -> IReadOnlyDictionary<ItemId, long>` `0.2.7` — `Bases.Stock` plus the finished items still in the base's production stations: each station's `StationInfo.Outputs`...
- `static Storage(BaseId id, bool deposit = false) -> IReadOnlyList<EntityId>` — The base's crafting-stock containers, in the game's order (the order its fill takes from them): the Magic storage...
- `static TryGet(BaseId id, out BaseInfo? info) -> bool` — The base with this id, when one has it now.
- `static WaterSources(BaseId id) -> IReadOnlyList<EntityId>` — The base's water sources that hold water (wells, water barrels), in the game's order; empty when no base has this id...

## BasesChangedEvent `0.2.6` — The game rebuilt its bases (a flame altar placed, removed or upgraded): base indices may have changed, and a base's id changes when an overlapping altar comes or goes.
- `Added { get; } -> IReadOnlyList<BaseId>` — Bases that share no altar with a base before.
- `Changed { get; } -> IReadOnlyList<BaseIdChange>` — Bases whose id changed: old and new share at least one altar (two bases that merged both map to the new one; a base...
- `Count { get; } -> int` — The number of bases now.
- `Kept { get; } -> IReadOnlyList<BaseId>` — Bases whose id is the same before and after (their altars may have changed).
- `Removed { get; } -> IReadOnlyList<BaseId>` — Bases before that share no altar with a base now.

## BedBlock — A bed: the game's sleep and rest (a player who sleeps in it gets the game's rested status, by the comfort around them), and comfort of the bed category.
- `Lies { get; init; } -> SpotBlock?` — Where the player lies.
- `SkipsNight { get; init; } -> bool` — When every player in the world sleeps at night, in beds that say this, the morning comes.
- `SpawnPoint { get; init; } -> bool` — A player who dies comes back at the last of these beds they slept in, instead of at the flame altar.

## BlueprintBlock — A new shape on the game's building materials: in the build menu beside the walls and floors, and uses their materials unless MaterialPool selects an opt-in pool.
- `Cells { get; init; } -> IReadOnlyList<IReadOnlyList<string>>` — The shape, half a metre a cell: layers from the bottom up; in a layer, rows from the back (+z) to the front; in a...
- `Cost { get; init; } -> int?` — Not used.
- `Description { get; init; } -> string?` — The shape's description.
- `MaterialPool { get; init; } -> PartRef<BuildingMaterialPoolBlock>?` `0.2.4` — An opt-in custom pool.
- `Name { get; init; } -> string` — The shape's name, as players read it in the build menu.
- `Recipe { get; init; } -> RecipeBlock?` — How the shape is made.
- `Roof { get; init; } -> bool` — A roof shape: it cycles through the roof materials, not the walls'.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the blueprint item: `Tweak = { { GameFields.Items.MaxStackSize, 5 } }`.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## BoxCollider — A collision box, in metres, around the model's origin as the model is drawn.
- `Center { get; init; } -> Vector3?` — Where its centre is.
- `Size { get; init; } -> Vector3` — Width, height and depth.

## Breaks — Which of a tool's class modifiers applies when it digs a material.
enum: None, Wood, Stone, Metal

## BuildingMaterialBlock — A new building material: an id and a GUID in the game's tables, its strength, and a block item the game's shapes (walls, floors, stairs, columns) can list.
- `Description { get; init; } -> string?` — The block item's description.
- `Icon { get; init; } -> string?` — The block item's picture: a PNG in the mod's folder, or `item:<name>`.
- `Name { get; init; } -> string` — The block item's name, as players read it.
- `Recipe { get; init; } -> RecipeBlock?` — How the block item is made.
- `Roof { get; init; } -> bool` — It's a roof material: the roof pieces list it, not the walls.
- `Strength { get; init; } -> PartRef<StrengthBlock>?` — What it takes to dig it.
- `Surface { get; init; } -> PartRef<SurfaceBlock>?` — How it looks.
- `Was { get; init; } -> string?` — The block's old name, after a rename: the material's id comes from it, so its walls stay in every world.

## BuildingMaterialPoolBlock `0.2.4` — A boot-time pool of 1..64 building-material items for opt-in new blueprint shapes.
- `Materials { get; init; } -> IReadOnlyList<BuildingMaterialPoolEntry>` — One to 64 distinct live building-material block items, in selector order.
- `Name { get; init; } -> string` — The declaration's readable name.
- `Roof { get; init; } -> bool` — Uses the roof equipment slot; every member and bound shape must agree.
- `Was { get; init; } -> string?` — The old key after a rename.

## BuildingMaterialPoolEntry `0.2.4` — One selector entry.
- `BuildingMaterialPoolEntry(PartRef<BuildingMaterialBlock>? Material = null, string? GameMaterial = null)`
- `GameMaterial { get; init; } -> string?` — A building material of the game, by its catalog name.
- `Material { get; init; } -> PartRef<BuildingMaterialBlock>?` — A building material of this mod by its name, or `"other-mod:name"`.

## CategoryChoice `0.2.6` — For a recipe input "any item of category X", the option chosen (the game's `RecipeInputCategorySelection`).
- `CategoryChoice()`
- `CategoryChoice(uint Category, int Index)`
- `Category { get; init; } -> uint` — The ingredient category's id.
- `Index { get; init; } -> int` — The option's index in the category (`IngredientCategory.Options`).

## CollectWhat `0.2.6` — What `Stations.Collect` moves.
enum: Outputs, Leftovers, All

## ComfortBlock — What the game's comfort counts, for whoever rests within range.
- `Amount { get; init; } -> int` — How much comfort it counts, a whole number.
- `Category { get; init; } -> ComfortCategory` — Only the best of a category counts near a player, as the game's rule goes.
- `Range { get; init; } -> double` — Metres.
- `Warmth { get; init; } -> int` — Resistance to the cold near it.

## ComfortCategory — The game's comfort categories.
enum: Hearth, Bed, Chair, Table, Bench, Throne, Bathtub, Sink, Toilet, Shower, Carpet, Illumination, Flame, Trophy, Stool, Kitchen, Wellness

## ContainerBlock — Something that holds items, as the game's chests do.
- `DefaultLoot { get; init; } -> PartRef<LootTableBlock>?` — What it holds when it is placed: a `LootTableBlock` of this mod by name (`"other-mod:name"` for another's), or one...
- `Lid { get; init; } -> LidBlock?` — How its lid moves.
- `Slots { get; init; } -> int?` — Inventory slots, 1..120.

## Crafting `0.2.15` — Player crafting sources for station, NPC and inventory crafting.
- `static AddSource(EntityId player, EntityId container, CraftSourceOptions? options = null) -> CraftSourceHandle` — Registers a container for this player, in any base or none.
- `const SourcesCapability = "crafting.sources" -> string` — Server payment through the recipe transaction, with player-bound linked storage.
- `const SourceUiCapability = "crafting.source_ui" -> string` — A Kiln client's crafting screens count the server's linked-storage snapshot.

## CraftSourceFilter `0.2.15` — How `CraftSourceOptions.Items` is interpreted.
enum: Deny, Allow

## CraftSourceHandle `0.2.15` — A session-only player crafting source.
- `Container { get; } -> EntityId` — The registered storage container, independent of its base.
- `Dispose() -> void` — Removes this registration, on any thread.
- `IsDisposed { get; } -> bool` — Whether this handle has been disposed.
- `Player { get; } -> EntityId` — The only player this registration can supply.

## CraftSourceOptions `0.2.15` — Options copied when a linked crafting source is registered.
- `Filter { get; init; } -> CraftSourceFilter` — Allow-list or deny-list interpretation of `CraftSourceOptions.Items`; default deny.
- `Items { get; init; } -> IReadOnlyCollection<ItemId>?` — The item set, copied eagerly on registration; null means an empty set.
- `Order { get; init; } -> CraftSourceOrder` — After the bag/base stock, before or after opted-in station outputs.

## CraftSourceOrder `0.2.15` — Where linked storage pays, after the bag and the crafting base's stock.
enum: BeforeStationOutputs, AfterStationOutputs

## DayPart — Day (the game's day phase, from 06:00 by default) or night.
enum: Day, Night

## DoorBlock — A door: it opens and closes for whoever uses it, and paths and water follow it, as the game's doors do.
- `Opens { get; init; } -> DoorSide?` — Which way it swings.
- `Sides { get; init; } -> DoorSide?` — Which side a player can use it from.

## DoorSide — A door's side: its front, its back, or both.
enum: Both, Front, Back

## FilledContainers — The containers of a world that were given their default loot: its template and spot.
- `Filled { get; set; } -> Dictionary<string, long>` — "template/x/y/z" (the position in 1/16 m cells) -> the step counter's time, for the record only.
- `Pending { get; set; } -> Dictionary<string, FilledContainers.FillProgress>` — A roll in progress.

## FilledContainers.FillProgress — A fill in progress for one container: the stacks a loot table rolled and how far giving them has got.
- `ItemGuids { get; set; } -> Dictionary<string, Guid>` — Canonical item identities captured before a grant or save.
- `Lost { get; set; } -> int` — How many items did not fit in the container.
- `Next { get; set; } -> int` — The index in `FillProgress.Stacks` of the next stack to give.
- `Stacks { get; set; } -> List<LootStack>` — The stacks the loot table rolled for this container.

## FillInput `0.2.6` — One input of a fill plan: what one craft needs, what the station holds, what the base's stock can give (the Magic chests after the reserves, and the named player's bag).
- `FillInput()`
- `FillInput(ItemId Item, int PerCraft, int Have, long Stock)`
- `Chests { get; init; } -> long` `0.2.9` — What the named ordinary chests hold of it, taken after `FillInput.Stock` and before `FillInput.Outputs`.
- `Have { get; init; } -> int` — How many the station holds.
- `Item { get; init; } -> ItemId` — The item.
- `Outputs { get; init; } -> long` `0.2.9` — What opted-in stations of the base lend of it (their finished items), taken after `FillInput.Stock` and...
- `PerCraft { get; init; } -> int` — How many one craft needs.
- `Stock { get; init; } -> long` — How many the base's stock can give: the Magic chests after the reserves, and the named player's bag.

## FillLimit `0.2.6` — What kept a fill below what was asked.
enum: None, Stock, Reserve, Room, Water

## FillMode `0.2.6` — How `FillOptions.Crafts` counts.
enum: UpTo, Add

## FillOptions `0.2.6` — Options of `Stations.Fill`.
- `Chests { get; init; } -> IReadOnlyList<EntityId>?` `0.2.9` — Ordinary chests in the station's base to take from after the Magic chests and before opted-in station outputs.
- `Choices { get; init; } -> IReadOnlyList<CategoryChoice>?` — The category choices for `FillOptions.Recipe`; null: the station's own.
- `Crafts { get; init; } -> int` — Crafts to fill for, 1..1000 (see `FillOptions.Mode`).
- `DryRun { get; init; } -> bool` — Run the game's function and roll back: the result says what would move.
- `Exact { get; init; } -> bool` — All of it or nothing; default false: as many crafts as the base's stock and the station's room cover.
- `Keep { get; init; } -> IReadOnlyDictionary<ItemId, long>?` — Reserves: at least this many of each item stay in the base's stock containers (up to 64 items).
- `Mode { get; init; } -> FillMode` — `FillMode.UpTo` (default) or `FillMode.Add`.
- `Player { get; init; } -> EntityId` — Also take from this player's inventories first, as the game's own button does.
- `Recipe { get; init; } -> uint?` — The recipe to fill for; null: the station's selected recipe.
- `UseStationOutputs { get; init; } -> bool` `0.2.9` — Also take, last, from the finished items of the base's stations that are opted in as crafting stock...
- `Water { get; init; } -> bool` — Pour the recipe's water from the base's water sources (default true).

## FillPlan `0.2.6` — What a fill would do now (`Stations.Plan`).
- `FillPlan(StationActionStatus Status, EntityId Station, uint Recipe, int CraftsLoaded, int CraftsAffordable, int CraftsPlanned, IReadOnlyList<FillInput> Inputs, string Message)`
- `CraftsAffordable { get; init; } -> int` — More crafts the base's stock covers (after the reserves; with the opted-in stations' outputs, since 0.2.0-dev.29.17).
- `CraftsLoaded { get; init; } -> int` — Complete crafts the station holds now.
- `CraftsPlanned { get; init; } -> int` — More crafts a fill with these options would ask the game for.
- `Inputs { get; init; } -> IReadOnlyList<FillInput>` — The resolved inputs (category choices applied).
- `LimitedBy { get; init; } -> FillLimit` — What keeps the fill below what was asked.
- `Message { get; init; } -> string` — Why, when nothing (or less than asked) would go in.
- `Ok { get; } -> bool` — True when something went in: `FillPlan.Status` is Done or Partial.
- `Reason { get; } -> string?` — Why nothing (or less than asked) would go in: the message, else the status; null when `FillPlan.Ok`.
- `Recipe { get; init; } -> uint` — The recipe.
- `ShortItem { get; init; } -> ItemId?` — The item short (`FillLimit.Stock`, `FillLimit.Reserve`); null otherwise.
- `Station { get; init; } -> EntityId` — The station.
- `Status { get; init; } -> StationActionStatus` — Done when `FillPlan.CraftsPlanned` crafts would go in; Partial when fewer than asked; otherwise why none would.
- `UseAllOptions { get; init; } -> bool` — The station has "use all ingredient options" on: its cycles may consume other options of a category than the one a...
- `WaterHave { get; init; } -> int` — Water in the station now (units).
- `WaterPerCraft { get; init; } -> int` — Water one craft needs (Fuel units, 64 a charge); 0 when the recipe needs none or the options pour none.
- `WaterStock { get; init; } -> int` — Water in the base's water sources (units).

## Hardness — How hard a material is: which column of a tool's or an explosion's damage applies to it.
enum: Soft, SlightlyHard, ModeratelyHard, Hard, VeryHard, Unbreakable

## IngredientCategory `0.2.6` — An ingredient category of the game's recipes ("any wood"): its options in the game's order.
- `IngredientCategory(uint Id, IReadOnlyList<IngredientOption> Options)`
- `Id { get; init; } -> uint` — The category's id, as `CategoryChoice.Category` names it.
- `Options { get; init; } -> IReadOnlyList<IngredientOption>` — Its options, in the game's order.

## IngredientOption `0.2.6` — One option of an ingredient category: an item, and how many of it count as one (the game's scaling: a craft needs `ceil(count x Scaling)` of it).
- `IngredientOption()`
- `IngredientOption(ItemId Item, float Scaling)`
- `Item { get; init; } -> ItemId` — The item.
- `Scaling { get; init; } -> float` — How many of it count as one: a craft needs `ceil(count x Scaling)` of it.

## InteractBlock — What happens when a player uses a placed thing.
- `Effects { get; init; } -> IReadOnlyList<EffectRef>?` — What happens to the player once the game has accepted the use.
- `OnRefused { get; init; } -> IReadOnlyList<EffectRef>?` — What happens to the player when `InteractBlock.Requires` refuses them.
- `Plays { get; init; } -> InteractPlay?` — What the player plays when they use it: one of the game's (`InteractPlay.Sit`, `InteractPlay.Sleep`, ...), or a...
- `Prompt { get; init; } -> string?` — A prompt of your own instead: the same text in every language.
- `Requires { get; init; } -> RequiresBlock?` — What it takes to use it.
- `Toggle { get; init; } -> ToggleBlock?` — Two states, and each use flips it: the game's toggle, as a door's.
- `Upgrade { get; init; } -> UpgradeAction?` `0.2.12` — The use raises an item's rarity by one step of a rarity upgrade rule (`RarityUpgradeBlock`), on the server once the...
- `Verb { get; init; } -> InteractVerb?` — The prompt's word: one of the game's (`InteractVerb.Open`, `InteractVerb.Sit`, ...), so every language has it.

## InteractPlay — What a player plays when they use something: one of the game's sequences, or any sequence by GUID (`new InteractPlay("mine", guid)`; in JSON the GUID).
- `InteractPlay(string Key, Guid Sequence)`
- `static readonly Chest -> InteractPlay` — Opens a chest and its inventory screen: `Interaction_InventoryUi_StorageChest`.
- `static readonly Drink -> InteractPlay` — `Interaction_Drink_from_Container`, a well's.
- `Id { get; } -> uint` — The game's id of the sequence: FNV-1a of its GUID.
- `static readonly Inventory -> InteractPlay` — Opens the inventory screen: `Interaction_InventoryUi`.
- `Key { get; init; } -> string` — The name JSON uses for it, such as `"sit"`.
- `static readonly Read -> InteractPlay` — `Interaction_Read_Lore`.
- `Sequence { get; init; } -> Guid` — The GUID of the sequence that plays.
- `static readonly Sit -> InteractPlay` — Sits down on the spot the seat says: `Interaction_Sit`.
- `static readonly Sleep -> InteractPlay` — Lies down and sleeps, the game's rest: `Interaction_Sleep`.
- `static readonly Talk -> InteractPlay` — `Interaction_Talk`.
- `static readonly Toggle -> InteractPlay` — A hand on it: `Interaction_Toggle`, a door's.

## InteractVerb — A prompt's word: the game's own, so every language has it.
- `InteractVerb(string Key, uint Id, string? Reference = null)`
- `static readonly Activate -> InteractVerb` — The game's `activate` prompt.
- `static readonly Close -> InteractVerb` — The game's `close` prompt.
- `static readonly Collect -> InteractVerb` — The game's `collect` prompt.
- `static readonly Craft -> InteractVerb` — The game's `craft` prompt.
- `static readonly Deactivate -> InteractVerb` — The game's `deactivate` prompt.
- `static readonly Drink -> InteractVerb` — The game's `drink` prompt.
- `static readonly Flip -> InteractVerb` — The game's `flip` prompt.
- `static readonly Harvest -> InteractVerb` — The game's `harvest` prompt.
- `Id { get; init; } -> uint` — The game's loca tag.
- `static readonly Inspect -> InteractVerb` — The game's `inspect` prompt.
- `Key { get; init; } -> string` — The name JSON uses for it, such as `"open"`.
- `static readonly Open -> InteractVerb` — The game's `open` prompt.
- `static readonly Read -> InteractVerb` — The game's `read` prompt.
- `Reference { get; init; } -> string?` — The game's GUID of the word, or null where the game keeps none.
- `static readonly Search -> InteractVerb` — The game's `search` prompt.
- `static readonly Sit -> InteractVerb` — The game's `sit` prompt.
- `static readonly Sleep -> InteractVerb` — The game's `sleep` prompt.
- `static readonly Talk -> InteractVerb` — The game's `talk` prompt.
- `static readonly Use -> InteractVerb` — The game's `use` prompt.

## LidBlock — A container's lid: the game's toggle animation, which the container's use flips.
- `Graph { get; init; } -> Guid?` — The animation graph that moves it, by GUID: a game graph (`kiln catalog graphs` lists them, with their states) or...
- `Moves { get; init; } -> bool` — False: nothing moves (a barrel, a pile of sacks, a model of yours with no lid): the toggle animation is taken off.
- `static readonly Open -> Guid` — The state the game's lids and doors play while open.
- `OpenState { get; init; } -> Guid?` — The graph's state while the lid is open, by GUID.
- `static readonly Shut -> Guid` — The state the game's lids and doors play while shut.
- `ShutState { get; init; } -> Guid?` — The graph's state while the lid is shut, by GUID.

## MaterialTableChangedEvent `0.2.4` — A committed material-table hash change, or a deferred pool definition.
- `Deferred { get; } -> bool` — A next-start definition changed; no live table capacity was consumed.
- `Groups { get; } -> IReadOnlyList<Guid>` — Affected pool GUIDs, copied at observation.
- `Materials { get; } -> IReadOnlyList<Guid>` — Affected material GUIDs, copied at observation.
- `NewHash { get; } -> string` — The committed table hash; unchanged for a deferred definition.
- `OldHash { get; } -> string` — The previously observed table hash.

## MaterialTweakBlock — Changes a game material in place: `Material = GameMaterials.StoneRough, Strength = new() { Hardness = Hardness.Unbreakable }`.
- `Material { get; init; } -> string` — The game material: a `GameMaterials` name, `Cell<id>`, or the material's GUID.
- `Strength { get; init; } -> PartRef<StrengthBlock>?` — What it takes to dig it.
- `Surface { get; init; } -> PartRef<SurfaceBlock>?` — New pictures for it: a layer written here replaces the game's, the rest stays.

## MenuBlock — The crafting menu a station opens.
- `Like { get; init; } -> StationKind?` — The station whose menu it opens.

## PlacementBlock — Where and how a player can place something.
- `Consumed { get; init; } -> bool?` — Placing it takes the item from the stack.
- `Fog { get; init; } -> bool?` — It can be placed below the Shroud's line.
- `InBase { get; init; } -> bool?` — Only inside a base, as the game's furniture.
- `On { get; init; } -> Surface?` — The surfaces it can be placed on: floor, wall, ceiling, or several.
- `Size { get; init; } -> Vector3?` — Metres, width, height and depth: the box that has to be free to place it, standing on its floor.
- `Snap { get; init; } -> bool?` — The cursor starts out snapping to the voxel grid.
- `Water { get; init; } -> bool?` — It can stand in water.

## ProcessingBlock — How a station works on its own.
- `GoesOutInRain { get; init; } -> bool?` — Rain puts it out.
- `Running { get; init; } -> RunningBlock?` — What shows while it runs.
- `Slots { get; init; } -> int?` — Inventory slots, inputs and outputs in one.

## PropBlock — Something placed that stands there: furniture, a banner, a lamp.
- `PropBlock(PropBlock original)`
- `Collider { get; init; } -> BoxCollider?` — What a player bumps into and what hits stop at: one box.
- `Colliders { get; init; } -> IReadOnlyList<BoxCollider>?` — Several boxes in place of one: a table's top and its legs, a doorway's two posts.
- `Comfort { get; init; } -> PartRef<ComfortBlock>?` — The comfort it gives to whoever rests within range, as a block or the name of one defined once.
- `Description { get; init; } -> string?` — The description of the item that places it.
- `Light { get; init; } -> VfxRef?` — A VFX on it for as long as it stands, which is where the game's lamps have their light.
- `Loot { get; init; } -> LootBlock?` — Where else the item that places it is found: chests, world pickups and drop tables.
- `Name { get; init; } -> string` — The name of the item that places it, as players read it.
- `OnInteract { get; init; } -> PartRef<InteractBlock>?` — What happens when a player uses it: the prompt, what they play, a toggle, what it takes, effects.
- `Placement { get; init; } -> PartRef<PlacementBlock>?` — Where and how a player can place it, as a block or the name of one defined once.
- `Provides { get; init; } -> bool` — A recipe elsewhere can ask for it nearby: `NeedsProps = ["cold-forge"]` in a `RecipeBlock`.
- `Recipe { get; init; } -> RecipeBlock?` — How the item that places it is made.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the item it makes: `Tweak = { { GameFields.Items.MaxStackSize, 5 } }`.
- `TweakPlaced { get; init; } -> FieldValues` — The same for the template it places: `TweakPlaced = { { GameFields.Templates.Shelter.CheckRange, 20 } }`.
- `Visual { get; init; } -> PartRef<VisualBlock>?` — What it looks like, as a block or the name of one defined once.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## PropRef — A prop that `PropBlock.Provides` a crafting prop, by handle or name: a block of this mod, or `"other-mod:name"`.
- `PropRef()`
- `PropRef(string Value)`
- `static implicit operator PropRef(BlockHandle handle) -> PropRef` — A prop by its handle.
- `static implicit operator PropRef(string value) -> PropRef` — A prop by its name in this mod, or `"other-mod:name"`.
- `Value { get; init; } -> string` — The prop's name: a block of this mod, or `"other-mod:name"`.

## RequiresBlock — When a player may use it.
- `Count { get; init; } -> int` — How many of `RequiresBlock.Item`.
- `If { get; init; } -> Func<Player, bool>?` — From code: your own check, on the tick thread.
- `Item { get; init; } -> ItemRef?` — An item in the player's bag: a `GameItems` constant or an item of this mod.
- `Time { get; init; } -> DayPart?` — Only by day, or only by night.
- `Unless { get; init; } -> IReadOnlyList<PlayerStateRef>?` — Not while the player is in any of these.
- `Uses { get; init; } -> bool` — The use takes `RequiresBlock.Item` from the bag, as a lockpick goes.
- `When { get; init; } -> IReadOnlyList<PlayerStateRef>?` — Only while the player is in one of these states.

## RunningBlock — What a station shows while it runs.
- `Sound { get; init; } -> SoundRef?` — A looping sound in place of its kind's: a cue of this mod (then `clients: mod`), `"other-mod:name"`, or a cue's GUID.
- `Vfx { get; init; } -> VfxRef?` — In place of its kind's fire or steam.

## SeatBlock — A seat: the game's sitting, and comfort of the chair category.
- `Sits { get; init; } -> SpotBlock?` — Where the player sits.

## SpotBlock — Where on the thing the player sits or lies, in its own axes: metres from its origin, and which way they face, in degrees about the vertical (0: its +z).
- `At { get; init; } -> Vector3` — Metres from the thing's origin, in its own axes.
- `Facing { get; init; } -> double` — Degrees about the vertical, which way the player faces.

## StationAction `0.2.6` — Which station action.
enum: Fill, Stop, Resume, Select, Collect

## StationActionStatus `0.2.6` — What a station action did.
enum: Done, Partial, NotEnough, NoRoom, NotAStation, NoBase, NoRecipe, WrongWorkshop, Unchanged, Locked, Refused, Unavailable, Dropped, TimedOut, QueueFull, Canceled

## StationBlock — A station a player builds: a prop that makes things.
- `Kind { get; init; } -> StationKind?` — The game's station it starts from: its model, collision and sound, and the menu it opens.
- `Menu { get; init; } -> PartRef<MenuBlock>?` — The menu it opens.
- `OwnWorkshop { get; init; } -> bool?` — A workshop of its own, which is what decides which recipes it runs.
- `Processing { get; init; } -> PartRef<ProcessingBlock>?` — How it works on its own.
- `Recipes { get; init; } -> IReadOnlyList<RecipeBlock>?` — What it makes, each with `RecipeBlock.Output`.

## StationChangedEvent `0.2.6` — A Kiln.Stations call changed a station (the contract's change event of every write): a fill that moved something, a stop, a resume, a selection, a collect.
- `Action { get; } -> StationAction` — Which call changed it: fill, stop, resume, select or collect.
- `ByMod { get; } -> string?` — The mod that called; null for Kiln itself.
- `Involves(EntityId entity) -> bool` — True when `entity` is the station.
- `Result { get; } -> StationResult` — What the call did.
- `Station { get; } -> EntityId` — The station that changed.

## StationCraftedEvent `0.2.6` — A production station finished a cycle: its outputs are in its inventory (Kiln.Gameplay's `ItemCraftedEvent` of an automated station, once per cycle).
- `Base { get; } -> BaseId` — The base the station stands in; `BaseId.None` when none.
- `Involves(EntityId entity) -> bool` — True when `entity` is the station.
- `Outputs { get; } -> IReadOnlyList<StationStack>` — What the cycle made.
- `Recipe { get; } -> uint` — The recipe the cycle ran.
- `Station { get; } -> EntityId` — The station.
- `Workshop { get; } -> uint` — The station's workshop: which recipes it runs.

## StationInfo `0.2.6` — A production station now.
- `StationInfo(EntityId Entity, uint Workshop, WorldPosition Position, BaseId Base, int BaseIndex, StationState State, bool StoppedByPlayer, bool UseAllOptions, StationNeeds Needs, uint RunningRecipe, uint SelectedRecipe, IReadOnlyList<CategoryChoice> SelectedChoices, int CraftsLoaded, TimeSpan Duration, double Progress, int? Water, int? WaterMax, IReadOnlyList<InventorySlot> Slots, IReadOnlyList<InventorySlot> Outputs, IReadOnlyList<InventorySlot> Leftovers, bool StopPending, string? Template)`
- `Base { get; init; } -> BaseId` — The base it stands in; `BaseId.None` when none.
- `BaseIndex { get; init; } -> int` — That base's record index this session; -1 when none.
- `CraftsLoaded { get; init; } -> int` — Complete crafts of the selected recipe its inputs hold now (as the game counts them).
- `DisplayName { get; init; } -> string?` `0.2.7` — A name for players (since 0.2.0-dev.29.15): the name of the item that places it ("Charcoal Kiln"), else its...
- `Duration { get; init; } -> TimeSpan` — The cycle in progress' duration (its own recipe), else one cycle of the selected recipe; scaled by the server's...
- `Entity { get; init; } -> EntityId` — The station.
- `IsRunning { get; } -> bool` — A cycle runs.
- `Leftovers { get; init; } -> IReadOnlyList<InventorySlot>` — The slots holding something that is neither an input nor an output of the selected or running recipe (an old...
- `Needs { get; init; } -> StationNeeds` — What it lacks.
- `Outputs { get; init; } -> IReadOnlyList<InventorySlot>` — The slots holding an output of the selected or running recipe that is not also one of its inputs (category options...
- `Position { get; init; } -> WorldPosition` — Where it stands.
- `Progress { get; init; } -> double` — 0..1 of the cycle in progress (while paused: where it paused), 0 when none runs.
- `RunningRecipe { get; init; } -> uint` — The cycle in progress; 0 when none.
- `SelectedChoices { get; init; } -> IReadOnlyList<CategoryChoice>` — The category choices of the selected recipe.
- `SelectedRecipe { get; init; } -> uint` — The recipe it repeats; 0 when none.
- `Slots { get; init; } -> IReadOnlyList<InventorySlot>` — Its inventory: inputs and outputs share it.
- `State { get; init; } -> StationState` — The game's state.
- `StoppedByPlayer { get; init; } -> bool` — The stop button's flag (set by a player or by Kiln's stop).
- `StopPending { get; init; } -> bool` — A `StopWhen.AfterCurrentCraft` stop waits for the cycle's end (not saved: a restart forgets it).
- `Template { get; init; } -> string?` — The station's template (the game's debug name, e.g. `T1_Charcoal_Kiln_Factory`); null when Kiln.Spawn cannot read it.
- `UseAllOptions { get; init; } -> bool` — "Use all ingredient options" is on.
- `Water { get; init; } -> int?` — The station's water (units), null for a station without water.
- `WaterMax { get; init; } -> int?` — Its capacity (units), null for a station without water.
- `Workshop { get; init; } -> uint` — Which recipes it runs (`Crafting.workshopId`).

## StationInput `0.2.6` — A recipe input as the data has it: an item (`StationInput.Item`), or any item of a category (`StationInput.Category`, `StationInput.CategoryCount`).
- `StationInput()`
- `StationInput(ItemId Item, int Count, uint Category, int CategoryCount)`
- `Category { get; init; } -> uint` — The ingredient category's id; 0 for an input that names an item.
- `CategoryCount { get; init; } -> int` — How many items of the category.
- `Count { get; init; } -> int` — How many of the item.
- `IsCategory { get; } -> bool` — True for "any item of a category".
- `Item { get; init; } -> ItemId` — The item, for an input that names one.

## StationKind — A workshop of the game, with the item that places it.
- `StationKind(string Key, string Item, string Template, string Workshop, bool Timed = false)`
- `static readonly Alchemy -> StationKind` — The game's alchemy station.
- `static readonly Carpenter -> StationKind` — The game's carpenter's workbench.
- `static readonly CharcoalKiln -> StationKind` — The game's charcoal kiln.
- `static readonly DryingRack -> StationKind` — The game's drying rack.
- `static readonly Fireplace -> StationKind` — The game's fireplace, its cooking station.
- `static readonly Forge -> StationKind` — The game's forge.
- `static readonly Grinder -> StationKind` — The game's hand grinder.
- `Item { get; init; } -> string` — The game item that places it.
- `Key { get; init; } -> string` — The name JSON uses for it, such as `"drying-rack"`.
- `static readonly Kiln -> StationKind` — The game's kiln.
- `static readonly Kitchen -> StationKind` — The game's farmer's kitchen.
- `static readonly Oven -> StationKind` — The game's oven.
- `static readonly Smelter -> StationKind` — The game's smelter.
- `static readonly TableSaw -> StationKind` — The game's table saw.
- `static readonly Tannery -> StationKind` — The game's tanning station.
- `Template { get; init; } -> string` — The game's template of the placed station.
- `Timed { get; init; } -> bool` — The game's station works on its own, over time.
- `static readonly Workbench -> StationKind` — The game's workbench.
- `Workshop { get; init; } -> string` — The game's workshop, `0x...`: what decides the recipes it runs.

## StationNeeds — What keeps a station from running: the game's `FactoryStationRequirementMask`.
enum: None, Recipe, EnergySourceInRange, EnergyAmount, InputMaterials, OutputSpace, AboveWater

## StationPlacedEvent `0.2.6` — A production station appeared in the world (placed, or found by the watch's first scans).
- `Base { get; } -> BaseId` — The base it stands in; `BaseId.None` when none.
- `BaseIndex { get; } -> int` — The record index this session of the base it stands in; -1 when it stands in none.
- `Involves(EntityId entity) -> bool` — True when `entity` is the station.
- `Position { get; } -> WorldPosition` — Where it stands.
- `Station { get; } -> EntityId` — The station.
- `Workshop { get; } -> uint` — The station's workshop: which recipes it runs.

## StationPlayerAction `0.2.6` — What a player pressed at a station (the game's `CraftingActionType`).
enum: Craft, Fill, Select, UseAllOptions, Stop, Water

## StationPlayerActionEvent `0.2.6` — A player pressed a station button (fill, select, stop, use-all, water) or crafted at a bench (`StationPlayerActionEvent.Action StationPlayerAction.Craft`: then...
- `Action { get; } -> StationPlayerAction` — Which button.
- `Amount { get; } -> int` — Fill, Craft: how many crafts.
- `CanCancel { get; } -> bool` — True: the game has not run it and a cancel stops it.
- `Choices { get; } -> IReadOnlyList<CategoryChoice>` — The category choices made for the recipe.
- `Involves(EntityId entity) -> bool` — True when `entity` is the player or the station.
- `Player { get; } -> EntityId` — The player who pressed the button.
- `Recipe { get; } -> uint` — Select, Fill, Craft: the recipe.
- `Station { get; } -> EntityId` — The station, or the workbench for a `StationPlayerAction.Craft`.
- `UseAllOptions { get; } -> bool` — UseAllOptions: the new value.

## StationRecipe `0.2.6` — A recipe as a station runs it.
- `StationRecipe(uint Id, uint Workshop, IReadOnlyList<StationInput> Inputs, IReadOnlyList<StationStack> Outputs, TimeSpan Duration, int WaterCharges, bool AlwaysUnlocked)`
- `AlwaysUnlocked { get; init; } -> bool` — True when the recipe needs no knowledge (no unlock).
- `DisplayName { get; init; } -> string?` `0.2.7` — A name for players (since 0.2.0-dev.29.15): its first output's (`Items.DisplayName`), else the recipe's own name...
- `Duration { get; init; } -> TimeSpan` — One cycle, not scaled by the server's factory speed.
- `Id { get; init; } -> uint` — The recipe id.
- `Inputs { get; init; } -> IReadOnlyList<StationInput>` — Its inputs as the data has them (per craft).
- `Name { get; init; } -> string?` — The game's debug name of the recipe (Kiln.Content); null when the content registry does not list it.
- `Outputs { get; init; } -> IReadOnlyList<StationStack>` — What one craft makes.
- `WaterCharges { get; init; } -> int` — Water one craft uses (charges; 0: none).
- `Workshop { get; init; } -> uint` — The workshop it belongs to (a station runs the recipes of its `StationInfo.Workshop`).

## StationRemovedEvent `0.2.6` — A production station is gone (picked up, destroyed).
- `Base { get; } -> BaseId` — The base it stood in; `BaseId.None` when none.
- `BaseIndex { get; } -> int` — The record index this session of the base it stood in; -1 when it stood in none.
- `Involves(EntityId entity) -> bool` — True when `entity` is the station.
- `Position { get; } -> WorldPosition` — Where it stood.
- `Station { get; } -> EntityId` — The station.
- `Workshop { get; } -> uint` — The station's workshop: which recipes it ran.

## StationResult `0.2.6` — What a station action did.
- `StationResult(StationActionStatus Status, StationAction Action, EntityId Station, uint Recipe, int CraftsRequested, int CraftsBefore, int CraftsAfter, int CraftsAdded, int GameError, IReadOnlyList<MoveChange> Changes, StationState? StateBefore, StationState? StateAfter, bool DryRun, string Message)`
- `Action { get; init; } -> StationAction` — The action.
- `CanRetry { get; } -> bool` — The same call may succeed later as it is: `StationActionStatus.QueueFull`, `StationActionStatus.Dropped`, and the...
- `Changed { get; } -> bool` — Something changed (or would, in a dry run).
- `Changes { get; init; } -> IReadOnlyList<MoveChange>` — Fill: the ledger (a take from a chest: `From` set; a put into the station: `To` set); Collect: the moves.
- `CraftsAdded { get; init; } -> int` — Fill: the crafts the game's fill ran with.
- `CraftsAfter { get; init; } -> int` — Fill: complete crafts loaded after.
- `CraftsBefore { get; init; } -> int` — Fill: complete crafts loaded before.
- `CraftsRequested { get; init; } -> int` — Fill: more crafts asked for.
- `DryRun { get; init; } -> bool` — Fill: a dry run (nothing changed).
- `FromChests { get; init; } -> int` `0.2.9` — Fill: items taken from the ordinary chests named on the request, all inputs together; 0 when the player and the...
- `FromOutputs { get; init; } -> int` `0.2.9` — Fill: items taken from opted-in stations' finished items (`Stations.SetOutputSource`), all inputs together; 0 when...
- `GameError { get; init; } -> int` — Fill: the game's code of its last attempt (2 no room in the station, 4 missing materials), 0.
- `LimitedBy { get; init; } -> FillLimit` — Fill (not Done): what kept it below what was asked.
- `Message { get; init; } -> string` — Why, or a note.
- `Ok { get; } -> bool` — True when it happened: `StationResult.Status` is Done, Partial or Unchanged.
- `Reason { get; } -> string?` — Why it did not happen: the message, else the status; null when `StationResult.Ok`.
- `Recipe { get; init; } -> uint` — Fill / Select: the recipe.
- `ShortItem { get; init; } -> ItemId?` — Fill limited by `FillLimit.Stock` or `FillLimit.Reserve`: the item; null otherwise.
- `StateAfter { get; init; } -> StationState?` — After; null as `StateBefore`.
- `StateBefore { get; init; } -> StationState?` — The station's state before; null when nothing was read (a refusal before the station was looked at).
- `Station { get; init; } -> EntityId` — The station.
- `Status { get; init; } -> StationActionStatus` — The outcome.

## StationStack `0.2.6` — An amount of one item.
- `StationStack()`
- `StationStack(ItemId Item, int Count)`
- `Count { get; init; } -> int` — How many of it.
- `Item { get; init; } -> ItemId` — The item.

## StationState — What a station is doing: the game's `FactoryStationState`.
enum: NoRecipe, MissingMaterials, Paused, Stopped, Running, MissingOutputSpace

## StationStateChangedEvent `0.2.6` — A station's state, stop flag, selected or running recipe changed (seen in Pre, by kiln.stations' watch: the game, a player or a mod).
- `After { get; } -> StationState` — The station's state after the change.
- `Base { get; } -> BaseId` — The base the station stands in; `BaseId.None` when none.
- `BaseIndex { get; } -> int` — The base's record index this session; -1 when the station stands in none.
- `Before { get; } -> StationState` — The station's state before the change.
- `ByMod { get; } -> string?` — The mod whose Kiln.Stations stop, resume or select made exactly this change (the stop flag or the selection) in the...
- `Involves(EntityId entity) -> bool` — True when `entity` is the station.
- `Player { get; } -> EntityId?` — The player whose station button made this change, while Kiln holds the players' station buttons (a handler of...
- `Resumed { get; } -> bool` — The stop flag went off.
- `RunningAfter { get; } -> uint` — The recipe in progress after the change, or 0 for none.
- `RunningBefore { get; } -> uint` — The recipe in progress before the change, or 0 for none.
- `SelectedAfter { get; } -> uint` — The recipe the station repeats after the change, or 0 for none.
- `SelectedBefore { get; } -> uint` — The recipe the station repeated before the change, or 0 for none.
- `Station { get; } -> EntityId` — The station.
- `Stopped { get; } -> bool` — The stop flag went on.
- `StoppedAfter { get; } -> bool` — The stop flag after the change.
- `StoppedBefore { get; } -> bool` — The stop flag before the change.
- `Workshop { get; } -> uint` — The station's workshop: which recipes it runs.

## StationStatus — What a placed station is doing, as `Stations.Status` reads it from the game's `FactoryStation`.
- `StationStatus(StationState State, uint Running, uint Next, TimeSpan Started, StationNeeds Needs, bool StoppedByPlayer)`
- `Needs { get; init; } -> StationNeeds` — What keeps it from running, as flags.
- `Next { get; init; } -> uint` — The recipe it was told to make, which it goes back to as long as there are inputs.
- `Running { get; init; } -> uint` — The recipe in progress, or 0.
- `Started { get; init; } -> TimeSpan` — Its current cycle's start, on the game's clock.
- `State { get; init; } -> StationState` — What it is doing.
- `StoppedByPlayer { get; init; } -> bool` — True while a player's stop (the game's stop button) is on.

## StopWhen `0.2.6` — When `Stations.Stop` stops.
enum: Now, AfterCurrentCraft

## StrengthBlock — What it takes to dig a material, and what comes out.
- `Breaks { get; init; } -> Breaks?` — The tool's class modifier that applies.
- `Drops { get; init; } -> string?` — The loot item: a `GameItems` name or a GUID, or on a new material `StrengthBlock.OwnBlock`.
- `Hardness { get; init; } -> Hardness?` — The damage column a tool or an explosion uses against it.
- `Health { get; init; } -> int?` — Health points of a block.
- `ItemsPerBlocks { get; init; } -> int?` — How many blocks dug give one loot item.
- `const OwnBlock = "block" -> string` — `StrengthBlock.Drops` of a new material: its own block item, one per block dug unless `StrengthBlock.ItemsPerBlocks`...

## Surface — What a placed thing sits on.
enum: Floor, Wall, Ceiling

## SurfaceBlock — How a building material looks: walls, floors, ceilings and stairs of it.
- `Bevel { get; init; } -> float?` — Metres the edges are rounded by, 0 to 1.
- `Bottom { get; init; } -> SurfaceLayer?` — The bottom face.
- `Comment { init; }` — Data files may say it.
- `Decoration { get; init; } -> string?` — A game material whose edge decoration (crystals, moss, rivets) it shows: a `GameMaterials` name, or `"none"`.
- `Edge { get; init; } -> SurfaceLayer?` — The worn edges.
- `EdgeCurve { get; init; } -> float?` — How the edge layer blends in, 0 to 1, as the game keeps it: 1 is a hard edge (most of the game's), 0.5 a softer one.
- `EdgeOnlySide { get; init; } -> bool?` — The edge layer only on the sides of a block.
- `EdgeTop { get; init; } -> SurfaceLayer?` — The top of the worn edges.
- `Mapping { get; init; } -> SurfaceMapping?` — How the maps lie.
- `Noise { get; init; } -> SurfaceLayer?` — A layer the cooker mixes in by noise, to break up repeats.
- `NoiseRange { get; init; } -> (float Start, float Full)?` — Where the noise layer starts to show and where it covers all, in noise units.
- `Side { get; init; } -> SurfaceLayer?` — The sides of a block, and where the other layers start from on a new material.
- `Top { get; init; } -> SurfaceLayer?` — The top face.

## SurfaceDisplacement — How far the height map pushes the surface, in metres: the height's 0 to 1 times `SurfaceDisplacement.Scale`, plus `SurfaceDisplacement.Offset`.
- `Offset { get; init; } -> float?` — -2 to 2.
- `Scale { get; init; } -> float?` — 0 to 2.

## SurfaceEmissive — What glows.
- `Color { get; init; } -> string?` — `#rrggbb`, sRGB: multiplies the map.
- `Exposure { get; init; } -> float?` — 0 to 1: how much it follows the camera's exposure correction.
- `Gi { get; init; } -> float?` — How much it lights what is around it, through the game's global illumination.
- `static implicit operator SurfaceEmissive(string mapOrColour) -> SurfaceEmissive` — A map or a colour: a string that starts with `#` is the colour, any other is the map's path.
- `Map { get; init; } -> string?` — A PNG, sRGB.
- `Strength { get; init; } -> float?` — Intensity.

## SurfaceLayer — One layer of a surface.
- `Albedo { get; init; } -> string?` — Colour: a PNG, sRGB.
- `Ao { get; init; } -> SurfaceValue?` — Ambient occlusion: a PNG (red) or 0 to 1.
- `Displacement { get; init; } -> SurfaceDisplacement?` — How far the height map pushes the surface, in metres.
- `Emissive { get; init; } -> SurfaceEmissive?` — What glows: a PNG, `#rrggbb`, or a `SurfaceEmissive`.
- `Game { get; init; } -> string?` — A game material this layer starts from: a `GameMaterials` name.
- `Height { get; init; } -> SurfaceValue?` — Height, for displacement and for blending layers: a PNG (red) or 0 to 1.
- `static implicit operator SurfaceLayer(SurfacePart part) -> SurfaceLayer` — The same as another layer of this surface.
- `Layer { get; init; } -> SurfacePart?` — Which of its layers.
- `Metallic { get; init; } -> SurfaceValue?` — A PNG (red) or 0 to 1.
- `Normal { get; init; } -> NormalMap?` — A normal map, green up as in glTF and Blender: `"n.png"`, or with a strength or `YDown` for a DirectX map.
- `static Of(string gameMaterial, SurfacePart? layer = null) -> SurfaceLayer` — A game material's layer, as it is: `SurfaceLayer.Of(GameMaterials.StoneAncientMoonstone, SurfacePart.Edge)`.
- `Reflectance { get; init; } -> SurfaceValue?` — Non-metal specular: a PNG (red) or 0 to 1.
- `Roughness { get; init; } -> SurfaceValue?` — Perceptual roughness: a PNG (its red channel) or a value 0 to 1.
- `Tiling { get; init; } -> float?` — How many times the maps repeat every 4 m.
- `Tint { get; init; } -> string?` — `#rrggbb`, sRGB: multiplies the albedo.

## SurfaceMapping — How the maps lie on a block.
enum: Default, Wood

## SurfacePart — The six layers of a building surface.
enum: Side, Top, Bottom, Edge, EdgeTop, Noise

## SurfaceValue — A PNG (its red channel) or a value 0 to 1.
- `Equals(SurfaceValue other) -> bool`
- `static implicit operator SurfaceValue(float value) -> SurfaceValue` — A fixed value, 0 to 1.
- `static implicit operator SurfaceValue(string png) -> SurfaceValue` — A PNG by its path in the mod's folder; its red channel is the value.
- `static operator !=(SurfaceValue left, SurfaceValue right) -> bool`
- `static operator ==(SurfaceValue left, SurfaceValue right) -> bool`

## `TableBlockHandle<T>` — What `Blocks.Define` and `Blocks.Define` give back.
- `CellId { get; } -> int` — The material's id in a cell this session, or -1.
- `Status { get; } -> TableBlockStatus` — Whether this game has it now.

## TableBlockStatus — Whether a material block is in this game's tables now.
enum: Registered, Pending, Off

## TerrainMaterialBlock — A new terrain material: ground, rock or an ore vein, dug with the game's tools and placed by code with `Build.Add`, `Paint` or `Replace` and its...
- `Description { get; init; } -> string?` — The description of the item, when the material is `TerrainMaterialBlock.Placeable`.
- `Flatten { get; init; } -> bool?` — The game's flatten tool works on it.
- `Icon { get; init; } -> string?` — The item's picture: a PNG in the mod's folder, or `item:<name>`.
- `Like { get; init; } -> string?` — The game terrain material it starts from: a `GameMaterials` Terrain name or `Cell<1..127>`.
- `Look { get; init; } -> string?` — The game terrain material whose look it takes: the same layers, so it draws as that material does, and digs up its...
- `Name { get; init; } -> string` — What players read: its item's name when it's `TerrainMaterialBlock.Placeable`.
- `Placeable { get; init; } -> bool` — It has an item that the game's terrain shapes place, named `TerrainMaterialBlock.Name`, and digging gives it back.
- `Recipe { get; init; } -> RecipeBlock?` — How the item is made, for a `TerrainMaterialBlock.Placeable` material.
- `Sliding { get; init; } -> bool?` — Feet slide on it, as on ice.
- `Strength { get; init; } -> PartRef<StrengthBlock>?` — What it takes to dig it, and what comes out.
- `Was { get; init; } -> string?` — The block's old name, after a rename: the material's id comes from it, so its cells stay in every world.

## ToggleBlock — A toggle's prompts, and how it starts.
- `Off { get; init; } -> InteractVerb?` — The prompt while it is on: one of the game's words.
- `OffPrompt { get; init; } -> string?` — A prompt of your own while it is on.
- `On { get; init; } -> InteractVerb?` — The prompt while it is off: one of the game's words.
- `OnPrompt { get; init; } -> string?` — A prompt of your own while it is off, the same in every language.
- `StartsOn { get; init; } -> bool` — It is on when placed: a door stands open.

## UpgradeAction `0.2.12` — A rarity upgrade as what happens when a player uses something: on a placed thing (`InteractBlock.Upgrade`) or as an offer on any entity (`UpgradeStation.Offer`).
- `const DefaultRange = 6 -> double` — How far, in metres, a player may stand from the station when the pick of a `UpgradeTarget.Choose` window arrives...
- `Range { get; init; } -> double` — 1 to 64 metres: how far from the station a player may be when they pick an item in the `UpgradeTarget.Choose` window...
- `Rule { get; init; } -> string` — The rarity upgrade rule it runs: a `RarityUpgradeBlock` of this mod by name, or `"other-mod:name"`.
- `Target { get; init; } -> UpgradeTarget` — The item it raises (default `UpgradeTarget.Held`).

## UpgradeStation `0.2.12` — Rarity upgrades from code, the way a station's use runs them: `UpgradeStation.Use` as if a player used a station, and `UpgradeStation.Offer` to put the use on any...
- `static Available { get; } -> bool` — True when a station's use can upgrade (capability `blocks.upgrade_station`).
- `const Capability = "blocks.upgrade_station" -> string` — The capability: upgrades as a station's use (needs `items.gear.upgrade`).
- `static Offer(EntityId entity, UpgradeAction action, out string reason, OfferOptions? options = null) -> bool` — Puts the use on any entity of the world: an "E" prompt (the game's word Use, which every player's game shows) whose...
- `static Use(EntityId player, EntityId station, UpgradeAction action) -> Task<UpgradeResult?>` — Runs `action` for `player` as if they had used `station`: the held item is upgraded, or (`UpgradeTarget.Choose`, a...

## UpgradeTarget `0.2.12` — Which item an upgrade interaction raises (`UpgradeAction.Target`).
enum: Held, Choose

## WorldCraftingProp — A crafting prop a mod made, as it is in the game's table.
- `WorldCraftingProp(Guid Guid, uint Id, int Index, uint ItemId, string Mod, string Block)`
- `Block { get; init; } -> string` — The name of its prop block in that mod.
- `Guid { get; init; } -> Guid` — Its GUID, from the mod and the block's name.
- `Id { get; init; } -> uint` — The game's `CraftingPropId`: FNV-1a of the GUID.
- `Index { get; init; } -> int` — Its place in the mask of props, 51..63 (the game's own props have 0..50).
- `ItemId { get; init; } -> uint` — The item that places it.
- `Mod { get; init; } -> string` — The mod that defined it.

## WorldMaterial — A building or terrain material in the game's tables that a mod made, or that a world keeps for an absent mod.
- `WorldMaterial(int CellId, Guid Guid, string Mod, string Block, bool Live, bool Reused)`
- `Block { get; init; } -> string` — The block's name.
- `CellId { get; init; } -> int` — The id in a cell: 128..255 a building material, 1..127 a terrain one.
- `Guid { get; init; } -> Guid` — The material's GUID.
- `Live { get; init; } -> bool` — An enabled mod's.
- `Mod { get; init; } -> string` — The mod, as last seen for a kept material.
- `Reused { get; init; } -> bool` — It took a slot the game has marked deprecated.
- `Terrain { get; } -> bool` — A terrain material (`TerrainMaterialBlock`): cell ids 1..127 are the game's terrain.

## WorldMaterialCategory `0.2.4` — The byte material namespaces.
enum: Building, Terrain

## WorldTables — The game's shared building-material tables, merged from every enabled mod at boot. docs/kiln/services/worldtables.md.
- `static Available { get; } -> bool` — The merged tables are served, or there was nothing to change.
- `static CellIdOf(Guid material) -> int` — The cell id of a material's GUID, or -1: it isn't in the tables.
- `static CraftingPropIndex(Guid craftingProp) -> int` — The place (0..63) a crafting prop has in the game's mask of props, or -1: it isn't in the table.
- `static CraftingProps { get; } -> IReadOnlyList<WorldCraftingProp>` — The crafting props of mods in the game's table (`PropBlock.Provides`), in the order of their places.
- `static Forget(Guid material) -> bool` — Stops keeping an absent mod's material in every world.
- `static GameMaterialId(string material) -> int` — A game material's cell id from what a mod writes: a `GameMaterials` name, `Cell<id>`, or its GUID. -1: not a game...
- `static GetMaterialBudget(WorldMaterialCategory category) -> CapacityQueryResult` `0.2.4` — Reads an immutable process snapshot.
- `static GetMaterialPoolRegistration(Guid registrationId) -> DefinitionRegistrationResult` `0.2.4` — Reads a receipt without waiting.
- `static GetPoolBudget(string pool) -> CapacityQueryResult` `0.2.4` — Reads one 64-entry pool's process snapshot.
- `static Hash { get; } -> string` — What a client and a server have to agree on: 16 hex digits covering live materials, crafting props and, when...
- `static Materials { get; } -> IReadOnlyList<WorldMaterial>` — Every material in the tables: the building materials, then the terrain ones, live ones first in each.
- `static RemoveStoredMaterialPool(string pool) -> bool` `0.2.4` — Removes the stored definition of one material pool, `"mod:key"`: what a `Blocks.Define` with...
- `static State { get; } -> string` — `off`, `waiting`, `untouched` or `merged`.
- `static StatusJson() -> string` — JSON: state, hash, ids, tweaks, each table's bytes against the game's, what was refused and why.
- `static Tweaks { get; } -> IReadOnlyList<(int CellId, string Mod, string Block)>` — The tweaks of game materials in the tables, in load order: (cell id, mod, block).

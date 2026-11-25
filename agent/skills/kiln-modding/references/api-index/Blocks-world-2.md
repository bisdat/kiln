# Blocks (Kiln.Blocks): world, part 2 of 2

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

## StationMenuDefinition `0.2.17` — A layout of a station's menu that a mod lays over it at run time: `StationMenus.Define`.
- `Key { get; init; } -> string` — The layout's key in your mod: letters, digits, '.', '_' and '-', at most 64.
- `Tabs { get; init; } -> IReadOnlyList<StationMenuTabBlock>` — The tabs, 1 to 15.

## StationMenuRegistration `0.2.17` — A layout `StationMenus.Define` laid over a station's menu.
- `DefinitionId { get; } -> Guid` — The layout's id: the same for your mod and the definition's key on every machine.
- `Dispose() -> void` — Takes the layout off.
- `Generation { get; } -> ulong` — The generation of this game's station menus now: it goes up by one with each change of any of them.

## StationMenus `0.2.17` — Station menus with tabs of their own, laid out at run time.
- `static Define(BlockHandle<StationBlock> station, StationMenuDefinition definition) -> MenuDefinitionResult` — Lays `definition` over the menu of `station`, as a layer of the calling mod.

## StationMenuSetBlock `0.2.17` — A set of recipes in a tab, under a heading of its own.
- `Heading { get; init; } -> UiText?` — The heading over the set.
- `Key { get; init; } -> string` — The set's key: letters, digits, '.', '_' and '-', at most 64.
- `Recipes { get; init; } -> IReadOnlyList<StationRecipeRef>` — The recipes it lists, in this order, by their `RecipeBlock.MenuKey`.

## StationMenuTabBlock `0.2.17` — A tab of a station's crafting menu: its name, the icon it borrows, and its sets of recipes, in this order.
- `IconFromTab { get; init; } -> int` — The icon of the n-th tab (0 the first) of the menu the station is like (`MenuBlock.Like`, else its kind's), as the...
- `Key { get; init; } -> string` — The tab's key: letters, digits, '.', '_' and '-', at most 64.
- `Name { get; init; } -> UiText` — What the tab says: one text for every language, or one per language (at most 35, each 1 to 4096 bytes).
- `Sets { get; init; } -> IReadOnlyList<StationMenuSetBlock>` — Its sets of recipes, 1 to 28 (a tab that lists nothing has one set with no recipes).

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

## StationRecipeRef `0.2.17` — A recipe in a station's menu, by its `RecipeBlock.MenuKey`: `"key"` for a recipe of the mod that defines the station, `"other-mod:key"` for another mod's.
- `StationRecipeRef()`
- `StationRecipeRef(string Value)`
- `static implicit operator StationRecipeRef(string value) -> StationRecipeRef` — A recipe by its key, or `"other-mod:key"`.
- `Value { get; init; } -> string` — The recipe's menu key as written: `"key"` for the station's own mod, `"other-mod:key"` for another mod's.

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
- `FromLinked { get; init; } -> int` `0.2.20` — Fill: items taken from the player's linked sources (`FillOptions.UseLinkedSources`), all inputs together; 0 when the...
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

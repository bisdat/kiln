# Content (Kiln.Content)

## Content — The game's content registries (Forge: registries + data packs): read and change items, recipes, random loot and drop tables at run time.
- `static Apply(string json, string? source = null) -> ContentReport` — Applies a data-pack document (the `content/*.json` format) as the calling mod's layers.
- `static ApplyFile(string path) -> ContentReport` — Applies a JSON file (relative paths: from the calling mod's folder).
- `static Arsenals { get; } -> GameDataRegistry` — Enemy arsenals: `attacks[1].description.maxRange`, `behaviors[0].settings.chance`.
- `static Available { get; } -> bool` — The native service is loaded (capability `kiln.content`).
- `static Buffs { get; } -> GameDataRegistry` — Buff types: `defaultLifeTime`, `impactValues.Damage`.
- `static ConfigVocabulary() -> string` — The config vocabulary (config ids, their aliases from the game's Impact programs, C++-read costs).
- `static Drops { get; } -> ContentRegistry` — Drop tables (default inventories of enemies and fixed chests), as the game has used them so far.
- `static Dump(string? path = null) -> string` — Writes every registry (schema, entries with their non-zero fields, recipe lists, drop stacks, what changed and its...
- `static GameData(GameDataSection section) -> GameDataRegistry` — The kiln.gamedata registry of `section`.
- `static GameDataAvailable { get; } -> bool` — kiln.gamedata is loaded (it is ready once `Content.Ready`; see `Content.GameDataStatus`).
- `static GameDataDefinitions() -> IReadOnlyList<GameDataDefinition>` — Every clone definition of kiln.gamedata (templates, sequences, perks, buffs; data packs, code, the content sync...
- `static GameDataStatus() -> string` — kiln.gamedata's state as JSON: `{ready, detail, resources, sections: {name: {available, count, changed}}, externals}`.
- `const GameTextsCapability = "content.game_texts" -> string` `0.2.22` — The capability row of `Content.TextIn`, `Content.TryGetTexts` and `Content.LoadTextsAsync`: available on a game...
- `static Gear(ContentEntry item) -> GearStats` `0.2.12` — The typed gear values of an entry of `Content.Items`.
- `static Gear(ItemId item) -> GearStats?` `0.2.12` — The typed gear values of an item type; null when there is no such item.
- `static Gear(string item) -> GearStats?` `0.2.12` — The typed gear values of an item type (a debug name, `"0x..."` id or GUID), in the game's tooltip numbers; null when...
- `const GearCapability = "content.gear" -> string` `0.2.12` — The capability of `Content.Gear` and the data packs' "gear" section.
- `static ItemData { get; } -> GameDataRegistry` — Items through kiln.gamedata (config values, lists): `impactValues.ManaCost`, `sequences[0].triggeredBy`.
- `static Items { get; } -> ContentRegistry` — Items (`keen::ItemInfo`): id = ItemId, name = debug name (`Material_T1_Wood_Logs`).
- `static LoadTextsAsync(string? language, CancellationToken cancellationToken = default(CancellationToken)) -> Task<GameTextTable>` `0.2.22` — Reads one language's texts (see `Content.TryGetTexts` for `language`) and completes with the table, on a worker...
- `static LoadVoxelMaterialsAsync(string? language = null, CancellationToken cancellationToken = default(CancellationToken)) -> Task<VoxelMaterialCatalog>` `0.2.22` — Reads the game's voxel materials and `language`'s texts (see `Content.TryGetVoxelMaterials`) and completes with the...
- `static Loot { get; } -> ContentRegistry` — Random loot per item (`keen::LootableItemInfo`): id = the item's ItemId.
- `static OnReady(Action<ContentReadyEvent> handler) -> void` — Runs `handler` as the calling mod every time the registries are (re)applied: at world load after the data packs, and...
- `static PackReport() -> string` — The last data-pack application as JSON...
- `static Perks { get; } -> GameDataRegistry` — Perks: `damageModifier.distribution.magicalIceDamage`, `impactValues.*`.
- `static Ready { get; } -> bool` — The registries are loaded and the data packs applied: the question for the row `content.ready`, true only when...
- `static Recipes { get; } -> ContentRegistry` — Recipes (`keen::RecipeInfo`): id = RecipeId, name = the recipe's debug name.
- `static RegisterSection(string name, Func<ContentSectionCall, ContentSectionResult> apply, Action<string, string?> revert) -> void` — Adds a data-pack section for another service (Forge: a custom data-pack reload listener).
- `static Registry(ContentKind kind) -> ContentRegistry` — The registry of `kind`: `Content.Items`, `Content.Recipes`, `Content.Loot` or `Content.Drops`.
- `static ReloadPacks() -> void` — Re-reads every mod's `content/*.json` now.
- `static Revert() -> int` — Takes every layer of the calling mod off (data packs of its folder stay; they follow the mod's load state).
- `static Sequences { get; } -> GameDataRegistry` — Actor sub-sequences: `events[SpawnEntityEvent#0].time`, `length`.
- `static Skills { get; } -> GameDataRegistry` — Skill-tree nodes: `costs`, `maxLevel`, `configValues.*`.
- `static StackGuard { get; set; } -> bool` — The stack guard: while on, the game no longer cuts stacks down to their item's max stack size (its "Deleted N items...
- `static Status() -> ContentStatus?` — The service's state, or null when it is not loaded.
- `static Templates { get; } -> GameDataRegistry` — Entity templates: creatures, projectiles, explosions.
- `static Text(Guid locaTag) -> string?` `0.2.10` — The text of a LocaTag reference (a GUID, as `ItemInfo.name`); see `Content.Text`.
- `static Text(string locaTag) -> string?` `0.2.10` — A loca tag as a GUID (hashed with the game's FNV-1a), as `0x1234abcd` or as a decimal id; see `Content.Text`.
- `static Text(uint locaId) -> string?` `0.2.10` — The game's own text of a loca tag id (`FNV-1a` over the tag GUID's 16 bytes: a tooltip row's `locaId`, `Texts.TagOf`...
- `static TextAvailable { get; } -> bool` `0.2.10` — True when `Content.Text` can answer here: a game client whose game has looked up a text already.
- `const TextCapability = "content.text" -> string` `0.2.10` — The capability row of `Content.Text`: available on a game client once the game has looked up its first text (its...
- `static TextIn(Guid locaTag, string? language) -> string?` `0.2.22` — The text of a LocaTag reference (a GUID, hashed as the game does) in `language`; see `Content.TextIn`.
- `static TextIn(uint locaId, string? language) -> string?` `0.2.22` — The text of a loca tag id in `language` with the game's markup, on any thread, without waiting...
- `static TextLanguage { get; } -> string?` `0.2.10` — The language the game shows its texts in now, as the game's key (`"en-us"`, `"de-de"`, `"pt-br"`), or null when...
- `static TextLanguages { get; } -> IReadOnlyList<string>` `0.2.22` — The languages the game ships texts in, as the game keys them (`"en-us"`, `"de-de"`, `"pt-br"`, ...
- `static TooltipValues(ItemId item) -> IReadOnlyList<TooltipValue>` `0.2.10` — The rows of an item type's tooltip (its `uiValues`) with their labels in the player's language: the numbers a spell...
- `static TryGetTexts(string? language) -> GameTextTable?` `0.2.22` — One language's texts when they are read already, without waiting: `language` is a key the game ships...
- `static TryGetVoxelMaterials(string? language = null) -> VoxelMaterialCatalog?` `0.2.22` — The game's voxel materials with labels in `language` (a key the game ships, `"de-de"`; null the player's game...
- `static UnregisterSection(string name) -> bool` — Removes a section registered with `Content.RegisterSection`.
- `const VoxelMaterialsCapability = "content.voxel_materials" -> string` `0.2.22` — The capability row of `Content.TryGetVoxelMaterials` and `Content.LoadVoxelMaterialsAsync`: available on both sides...

## ContentChange `0.2.2` — What a `ContentChangedEvent` reports.
enum: Set, Reset, Applied, Reverted

## ContentChangedEvent `0.2.2` — A mod changed game content through Kiln.Content from code: a value of an entry, a data-pack document applied, or its layers taken off.
- `Change { get; } -> ContentChange` — What happened.
- `Id { get; } -> uint` — The entry's id; 0 for a document or a mod's layers as a whole.
- `Kind { get; } -> ContentKind?` — The registry of the entry (`ContentChange.Set`, `ContentChange.Reset`); null for a document or a mod's layers as a...
- `Name { get; } -> string` — The entry's name; "" for a document or a mod's layers as a whole.
- `Origin { get; } -> string` — The id of the mod that made the change ("" when Kiln made it for a mod without code).
- `Path { get; } -> string` — The field's path as the call gave it, a reference key (`model`), `inputs`, `outputs` or `stacks`; for...

## ContentConflict — A conflict: two owners (mods / data packs) set the same field to different values; the later one wins.
- `ContentConflict(string Field, string Winner, string WinnerValue, string Loser, string LoserValue)`
- `Field { get; init; } -> string` — The field both set.
- `Loser { get; init; } -> string` — The owner whose value is overridden.
- `LoserValue { get; init; } -> string` — The value the loser set.
- `Winner { get; init; } -> string` — The owner whose value is in effect.
- `WinnerValue { get; init; } -> string` — The value the winner set.

## ContentDefinition — A registered definition, as kiln.content reports it.
- `ContentDefinition(ContentKind Kind, Guid Guid, uint Id, string Name, string State, IReadOnlyList<string> Worlds, string? Owner, string? Problem)`
- `DefinitionJson { get; init; } -> string?` — The normalized definition (the data-pack form).
- `Guid { get; init; } -> Guid` — The definition's GUID.
- `Id { get; init; } -> uint` — The id it has in the game: the item's or the recipe's id.
- `IsPlaceholder { get; init; } -> bool` — A placeholder "[Missing: mod:item]" that keeps saved stacks of an item whose mod is gone.
- `Kind { get; init; } -> ContentKind` — Whether it is an item or a recipe.
- `Mod { get; init; } -> string?` — The id of the mod that defined it (its code or its data pack); for a placeholder, the missing mod's.
- `Name { get; init; } -> string` — The definition's name.
- `Owner { get; init; } -> string?` — The owner that registered it, as kiln.content reports it; null right after `Register`.
- `Problem { get; init; } -> string?` — Why it is not (yet) in a world, if it is not.
- `State { get; init; } -> string` — `defined` / `unchanged` / `redefined` / `adopted` right after Register; in `NewContent.Definitions`: `pending` (no...
- `Warnings { get; init; } -> IReadOnlyList<string>` — Parts of the definition that were skipped (a kitbash reference that is not loaded, ...).
- `Worlds { get; init; } -> IReadOnlyList<string>` — The worlds whose registries hold it (`server`, `client`).

## ContentEntry — One entry of a registry: an item, a recipe, an item's random-loot data, or a drop table.
- `CraftingDuration { get; set; } -> TimeSpan` — Recipes: the crafting duration (`craftingDuration`; used by timed stations such as kilns).
- `CraftingWater { get; set; } -> int` `0.2.13` — Recipes: the tank water a craft takes, in water charges (`waterCharges`; one charge is 64 of the game's water...
- `Get(string path) -> ContentValue` — The current value of a field (drops: `"[3].countMin"` addresses stack 3).
- `GetNumber(string path) -> double` — The current value of a field as a number: `ContentEntry.Get` read as `ContentValue.AsDouble`.
- `GetRef(string path) -> Guid` — The GUID a reference field holds now (`Guid.Empty` = none).
- `Guid { get; } -> Guid` — The entry's GUID.
- `Id { get; } -> uint` — The entry's id: the item's id for items and loot, the recipe's id for recipes.
- `Inputs { get; } -> IReadOnlyList<RecipeStack>` — Recipes: the ingredients as the game sees them now.
- `Kind { get; } -> ContentKind` — The registry the entry belongs to.
- `MaxStack { get; set; } -> int` — Items: the max stack size (`maxStackSize`, 1..65535).
- `Modified { get; } -> bool` — At least one field differed from the game's original when this handle was made.
- `Name { get; } -> string` — The entry's debug name.
- `Original(string path) -> ContentValue` — The game's original value of a field (before any mod changed it).
- `Outputs { get; } -> IReadOnlyList<RecipeStack>` — Recipes: the results.
- `Reset(string path) -> bool` — Takes the calling mod's layer of a field off.
- `Set(string path, TimeSpan value) -> void` — As `ContentEntry.Set`, with a time (written in seconds).
- `Set(string path, bool value) -> void` — As `ContentEntry.Set`, with a boolean value.
- `Set(string path, double value) -> void` — Sets a field as the calling mod (a layer over the original; reverted when the mod unloads).
- `Set(string path, int value) -> void` — As `ContentEntry.Set`, with an integer value.
- `Set(string path, long value) -> void` — As `ContentEntry.Set`, with an integer value.
- `Set(string path, string value) -> void` — Sets an enum field by name (`"Epic"`), a flag set by names (`"A,B"`), or a number in text.
- `SetInputs(IEnumerable<RecipeStack> stacks) -> void` — Recipes: replaces the ingredients (1..32; a category input keeps its category id).
- `SetOutputs(IEnumerable<RecipeStack> stacks) -> void` — Recipes: replaces the results (1..32).
- `SetPerks(IReadOnlyList<string?> perks, bool allowGameItem = false) -> IReadOnlyList<string>` — Items: sets perk slots (up to 5: a perk GUID, `item:<name>` = that item's perk in the same slot, `none`, null = keep...
- `SetRef(string key, string value, bool allowGameItem = false) -> IReadOnlyList<string>` — Points a reference of this item at a resource the game already loaded, as the calling mod's layer (reverted with the...
- `SetRefJson(string key, string valueJson, bool allowGameItem = false) -> IReadOnlyList<string>` — `ContentEntry.SetRef` with any JSON value (the data-pack form).
- `SetStacks(IEnumerable<DropStack> stacks) -> void` — Drops: changes counts / chances / weights of the table's stacks (same stacks, same order, same items).
- `Stacks { get; } -> IReadOnlyList<DropStack>` — Drops: every stack of the table, depth first over its groups.

## ContentEnumKind — Plain, enum or flag-set integer.
enum: Plain, Enum, Flags

## ContentField — A field of a registry's struct, from the game's own reflection, as a dotted path (`maxStackSize`, `damageSetup.dmgMod`, `perkIds[2]`).
- `ContentField(int Index, string Path, string TypeName, int Offset, int Bytes, ContentValueType Type, bool Signed, bool Writable, ContentEnumKind EnumKind, IReadOnlyList<(string Name, long Value)> Values)`
- `Bytes { get; init; } -> int` — The field's size in bytes (1, 2, 4, 8 or 16).
- `EnumKind { get; init; } -> ContentEnumKind` — Whether the field is a plain integer, an enum or a flag set.
- `Index { get; init; } -> int` — The field's index in the registry's field list.
- `IsReference { get; init; } -> bool` — A reference to another resource (a 16-byte GUID typed by the game: a model, a template, a perk, ...): not writable...
- `Offset { get; init; } -> int` — The field's offset in bytes from the start of the struct.
- `Path { get; init; } -> string` — The field's dotted path.
- `Signed { get; init; } -> bool` — True for a signed integer.
- `Type { get; init; } -> ContentValueType` — How the field's value is stored.
- `TypeName { get; init; } -> string` — The game's type of the field, such as `keen::float32`.
- `Values { get; init; } -> IReadOnlyList<(string Name, long Value)>` — The names and numbers of an enum's or flag set's values.
- `Writable { get; init; } -> bool` — True when a mod can set the field.

## ContentKind — The game registries kiln.content exposes (Forge: `ForgeRegistries.ITEMS`, recipe manager, loot tables).
enum: Items, Recipes, Loot, Drops

## ContentReadyEvent — The content registries are loaded and every data pack is applied (Forge: the moment after data packs load, `TagsUpdatedEvent` / `ServerStartedEvent` for registries).
- `Generation { get; } -> ulong` — +1 per application (1 = world load).
- `Reload { get; } -> bool` — True after a data-pack reload (a pack file changed, or a mod with packs was loaded / unloaded).

## ContentRegistry — One registry (items, recipes, loot, drops).
- `Count { get; } -> int` — Entries in the registry (0 until `Content.Ready`).
- `Field(string path) -> ContentField?` — The field with this path (ignoring case), or null.
- `Fields { get; } -> IReadOnlyList<ContentField>` — Every field of the registry's struct, from the game's reflection.
- `Get(ItemId item) -> ContentEntry?` — The entry of this item, or null.
- `Get(string key) -> ContentEntry?` — The entry for a name, id or GUID, or null.
- `Get(uint id) -> ContentEntry?` — The entry with this id (an item's id, a recipe's id), or null.
- `GetEnumerator() -> IEnumerator<ContentEntry>` — Every entry of the registry, in order.
- `Kind { get; } -> ContentKind` — Which registry this is.
- `this[int index] { get; } -> ContentEntry` — The entry at `index`, 0 to `ContentRegistry.Count` - 1.

## ContentRegistryDefinitions — `Content.Items.Register(...)` / `Content.Recipes.Register(...)`.
- `static Register(this ContentRegistry registry, ItemDefinition def) -> ContentDefinition` — Registers a new item (`registry` must be `Content.Items`).
- `static Register(this ContentRegistry registry, RecipeDefinition def) -> ContentDefinition` — Registers a new recipe (`registry` must be `Content.Recipes`).

## ContentReport — What applying a data-pack document did.
- `ContentReport(int Applied, IReadOnlyList<string> Errors, IReadOnlyList<string> Warnings, IReadOnlyList<ContentConflict> Conflicts)`
- `Applied { get; init; } -> int` — How many changes were applied.
- `Conflicts { get; init; } -> IReadOnlyList<ContentConflict>` — Fields that more than one owner set to different values.
- `Errors { get; init; } -> IReadOnlyList<string>` — What went wrong, one message each.
- `Ok { get; } -> bool` — True when there are no errors.
- `Warnings { get; init; } -> IReadOnlyList<string>` — What was skipped or looked wrong, one message each.

## ContentRoundTrip — Result of `ContentSync.CheckRoundTrip`.
- `ContentRoundTrip(bool Ok, int Fields, int Applied, int Reset, double Milliseconds, IReadOnlyList<string> Problems)`
- `Applied { get; init; } -> int` — How many of them were applied again with no change in the state.
- `Fields { get; init; } -> int` — How many changes this process's state exported.
- `Milliseconds { get; init; } -> double` — How long the check took.
- `Ok { get; init; } -> bool` — True when every check passed.
- `Problems { get; init; } -> IReadOnlyList<string>` — What differed, one message each; empty when it passed.
- `Reset { get; init; } -> int` — How many were put back to the game's original to match a vanilla server.

## ContentSectionCall — A call of a registered pack section (see `Content.RegisterSection`).
- `ContentSectionCall(string Section, string Json, string Owner, string Source, ulong Generation, bool Reload)`
- `Generation { get; init; } -> ulong` — The content generation: +1 per application of the packs (`ContentReadyEvent.Generation`).
- `Json { get; init; } -> string` — The section's value, verbatim.
- `Owner { get; init; } -> string` — `pack:<mod>` for data packs, `mod:<mod>` for `Content.Apply`, `sync` for the content sync.
- `Reload { get; init; } -> bool` — Part of a re-application of every pack.
- `Section { get; init; } -> string` — The name of the section.
- `Source { get; init; } -> string` — The pack file (`<mod>/content/<file>.json`) or the Apply source.

## ContentSectionResult — What a pack section handler did.
- `ContentSectionResult(int Applied, IReadOnlyList<string> Errors, IReadOnlyList<string> Warnings)`
- `Applied { get; init; } -> int` — How many changes the handler applied.
- `Errors { get; init; } -> IReadOnlyList<string>` — What went wrong, one message each.
- `static Ok(int applied) -> ContentSectionResult` — A result with `applied` changes and no errors or warnings.
- `Warnings { get; init; } -> IReadOnlyList<string>` — What was skipped or looked wrong, one message each.

## ContentStatus — State of kiln.content.
- `ContentStatus(bool Ready, bool HasItems, bool HasRecipes, bool HasLoot, bool HasDrops, bool StackGuard, int Items, int Recipes, int Loot, int Drops, int Packs, int PackErrors, int Conflicts, int Changed, ulong Generation, string Detail)`
- `Changed { get; init; } -> int` — How many fields currently differ from the game's original.
- `Conflicts { get; init; } -> int` — How many fields more than one owner set to different values.
- `Definitions { get; init; } -> int` — New items and recipes defined by mods and data packs (all states).
- `Detail { get; init; } -> string` — A short text with more detail.
- `Drops { get; init; } -> int` — How many entries the drops registry holds.
- `Generation { get; init; } -> ulong` — How many times the registries were applied (+1 per world load or pack reload).
- `HasDrops { get; init; } -> bool` — True when the drops registry works on this game build.
- `HasItems { get; init; } -> bool` — True when the items registry works on this game build.
- `HasLoot { get; init; } -> bool` — True when the loot registry works on this game build.
- `HasRecipes { get; init; } -> bool` — True when the recipes registry works on this game build.
- `Items { get; init; } -> int` — How many entries the items registry holds.
- `Loot { get; init; } -> int` — How many entries the loot registry holds.
- `PackErrors { get; init; } -> int` — How many problems the last application of the packs had.
- `Packs { get; init; } -> int` — How many data-pack files were applied.
- `Placeholders { get; init; } -> int` — Placeholders "[Missing: mod:item]" registered for items of mods that are gone (missing content).
- `Ready { get; init; } -> bool` — True when the registries are loaded and the data packs are applied.
- `Recipes { get; init; } -> int` — How many entries the recipes registry holds.
- `StackGuard { get; init; } -> bool` — True when stacks above an item's maximum are kept instead of being cut down by the game.

## ContentSync — Kiln's content sync (runs by itself with Kiln.Content and Kiln.Network).
- `const ChannelName = "kiln:content" -> string` — The name of the Kiln.Network channel the sync uses.
- `const ChannelVersion = "1" -> string` — The version of the sync protocol on that channel.
- `static CheckRoundTrip() -> ContentRoundTrip` — A self-check of the sync format on this process's own registries (tick thread; a lab / diagnostic tool, also usable...
- `static Serving { get; } -> (uint StateId, uint Fields, int Bytes, int Parts)?` — Server side: the content state being served, or null when none is (not a server, nothing exported yet): its id (the...
- `static Status { get; } -> ContentSyncStatus` — Client side: what the content sync did last.

## ContentSyncedEvent — Client: a Kiln server's content changes were applied to this game's registries (`ContentSyncedEvent.Restored` false), or taken off again after leaving (true).
- `Applied { get; } -> int` — Field / list changes taken from the server (on restore: fields put back).
- `Errors { get; } -> IReadOnlyList<string>` — What went wrong while applying, one message each; empty when it all went in.
- `Reset { get; } -> int` — Fields this game had changed on its own that were put back to the game's original to match the server.
- `Restored { get; } -> bool` — False when the server's changes were applied; true when they were taken off again after leaving.
- `StateId { get; } -> uint` — The server's content state (hash of its document).

## ContentSyncState — Client side of the content sync, for status displays.
enum: Off, Waiting, Applied, Hosting, Failed

## ContentSyncStatus — Client side of the content sync, as of now.
- `ContentSyncStatus()`
- `ContentSyncStatus(ContentSyncState State, uint StateId, int ServerFields, int Applied, int Reset, int Errors, int Bytes, int CompressedBytes, DateTime? AppliedAt, string Detail)`
- `Applied { get; init; } -> int` — How many of them were applied to this game's registries.
- `AppliedAt { get; init; } -> DateTime?` — When the changes were applied (UTC); null when they have not been.
- `Bytes { get; init; } -> int` — The size of the server's document, uncompressed.
- `CompressedBytes { get; init; } -> int` — The size of the server's document as it was sent.
- `Detail { get; init; } -> string` — A short text: how many changes came from the server, or why it failed.
- `Errors { get; init; } -> int` — How many errors the application had.
- `Reset { get; init; } -> int` — How many fields this game had changed on its own that were put back to the game's original to match the server.
- `ServerFields { get; init; } -> int` — How many changes the server sent.
- `State { get; init; } -> ContentSyncState` — Where the sync stands.
- `StateId { get; init; } -> uint` — The server's content state (the hash of its document); 0 when there is none.

## ContentValue — One value read from a registry.
- `ContentValue()`
- `ContentValue(ContentValueType Type, long Integer, double Number, Guid Guid, string? Name)`
- `AsDouble { get; } -> double` — The value as a double (integers, floats, seconds for times).
- `Bool { get; } -> bool` — The value as a boolean: true when `ContentValue.Integer` is not 0.
- `Guid { get; init; } -> Guid` — The value of a `ContentValueType.Guid` field.
- `Integer { get; init; } -> long` — The value of an `ContentValueType.Int` (integer, enum or flag set) or `ContentValueType.Bool` field (0 or 1).
- `Name { get; init; } -> string?` — The enum value's name (enums), the flag names joined by "," (flag sets), else null.
- `Number { get; init; } -> double` — The value of a `ContentValueType.Float` field, or of a `ContentValueType.Time` field in seconds.
- `Type { get; init; } -> ContentValueType` — How the value is stored.

## ContentValueType — How a field's value is stored.
enum: None, Bool, Int, Float, Time, Guid

## CraftingGroupRef `0.2.14` — A tab of a workshop's crafting menu that the game already has, and in it one list (subcategory): where a recipe is listed.
- `CraftingGroupRef()` — A tab, and a subcategory in it (`Guid.Empty`: the tab's first list, the one without a header).
- `CraftingGroupRef(uint workshop, Guid group, Guid set = default(Guid))` — A tab, and a subcategory in it (`Guid.Empty`: the tab's first list, the one without a header).
- `Equals(CraftingGroupRef other) -> bool`
- `Group { get; } -> Guid` — The tab's GUID.
- `static implicit operator CraftingGroupRef(string text) -> CraftingGroupRef` — The text form, as `CraftingGroups` constants are.
- `static operator !=(CraftingGroupRef left, CraftingGroupRef right) -> bool`
- `static operator ==(CraftingGroupRef left, CraftingGroupRef right) -> bool`
- `static Parse(string text) -> CraftingGroupRef` — Reads the text form.
- `Set { get; } -> Guid` — The subcategory's GUID; `Guid.Empty`: the tab's first list.
- `static TryParse(string? text, out CraftingGroupRef result) -> bool` — Reads the text form; false when it is not one.
- `Workshop { get; } -> uint` — The workshop whose crafting menu it is (0: the player's own).

## CraftingGroups `0.2.14` — The tabs of the game's crafting menus, as `CraftingGroupRef` constants: `CraftingGroups.Carpenter.Essentials`.

## CraftingGroups.Alchemist `0.2.14` — Alchemist (NPC_Crafter_Alchemist01): workshop 0xc86873f9.
- `const Ammo = "0xc86873f9/a9502a9d-8b4a-47b1-89f8-84d6443c9fb2" -> string` — "Ammo".
- `const Armor = "0xc86873f9/b50bb867-6d23-4d69-8c51-9e61212e2aec" -> string` — "Armor".
- `const MagicalWeapons = "0xc86873f9/8e76b57f-2fdb-4eff-b31b-85522f4c0d74" -> string` — "Magical Weapons".
- `const OtherFurniture = "0xc86873f9/a8419106-81c9-46e8-86ea-ef45c884ba21" -> string` — "Other Furniture".
- `const ProductionPlace = "0xc86873f9/aa007b5a-f979-46fc-b4bc-4946f5cd684f" -> string` — "Production place".
- `const Resources = "0xc86873f9/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const Supplies = "0xc86873f9/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".
- `const ToolsAndComponents = "0xc86873f9/35b13ff3-9156-4ec5-99bc-bee7d4508116" -> string` — "Tools & Components".
- `const Weapons = "0xc86873f9/2632c31c-37fe-48f6-b788-dfa649065edb" -> string` — "Weapons".

## CraftingGroups.AlchemyStation `0.2.14` — Alchemy Station (Prop_Production_T3_Alchemy_Station): workshop 0xd08b9300.
- `const Resources = "0xd08b9300/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.AncientResearcher `0.2.14` — Ancient Researcher (NPC_Townsfolk_Female_Villager_06_LoraineCrim_AncientResearcher): workshop 0xfb819774.
- `const Armor = "0xfb819774/b50bb867-6d23-4d69-8c51-9e61212e2aec" -> string` — "Armor".
- `const Decorative = "0xfb819774/b9c387f3-04fc-4d02-b9f7-5b01b29282fc" -> string` — "Decorative".
- `const ProductionPlace = "0xfb819774/aa007b5a-f979-46fc-b4bc-4946f5cd684f" -> string` — "Production place".
- `const Resources = "0xfb819774/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const Weapons = "0xfb819774/2632c31c-37fe-48f6-b788-dfa649065edb" -> string` — "Weapons".

## CraftingGroups.Bard `0.2.14` — Bard (NPC_Crafter_Bard01): workshop 0xe672b030.
- `const ComfortFurniture = "0xe672b030/84de1663-d5f4-4a39-95d2-7fc5a5d3a12b" -> string` — "Comfort Furniture".
- `const Cosmetic = "0xe672b030/ed4a5a30-3917-45a1-9a70-bae4d6d6db2f" -> string` — "Cosmetic".
- `const Decorative = "0xe672b030/b9c387f3-04fc-4d02-b9f7-5b01b29282fc" -> string` — "Decorative".
- `const Essentials = "0xe672b030/8c91ca52-be38-468c-ae74-bc6a5ffab28e" -> string` — "Essentials".
- `const Instruments = "0xe672b030/783600e0-270f-43a6-b7c3-90ded5aaf21c" -> string` — "Instruments".
- `const OtherFurniture = "0xe672b030/a8419106-81c9-46e8-86ea-ef45c884ba21" -> string` — "Other Furniture".

## CraftingGroups.Beehive `0.2.14` — Beehive (Prop_Production_T4_Beehive): workshop 0x269a27a7.
- `const Resources = "0x269a27a7/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const Supplies = "0x269a27a7/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".

## CraftingGroups.Blacksmith `0.2.14` — Blacksmith (NPC_Crafter_Blacksmith01): workshop 0x41bf48ce.
- `const Armor = "0x41bf48ce/b50bb867-6d23-4d69-8c51-9e61212e2aec" -> string` — "Armor".
- `const Essentials = "0x41bf48ce/8c91ca52-be38-468c-ae74-bc6a5ffab28e" -> string` — "Essentials".
- `const ProductionPlace = "0x41bf48ce/aa007b5a-f979-46fc-b4bc-4946f5cd684f" -> string` — "Production place".
- `const Resources = "0x41bf48ce/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const Survival = "0x41bf48ce/65e6c94a-31c7-4c1f-ba8d-feb85bb87d42" -> string` — "Survival".
- `const ToolsAndComponents = "0x41bf48ce/35b13ff3-9156-4ec5-99bc-bee7d4508116" -> string` — "Tools & Components".
- `const Weapons = "0x41bf48ce/2632c31c-37fe-48f6-b788-dfa649065edb" -> string` — "Weapons".

## CraftingGroups.BlastFurnace `0.2.14` — Blast Furnace (Prop_Production_T5_BlastFurnace): workshop 0x485ec29c.
- `const Resources = "0x485ec29c/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.ButterChurn `0.2.14` — Butter Churn (Prop_Production_T3_Butter_Churn): workshop 0x6ad05fce.
- `const Resources = "0x6ad05fce/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const Supplies = "0x6ad05fce/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".

## CraftingGroups.Carpenter `0.2.14` — Carpenter (NPC_Crafter_Carpenter01): workshop 0xbddd74e6.
- `const ComfortFurniture = "0xbddd74e6/84de1663-d5f4-4a39-95d2-7fc5a5d3a12b" -> string` — "Comfort Furniture".
- `const Essentials = "0xbddd74e6/8c91ca52-be38-468c-ae74-bc6a5ffab28e" -> string` — "Essentials".
- `const OtherFurniture = "0xbddd74e6/a8419106-81c9-46e8-86ea-ef45c884ba21" -> string` — "Other Furniture".
- `const ProductionPlace = "0xbddd74e6/aa007b5a-f979-46fc-b4bc-4946f5cd684f" -> string` — "Production place".
- `const Resources = "0xbddd74e6/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const ToolsAndComponents = "0xbddd74e6/35b13ff3-9156-4ec5-99bc-bee7d4508116" -> string` — "Tools & Components".

## CraftingGroups.CharcoalKiln `0.2.14` — Charcoal Kiln (Prop_Production_T1_Charcoal_Kiln): workshop 0x2546ae49.
- `const Resources = "0x2546ae49/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.CoffeeBrewer `0.2.14` — Coffee Brewer (Prop_Production_T6_coffeemaker_brewer): workshop 0x6b8247d6.
- `const Supplies = "0x6b8247d6/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".

## CraftingGroups.Collector `0.2.14` — Collector (NPC_Crafter_CryptKeeper01): workshop 0xad433e64.
- `const Ammo = "0xad433e64/a9502a9d-8b4a-47b1-89f8-84d6443c9fb2" -> string` — "Ammo".
- `const ComfortFurniture = "0xad433e64/84de1663-d5f4-4a39-95d2-7fc5a5d3a12b" -> string` — "Comfort Furniture".
- `const Decorative = "0xad433e64/b9c387f3-04fc-4d02-b9f7-5b01b29282fc" -> string` — "Decorative".
- `const Essentials = "0xad433e64/8c91ca52-be38-468c-ae74-bc6a5ffab28e" -> string` — "Essentials".
- `const OtherFurniture = "0xad433e64/a8419106-81c9-46e8-86ea-ef45c884ba21" -> string` — "Other Furniture".
- `const ProductionPlace = "0xad433e64/aa007b5a-f979-46fc-b4bc-4946f5cd684f" -> string` — "Production place".
- `const Resources = "0xad433e64/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const Supplies = "0xad433e64/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".

## CraftingGroups.ColoringStation `0.2.14` — Coloring Station (_Prop_Production_T2_ColoringStation): workshop 0x3b51fb95.
- `const Resources = "0x3b51fb95/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.CookingStation `0.2.14` — Cooking Station (Prop_production_T2_Cooking_Station): workshop 0x39fbd98f.
- `const Supplies = "0x39fbd98f/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".

## CraftingGroups.DecorationWorkbench `0.2.14` — Decoration Workbench (Craftingstation_Carpenter_Workbench): workshop 0x8996a8c4.
- `const Decorative = "0x8996a8c4/b9c387f3-04fc-4d02-b9f7-5b01b29282fc" -> string` — "Decorative".

## CraftingGroups.DryingRack `0.2.14` — Drying Rack (Prop_Production_T1_Drying_Rack): workshop 0x6b95dc33.
- `const Resources = "0x6b95dc33/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const Supplies = "0x6b95dc33/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".

## CraftingGroups.EctoplasmPress `0.2.14` — Ectoplasm Press (Prop_NPC_T2_cryptKeeper_EctoplasmPress): workshop 0x1abf2d27.
- `const Resources = "0x1abf2d27/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.Farmer `0.2.14` — Farmer (NPC_Crafter_Farmer01): workshop 0xd9a35a6f.
- `const AnimalHusbandry = "0xd9a35a6f/95a1cb62-ff2b-4b44-865f-0b6932c9fbe5" -> string` — "Animal Husbandry".
- `const Decorative = "0xd9a35a6f/b9c387f3-04fc-4d02-b9f7-5b01b29282fc" -> string` — "Decorative".
- `const Essentials = "0xd9a35a6f/8c91ca52-be38-468c-ae74-bc6a5ffab28e" -> string` — "Essentials".
- `const OtherFurniture = "0xd9a35a6f/a8419106-81c9-46e8-86ea-ef45c884ba21" -> string` — "Other Furniture".
- `const ProductionPlace = "0xd9a35a6f/aa007b5a-f979-46fc-b4bc-4946f5cd684f" -> string` — "Production place".
- `const Resources = "0xd9a35a6f/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const Soil = "0xd9a35a6f/74c737bf-3f4f-4e29-b7fe-7037a1685949" -> string` — "Soil".
- `const ToolsAndComponents = "0xd9a35a6f/35b13ff3-9156-4ec5-99bc-bee7d4508116" -> string` — "Tools & Components".

## CraftingGroups.Fisher `0.2.14` — Fisher (NPC_Townsfolk_Male_Villager_08_fisher_Trader): workshop 0xc4acc104.
- `const Essentials = "0xc4acc104/8c91ca52-be38-468c-ae74-bc6a5ffab28e" -> string` — "Essentials".
- `const OtherFurniture = "0xc4acc104/a8419106-81c9-46e8-86ea-ef45c884ba21" -> string` — "Other Furniture".
- `const Resources = "0xc4acc104/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const Supplies = "0xc4acc104/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".

## CraftingGroups.Forge `0.2.14` — Forge (Prop_Production_T1_Forge): workshop 0x8dca2c49.
- `const Resources = "0x8dca2c49/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.GrindingStones `0.2.14` — Grinding Stones (Prop_Production_T1_HandGrinder): workshop 0x056d94f8.
- `const Resources = "0x056d94f8/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const TerrainMaterial = "0x056d94f8/6c66ad67-1cc5-45f2-badc-f94971b1f120" -> string` — "Terrain Material".

## CraftingGroups.Hand `0.2.14` — The player's own crafting menu: workshop 0x00000000.
- `const Armor = "0x00000000/b50bb867-6d23-4d69-8c51-9e61212e2aec" -> string` — "Armor".
- `const Essentials = "0x00000000/8c91ca52-be38-468c-ae74-bc6a5ffab28e" -> string` — "Essentials".
- `const Supplies = "0x00000000/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".
- `const Survival = "0x00000000/65e6c94a-31c7-4c1f-ba8d-feb85bb87d42" -> string` — "Survival".
- `const WaterUtilities = "0x00000000/1924563b-8056-4b8e-9317-9d848d084b3b" -> string` — "Water utilities".
- `const Weapons = "0x00000000/2632c31c-37fe-48f6-b788-dfa649065edb" -> string` — "Weapons".

## CraftingGroups.HandSpindle `0.2.14` — Hand Spindle (Prop_Production_T2_NPC_Hand_Spindle): workshop 0x73306967.
- `const Resources = "0x73306967/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.Hunter `0.2.14` — Hunter (NPC_Crafter_Hunter01): workshop 0x841ad755.
- `const Ammo = "0x841ad755/a9502a9d-8b4a-47b1-89f8-84d6443c9fb2" -> string` — "Ammo".
- `const Armor = "0x841ad755/b50bb867-6d23-4d69-8c51-9e61212e2aec" -> string` — "Armor".
- `const ComfortFurniture = "0x841ad755/84de1663-d5f4-4a39-95d2-7fc5a5d3a12b" -> string` — "Comfort Furniture".
- `const Decorative = "0x841ad755/b9c387f3-04fc-4d02-b9f7-5b01b29282fc" -> string` — "Decorative".
- `const Essentials = "0x841ad755/8c91ca52-be38-468c-ae74-bc6a5ffab28e" -> string` — "Essentials".
- `const ProductionPlace = "0x841ad755/aa007b5a-f979-46fc-b4bc-4946f5cd684f" -> string` — "Production place".
- `const Resources = "0x841ad755/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const Supplies = "0x841ad755/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".
- `const Survival = "0x841ad755/65e6c94a-31c7-4c1f-ba8d-feb85bb87d42" -> string` — "Survival".
- `const Weapons = "0x841ad755/2632c31c-37fe-48f6-b788-dfa649065edb" -> string` — "Weapons".

## CraftingGroups.InfusionAltar `0.2.14` — Infusion Altar (Prop_Production_T7_Infusion_Station): workshop 0x865085ea.
- `const Resources = "0x865085ea/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.KilnStation `0.2.14` — Kiln (Prop_Production_T2_Kiln): workshop 0x847eab53.
- `const Decorative = "0x847eab53/b9c387f3-04fc-4d02-b9f7-5b01b29282fc" -> string` — "Decorative".
- `const Resources = "0x847eab53/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.Kitchen `0.2.14` — Kitchen (Craftingstation_Farmer_Kitchen): workshop 0x07ce61f0.
- `const Supplies = "0x07ce61f0/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".

## CraftingGroups.Laboratory `0.2.14` — Laboratory (Prop_Production_T4_Laboratory): workshop 0x20b86c20.
- `const Resources = "0x20b86c20/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.Loom `0.2.14` — Loom (Prop_NPC_Loom): workshop 0x3a3cc728.
- `const Resources = "0x3a3cc728/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.LunarNewYearTrader `0.2.14` — Lunar New Year Trader (NPC_Townsfolk_Female_Villager_05_name_chineseNewYear_Trader): workshop 0x8554479a.
- `const ComfortFurniture = "0x8554479a/84de1663-d5f4-4a39-95d2-7fc5a5d3a12b" -> string` — "Comfort Furniture".
- `const Cosmetic = "0x8554479a/ed4a5a30-3917-45a1-9a70-bae4d6d6db2f" -> string` — "Cosmetic".
- `const Decorative = "0x8554479a/b9c387f3-04fc-4d02-b9f7-5b01b29282fc" -> string` — "Decorative".
- `const Fun = "0x8554479a/98541544-4d4f-432f-9102-f2481c912e2e" -> string` — "Fun".
- `const MagicalWeapons = "0x8554479a/8e76b57f-2fdb-4eff-b31b-85522f4c0d74" -> string` — "Magical Weapons".
- `const OtherFurniture = "0x8554479a/a8419106-81c9-46e8-86ea-ef45c884ba21" -> string` — "Other Furniture".
- `const Weapons = "0x8554479a/2632c31c-37fe-48f6-b788-dfa649065edb" -> string` — "Weapons".

## CraftingGroups.Mill `0.2.14` — Mill (Prop_Production_T6_improvedGrinder): workshop 0x7251e0d8.
- `const Resources = "0x7251e0d8/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.Oven `0.2.14` — Oven (Prop_Production_T3_Oven): workshop 0x1ee9f0bf.
- `const Resources = "0x1ee9f0bf/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const Supplies = "0x1ee9f0bf/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".

## CraftingGroups.Roaster `0.2.14` — Roaster (Prop_Production_T6_coffeemaker_roaster): workshop 0xa6ebcde3.
- `const Resources = "0xa6ebcde3/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.SeafoodGrill `0.2.14` — Seafood Grill (Prop_Production_T6_MedievalGrill): workshop 0x7d96fde8.
- `const Supplies = "0x7d96fde8/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".

## CraftingGroups.Seedbed `0.2.14` — Seedbed (Prop_Production_T1_Seedbed): workshop 0xba67134f.
- `const Seedlings = "0xba67134f/c23c3be9-5824-4b9a-ba64-daeaa16771da" -> string` — "Seedlings".

## CraftingGroups.Smelter `0.2.14` — Smelter (Prop_Production_T2_Smelter): workshop 0xf5aab50a.
- `const Resources = "0xf5aab50a/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.SparkworksWorkbench `0.2.14` — Sparkworks Workbench (Craftingstation_Logic_System_Workbench): workshop 0xe989624a.
- `const Essentials = "0xe989624a/8c91ca52-be38-468c-ae74-bc6a5ffab28e" -> string` — "Essentials".
- `const SparkworksTools = "0xe989624a/4f2c4640-be1b-46cb-ba34-d8b926385a89" -> string` — "Sparkworks Tools".

## CraftingGroups.SpinningMachine `0.2.14` — Spinning Machine (Prop_Production_T5_CardingMachine_Spinning): workshop 0xd4796c9a.
- `const Resources = "0xd4796c9a/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const Supplies = "0xd4796c9a/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".

## CraftingGroups.SpinningWheel `0.2.14` — Spinning Wheel (Prop_Production_T4_Spinning_Wheel): workshop 0xe26134b5.
- `const Resources = "0xe26134b5/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".
- `const Supplies = "0xe26134b5/dc3627f7-773c-416c-a8c4-6e27a75eb1e1" -> string` — "Supplies".

## CraftingGroups.TableSaw `0.2.14` — Table Saw (Prop_Production_T2_Table_Saw): workshop 0x1abf8143.
- `const Resources = "0x1abf8143/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.TanningStation `0.2.14` — Tanning Station (Prop_Production_T3_Tanning_Station): workshop 0xa4b9eaa5.
- `const Resources = "0xa4b9eaa5/cf73af1a-9618-422f-ba2f-32a332a23787" -> string` — "Resources".

## CraftingGroups.Workbench `0.2.14` — Workbench (Craftingstation_Workbench): workshop 0x4da88f73.
- `const Blocks = "0x4da88f73/d66069c5-80f8-4e6d-9d73-30b3b145cd79" -> string` — "Blocks".
- `const ComfortFurniture = "0x4da88f73/84de1663-d5f4-4a39-95d2-7fc5a5d3a12b" -> string` — "Comfort Furniture".
- `const Decorative = "0x4da88f73/b9c387f3-04fc-4d02-b9f7-5b01b29282fc" -> string` — "Decorative".
- `const Essentials = "0x4da88f73/8c91ca52-be38-468c-ae74-bc6a5ffab28e" -> string` — "Essentials".
- `const Fun = "0x4da88f73/98541544-4d4f-432f-9102-f2481c912e2e" -> string` — "Fun".
- `const OtherFurniture = "0x4da88f73/a8419106-81c9-46e8-86ea-ef45c884ba21" -> string` — "Other Furniture".
- `const RoadBlocks = "0x4da88f73/fa99ee99-ad44-4ebd-83ef-316ea970f273" -> string` — "Road Blocks".
- `const RoofBlocks = "0x4da88f73/61c837d4-b10e-4361-9c66-156e1fae187d" -> string` — "Roof Blocks".
- `const TerrainMaterial = "0x4da88f73/6c66ad67-1cc5-45f2-badc-f94971b1f120" -> string` — "Terrain Material".

## DropStack — One stack of a drop table (depth-first over the table's groups).
- `DropStack()`
- `DropStack(ItemId Item, int CountMin, int CountMax, float Chance, float Weight, int Group)`
- `Chance { get; init; } -> float` — spawnProbability, 0..1.
- `CountMax { get; init; } -> int` — The most the stack drops.
- `CountMin { get; init; } -> int` — The fewest the stack drops.
- `Group { get; init; } -> int` — Index of the group the stack belongs to (read-only).
- `Item { get; init; } -> ItemId` — The item the stack drops.
- `Weight { get; init; } -> float` — Weight inside a selector group (one of the group's stacks is picked by weight).

## `Field<T>` — A path into a game-data entry with the type of its value: `GameFields.Items.DamageSetup.DmgMod` is a `Field<float>` with the path `damageSetup.dmgMod`.
- `Field()`
- `Field(string Section, string Path)`
- `Below<TBelow>(string rest) -> Field<TBelow>` — A path below this one that the catalog does not list, such as a field of one type of a list of variants...
- `Path { get; init; } -> string` — The string a data pack writes.
- `Section { get; init; } -> string` — The catalog section it belongs to: `items`, `templates`, ...

## FieldAccess — Typed access to game data: the overloads of `Get`, `Original`, `Set` and `Reset` that take a `Field`.
- `static Get<T>(this ContentEntry entry, Field<T> field) -> T` — The current value.
- `static Get<T>(this ContentEntry entry, ReadOnlyField<T> field) -> T` — The current value.
- `static Get<T>(this GameDataEntry entry, Field<T> field) -> T` — The current value.
- `static Get<T>(this GameDataEntry entry, ReadOnlyField<T> field) -> T` — The current value.
- `static Original<T>(this ContentEntry entry, Field<T> field) -> T` — The game's original value.
- `static Original<T>(this GameDataEntry entry, Field<T> field) -> T` — The game's original value.
- `static Reset<T>(this ContentEntry entry, Field<T> field) -> bool` — Takes the calling mod's layers of the field (and below) off.
- `static Reset<T>(this GameDataEntry entry, Field<T> field) -> bool` — Takes the calling mod's layers of the field (and below) off.
- `static Set<T>(this ContentEntry entry, Field<T> field, FieldValue<T> value) -> int` — Sets a field as the calling mod.
- `static Set<T>(this ContentEntry entry, ReadOnlyField<T> field, FieldValue<T> value) -> int` — Not possible: Kiln does not change identity fields and names.
- `static Set<T>(this GameDataEntry entry, Field<T> field, FieldValue<T> value) -> int` — Sets a field as the calling mod.
- `static Set<T>(this GameDataEntry entry, ReadOnlyField<T> field, FieldValue<T> value) -> int` — Not possible: Kiln does not change identity fields and names.

## FieldPath — Paths for generated code.
- `static Element(string list, int index, int count = 0) -> string` — The path of a list's element.

## `FieldValue<T>` — The value of a `Field`.
- `static implicit operator FieldValue<T>(T value) -> FieldValue<T>` — A value of the field's type as a `FieldValue`.
- `Value { get; } -> T` — The value.

## FieldValues — Field values for a definition, by typed path: `new FieldValues { { GameFields.Items.MaxStackSize, 1 } }`.
- `Add<T>(Field<T> field, FieldValue<T> value) -> void` — Adds a value for a typed field.
- `ContainsKey(string key) -> bool` — True when a value for this path was added.
- `Count { get; } -> int` — How many fields there are.
- `GetEnumerator() -> IEnumerator<KeyValuePair<string, object>>` — Every path with its value.
- `Keys { get; } -> IEnumerable<string>` — The paths of the fields.
- `this[string key] { get; } -> object` — The value of the field with this path.
- `TryGetValue(string key, out object value) -> bool` — Gets the value of the field with this path; false when there is none.
- `Values { get; } -> IEnumerable<object>` — The values, as the JSON a data pack writes.

## GameDataClone — A clone definition for `GameDataRegistry.Define`: the data-pack `"add"` entry as an object.
- `Base { get; init; } -> string` — The entry to copy: a name, GUID or `0x` id of the same section (a clone too).
- `Description { get; init; } -> string?` — Perks, buffs: the shown description in every language.
- `DisplayName { get; init; } -> string?` — Perks, buffs: the shown name / description in every language (`GameDataClone.Texts` per language win).
- `Fields { get; init; } -> Dictionary<string, Json.Nodes.JsonNode?>` — Field values by path, as the JSON values a data pack writes.
- `Guid { get; init; } -> Guid` — The clone's fixed GUID.
- `Name { get; init; } -> string` — The clone's name (letters, digits, `_ - .`): templates and perks get it as their debug name; rules, references and...
- `Texts { get; init; } -> Dictionary<string, GameDataTexts>` — Perks, buffs: texts per language key ("en", "de", "pt-BR"; "*" = every language).
- `ToJson() -> Json.Nodes.JsonObject` — The clone as the data pack's `"add"` object.
- `Values { get; } -> FieldValues` — `GameDataClone.Fields` by typed path...

## GameDataConfig — A config value of an entry (`GameDataEntry.Config`).
- `Exists { get; } -> bool` — The entry has this config (else `GameDataConfig.Set` adds it).
- `Get() -> ContentValue` — The config value now.
- `Original() -> ContentValue` — The game's original config value.
- `Path { get; } -> string` — The config value's path in the entry, such as `impactValues.ManaCost`.
- `Reset() -> bool` — Takes the calling mod's change of the value off.
- `Set(double value) -> void` — Sets the config value as the calling mod; adds the config when the entry does not have it.
- `Set(long value) -> void` — As `GameDataConfig.Set`, with an integer value.
- `Set(string value) -> void` — As `GameDataConfig.Set`, with a name or other text value.

## GameDataDefinition — One clone definition as kiln.gamedata keeps it (`Content.GameDataDefinitions`).
- `GameDataDefinition(GameDataSection Section, Guid Guid, uint Id, string Name, string Base, string State, string Owner, string? Mod, bool IsPlaceholder, string? Error, IReadOnlyList<string> Warnings, string DefinitionJson)`
- `Base { get; init; } -> string` — The entry it was copied from.
- `DefinitionJson { get; init; } -> string` — The definition as given (what Kiln clients get through the content sync).
- `Error { get; init; } -> string?` — Why the clone is not in the registry, if it is not.
- `Guid { get; init; } -> Guid` — The clone's GUID.
- `Id { get; init; } -> uint` — The clone's id.
- `IsPlaceholder { get; init; } -> bool` — True for a placeholder "[Missing: mod:name]" that keeps saved items and buffs of a clone whose mod is gone.
- `Mod { get; init; } -> string?` — The id of the mod that defined it (its code or its data pack); for a placeholder, the missing mod's.
- `Name { get; init; } -> string` — The clone's name.
- `Owner { get; init; } -> string` — The owner that defined it, as kiln.gamedata reports it.
- `Section { get; init; } -> GameDataSection` — The registry the clone is in.
- `State { get; init; } -> string` — `registered`, `pending` (waiting for a world), `waiting` (the world's registry is not loaded yet), `error`...
- `Warnings { get; init; } -> IReadOnlyList<string>` — Parts of the definition that were skipped.

## GameDataEntry — One template, arsenal, sub-sequence, buff, perk, skill node or item.
- `Address { get; } -> ulong` — The struct in game memory (diagnostics).
- `Config(string alias) -> GameDataConfig` — A config value by alias or `0x` id (`ManaCost`, `NoiseRange`, `0xa19409b2`): items, perks and buffs `impactValues`...
- `Event(string type, int k = 0) -> GameDataEvent` — Sequences: the k-th event of a type (`SpawnEntityEvent`, `PayUsageCost`, ...).
- `Fields(string? prefix = null, int limit = 0) -> IReadOnlyList<GameDataField>` — Every leaf below `prefix` (null: the whole entry), from the game's reflection.
- `Get(string path) -> ContentValue` — The current value of a leaf.
- `GetJson(string path = "") -> Json.Nodes.JsonNode?` — Any node as JSON: `{path, type, value, original, writable, address}` (an array when the path fans out with `[*]`...
- `GetNumber(string path) -> double` — The current value of a leaf as a number: `GameDataEntry.Get` read as `ContentValue.AsDouble`.
- `Guid { get; } -> Guid` — The entry's GUID.
- `Id { get; } -> uint` — The entry's id.
- `IsClone { get; } -> bool` — The entry is a clone (an `"add"` definition), not one of the game's.
- `Modified { get; } -> bool` — At least one leaf differs from the game's original.
- `Name { get; } -> string` — The entry's name.
- `Original(string path) -> ContentValue` — The game's original value of a leaf.
- `Reset(string path) -> bool` — Takes the calling mod's layers of the path (and below) off.
- `Section { get; } -> GameDataSection` — The registry the entry belongs to.
- `Set(string path, Guid value) -> void` — As `GameDataEntry.Set`, with the GUID of a referenced resource.
- `Set(string path, TimeSpan value) -> void` — As `GameDataEntry.Set`, with a time (written in seconds).
- `Set(string path, bool value) -> void` — As `GameDataEntry.Set`, with a boolean value.
- `Set(string path, double value) -> void` — Sets a leaf as the calling mod (a layer over the original, reverted when the mod unloads).
- `Set(string path, int value) -> void` — As `GameDataEntry.Set`, with an integer value.
- `Set(string path, long value) -> void` — As `GameDataEntry.Set`, with an integer value.
- `Set(string path, string value) -> void` — An enum name, a GUID, a template / sequence name for references, or `"0x..."`.
- `SetJson(string path, string json) -> int` — Any JSON value for the path, as in data packs: numbers, names, flag lists, objects for structs, `{"mul": 1.5}`.
- `TooltipValues(string list = "uiValues") -> IReadOnlyList<TooltipValue>` `0.2.10` — The tooltip rows of this item, perk, buff or skill node (`list`: `uiValues`, or an item's `uiBuffValues`), with...

## GameDataEvent — A sequence event (`GameDataEntry.Event`): time and duration in seconds, other fields by path.
- `Duration { get; set; } -> TimeSpan` — The event's `duration` field.
- `Get(string field) -> ContentValue` — The current value of a field of the event.
- `Path { get; } -> string` — The event's path in the entry.
- `SetJson(string field, string json) -> void` — Sets a field of the event from a JSON value, as `GameDataEntry.SetJson` does.
- `Time { get; set; } -> TimeSpan` — The event's `time` field.

## GameDataField — One leaf below an entry (from the game's reflection).
- `GameDataField(string Path, string Type, Json.Nodes.JsonNode? Value, bool Writable, Json.Nodes.JsonNode? Original)`
- `Original { get; init; } -> Json.Nodes.JsonNode?` — The game's original value, as JSON.
- `Path { get; init; } -> string` — The leaf's path below the entry.
- `Type { get; init; } -> string` — The game's type of the leaf.
- `Value { get; init; } -> Json.Nodes.JsonNode?` — The current value, as JSON.
- `Writable { get; init; } -> bool` — True when a mod can set it.

## GameDataItemExtensions — Item config values (kiln.gamedata) on kiln.content's item entries.
- `static Config(this ContentEntry item, string alias) -> GameDataConfig` — A config value of an item by alias (`ManaCost`, `StaminaCost`, `Damage`) or `0x` id: `impactValues.<alias>`; its...
- `static Data(this ContentEntry item) -> GameDataEntry` — The item as a kiln.gamedata entry (config values, lists).

## GameDataRegistry — One kiln.gamedata registry.
- `Count { get; } -> int` — How many entries the registry holds.
- `Define(GameDataClone clone) -> GameDataDefinition` — Defines a clone (templates, sequences, perks, buffs): a new entry with a fixed GUID made from `base`, with its own...
- `Define(GameDataClone clone, DefinitionRegistrationOptions options) -> DefinitionRegistrationResult` `0.2.4` — `Content.Buffs` only: defines a buff clone and says what became of it (chapter 17's result-bearing definition).
- `Define(Json.Nodes.JsonObject definition) -> GameDataDefinition` — Defines a clone (templates, sequences, perks, buffs): a new entry with a fixed GUID made from `base`, with its own...
- `Define(Json.Nodes.JsonObject definition, DefinitionRegistrationOptions options) -> DefinitionRegistrationResult` `0.2.4` — `Content.Buffs` only: defines a buff clone and says what became of it (chapter 17's result-bearing definition).
- `Definitions() -> IReadOnlyList<GameDataDefinition>` — This section's clone definitions (any thread).
- `Dump(string? key = null, string? path = null) -> string` — Writes entries as JSON (key null: all of them) to `path` (null: `kiln/data/content/gamedata-<section>.json`).
- `Get(Guid guid) -> GameDataEntry?` — The entry with this GUID, or null.
- `Get(string key) -> GameDataEntry?` — The entry for a key, or null.
- `Get(uint id) -> GameDataEntry?` — The entry with this id, or null.
- `GetBudget() -> CapacityQueryResult` `0.2.4` — `Content.Buffs` only: Kiln's room for buff clones, all mods together, for this process.
- `GetEnumerator() -> IEnumerator<GameDataEntry>` — Every entry of the registry, in order.
- `GetRegistration(Guid registrationId) -> DefinitionRegistrationResult` `0.2.4` — `Content.Buffs` only: what became of a definition since its `GameDataRegistry.Define` call, by its...
- `IsClone(Guid guid) -> bool` — The entry is a clone (an "add" definition), not one of the game's.
- `Section { get; } -> GameDataSection` — Which registry this is.
- `this[int index] { get; } -> GameDataEntry` — The entry at `index`, 0 to `GameDataRegistry.Count` - 1.

## GameDataSection — The game-data registries of kiln.gamedata.
enum: Templates, Arsenals, Sequences, Buffs, Perks, Skills, Items

## GameDataTexts — Texts of a perk or buff clone in one language.
- `GameDataTexts(string? Name = null, string? Description = null)`
- `Description { get; init; } -> string?` — The shown description; null: not given.
- `Name { get; init; } -> string?` — The shown name; null: not given.

## GameTextSource `0.2.22` — Where a `GameTextTable`'s texts come from.
enum: None, Game, KilnCatalog

## GameTextTable `0.2.22` — One language's game texts: loca tag id -> text, with the game's markup (`<b>`, `<lorename>`, ...; `GameTextTable.GetPlain` strips it).
- `Available { get; } -> bool` — The table has texts (`GameTextTable.Source` is not `GameTextSource.None`).
- `ContainsKey(uint key) -> bool` — The table has a text for the tag id.
- `Count { get; } -> int` — How many texts the table has.
- `Get(Guid locaTag) -> string?` — The text of a LocaTag reference (a GUID, as `ItemInfo.name`), hashed as the game does.
- `Get(uint locaId) -> string?` — The text of a loca tag id with the game's markup, or null (no such text, an empty one).
- `GetEnumerator() -> IEnumerator<KeyValuePair<uint, string>>` — Every (tag id, text) pair, with markup.
- `GetPlain(uint locaId) -> string?` — As `GameTextTable.Get` without the game's markup tags, trimmed; null for none or an empty one.
- `Keys { get; } -> IEnumerable<uint>` — Every tag id the table has a text for.
- `Language { get; } -> string` — The language, as the game keys it (`"de-de"`).
- `Reason { get; } -> string` — Why there are no texts ("" when `GameTextTable.Available`): a dedicated server, a language the game does not ship, a...
- `Source { get; } -> GameTextSource` — Where the texts come from; `GameTextSource.None` when there are none.
- `this[uint key] { get; } -> string` — The text of a tag id (with markup).
- `TryGetValue(uint key, out string value) -> bool` — The text of a tag id (with markup), when the table has one.
- `Values { get; } -> IEnumerable<string>` — Every text (with markup).

## GearArmorKind `0.2.12` — A kind of armour an item gives (`GearStats.Armor`).
enum: Physical, Magical, Cut, Pierce, Blunt, Fire, Ice, Shroud, Lightning

## GearDamageKind `0.2.12` — A kind of damage a weapon's hit is shared out in (`GearStats.Damage`), or one of the whole numbers the game keeps per kind for what a tool harvests and breaks (wood...
enum: Cut, Pierce, Blunt, Fire, Ice, Shroud, Lightning, Poison, Healing, Wood, Stone, Metal, Explosion

## GearStats `0.2.12` — The gear values of one item type, in the game's tooltip numbers (from `Content.Gear`).
- `Armor(GearArmorKind kind) -> int` — The armour of a kind the tooltip shows at `GearStats.Level`.
- `Block { get; set; } -> double` — How well the item blocks (`blockSetup.quality`, 0..1).
- `Damage(GearDamageKind kind) -> double` — A kind's share of the damage (0.5 = half of `GearStats.Power`), or for wood, stone, metal and explosion the whole...
- `HasArmor { get; } -> bool` — The item has armour values (`armorSetup.isSet`): armour, shields, some rings.
- `HasDamage { get; } -> bool` — The item deals damage of its own (`damageSetup.isSet`): weapons, wands, ammunition, tools.
- `Item { get; } -> string` — The item's debug name.
- `Level { get; } -> int` — The lowest level a copy is made at (`itemLevelRange.minLevel`): the level the recipe tooltip shows, and the one...
- `MaxLevel { get; } -> int` — The highest level a copy can roll (`itemLevelRange.maxLevel`); the same as `GearStats.Level` for crafted gear.
- `MaxStack { get; set; } -> int` — The max stack size (`maxStackSize`, 1..65535).
- `Power { get; set; } -> int` — The damage the tooltip shows at `GearStats.Level` (0 for an item without damage).
- `PowerAt(int level) -> int` — The damage the tooltip shows for a copy at `level` (the game's curve over the item's dmgMod and speed).
- `Rarity { get; set; } -> ItemRarity` — The item type's rarity (`rarity`): what the recipe tooltip shows and a copy is made with when the game does not roll...
- `Reset() -> int` — Takes the calling mod's layers off every field these values write (the game's values, or other mods', come back).
- `Rolled { get; set; } -> bool` — The game rolls the rarity of each new copy (`generateRarity`), as for loot weapons; false: every copy is...
- `SetArmor(GearArmorKind kind, int armor) -> void` — Sets the armour of a kind the tooltip shows at `GearStats.Level` (0..100000), solved into the item's...
- `SetDamage(GearDamageKind kind, double value) -> void` — Sets a kind's share of the damage (0..10), or the whole number of wood, stone, metal or explosion (0..1000000).
- `SetLevels(int lowest, int? highest = null) -> void` — Sets the levels a copy is made at (1..55, lowest first; one level when `highest` is left out).
- `Socket { get; set; } -> bool` — New copies get a gem socket (`gemSlotOverride`); on gear other than weapons this needs Kiln's sockets...
- `Speed { get; set; } -> double` — The damage factor of the weapon's speed class (`damageSetup.speedMod`); it multiplies `GearStats.Power`.

## ItemDefinition — A new item: a clone of an existing item (`ItemDefinition.BaseOn`: its model, icon, behaviour and stats) under a fixed GUID of its own, with its own debug name, texts...
- `BaseOn { get; init; } -> string` — The item to clone: debug name (`Weapon_T1_1H_SwordEpic_01`), `"0x..."` id or GUID.
- `Description { get; init; } -> string?` — The tooltip description.
- `DisplayName { get; init; } -> string?` — The name players see (all languages).
- `Drops { get; init; } -> IReadOnlyList<ItemDrop>?` — Drop tables (enemy drops, fixed chests) that get a stack of this item.
- `Fields { get; init; } -> IReadOnlyDictionary<string, object>?` — Field values that differ from the base item, by path (`"damageSetup.dmgMod"` = 1.5, `"rarity"` = "Legendary"...
- `Guid { get; init; } -> Guid` — The fixed GUID of the item (never change it once released).
- `Icon { get; init; } -> string?` — The icon: `"item:<name>"` (another item's icon and icon setup), an item GUID, or a PNG file (relative to the mod's...
- `Id { get; } -> uint` — The item id this definition gets (FNV-1a of the GUID).
- `Loot { get; init; } -> ItemLoot?` — Random loot: the item can be rolled in containers and world pickups like `ItemLoot.Like`.
- `Lore { get; init; } -> string?` — The lore text.
- `Model { get; init; } -> string?` — The model: `"item:<name>"` takes another item's whole visual setup (held / worn model, visual entity, model offsets...
- `Name { get; init; } -> string` — The debug name (`A-Z a-z 0-9 _ - .`), unique: how packs, recipes and admin tools name the item.
- `Perks { get; init; } -> IReadOnlyList<string?>?` — Perk slots (up to 5): a perk GUID, `"item:<name>"` (that item's perk in the same slot), `"none"`, or null to keep...
- `Refs { get; init; } -> IReadOnlyDictionary<string, string>?` — Other reference fields by path (`"equipment.impactEntity"`, `"equipment.program"`, `"pidEntity"`, ...)...
- `RolledLoot { get; init; } -> bool` — Opt-in for a rolled loot item: keeps the base's `generateRarity` and level range (a loot weapon: levels 1..55), so a...
- `Source { get; init; } -> string?` — Where to get it, in the tooltip (all languages).
- `Texts { get; init; } -> IReadOnlyDictionary<string, ItemTexts>?` — Texts per language (`"en"`, `"de"`, `"pt-BR"`, `"*"` = every language).
- `VanillaLook { get; init; } -> string?` `0.2.17` — The game item players without this item see in its place (a vanilla client, a Kiln client that lacks this mod's...
- `VisualModels { get; init; } -> IReadOnlyList<(uint Mask, string Model)>?` — A worn item's models for what the character wears and is, the game's `equipment.visualModels`: the client draws the...
- `WeaponType { get; init; } -> string?` — The weapon category (skills and bonuses that apply to a weapon type): `"item:<name>"` or a weapon category GUID.

## ItemDrop — A stack of a new item added to a drop table (a `DefaultInventoryResource`).
- `ItemDrop(string Table)`
- `Chance { get; init; } -> double` — spawnProbability, 0..1.
- `CountMax { get; init; } -> int` — The most items the stack drops (default 1).
- `CountMin { get; init; } -> int` — The fewest items the stack drops (default 1).
- `Table { get; init; } -> string` — The table's GUID, or a glob over table GUIDs; or `like:<item>`: every table that drops that item (the new stack goes...
- `Weight { get; init; } -> double` — Weight in a selector group (one stack of the group is picked by weight).

## ItemLoot — Random loot of a new item: rolled where `ItemLoot.Like` is (its rarities, label groups, stack sizes), with this item's own stack sizes when given.
- `Like { get; init; } -> string?` — The game item whose loot entry is copied (null: the definition's base).
- `StackMax { get; init; } -> int?` — The most items of a rolled stack; null: the copied entry's.
- `StackMin { get; init; } -> int?` — The fewest items of a rolled stack; null: the copied entry's.

## ItemTexts — The texts of an item in one language.
- `AcquisitionHint { get; init; } -> string?` — Where to get it (shown for items the player does not have yet).
- `Caption { get; init; } -> string?` — The line under the name (the item's kind, e.g. "One-Handed Sword").
- `Description { get; init; } -> string?` — The item's description.
- `Lore { get; init; } -> string?` — The item's lore text.
- `Name { get; init; } -> string?` — The item's name.
- `PrimaryAction { get; init; } -> string?` — The action hints (e.g. "Attack", "Block").
- `SecondaryAction { get; init; } -> string?` — The hint for the secondary action.
- `TertiaryAction { get; init; } -> string?` — The hint for the tertiary action.

## NewContent — New items and recipes: `Content.Items.Register(new ItemDefinition { ... })`, `Content.Recipes.Register(new RecipeDefinition { ... })`, `NewContent.Definitions`.
- `static Available { get; } -> bool` — The service supports new content (kiln.content v3 on a build with feature `content.new`).
- `static Definitions() -> IReadOnlyList<ContentDefinition>` — Every definition (of every mod and data pack) with its state.
- `static DefinitionsJson() -> string` — The definitions as kiln.content's JSON (for admin tools).
- `static IdOf(Guid guid) -> uint` — FNV-1a-32 over the GUID's 16 bytes (memory order): the game's ItemId / RecipeId of that GUID.
- `static InfoJson() -> string` — kiln.content v4's info: game languages, reference pools, placeholders (JSON; "{}" before v4).
- `static Placeholders() -> IReadOnlyList<ContentDefinition>` — Placeholders "[Missing: mod:item]" registered for items whose mod is gone (missing content).
- `static Register(ItemDefinition def) -> ContentDefinition` — Registers a new item as the calling mod's (see `ItemDefinition`).
- `static Register(RecipeDefinition def) -> ContentDefinition` — Registers a new recipe as the calling mod's (see `RecipeDefinition`).
- `static RegisterJson(ContentKind kind, string json) -> ContentDefinition` — Registers a definition given in the data-pack form (one entry of an `"add"` list).

## `ReadOnlyField<T>` — A field Kiln will not change: an identity field (ids, GUIDs) or a name.
- `ReadOnlyField()`
- `ReadOnlyField(string Section, string Path)`
- `Path { get; init; } -> string` — The string a data pack writes.
- `Section { get; init; } -> string` — The catalog section it belongs to: `items`, `templates`, ...

## RecipeDefinition — A new recipe: ingredients and results at a station, copied from `RecipeDefinition.BaseOn` (station, level, props, cooking, ...) or made for `RecipeDefinition.Station`.
- `BaseOn { get; init; } -> string?` — A recipe to copy (debug name, `"0x..."` id, GUID): its station and requirements.
- `DisplayName { get; init; } -> string?` — A recipe name (most recipes show their result's name).
- `Duration { get; init; } -> TimeSpan?` — The crafting duration (timed stations).
- `Fields { get; init; } -> IReadOnlyDictionary<string, object>?` — Other RecipeInfo fields by path (`"level"`, `"isCookingRecipe"`, ...).
- `Guid { get; init; } -> Guid` — The fixed GUID of the recipe (the recipe id is FNV-1a of it).
- `Id { get; } -> uint` — The recipe id this definition gets (FNV-1a of the GUID).
- `Inputs { get; init; } -> IReadOnlyList<RecipeIngredient>` — What a craft takes.
- `Menu { get; init; } -> string?` — The workshop (`"0x..."`) whose crafting menu lists it, when that is not the one it is crafted at.
- `MenuGroup { get; init; } -> CraftingGroupRef?` `0.2.14` — The tab (and subcategory) of the game's crafting menu that lists it, one the game already has: a `CraftingGroups`...
- `Name { get; init; } -> string` — The debug name, unique.
- `Outputs { get; init; } -> IReadOnlyList<RecipeIngredient>` — What a craft makes.
- `Props { get; init; } -> IReadOnlyList<RecipeProp>?` — Crafting props that must be near the station, added to those a `RecipeDefinition.BaseOn` recipe asks for: each one's...
- `Station { get; init; } -> string?` — Where it is crafted: a recipe whose station to use, a workshop id (`"0x..."`), `"hand"` (the player's own crafting...
- `StationItem { get; init; } -> string?` — At a `"workshop:<guid>"` of a mod's own: the item (name or GUID) that places its station.
- `TrackCrafted { get; init; } -> bool` `0.2.14` — Keeps the game's "crafted once" tracking of the recipe: its completion query reads flag 2 of the player's knowledge...
- `Unlock { get; init; } -> RecipeUnlock` — How the recipe is unlocked in the crafting UI (default `RecipeUnlock.Always`).
- `Water { get; init; } -> int?` `0.2.13` — The recipe's tank water, in water charges per craft (the game's own `waterCharges`; one charge is 64 of the game's...

## RecipeIngredient — One ingredient or result of a `RecipeDefinition`.
- `RecipeIngredient()`
- `RecipeIngredient(string? Item, int Count, uint Category = 0)`
- `Category { get; init; } -> uint` — Inputs only: an input category id (`RecipeStack.Category` of a game recipe), "any item of that category"; then...
- `Count { get; init; } -> int` — How many.
- `Deconstruct(out string? Item, out int Count, out uint Category) -> void`
- `Equals(RecipeIngredient other) -> bool`
- `static implicit operator RecipeIngredient((string Item, int Count) x) -> RecipeIngredient` — An ingredient from an `(item, count)` pair, so `Inputs = [("Material_T1_Wood_Logs", 2)]` works.
- `Item { get; init; } -> string?` — Item (debug name, `"0x..."` id, GUID, or the `ItemDefinition.Name` of a new item).
- `static Of(string item, int count) -> RecipeIngredient` — An ingredient: `count` of the item `item`.
- `static OfCategory(uint category, int count) -> RecipeIngredient` — An input: `count` of any item of the input category `category`.
- `static operator !=(RecipeIngredient left, RecipeIngredient right) -> bool`
- `static operator ==(RecipeIngredient left, RecipeIngredient right) -> bool`

## RecipeProp — A crafting prop a recipe asks for: its `RecipeProp.Id` (`keen::CraftingPropId`, FNV-1a of its GUID) and its `RecipeProp.Index`, the bit in the mask (0..63).
- `RecipeProp()`
- `RecipeProp(uint Id, int Index)`
- `Id { get; init; } -> uint` — The prop's id.
- `Index { get; init; } -> int` — The prop's place in the game's table of crafting props, 0..63.

## RecipeStack — One ingredient or result of a recipe.
- `RecipeStack()`
- `RecipeStack(ItemId Item, int Count, uint Category = 0)`
- `Category { get; init; } -> uint` — An input category id, or 0 for none.
- `Count { get; init; } -> int` — How many.
- `Deconstruct(out ItemId Item, out int Count, out uint Category) -> void`
- `Equals(RecipeStack other) -> bool`
- `Item { get; init; } -> ItemId` — The item.
- `static Of(string item, int count) -> RecipeStack` — A stack of `count` of the item named `item` (a debug name, id or GUID).
- `static operator !=(RecipeStack left, RecipeStack right) -> bool`
- `static operator ==(RecipeStack left, RecipeStack right) -> bool`

## RecipeUnlock — How a new recipe is unlocked in the crafting UI.
enum: Always, LikeBase

## StationMenuAppliedEvent `0.2.17` — Client: a station's menu was built from its layout, or refused (`StationMenuAppliedEvent.Ok` false, `StationMenuAppliedEvent.Reason`: `IconSourceMissing`...
- `DefinitionId { get; } -> Guid` — The id of the station menu definition (the layout) that was applied or refused.
- `FromServer { get; } -> bool` — The layout came from the server (a joined game).
- `Generation { get; } -> ulong` — The generation of the layout collection the menu was built from (the server's when...
- `Ok { get; } -> bool` — True when the menu was built from the layout; false when it was refused.
- `Reason { get; } -> string` — Why the layout was refused (`IconSourceMissing`, `RestartRequired`, ...) when not `StationMenuAppliedEvent.Ok`.
- `Session { get; } -> Guid` — The server's session of the layout (`Guid.Empty` for a local one).
- `Workshop { get; } -> uint` — The workshop whose menu this is (the game's workshop id).

## StationMenuDefinitionChangedEvent `0.2.17` — A station's menu layout was accepted, replaced or taken off (another layer, or the game's own layout, shows again).
- `DefinitionId { get; } -> Guid` — The definition (the layer): `BlockIds.Derive(mod, "menu-layout", key)`.
- `Generation { get; } -> ulong` — The local collection's generation after the change.
- `ModId { get; } -> string` — The mod that defined the layer.
- `Removed { get; } -> bool` — True: the layer was taken off.
- `Workshop { get; } -> uint` — The station's own workshop id.

## Texts — The game's English texts by loca tag.
- `static Available { get; } -> bool` — True when a table is loaded (this loads it on the first call).
- `static Get(Guid tag) -> string?` — The text a LocaTag reference names (`ItemInfo.name` and the like are such GUIDs).
- `static Get(string tag) -> string?` — A tag as `0x12345678`, as a decimal id, or as a GUID.
- `static Get(uint tag) -> string?` — The text of a loca tag id (the FNV-1a of the tag's GUID bytes), with the game's markup; null when unknown or when...
- `static GetPlain(Guid tag) -> string?` — As `Texts.GetPlain`, for a LocaTag's GUID.
- `static GetPlain(string tag) -> string?` — As `Texts.GetPlain`, for a tag given as `"0x..."`, a GUID or a decimal id.
- `static GetPlain(uint tag) -> string?` — As `Texts.Get` with the game's markup tags (`<b>`, `<lorename>`, ...) removed.
- `static Preload() -> Task<bool>` — Loads the table on a background thread; completes with whether there is one.
- `static Status { get; } -> TextsStatus` — What is loaded, or why not (this loads the table on the first call).
- `static TagOf(Guid tag) -> uint` — The tag id of a LocaTag reference: FNV-1a-32 of the GUID's 16 bytes in the game's order (.NET's `Guid.ToByteArray`).

## TextsStatus — Where `Texts` got its table from, and how it went.
- `TextsStatus(bool Loaded, string? Source, string? Build, int Count, double Seconds, string Reason)`
- `Build { get; init; } -> string?` — The game build the catalog was made from ("26430625 (svn 1287369)"), or null.
- `Count { get; init; } -> int` — How many texts the table holds.
- `Loaded { get; init; } -> bool` — A table is loaded (`TextsStatus.Count` texts).
- `Reason { get; init; } -> string` — Why there is no table ("" when loaded).
- `Seconds { get; init; } -> double` — How long the load took.
- `Source { get; init; } -> string?` — The catalog file the table came from, or null.

## TooltipValue `0.2.10` — One row of a tooltip (an entry of `uiValues`): its label, its number and how the game formats it.
- `TooltipValue(uint LocaId, string? Label, int Raw, double Value, string Display, TooltipValueFormat Format, bool IsSigned, bool IsScaled)`
- `Display { get; init; } -> string` — Kiln's rendering of the number ("53", "+5%", "2.5s", "2.6", "" for Numberless), invariant culture.
- `Format { get; init; } -> TooltipValueFormat` — How the game shows the number (the row's format in its data); `TooltipValueFormat.Unknown` for a format a later game...
- `IsScaled { get; init; } -> bool` — The game scales this number when it shows it (by the weapon's power, an attribute: `scaleData`); `TooltipValue.Raw`...
- `IsSigned { get; init; } -> bool` — The game shows a sign ("+5").
- `Label { get; init; } -> string?` — The label in the player's language, with the game's markup, or null where it could not be looked up here (a...
- `LocaId { get; init; } -> uint` — The label's loca tag id (FNV-1a of the config's loca tag GUID); `Content.Text` gives its text.
- `Raw { get; init; } -> int` — The value as the game's data stores it.
- `Value { get; init; } -> double` — The number a player reads: `TooltipValue.Raw` for Normal and Percentage (percent), seconds for Duration...

## TooltipValueFormat `0.2.10` — How the game shows a tooltip row's number (`impact::ImpcatValueNumberFormat`, spelled as the game spells it).
enum: Normal, Percentage, Duration, DurationMilliseconds, Float, Numberless, Unknown

## VoxelMaterial `0.2.22` — One of the game's voxel materials, keyed by its cell id (the low byte of a voxel cell): what it is, what digging it gives and at what rate, which items place it, and...
- `VoxelMaterial(int CellId, VoxelMaterialKind Kind, Guid Guid, string Name, ItemId LootItem, int? ExchangeRate, IReadOnlyList<ItemId> PlacingItems, string? Hardness, int HealthPoints, string? Label, VoxelMaterialLabelSource LabelSource)`
- `CellId { get; init; } -> int` — The cell id: 1..127 terrain, 128..255 building.
- `ExchangeRate { get; init; } -> int?` — How many cells of it one `VoxelMaterial.LootItem` is (`terrainPerLootItemExchangeRate` /...
- `Guid { get; init; } -> Guid` — The material's GUID as a save keeps it (`VoxelWorldResource.materialGuids`); `Guid.Empty` when the world table has...
- `Hardness { get; init; } -> string?` — The game's hardness category (`"Soft"` ...
- `HealthPoints { get; init; } -> int` — How many hits of strength 1 a cell takes (`healthPoints`).
- `Kind { get; init; } -> VoxelMaterialKind` — Terrain or building.
- `Label { get; init; } -> string?` — A player-facing name in `VoxelMaterialCatalog.Language`: an item's name (see `LabelSource`), without markup; null...
- `LabelSource { get; init; } -> VoxelMaterialLabelSource` — Which item `VoxelMaterial.Label` names.
- `LootItem { get; init; } -> ItemId` — What digging it gives (`terrainItem` / `materialItem`); `default` for none (an unbreakable material).
- `Name { get; init; } -> string` — Kiln's stable name of the game material (`"TerrainStone"`, `"WoodRough"`), the one `WorldTables.GameMaterialId` and...
- `PlacingItems { get; init; } -> IReadOnlyList<ItemId>` — The items that place it (their `equipment.voxelData`), the one `VoxelMaterial.Name` comes from first, the rest by...

## VoxelMaterialCatalog `0.2.22` — The game's voxel materials with labels in one language (`Content.TryGetVoxelMaterials`): every material the terraforming registry configures, by cell id.
- `Available { get; } -> bool` — The game's registry could be read: the catalog has its materials.
- `Count { get; } -> int` — How many materials (terrain and building).
- `Get(int cellId) -> VoxelMaterial?` — The material of a cell id (a cell's low byte), or null: 0, an id the game does not configure, out of range.
- `GetEnumerator() -> IEnumerator<VoxelMaterial>` — Every material, in cell id order.
- `Language { get; } -> string` — The language of the labels (`"de-de"`).
- `PlacedBy(ItemId item) -> VoxelMaterial?` — The material an item places (a building block, a terrain block of the build menu), or null.
- `Reason { get; } -> string` — Why there are no materials ("" when `VoxelMaterialCatalog.Available`).
- `this[int index] { get; } -> VoxelMaterial` — The materials in cell id order.
- `WithLootItem(ItemId item) -> IReadOnlyList<VoxelMaterial>` — The materials whose digging gives `item`, in cell id order (several share one: five rubble ids give the same...

## VoxelMaterialKind `0.2.22` — Which cell domain a `VoxelMaterial` is in.
enum: Terrain, Building

## VoxelMaterialLabelSource `0.2.22` — Which item a `VoxelMaterial.Label` is the name of.
enum: None, PlacingItem, LootItem

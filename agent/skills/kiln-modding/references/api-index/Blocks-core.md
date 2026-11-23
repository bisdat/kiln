# Blocks (Kiln.Blocks): core

## AreaAffects — Whom an area, a cone, a beam, a zone or a chain reaches, judged from the caster by the game's own enemy and friend checks.
enum: Enemies, Friends

## AreaBlock — An area: who is in it is found when it happens.
- `Affects { get; init; } -> AreaAffects` — Whom the area reaches: enemies of the caster (the default) or its friends, as the game's own enemy and friend checks...
- `Effects { get; init; } -> IReadOnlyList<EffectRef>` — What each one in the area gets.
- `Radius { get; init; } -> float` — Metres.
- `Vfx { get; init; } -> VfxRef?` — A VFX where the area is, once.

## BaseRulesBlock `0.2.16` — Base rules: room for more Flame Altars and larger build zones, without the game's names.
- `BuildAreaPercent { get; init; } -> int?` — Every altar level's build zone at this share of the game's own size (100 to 200 percent), at most 256 metres on an...
- `ExtraAltars { get; init; } -> int` — This many more Flame Altars than the game's own number at every Flame level (0 to 15); the result is at most 16.
- `const MaxAltars = 16 -> int` — The most Flame Altars a rule can ask for: the game counts no more than 16.
- `MinimumAltars { get; init; } -> int?` — At least this many Flame Altars at every Flame level (1 to 16); null leaves the game's numbers as the floor.
- `Sizes { get; init; } -> IReadOnlyList<AltarSize>?` — At least this full size for an altar of the given level (1 to 5), on each axis: finite, 0.5 to 256 metres, a...

## BlockApplicationChangedEvent `0.2.3` — A block of an external kind changed state for its mod: applied, refused, or gone.
- `After { get; } -> BlockApplyResult` — Where the plan stands now.
- `Before { get; } -> BlockApplyResult` — Where the plan stood before the change.
- `ModId { get; } -> string` — The id of the consumer mod: whose block it is.
- `Name { get; } -> string` — The block's name in that mod.

## BlockApplyResult `0.2.3` — A snapshot of where a plan stands.
- `ContentGeneration { get; init; } -> ulong` — The game's content generation the plan was applied (or last replayed) against.
- `GameName { get; init; } -> string` — The game's debug name of the primary block.
- `Generation { get; init; } -> ulong` — Counts applications in this process: the same for a plan applied again, 0 before it is applied, a new one when the...
- `Guid { get; init; } -> Guid` — The GUID, id and game name of the plan's primary block: known as soon as the plan is submitted.
- `Id { get; init; } -> uint` — The game's id of the primary block (FNV-1a of `BlockApplyResult.Guid`).
- `Ok { get; init; } -> bool` — True only when `BlockApplyResult.State` is `BlockApplyState.Applied`.
- `Reason { get; init; } -> string` — Why the plan is not applied: one word for the cause, then the detail (the causes are listed on `BlockApplyResult`).
- `RequestId { get; init; } -> ulong` — The compile this answers for.
- `State { get; init; } -> BlockApplyState` — Where the plan stands.

## BlockApplyState `0.2.3` — Where a block of an external kind stands for its consumer mod.
enum: PendingContent, Applied, Refused, Removed

## BlockClientsAttribute — What a block of this type asks of players' games, unless one of its fields asks for more.
- `BlockClientsAttribute(ClientsLevel level)`
- `Level { get; } -> ClientsLevel` — The level the block type asks of players' games.

## BlockCompileContext `0.2.3` — What a kind's compiler works with, for one block.
- `Effect(string key, EffectBlock block) -> BlockProduct` — An effect as a product of the plan: the primary, or one of up to 32 the primary names by...
- `GuidOf(string domain, string localName) -> Guid` — A stable GUID for something this block makes: the same for ever for this consumer mod, block name, `domain` and...
- `Item(string key, ItemBlock block) -> BlockProduct` — An item as a product of the plan.
- `KindId { get; } -> string` — The block's kind: `provider-mod:kind`.
- `ModId { get; } -> string` — The consumer mod: whose block this is.
- `Name { get; } -> string` — The block's name in the consumer mod.
- `Plan(string primaryKey, IReadOnlyList<BlockProduct> products) -> BlockPlan` — The plan: `primaryKey` names the one primary product; every other product is an effect.
- `ProductName(string key) -> string` — The name by which a block of this plan names another product of it (an effect, in a field that takes an effect by...
- `Resolve<T>(BlockRef<T> reference) -> ResolvedBlock<T>` — The identity of a block declared before this compile began: one of the consumer's (`name`) or another mod's...
- `Spell(string key, SpellBlock block) -> BlockProduct` — A spell as a product of the plan.
- `Status(string key, StatusBlock block) -> BlockProduct` — A status as a product of the plan.
- `Weapon(string key, WeaponBlock block) -> BlockProduct` — A weapon as a product of the plan.

## BlockError — A block is wrong: which block, which field and why.
- `BlockError(string block, string? field, string problem)`
- `Block { get; } -> string` — The name of the block that is wrong.
- `Field { get; } -> string?` — The field that is wrong, by its path in the block.
- `Problem { get; } -> string` — What is wrong with it, in words a mod author can act on.

## BlockHandle — What `Blocks.Define` gives back: a typed reference to the block and what it became.
- `GameName { get; } -> string` — The debug name it has in the game: what `Content.Items.Get`, `Items.Give` and data packs take.
- `Guid { get; } -> Guid` — The GUID of what it became in the game (an item, a buff).
- `Id { get; } -> uint` — The game's id of it (FNV-1a of the GUID).
- `Mod { get; } -> string` — The mod that defined it.
- `Name { get; } -> string` — The block's name in that mod.

## `BlockHandle<T>`
- `Block { get; } -> T` — The block as defined.

## BlockIds — Ids of what a block becomes.
- `static Derive(string mod, string kind, string name) -> Guid` — The derived GUID (game byte order = `Guid.ToByteArray`).
- `static Fnv(Guid g) -> uint` — FNV-1a-32 over a GUID's bytes: the game's id of an item, recipe or buff with that GUID.

## BlockKindDescriptor `0.2.3` — What a block kind is called and asks for.
- `Clients { get; init; } -> ClientsLevel` — The least this kind asks of players' games.
- `Format { get; init; } -> int` — The format of the kind's blocks.
- `Id { get; init; } -> string` — `provider-mod:kind`.
- `JsonSection { get; init; } -> string` — The section of blocks/*.json this kind reads: `provider-mod:plural`.
- `Requires { get; init; } -> IReadOnlyList<string>` — Capability names a plan of this kind needs when it is applied.
- `SchemaJson { get; init; } -> string` — JSON Schema (2020-12) of one block's payload, without `format`.

## BlockKindRegisteredEvent `0.2.3` — A kind was registered, or (`BlockKindRegisteredEvent.Removed`) taken away.
- `KindId { get; } -> string` — The kind that was registered or taken away: `provider-mod:kind`.
- `ProviderModId { get; } -> string` — The id of the mod that registered the kind.
- `Removed { get; } -> bool` — True when the kind was taken away, false when it was registered.

## BlockKindRegistration `0.2.3` — What `Blocks.RegisterKind` gives the provider: dispose it to take the kind away.
- `Dispose() -> void` — Takes the kind away.
- `Id { get; } -> string` — The kind's id: `provider-mod:kind`.

## BlockKindResult `0.2.3` — What `Blocks.RegisterKind` answers.
- `Ok { get; init; } -> bool` — True when the kind was registered.
- `Reason { get; init; } -> string` — Why it was not (`AlreadyRegistered`, `TooManyKinds`).
- `Registration { get; init; } -> BlockKindRegistration?` — The registration: dispose it to take the kind away.

## BlockPlan `0.2.3` — What one block of an external kind becomes: frozen.
- `ConsumerModId { get; } -> string` — The consumer mod: whose block this is.
- `InputHash { get; } -> string` — SHA-256 (hex) of the canonical record, the descriptor, the frozen products and the selected primary.
- `KindId { get; } -> string` — The kind that compiled it: `provider-mod:kind`.
- `Name { get; } -> string` — The block's name in the consumer mod.
- `PrimaryKey { get; } -> string` — The `BlockProduct.Key` of the product that is the plan's primary block.
- `Products { get; } -> IReadOnlyList<BlockProduct>` — The products, in key order.

## BlockProduct `0.2.3` — One frozen built-in block of a plan.
- `CanonicalJson { get; } -> string` — The built-in block, as canonical JSON (the shape blocks/*.json takes, keys in ordinal order).
- `Key { get; } -> string` — The product's name inside its plan.
- `Kind { get; } -> BuiltInBlockProductKind` — Which built-in block it is.

## `BlockRef<T>` `0.2.3` — A reference a block kind's record makes to another block, by name: `name` (the consumer mod's) or `mod:name`.
- `BlockRef()`
- `BlockRef(string Value)`
- `Value { get; init; } -> string` — The name: the consumer mod's block `name`, or `mod:name` for another mod's.

## Blocks — Building blocks: say what you mean, Kiln makes the game's resources.
- `static Application(BlockHandle block) -> BlockApplyResult` `0.2.3` — Where a block of an external kind stands: the handle `Blocks.Define` gave for it.
- `static Application(BlockPlan plan) -> BlockApplyResult` `0.2.3` — Where a plan stands for the mod that compiled it.
- `static Apply(BlockPlan plan) -> BlockApplyResult` `0.2.3` — Applies a plan the calling mod compiled: Kiln lowers its frozen products with its own compilers and registers them...
- `static Available { get; } -> bool` — The services blocks compile to are there on this build.
- `static Compile<T>(string name, T block) -> BlockPlan` `0.2.3` — Compiles a block of an external kind as the calling mod's, and registers nothing: the provider's compiler runs once...
- `static Define(string name, AffixBlock block) -> AffixHandle` `0.2.2` — Defines an affix (a kind of rolled modifier on one item) as the calling mod's: the same registration as...
- `static Define(string name, AffixPoolBlock block) -> AffixPoolHandle` `0.2.2` — Defines an affix pool (which items roll which affixes) as the calling mod's: the same as `Affixes.DefinePool` and a...
- `static Define(string name, BuildingMaterialBlock block) -> TableBlockHandle<BuildingMaterialBlock>` — Defines a new building material as the calling mod's.
- `static Define(string name, BuildingMaterialPoolBlock block, DefinitionRegistrationOptions options) -> DefinitionRegistrationResult` `0.2.4` — Returns whether a pool is in the served boot table, durably accepted for the next start, or refused.
- `static Define(string name, DialogueBlock block) -> DialogueHandle` — Defines a dialogue as the calling mod's.
- `static Define(string name, LootTableBlock block) -> LootTableHandle` — Defines a loot table as the calling mod's.
- `static Define(string name, MaterialBlock block) -> MaterialBlockHandle` — Defines a material as the calling mod's.
- `static Define(string name, MaterialTweakBlock block) -> TableBlockHandle<MaterialTweakBlock>` — Changes a game material in place, as the calling mod's.
- `static Define(string name, QuestBlock block) -> QuestHandle` — Defines a quest as the calling mod's: `Quests.Define` gets what it lowers to.
- `static Define(string name, RarityRollBlock block) -> BlockHandle<RarityRollBlock>` `0.2.12` — Defines a rarity roll (which item types roll which rarity when they are crafted or dropped as loot) as the calling...
- `static Define(string name, RarityUpgradeBlock block) -> BlockHandle<RarityUpgradeBlock>` `0.2.12` — Defines a rarity upgrade (which items it takes, up to which rarity, and what each step costs) as the calling mod's...
- `static Define(string name, TerrainMaterialBlock block) -> TableBlockHandle<TerrainMaterialBlock>` — Defines a new terrain material as the calling mod's.
- `static Define(string name, VfxBlock block) -> VfxBlockHandle` — Defines a VFX as the calling mod's.
- `static Define(string name, VfxBlock block, DefinitionRegistrationOptions options) -> DefinitionRegistrationResult` `0.2.4` — Client: defines a VFX as the calling mod's and says what became of it: registered in this process, accepted for the...
- `static Define<T>(string name, T block) -> BlockHandle<T>` — Defines a block as the calling mod's.
- `static DumpPrograms(string directory) -> int` — Writes every program blocks made to `directory` (`<guid>.bin` + `.txt`, for `tools/impact/impact.py dis` and `lint`).
- `static Find(string name, string? mod = null) -> BlockHandle?` — A block a mod defined (its id, or null: the calling mod), or null.
- `static GetVfxRegistration(Guid registrationId) -> DefinitionRegistrationResult` `0.2.4` — Client: what became of a VFX definition since its `Blocks.Define` call, by its...
- `static ImportStat(ModValueKey key) -> UiStatImportRegistration` `0.2.16` — Declares that this mod's UI blocks show another mod's stat as `stats.<mod>/<id>` (since 0.2.0-dev.31; in a block...
- `static Lowered(string name, string? mod = null) -> string?` — What a block became, as JSON: the items, recipes, templates, buffs and programs, with the values given to them.
- `static MovesStatus(BlockHandle<WeaponBlock> weapon) -> MoveApplicationResult` `0.2.3` — Where a weapon's moves with follow-ups are: waiting for the game's data, applied, refused (and why), or removed with...
- `static RegisterKind<T>(BlockKindDescriptor descriptor, IBlockKind<T> compiler) -> BlockKindResult` `0.2.3` — Registers a block kind as the calling mod's: a record of the mod's own (`T`) and the pure compiler that turns it...
- `static Report() -> IReadOnlyDictionary<string, (IReadOnlyList<string> Blocks, IReadOnlyList<string> Errors)>` — The mods with blocks, each with its block names and the errors of its data files.
- `static Warnings(string? mod = null) -> IReadOnlyList<string>` — What is not wrong but may not be what was meant: a block renamed without `Was`.

## BlockUi `0.2.16` — UI blocks at run time: a window's state (`WindowBlock.State`), a mod's own data for tables and trees (`Bindings`), the live game values windows show, and commands run...
- `static CommandStatus(string nonce, ulong requestId) -> Task<UiCommandResult>` — The state of a request `BlockUi.ExecuteOnServer` sent: from this game's receipt when its answer was final, else...
- `static DemandWorld(UiWorldFields fields) -> UiDemandRegistration` — Asks kiln-core to collect `fields` (any combination of `UiWorldFields`) until the registration is disposed or the...
- `static ExecuteOnServer(string literalCommand, CancellationToken cancellation = default(CancellationToken)) -> Task<UiCommandResult>` — Runs `literalCommand` on the server this game joined, as this game's player there (the server takes the player from...
- `static ReadWorld() -> UiWorldReadResult` — The newest publication of live game values: the local player's health, stamina and mana as the game has them (before...
- `static SetState(WindowRef window, string key, UiStateValue value, UiStateVersion? expected = null) -> Task<UiStateResult>` — Sets one state entry of one of this mod's windows.
- `static State(WindowRef window) -> UiStateReadResult` — The state of one of this mod's windows: its declared entries and its tab bars' and trees' own state, at one version.
- `const StateCapability = "ui.blocks.state" -> string` — The capability of window state and of these calls.

## BlockUi.Bindings `0.2.16` — A mod's own data for its UI blocks: define a source (a scalar, rows or a tree with typed fields) before the blocks that bind it as `mod.<id>`, on every side, then...
- `const Capability = "ui.blocks.bindings" -> string` — The capability of mods' own data.
- `static Define(string id, UiDataSchema schema) -> UiDataRegistration` — Declares a data source of this mod (at most 64).
- `static Publish(string id, UiDataSnapshot snapshot) -> Task<UiDataPublishResult>` — Publishes a snapshot of a source: copied and checked at once, committed at Kiln's next service tick...

## `BootBlockId<T>` `0.2.3` — A staged definition's name: what other definitions and data files name it by (`BootBlockId.QualifiedName`).
- `BootBlockId(string Mod, string Kind, string Name)`
- `Kind { get; init; } -> string` — The kind (plural, as in blocks/*.json).
- `Mod { get; init; } -> string` — The defining mod.
- `Name { get; init; } -> string` — The block's name.
- `QualifiedName { get; } -> string` — `mod:name`, as a reference from another mod.

## `BootDefinitionResult<T>` `0.2.3` — What `BootDefinitions.Define` did.
- `BootDefinitionResult(bool Ok, string? Reason, BootBlockId<T>? Reference)`
- `Ok { get; init; } -> bool` — True when staged (the same definition again is fine).
- `Reason { get; init; } -> string?` — Why not: `DuplicateDefinition`, `PhaseClosed`, `TooMany`, `NotInBootKinds`; null when staged.
- `Reference { get; init; } -> BootBlockId<T>?` — The staged definition's name.

## BootDefinitions `0.2.3` — Stages a mod's block definitions in its `Mod.OnBoot`: `BootDefinitions.For(boot).Define("cold", new StatusBlock { ... })`.
- `Define<T>(string name, T definition) -> BootDefinitionResult<T>` — Stages `definition` as the mod's block `name`.
- `DefineJson(string kind, string name, string json) -> BootJsonDefinitionResult` — Stages one block given as JSON (the value of `blocks/*.json`'s `<kind>.<name>`).
- `static For(BootContext context) -> BootDefinitions` — The staging of `context`'s frame.

## BootDefinitionsCommittedEvent `0.2.3` — A mod's boot definitions are in the published generation: once per runtime activation of the mod (a hosted world's new instance gets it again, with the same...
- `BootDefinitionsCommittedEvent(string modId, ulong generation, IEnumerable<string> definitions)` — A new event.
- `Definitions { get; } -> IReadOnlyList<string>` — Its committed definitions, as `kind/key`.
- `Generation { get; } -> ulong` — The boot generation.
- `ModId { get; } -> string` — The mod.

## BootJsonDefinitionResult `0.2.3` — What `BootDefinitions.DefineJson` did.
- `BootJsonDefinitionResult(bool Ok, string? Reason, string Kind, string Key)`
- `Key { get; init; } -> string` — The block's name.
- `Kind { get; init; } -> string` — The kind.
- `Ok { get; init; } -> bool` — True when staged.
- `Reason { get; init; } -> string?` — Why not; null when staged.

## BuiltInBlockProductKind `0.2.3` — The built-in blocks a kind may make.
enum: Item, Weapon, Spell, Status, Effect

## CastAnimation — A cast animation with its timing.
- `CastAnimation(string Key, string Sequence, double ChargeSeconds)`
- `Animation { get; init; } -> MoveAnimation?` — The state the cast plays, as `MoveBlock.Animation` takes it: the full-body cast state when the motion has one, else...
- `ChargeSeconds { get; init; } -> double` — The charge time the spell's tooltip shows, in seconds.
- `static readonly FastBolt -> CastAnimation` — The quick bolt from the staff (Ice Bolt's).
- `HitAt { get; init; } -> double?` — Seconds into the cast the spell leaves the staff, as `CastBlock.ReleaseAt` (give one of them, or the same number).
- `Key { get; init; } -> string` — The name JSON gives this animation (`"fast-bolt"`, `"lob"`).
- `static readonly Lob -> CastAnimation` — The longer lob (Fireball's).
- `Sequence { get; init; } -> string` — The game's own cast sequence it plays, by name.

## CastBlock — How a spell is cast.
- `Animation { get; init; } -> CastAnimation?` — The motion.
- `Charges { get; init; } -> int?` — How many a cast takes of the spell's stack, or of an ability's item.
- `ManaCost { get; init; } -> int` — Mana one cast takes.
- `ReleaseAt { get; init; } -> double?` — Seconds into the animation the spell leaves the staff (cost, sound and effects move with it).
- `StaminaCost { get; init; } -> int` — Stamina one cast takes.
- `Vfx { get; init; } -> VfxRef?` — A VFX on the caster's hand, when the animation has its own: it takes that one's place and moment.

## ClientsLevel — What a mod asks of players' games, lowest first: nothing, Kiln (definitions by the content sync), the mod itself (its own assets).
enum: None, Kiln, Mod

## ClientsModWhenAttribute — A field whose value can name the mod's own file or cue, which has to be on every player's game: a value that matches this regular expression asks for `ClientsLevel.Mod`.
- `ClientsModWhenAttribute(string pattern)`
- `Pattern { get; } -> string` — The regular expression a field's value is tested against: a value that matches asks for `ClientsLevel.Mod`.

## Damage — Shorthands for `DamageBlock`.
- `static Blunt(float n) -> DamageBlock` — A `DamageBlock` of `n` blunt damage.
- `static Cut(float n) -> DamageBlock` — A `DamageBlock` of `n` cutting damage.
- `static Fire(float n) -> DamageBlock` — A `DamageBlock` of `n` fire damage.
- `static Ice(float n) -> DamageBlock` — A `DamageBlock` of `n` ice damage.
- `static Lightning(float n) -> DamageBlock` — A `DamageBlock` of `n` lightning damage.
- `static Pierce(float n) -> DamageBlock` — A `DamageBlock` of `n` piercing damage.
- `static Poison(float n) -> DamageBlock` — A `DamageBlock` of `n` poison damage.
- `static Shroud(float n) -> DamageBlock` — A `DamageBlock` of `n` shroud damage.

## DamageBlock — Damage per kind.
- `Blunt { get; init; } -> float` — Blunt damage.
- `Cut { get; init; } -> float` — Cutting damage.
- `Explosion { get; init; } -> int` — Explosion damage, the game's own `explosionDamage` in the hit's damage distribution.
- `Fire { get; init; } -> float` — Fire damage.
- `Flat { get; init; } -> bool` — On a spell: exactly this number, with no share of the staff's power.
- `Ice { get; init; } -> float` — Ice damage.
- `Lightning { get; init; } -> float` — Lightning damage.
- `Metal { get; init; } -> int` — Damage to metal, the game's own `metalDamage` in the hit's damage distribution.
- `static operator +(DamageBlock a, DamageBlock b) -> DamageBlock` — Adds two damages kind by kind.
- `Pierce { get; init; } -> float` — Piercing damage.
- `Poison { get; init; } -> float` — Poison damage.
- `Shroud { get; init; } -> float` — Shroud damage.
- `Stone { get; init; } -> int` — Damage to stone, the game's own `stoneDamage` in the hit's damage distribution.
- `Total { get; } -> float` — The kinds that hurt, added up.
- `Wood { get; init; } -> int` — Damage to wood, the game's own `woodDamage` in the hit's damage distribution.

## EffectBlock — What happens.
- `Applies { get; init; } -> StatusApply?` — A status put on the target.
- `Area { get; init; } -> AreaBlock?` — Everything around the place the effect happens gets `AreaBlock.Effects`.
- `Casts { get; init; } -> AbilityRef?` — An ability the target casts, as if from code: its costs and cooldown apply, and a cast that is refused does nothing.
- `Chance { get; init; } -> double?` — 0..1: the whole effect happens this often.
- `Damage { get; init; } -> DamageBlock?` — Damage to the target.
- `Flinch { get; init; } -> Flinch` — How the target flinches.
- `Heal { get; init; } -> int?` — Health given to the target.
- `Knockback { get; init; } -> Knockback` — How far the hit pushes the target.
- `On { get; init; } -> EffectOn` — Who it is for: what the trigger gives it (what was hit, who has the status), or the caster, whoever wields the...
- `Removes { get; init; } -> IReadOnlyList<StatusRef>?` — Statuses taken off the target: blocks of a mod, or `GameBuffs` constants.
- `Restores { get; init; } -> RestoreBlock?` — Mana, stamina and health given to the target.
- `Run { get; init; } -> NativeRef?` `0.2.4` — A mod's `[ImpactNative]` method, called first, in the effect's own program, the moment the effect happens...
- `Screen { get; init; } -> ScreenRef?` — A full-screen effect for the target, when it is a player, or for the players near the place.
- `Sound { get; init; } -> SoundRef?` — A sound at the target or the place, for everyone near it who has Kiln.
- `Spawns { get; init; } -> TemplateRef?` — Spawned at the target, or at the place: a `GameTemplates` constant or a template's GUID.
- `Then { get; init; } -> IReadOnlyList<EffectRef>?` — More effects, run after this one on the same target.
- `Vfx { get; init; } -> VfxRef?` — A VFX at the target or the place.

## EffectOn — Who an `EffectBlock` is for.
enum: Target, Caster

## EffectRef — An effect: inline, from `Blocks.Define`, or by name.
- `static implicit operator EffectRef(BlockHandle<EffectBlock> handle) -> EffectRef` — The effect that `Blocks.Define` returned: the reference keeps the block and its `mod:name`.
- `static implicit operator EffectRef(EffectBlock block) -> EffectRef` — An effect written in place.
- `static implicit operator EffectRef(string name) -> EffectRef` — An effect by name: a block of this mod, or `"other-mod:name"` for another mod's.
- `Inline { get; } -> EffectBlock?` — The effect itself, when the reference was made from a block or a handle.
- `Name { get; } -> string?` — The effect's name: `mod:name` when made from a handle.

## Flinch — How hard a hit makes its target flinch: the game's hit reaction, by its number.
enum: None, VeryLight, Light, Medium, Heavy

## IBlock — Something `Blocks.Define` takes: a spell, a weapon, a status, an effect.

## `IBlockKind<T>` `0.2.3` — A block kind's compiler.
- `Compile(BlockCompileContext context, T block) -> BlockPlan` — What `block` becomes.

## `IBlockRef<T>` — A reference to a block of type `T`: the block written in place, a handle, or a name (in JSON, a string).
- `Inline { get; } -> T?` — The block itself, when the reference holds one.
- `Make(T? inline, string? name) -> IBlockRef<T>` — Makes a reference from what was read: an inline block, or a name.
- `Name { get; } -> string?` — The block's name, when the reference has one.

## IDeliveryBlock — How a spell, an attack or an ability reaches what it affects: `ProjectileBlock`, `SelfBlock`, `AroundBlock`, `ConeBlock`, `BeamBlock`, `ZoneBlock`, `ChainBlock`...

## IPremade — Premade instances that JSON names by `IPremade.Key`.
- `Key { get; } -> string` — The name JSON uses for this premade.

## ItemRef — An item a recipe needs: a `GameItems` constant, or an item this mod defines (a handle or its block name).
- `ItemRef()`
- `ItemRef(string Value)`
- `static implicit operator ItemRef(BlockHandle handle) -> ItemRef` — The item that `Blocks.Define` returned, as the game's debug name (`BlockHandle.GameName`).
- `static implicit operator ItemRef(string value) -> ItemRef` — An item by name: a `GameItems` constant, or the block name of an item this mod defines.
- `Value { get; init; } -> string` — The item as written: a `GameItems` constant, an item of this mod by its block name, or (made from a handle) the...

## ITweakable — A block that lets its author go below it: game fields it does not cover, set on what it made.
- `Tweak { get; } -> FieldValues` — The game fields to set on what the block made.

## Knockback — How far a hit pushes its target: the game's push-back strength, by its number.
enum: None, Small, Normal, Big, VeryBig, VerySmall, PullSmall, PullNormal

## ProjectileBlock — A projectile: it flies, and where it stops `ProjectileBlock.OnImpact` happens.
- `Gravity { get; init; } -> float` — 0 flies straight, 1 falls like a thrown stone.
- `Homing { get; init; } -> HomingBlock?` — It turns toward what it was aimed at.
- `Look { get; init; } -> ProjectileLook` — What the projectile and its impact look and sound like.
- `OnHit { get; init; } -> IReadOnlyList<EffectRef>?` — What happens to the creature it strikes, if it struck one.
- `OnImpact { get; init; } -> IReadOnlyList<EffectRef>?` — What happens at the place it stops.
- `Pierce { get; init; } -> int` — How many creatures it flies through, each struck once (`ProjectileBlock.OnHit` on each), before the next one stops it.
- `Range { get; init; } -> float` — Metres before it fizzles.
- `Speed { get; init; } -> float` — Metres per second.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the template it flies as...
- `Vfx { get; init; } -> VfxRef?` — A VFX on the projectile while it flies, in place of the `ProjectileBlock.Look`'s.

## ProjectileLook — What a projectile and its impact look and sound like.
- `ProjectileLook(string Key, string Projectile, string Impact, string HandItem)`
- `static readonly Fireball -> ProjectileLook` — The Fireball staff spell's projectile, impact and hand item.
- `HandItem { get; init; } -> string` — The game's own spell item that shows in the hand, by name.
- `static readonly IceBolt -> ProjectileLook` — The Ice Bolt staff spell's projectile, impact and hand item.
- `Impact { get; init; } -> string` — The game's own template that plays where it stops, by name.
- `Key { get; init; } -> string` — The name JSON gives this look (`"ice-bolt"`, `"fireball"`).
- `Projectile { get; init; } -> string` — The game's own projectile template it flies as, by name.

## `ResolvedBlock<T>` `0.2.3` — What a `BlockRef` stands for: the identity of the block, copied.
- `GameName { get; init; } -> string` — The debug name the block has in the game: what `Content.Items.Get`, `Items.Give` and data packs take.
- `Guid { get; init; } -> Guid` — The GUID of what the block became in the game.
- `Id { get; init; } -> uint` — The game's id of it (FNV-1a of the GUID).
- `KindId { get; init; } -> string` — The block's kind: `kiln:item` for a built-in, `provider-mod:kind` for an external one.
- `ModId { get; init; } -> string` — The id of the mod that defined the block.
- `Name { get; init; } -> string` — The block's name in that mod.

## ResourceListAppend `0.2.7` — Values a resource patch adds to the end of a list (`ResourcePatchBlock.Append`).
- `AboveLast { get; init; } -> bool` — Only a value greater than the list's last (the last one added counts) is added: a sorted list stays sorted, and...
- `Values { get; init; } -> List<ResourceValue>` — The values, in order: numbers, bools or enum members' names (a change such as `ResourceValue.Times` is refused...

## ResourceListReplace `0.2.9` — The values a list holds after a resource patch (`ResourcePatchBlock.Replace`).
- `Values { get; init; } -> List<ResourceValue>` — The values, in order: numbers, bools or enum members' names (a change such as `ResourceValue.Times` is refused).

## ResourcePatchBlock `0.2.3` — Fields of one game resource, set before the game reads it.
- `AddComponents { get; init; } -> List<string>?` `0.2.7` — Components of the game's types a template gets (by type name, `"PickupItemZone"`), zeroed; one it has already stays...
- `Append { get; init; } -> Dictionary<string, ResourceListAppend>?` `0.2.7` — List path to values added at its end: a list of numbers, bools or enum members...
- `Clear { get; init; } -> List<string>?` `0.2.9` — List paths made empty, any element type (`"noBuildZones"`, `"resetPlayersOutOfBounds.playableAreas"`).
- `CopyComponents { get; init; } -> Dictionary<string, Guid>?` `0.2.9` — Component type name to the GUID of another template it is copied from, whole: its data with its lists, strings and...
- `Part { get; init; } -> int` — The part index; 0 for nearly every type.
- `RemoveComponents { get; init; } -> List<string>?` `0.2.7` — A template's components to take out, by type name (`"SpawnTime"`); one the template hasn't got is skipped.
- `Replace { get; init; } -> Dictionary<string, ResourceListReplace>?` `0.2.9` — List path to the values it holds afterwards, exactly: a list of numbers, bools or enum members, as long as the...
- `Resource { get; init; } -> Guid` — The resource's GUID.
- `Set { get; init; } -> Dictionary<string, ResourceValue>` — Member path to value.
- `Type { get; init; } -> string` — The resource's qualified type name, as `keen::BalancingTable`.

## ResourceValue `0.2.3` — What a resource patch writes into one field: a plain value, or a change of the game's own value (times `ResourceValue.Mul`, plus `ResourceValue.Add`, then held...
- `Add { get; init; } -> double?` — Then this is added.
- `static AtLeast(double min) -> ResourceValue` — The game's value, but at least `min`.
- `static AtMost(double max) -> ResourceValue` — The game's value, but at most `max`.
- `Equals(ResourceValue other) -> bool`
- `static implicit operator ResourceValue(bool value) -> ResourceValue` — The member is set to `value`.
- `static implicit operator ResourceValue(double value) -> ResourceValue` — The member is set to `value`.
- `static implicit operator ResourceValue(int value) -> ResourceValue` — The member is set to `value`.
- `static implicit operator ResourceValue(long value) -> ResourceValue` — The member is set to `value`.
- `static implicit operator ResourceValue(string name) -> ResourceValue` — The member, an enum, is set to the member named `name`.
- `LowerOnly { get; init; } -> bool` — The result is never above the game's value.
- `Max { get; init; } -> double?` — The result is at most this.
- `Min { get; init; } -> double?` — The result is at least this.
- `Mul { get; init; } -> double?` — The game's value is multiplied by this first.
- `static operator !=(ResourceValue left, ResourceValue right) -> bool`
- `static operator ==(ResourceValue left, ResourceValue right) -> bool`
- `static Plus(double amount) -> ResourceValue` — The game's value plus `amount`.
- `RaiseOnly { get; init; } -> bool` — The result is never below the game's value.
- `static Times(double factor) -> ResourceValue` — The game's value times `factor`.
- `Value { get; init; } -> object?` — The plain value: a number, a bool, or an enum member's name.

## Restore — Shorthands for `RestoreBlock`.
- `static Health(int n) -> RestoreBlock` — A `RestoreBlock` of `n` health.
- `static Mana(int n) -> RestoreBlock` — A `RestoreBlock` of `n` mana.
- `static Stamina(int n) -> RestoreBlock` — A `RestoreBlock` of `n` stamina.

## RestoreBlock — Mana, stamina and health given back.
- `Health { get; init; } -> int` — Health given back.
- `Mana { get; init; } -> int` — Mana given back.
- `static operator +(RestoreBlock a, RestoreBlock b) -> RestoreBlock` — Adds two restores kind by kind.
- `Stamina { get; init; } -> int` — Stamina given back.

## SoundBlock — A sound of the mod's own: one or more clips (a cue that picks one at random each time), how loud, and how far it carries.
- `Clips { get; init; } -> IReadOnlyList<string>` — Audio files in the mod's folder (WAV, FLAC, Ogg Vorbis, MP3): `["audio/frost_hum.wav"]`.
- `Loop { get; init; } -> bool?` — It plays until stopped, as a station's running sound or an ambience.
- `Normalize { get; init; } -> double?` — Its sources' level, as the cook sets it: the active RMS in dBFS (the game's effects sit near -23).
- `PitchRandomness { get; init; } -> double?` — 0..1: how much each play's pitch varies.
- `Range { get; init; } -> double[]?` — Metres `[min, max]`: full volume within min, silent beyond max.
- `Scene { get; init; } -> string?` — The player's volume slider it follows: `Sfx`, `Music`, `Ui` or `Voice`.
- `Volume { get; init; } -> double?` — 0..1, the cue's volume.

## SoundRef — A sound: a `SoundBlock` or a cue of this mod's cook recipe by name, `"other-mod:name"`, or a cue's GUID (`GameSounds.Guids`).
- `SoundRef()`
- `SoundRef(string Value)`
- `static implicit operator SoundRef(BlockHandle<SoundBlock> sound) -> SoundRef` — A `SoundBlock` of this mod, by its handle.
- `static implicit operator SoundRef(Guid guid) -> SoundRef` — A sound by a cue's GUID.
- `static implicit operator SoundRef(string value) -> SoundRef` — A sound by name: a cue of this mod's cook recipe, `"other-mod:name"`, or a cue's GUID as text.
- `Value { get; init; } -> string` — A cue's name or `"other-mod:name"`, or a cue's GUID as text.

## SpellBlock — A staff spell: an item in the stack per charge.
- `Cast { get; init; } -> CastBlock` — How it is cast: animation, timing, costs.
- `Delivery { get; init; } -> IDeliveryBlock` — How it gets there, and what it does when it does.
- `Description { get; init; } -> string?` — What the spell does, as players read it on its item.
- `Level { get; init; } -> int` — The spell's item level.
- `MaxCharges { get; init; } -> int` — Charges in one stack.
- `Name { get; init; } -> string` — The spell's name, as players read it on its item.
- `OnCast { get; init; } -> IReadOnlyList<EffectRef>?` — What happens to the caster the moment it leaves the staff.
- `Recipe { get; init; } -> RecipeBlock?` — How it is crafted.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the item it makes: `Tweak = { { GameFields.Items.MaxStackSize, 5 } }`.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## Stat — A stat of a character.
- `Stat(string Key, Guid Attribute, bool Whole = false, bool Subtract = false)`
- `Attribute { get; init; } -> Guid` — The GUID of the game's attribute it changes.
- `static readonly Block -> Stat` — Block, in whole points.
- `static readonly Constitution -> Stat` — The constitution attribute, in whole points.
- `static readonly CritChance -> Stat` — Critical hit chance, as a share: 0.1 is 10% more.
- `static readonly CritDamage -> Stat` — Critical hit damage, as a share: 0.1 is 10% more.
- `static readonly Dexterity -> Stat` — The dexterity attribute, in whole points.
- `static readonly Endurance -> Stat` — The endurance attribute, in whole points.
- `static readonly Healing -> Stat` — Healing, as a share: 0.1 is 10% more.
- `static readonly HealthRegen -> Stat` — Health regeneration, in whole points.
- `static readonly Intelligence -> Stat` — The intelligence attribute, in whole points.
- `Key { get; init; } -> string` — The name JSON gives this stat (`"max-health"`).
- `static readonly MagicalArmor -> Stat` — Magical armour, in whole points.
- `static readonly MagicDamage -> Stat` — Magic damage, as a share: 0.1 is 10% more.
- `static readonly ManaRegen -> Stat` — Mana regeneration, in whole points.
- `static readonly MaxHealth -> Stat` — Maximum health, in whole points.
- `static readonly MaxMana -> Stat` — Maximum mana, in whole points.
- `static readonly MaxOxygen -> Stat` — Maximum oxygen, in whole points.
- `static readonly MaxStamina -> Stat` — Maximum stamina, in whole points.
- `static readonly MeleeDamage -> Stat` — Melee damage, as a share: 0.1 is 10% more.
- `static readonly MoveSpeed -> Stat` — Movement speed, as a share: 0.1 is 10% more, -0.5 is half.
- `static readonly Parry -> Stat` — Parry, as a share: 0.1 is 10% more.
- `static readonly PhysicalArmor -> Stat` — Physical armour, in whole points.
- `static readonly RangedDamage -> Stat` — Ranged damage, as a share: 0.1 is 10% more.
- `static readonly ResistBlunt -> Stat` — Resistance to blunt damage: the share of it taken away (0.25 takes a quarter less, -0.25 a quarter more).
- `static readonly ResistCut -> Stat` — Resistance to cut damage: the share of it taken away (0.25 takes a quarter less, -0.25 a quarter more).
- `static readonly ResistFire -> Stat` — Resistance to fire damage: the share of it taken away (0.25 takes a quarter less, -0.25 a quarter more).
- `static readonly ResistIce -> Stat` — Resistance to ice damage: the share of it taken away (0.25 takes a quarter less, -0.25 a quarter more).
- `static readonly ResistLightning -> Stat` — Resistance to lightning damage: the share of it taken away (0.25 takes a quarter less, -0.25 a quarter more).
- `static readonly ResistMagical -> Stat` — Resistance to magical damage: the share of it taken away (0.25 takes a quarter less, -0.25 a quarter more).
- `static readonly ResistPhysical -> Stat` — Resistance to physical damage: the share of it taken away (0.25 takes a quarter less, -0.25 a quarter more).
- `static readonly ResistPierce -> Stat` — Resistance to pierce damage: the share of it taken away (0.25 takes a quarter less, -0.25 a quarter more).
- `static readonly ResistPoison -> Stat` — Resistance to poison damage: the share of it taken away (0.25 takes a quarter less, -0.25 a quarter more).
- `static readonly ResistShroud -> Stat` — Resistance to shroud damage: the share of it taken away (0.25 takes a quarter less, -0.25 a quarter more).
- `static readonly ShroudTime -> Stat` — Shroud time, in whole points.
- `static readonly Spirit -> Stat` — The spirit attribute, in whole points.
- `static readonly SprintSpeed -> Stat` — Sprint speed, as a share: 0.1 is 10% more.
- `static readonly StaminaRegen -> Stat` — Stamina regeneration, in whole points.
- `static readonly StaminaUseClimb -> Stat` — Stamina used for climbing, as a share: -0.1 is 10% less.
- `static readonly StaminaUseGlide -> Stat` — Stamina used for gliding, as a share: -0.1 is 10% less.
- `static readonly StaminaUseSprint -> Stat` — Stamina used for sprinting, as a share: -0.1 is 10% less.
- `static readonly Strength -> Stat` — The strength attribute, in whole points.
- `Subtract { get; init; } -> bool` — True: the game applies a modifier as a subtraction, as the resistances do (0.25 takes a quarter of that damage away).
- `Whole { get; init; } -> bool` — True: whole points (strength, maximum health, armour), so a modifier takes whole numbers.

## StatusApply — A status to apply: the status, and optionally for how long.
- `static implicit operator StatusApply(BlockHandle<StatusBlock> status) -> StatusApply` — The status that `Blocks.Define` returned, for its own `StatusBlock.Seconds`.
- `static implicit operator StatusApply(StatusBlock status) -> StatusApply` — The status, written inline, for its own `StatusBlock.Seconds`.
- `static implicit operator StatusApply(StatusRef status) -> StatusApply` — The status, for its own `StatusBlock.Seconds`.
- `static implicit operator StatusApply(string status) -> StatusApply` — A status by block name, for its own `StatusBlock.Seconds`.
- `Seconds { get; init; } -> double?` — Seconds.
- `Status { get; init; } -> StatusRef` — The status to put on.

## StatusBlock — A buff or debuff: its own block, made of effects.
- `Description { get; init; } -> string?` — What the status does, as players read it.
- `Icon { get; init; } -> string?` — The icon, when it is not the look's: a `GameBuffs` constant, whose icon it takes, or a PNG in the mod's folder...
- `Kind { get; init; } -> StatusKind` — Whether it is a buff or a debuff: the slot it takes in the buff bar, and the look it gets when `StatusBlock.Look`...
- `Look { get; init; } -> StatusLook?` — What it looks like: its icon and body effect.
- `Modifies { get; init; } -> IDictionary<Stat, double>` — Stats changed on whoever has it, for as long as they have it: `Modifies = { [Stat.MoveSpeed] = -0.5 }` is half speed.
- `Name { get; init; } -> string` — The status's name, as players read it in the buff bar.
- `OnApply { get; init; } -> IReadOnlyList<EffectRef>?` — Runs on whoever has it when it starts.
- `OnExpire { get; init; } -> IReadOnlyList<EffectRef>?` — Runs on whoever has it when it ends.
- `OnTick { get; init; } -> IReadOnlyList<EffectRef>?` — Runs on whoever has it every `StatusBlock.TickEvery` seconds.
- `Refreshes { get; init; } -> bool` — Applied again while it runs: the timer starts over (true), or the new one replaces the old (false).
- `Screen { get; init; } -> ScreenRef?` — A full-screen effect on the screen of the player who has it, from when it starts until it ends, then by its fade out.
- `Seconds { get; init; } -> double` — Seconds it lasts unless whoever applies it says otherwise.
- `TickEvery { get; init; } -> double` — Seconds between runs of `StatusBlock.OnTick`.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the buff it makes: `Tweak = { { GameFields.Buffs.HideInUi, true } }`.
- `Vfx { get; init; } -> VfxRef?` — A VFX on whoever has it, for as long as they have it.
- `Was { get; init; } -> string?` — The name this block had before a rename.
- `While { get; init; } -> PlayerStateRef?` — A player has it for as long as they are in this state: any of the game's, `PlayerStateId.Gliding`, in JSON a name...

## StatusKind — Whether a status is a buff or a debuff.
enum: Debuff, Buff

## StatusLook — What a status looks like.
- `StatusLook(string Key, string Starter)`
- `Body { get; init; } -> VfxRef?` — What shows on the body, when the starter's own isn't it: a game VFX on a slot.
- `static readonly Fire -> StatusLook` — The game's Burning on enemies: its flame icon, and the flames the game puts on the chest of what burns.
- `static readonly Frost -> StatusLook` — The frost slow's icon (a snail) and body effect.
- `Key { get; init; } -> string` — The name JSON gives this look.
- `static readonly Poison -> StatusLook` — The poison icon and body effect.
- `Starter { get; init; } -> string` — The game's own status it is made from, by name or `0x` id: it supplies the icon and body effect.

## StatusRef — A status: inline, from `Blocks.Define`, or by name.
- `static implicit operator StatusRef(BlockHandle<StatusBlock> handle) -> StatusRef` — The status that `Blocks.Define` returned: the reference keeps the block and its `mod:name`.
- `static implicit operator StatusRef(StatusBlock block) -> StatusRef` — A status written in place.
- `static implicit operator StatusRef(string name) -> StatusRef` — A status by name: a block of this mod, or `"other-mod:name"` for another mod's.
- `Inline { get; } -> StatusBlock?` — The status itself, when the reference was made from a block or a handle.
- `Name { get; } -> string?` — The status's name: `mod:name` when made from a handle.

## TemplateRef — Something to spawn: a `GameTemplates` constant, or a template's GUID.
- `TemplateRef()`
- `TemplateRef(string Value)`
- `static implicit operator TemplateRef(Guid guid) -> TemplateRef` — A template by its GUID.
- `static implicit operator TemplateRef(string value) -> TemplateRef` — A template by name: a `GameTemplates` constant, or a template's GUID as text.
- `Value { get; init; } -> string` — A `GameTemplates` constant, or a template's GUID as text.

## UiCommandCode `0.2.16` — Why a request to the server ended as it did.
enum: None, PermissionDenied, ParseFailure, CanceledByCommandEvent, QueueFull, RateLimited, ExpiredBeforeStart, CanceledBeforeStart, TooLateToCancel, AssociationChanged, HandlerFailure, RunningTimeout, ReplayExpired, ProtocolConflict, UnknownRequest, CanceledBeforeSend, CanceledAfterStart, TimedOutUnknown

## UiCommandCompletedEvent `0.2.16` — A wait of `BlockUi.ExecuteOnServer` (or of a block's server command) ended: exactly one per request.
- `Result { get; } -> UiCommandResult` — What the wait ended with (the same result its task returned).

## UiCommandOutcome `0.2.16` — Whether a command ran, as far as this game can know.
enum: NotSent, NotStarted, Completed, Unknown

## UiCommandResult `0.2.16` — A request's outcome or state.
- `UiCommandResult(bool Ok, string? Reason, IReadOnlyList<string> Output, ulong RequestId, string Nonce, UiCommandState State, UiCommandOutcome Outcome, UiCommandCode Code, bool Truncated)`
- `Code { get; init; } -> UiCommandCode` — Why it ended as it did.
- `Nonce { get; init; } -> string` — The connection's challenge the request was sent under (for `BlockUi.CommandStatus`).
- `Ok { get; init; } -> bool` — The command ran and succeeded.
- `Outcome { get; init; } -> UiCommandOutcome` — Whether it ran, as far as this game can know.
- `Output { get; init; } -> IReadOnlyList<string>` — What the command answered, at most 16 lines and 4096 bytes.
- `Reason { get; init; } -> string?` — Why not, in plain words (the command's own error text where it gave one); null when `Ok`.
- `RequestId { get; init; } -> ulong` — The request's id on its connection; 0 when nothing was sent.
- `State { get; init; } -> UiCommandState` — Where the request stands.
- `Truncated { get; init; } -> bool` — The server cut `Output` to fit.

## UiCommandState `0.2.16` — Where a request to the server stands.
enum: Queued, Started, RunningOverBudget, Final

## UiCommandStatusChangedEvent `0.2.16` — News of a request from the server: it was queued, started, ran over its budget, or ended (also after its wait ended: a late final answer).
- `Nonce { get; } -> string` — The connection's challenge the request was sent under.
- `RequestId { get; } -> ulong` — The request's id.
- `Result { get; } -> UiCommandResult?` — The final result once `UiCommandStatusChangedEvent.State` is `UiCommandState.Final`; null before.
- `State { get; } -> UiCommandState` — Its state now.

## UiCommandTarget `0.2.16` — Where a command of a button or a key runs (`UiAction.CommandTarget`).
enum: Local, Server

## UiWorldSnapshotChangedEvent `0.2.16` — A new publication of live game values (`BlockUi.ReadWorld`) is there.
- `NewSnapshot { get; } -> UiWorldSnapshot` — The new publication.
- `OldPublication { get; } -> ulong` — The publication this subscriber's feed saw before (0 for the first one).

## VfxRef — A VFX and where it shows: the slot on every field that shows one (`EffectBlock.Vfx`, `CastBlock.Vfx`, `ProjectileBlock.Vfx`, `StatusBlock.Vfx`, `AreaBlock.Vfx`, an...
- `VfxRef(string name)` — A VFX by name: a `VfxBlock` of this mod, `"other-mod:name"` for another mod's, or `"game:name"` for one of the...
- `Attach { get; init; } -> bool` — Follows the slot while it plays (true), or stays where it started (false).
- `static Game(string name) -> VfxRef` — One of the game's VFX, by its catalog name (`GameVfx`).
- `static implicit operator VfxRef(BlockHandle handle) -> VfxRef` — The VFX block that `Blocks.Define` returned, by its `mod:name`.
- `static implicit operator VfxRef(string name) -> VfxRef` — A VFX by name, as `Kiln.VfxRef` takes it.
- `Name { get; init; } -> string` — The block's name, `"mod:name"`, or `"game:name"`.
- `Offset { get; init; } -> Vector3` — Metres from the slot, in the slot's own axes.
- `Parameters { get; init; } -> Dictionary<string, VfxValue>?` — Named parameters.
- `Seconds { get; init; } -> double?` — On an effect, how long it plays.
- `Slot { get; init; } -> VfxSlot?` — Where on the body it shows, where there is a body.

## VfxSlot — A place on a body: the game's attachment slots.
enum: Root, Head, Chest, LeftHand, RightHand, Feet, Weapon

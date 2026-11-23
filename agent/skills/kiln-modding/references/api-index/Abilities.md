# Abilities (Kiln.Abilities)

## Abilities — The game's Impact VM for mods: observe its programs (instances, native calls), change them per cast (config values before an instance's first run, wrappers around...
- `static ApplyBuff(EntityId target, Guid buff, TimeSpan? duration = null, EntityId source = default(EntityId)) -> Task<EntityId>` — Applies a buff or debuff to an entity through the game's own applyBuff native (the path the game's programs use: the...
- `static Available { get; } -> bool` — The service runs on this build.
- `static BuffsOf(EntityId entity) -> IReadOnlyList<EntityBuff>` `0.2.2` — Every buff on an entity (a player, a creature, an NPC), from the game's buff slots.
- `static Cast(EntityId caster, string abilityId, CastOrigin origin = CastOrigin.Server) -> CastResult` — Casts an ability for `caster` (a player or any actor with the attributes it costs): the same checks as a key press...
- `static CooldownLeft(EntityId caster, string abilityId) -> TimeSpan`
- `static readonly GenericImpactTemplate -> Guid` — The default template the game spawns for a SpawnImpact sequence event without its own template (an entity with only...
- `static HashOf(string name) -> uint` — FNV-1a of a name (native hashes: "ApplyDamage" 0xb04d0e7a, "mymod.Chill").
- `static HoldPrograms(params uint[] programIds) -> IDisposable` — Holds only instances of these programs for `ImpactStartingEvent` (the union over all callers; no filter = every new...
- `static IdOf(Guid guid) -> uint` — FNV-1a of a GUID's bytes (program, sequence, skill ids).
- `static Instance(uint impactId) -> ImpactInstance?` — A live instance by impact id (null when it does not exist).
- `static OnDamage(DamageHandler handler, int priority = 0) -> IDisposable` — Synchronous applyDamage hook (job thread): scale or cancel a program's hit before the game computes it (armour, crit...
- `static PlaySequence(EntityId actor, Guid sequence, SequenceOptions options) -> bool` — Starts an existing action sequence with explicit options (see the other overload).
- `static PlaySequence(EntityId actor, Guid sequence, bool abortCurrent = true) -> bool` — Starts an existing action sequence (its animation state, VFX/SFX notifies, timed SpawnImpacts, costs and ability...
- `static ProgramOf(EntityId entity) -> uint` — The program id of an entity's Impact component (FNV of its program GUID), 0 if none.
- `static Register(AbilityDefinition definition) -> AbilityHandle` — Registers a scripted ability (see `AbilityDefinition`).
- `static Registered { get; } -> IReadOnlyList<AbilityDefinition>` — Every registered ability.
- `static RemoveBuff(EntityId target, Guid buff) -> Task<BuffRemoveResult>` `0.2.2` — Takes a buff off an entity (a player, a creature, an NPC) through the game's own removeBuff native, the call the...
- `static RemoveBuff(EntityId target, uint buffId) -> Task<BuffRemoveResult>` `0.2.2` — Takes a buff off an entity by its id (`GameBuffs.Ids.X`, `EntityBuff.Id`): the game's removeBuff only hashes the...
- `static ResetCooldown(EntityId caster, string abilityId) -> void`
- `static RunImpact(Guid template, WorldPosition position, ImpactOptions? options = null) -> Task<EntityId>` — Spawns an impact template (a projectile, explosion or zone that has an Impact component) at `position` and binds it...
- `static RunImpact(string template, WorldPosition position, ImpactOptions? options = null) -> Task<EntityId>` — Spawns an impact template (a projectile, explosion or zone that has an Impact component) at `position` and binds it...
- `static RunProgram(Guid program, WorldPosition position, ImpactOptions? options = null) -> Task<EntityId>` — Runs a program (a game's or one Kiln registered) on the game's generic impact entity at `position`: the entity gets...
- `static SequenceOf(EntityId actor) -> (uint Sequence, long TriggerTime, int CallIndex)?` — The actor's current action sequence: (sequence id = FNV-1a of the ActorSequenceResource GUID, trigger time in ECS...
- `static Status { get; } -> AbilitiesStatus?` — kiln.abilities' state and counters, or null when it is not loaded.
- `static UseItem(EntityId player, SlotId slot) -> ItemUseResult` `0.2.13` — The player uses the consumable in `slot` the way the game does when they use it from the hotbar: the item's own...

## Abilities.Attributes — Health, mana, stamina and the special-ability charge of an entity, through the game's attribute system (writes re-run its formulas; replicated to clients like the...
- `static Add(EntityId entity, AbilityResource resource, int delta) -> bool` — Adds `delta` (negative to spend) through the game's attribute code.
- `static Get(EntityId entity, AbilityResource resource) -> int?` — The current value, or null when the entity has no such attribute.
- `static Get(EntityId entity, string component, uint attribute) -> int?` — Any attribute by component and attribute id (e.g. "Mana", 0x6aa2574e).
- `static Max(EntityId entity, AbilityResource resource) -> int?` — The maximum, or null when unknown.

## Abilities.Natives — The game's natives and Kiln's: list them, add managed natives, observe or wrap vanilla ones.
- `static All { get; } -> IReadOnlyList<ImpactNativeInfo>` — Every native in the server world's registry (the game's 107 plus Kiln's).
- `static Attach(uint program, string native, NativeStage stage, NativeWrapHandler handler, uint site = 0, int priority = 0) -> AttachResult` `0.2.4` — Attaches `handler` to the calls of the game's native `native` made by program `program` (at call site `site`), at...
- `static Declare(string fullName, int argc) -> void` — Declares a native from a mod's impact/natives.json before it is registered, so programs that call it pass the...
- `static DeclareTyped(string fullName, params NativeArg[] args) -> void` `0.2.4` — Declares Kiln native `fullName` with the type of each argument (since 0.2.0-dev.29; what a `"sync": true` entry of a...
- `static NameOf(uint hash) -> string`
- `static Observe(string nameOrHash) -> void` — Observes a vanilla native: its calls arrive as `ImpactNativeEvent` (applyDamage, applyBuff and spawnEntity are...
- `static Register(string name, int argc, NativeHandler handler) -> uint` — Registers a managed native "\<modid>.`name`" (hash = FNV-1a-32 of the full name; ECALL it from a program).
- `static RegisterSync(string name, int argc, NativeSyncHandler handler) -> NativeResult` `0.2.4` — Registers a synchronous native "\<modid>.`name`" (hash = FNV-1a-32 of the full name): `handler` runs inside the...
- `static StateOf(uint hash) -> NativeState` `0.2.4` — Where the Kiln native `hash` stands (any thread): one registered before the server world existed waits for its...
- `static Wrap(string nameOrHash, NativeWrapHandler handler, int priority = 0) -> IDisposable` — Wraps a vanilla native: `handler` runs synchronously on the game's job thread for every call, before the game's...

## Abilities.Programs — The game's ImpactRegistry: list, inspect and add programs (keen::ImpactProgram blobs, e.g. from tools/impact).
- `static Count { get; } -> int`
- `static IdOf(Guid program) -> uint`
- `static Info(uint id) -> ImpactProgramInfo?`
- `static Refresh() -> int` — Lists kiln.assets' ImpactProgram resources again and registers the new ones.
- `static Register(byte[] blob) -> uint` — Like `Programs.Register`, without the waiting flag.
- `static Register(byte[] blob, out bool waiting) -> uint` — Verifies and inserts a program blob (copied).
- `static Variables(uint id) -> IReadOnlyList<ImpactVariable>`

## AbilitiesStatus — Counters of kiln.abilities (diagnostics).
- `AbilitiesStatus(bool Natives, bool Programs, bool Instances, bool Attributes, int VanillaNatives, int NativeCapacity, int KilnNatives, int ProgramsTotal, int ProgramsKiln, int HeldNow, uint Interest, ulong InstancesCreated, ulong InstancesHeld, ulong OverridesApplied, ulong OverridesMismatched, ulong Runs, ulong NativeCalls, ulong NativeCallsObserved, ulong Signals, ulong ManagedCalls, ulong ManagedSuspends, ulong EventsDelivered, ulong EventsDropped, ulong WrapCalls, ulong ProgramsRefused, ulong Step)`
- `AttachmentsActive { get; init; } -> int` `0.2.4` — Attachments checked and taking calls (since 0.2.0-dev.29).
- `AttachmentsOff { get; init; } -> int` `0.2.4` — Attachments that take no call: their program is not loaded or does not call the native there (checked again whenever...
- `AttachmentsPending { get; init; } -> int` `0.2.4` — Attachments whose program and site are not checked yet (no server world's programs yet); since 0.2.0-dev.29, 0 with...
- `Attributes { get; init; } -> bool`
- `EventsDelivered { get; init; } -> ulong`
- `EventsDropped { get; init; } -> ulong`
- `HeldNow { get; init; } -> int`
- `Instances { get; init; } -> bool`
- `InstancesCreated { get; init; } -> ulong`
- `InstancesHeld { get; init; } -> ulong`
- `Interest { get; init; } -> uint`
- `KilnNatives { get; init; } -> int`
- `ManagedCalls { get; init; } -> ulong`
- `ManagedSuspends { get; init; } -> ulong`
- `NativeCalls { get; init; } -> ulong`
- `NativeCallsObserved { get; init; } -> ulong`
- `NativeCapacity { get; init; } -> int`
- `Natives { get; init; } -> bool`
- `OverridesApplied { get; init; } -> ulong`
- `OverridesMismatched { get; init; } -> ulong`
- `Programs { get; init; } -> bool`
- `ProgramsKiln { get; init; } -> int`
- `ProgramsRefused { get; init; } -> ulong`
- `ProgramsTotal { get; init; } -> int`
- `Runs { get; init; } -> ulong`
- `Signals { get; init; } -> ulong`
- `Step { get; init; } -> ulong`
- `VanillaNatives { get; init; } -> int`
- `WrapCalls { get; init; } -> ulong`

## AbilityCast — One cast of an ability (server, tick thread).
- `Ability { get; } -> AbilityDefinition`
- `At(TimeSpan sinceStart) -> Task` — Completes on the tick thread once `sinceStart` has passed since the cast started.
- `Caster { get; } -> EntityId`
- `CasterPosition { get; } -> WorldPosition`
- `CasterYaw { get; } -> float`
- `Damage(EntityId target, int amount, DamageOptions? options = null) -> Task<HitResult>` — Damages an entity as the caster (kiln.combat: a real engine hit with kill credit).
- `Impact(string template, WorldPosition? at = null, ImpactConfigs? configs = null) -> Task<EntityId>` — Spawns an impact template bound to the caster (`Abilities.RunImpact`), at `at` or the cast's `AbilityCast.Position`.
- `Origin { get; } -> CastOrigin`
- `PlaySequence(Guid sequence, bool abortCurrent = true) -> bool` — Plays an existing action sequence on the caster (`Abilities.PlaySequence`).
- `Position { get; } -> WorldPosition` — The point the ability aims at.
- `StartedMs { get; } -> long`
- `Step { get; } -> ulong`
- `Target { get; } -> EntityId` — The target entity (NearestEnemy: the enemy; Self: the caster; Forward: none).

## AbilityCastedEvent — A scripted ability was cast (cost paid, cooldown started; its OnCast is running).
- `Cast { get; } -> AbilityCast`
- `Paid { get; } -> AbilityCost`

## AbilityCastEvent — A scripted ability is about to be cast (all checks passed, nothing paid yet).
- `Cast { get; } -> AbilityCast`
- `Cost { get; set; } -> AbilityCost`
- `Reason { get; set; } -> string`

## AbilityCost — What an ability costs: mana, stamina and special-ability charge (attribute points; the game's own costs come from the same attributes).
- `AbilityCost()`
- `AbilityCost(int Mana = 0, int Stamina = 0, int Special = 0)`
- `Deconstruct(out int Mana, out int Stamina, out int Special) -> void`
- `Equals(AbilityCost other) -> bool`
- `IsFree { get; } -> bool`
- `Mana { get; init; } -> int`
- `static readonly None -> AbilityCost`
- `static OfMana(int n) -> AbilityCost`
- `static OfSpecial(int n) -> AbilityCost`
- `static OfStamina(int n) -> AbilityCost`
- `static operator !=(AbilityCost left, AbilityCost right) -> bool`
- `static operator +(AbilityCost a, AbilityCost b) -> AbilityCost`
- `static operator ==(AbilityCost left, AbilityCost right) -> bool`
- `Special { get; init; } -> int`
- `Stamina { get; init; } -> int`

## AbilityDefinition — A scripted ability.
- `CanCast { get; init; } -> Func<AbilityCast, string?>?` — An extra check before the cost is paid: return a refusal reason, or null to allow.
- `CastSequence { get; init; } -> Guid?` — An existing action sequence (ActorSequenceResource GUID) the caster plays when the cast starts: its animation...
- `CastTime { get; init; } -> TimeSpan` — Cast lock: the caster cannot start another Kiln ability for this long (it does not lock the game's own attacks).
- `ChatCommand { get; init; } -> string?` — A chat command that casts it (without the prefix, e.g. "nova"); null = only `/cast <id>`.
- `Cooldown { get; init; } -> TimeSpan` — Kiln's cooldown per caster (the game has no cooldown system for players).
- `Cost { get; init; } -> AbilityCost`
- `DefaultKey { get; init; } -> string?` — The client's default key ("G", "Shift+F"); null = no key binding.
- `Description { get; init; } -> string?`
- `DisplayName { get; init; } -> string?`
- `Id { get; init; } -> string` — "\<modid>:\<name>", e.g. "firenova:fire-nova".
- `Name { get; } -> string`
- `OnCast { get; init; } -> Func<AbilityCast, Task>` — The effect (server, tick thread).
- `PlayersOnly { get; init; } -> bool` — Only players may cast it through keys and commands (server code may cast for any actor).
- `Range { get; init; } -> double`
- `Targeting { get; init; } -> AbilityTargeting`

## AbilityHandle — A registered ability; dispose to take it off (also when the mod unloads).
- `Definition { get; } -> AbilityDefinition`
- `Dispose() -> void`

## AbilityResource — A resource attribute an ability can cost or change.
enum: Health, Mana, Stamina, SpecialAbility

## AbilityTargeting — Where an ability aims.
enum: Self, Forward, NearestEnemy

## AttachResult `0.2.4` — What `Natives.Attach` did: `AttachResult.Ok` with the `AttachResult.Handle` that detaches it, or why not (`AttachResult.Reason`: program not loaded, site not found, a...
- `AttachResult(bool Ok, string? Reason, NativeAttachment? Handle)`
- `Handle { get; init; } -> NativeAttachment?` — Dispose to detach (also when the mod unloads); null when not `Ok`.
- `Ok { get; init; } -> bool` — The handler is attached.
- `Reason { get; init; } -> string?` — Why not; null when `Ok`.

## BuffRemovedEvent `0.2.2` — A buff was taken off an entity through `Abilities.RemoveBuff`: the game's removeBuff said it removed it.
- `Buff { get; } -> uint` — The buff's id (FNV-1a of its GUID).
- `Mod { get; } -> string?` — The id of the mod that removed it; null when Kiln did, outside any mod.
- `Target { get; } -> EntityId` — The entity the buff was on.

## BuffRemoveResult `0.2.2` — The answer to `Abilities.RemoveBuff`: whether the game's removeBuff took the buff off.
- `Buff { get; } -> uint` — The buff's id (FNV-1a of its GUID).
- `Ok { get; } -> bool` — True when the game removed the buff.
- `Reason { get; } -> string?` — Why nothing was removed; null when `BuffRemoveResult.Ok`.
- `Target { get; } -> EntityId` — The entity the buff was taken from.

## CastOrigin — How a cast was started.
enum: Key, Command, Server

## CastResult — The outcome of a cast request.
- `CastResult(bool Ok, string? Reason, TimeSpan CooldownLeft, AbilityCast? Cast = null)`
- `Cast { get; init; } -> AbilityCast?`
- `CooldownLeft { get; init; } -> TimeSpan`
- `Ok { get; init; } -> bool`
- `Reason { get; init; } -> string?`
- `static Refused(string reason, TimeSpan cooldown = default(TimeSpan)) -> CastResult`

## DamageCall — A synchronous applyDamage call (job thread): change it before the game applies it, or cancel it.
- `Attacker { get; } -> EntityId`
- `AttackMod { get; set; } -> float` — The attack multiplier (variable AttackMod, 1.0 by default).
- `Cancel() -> void`
- `Damage { get; set; } -> DamageSetup` — The damage setup argument (per-type factors), when connected.
- `HasAttackMod { get; } -> bool`
- `ImpactId { get; } -> uint`
- `ProgramId { get; } -> uint`
- `Self { get; } -> EntityId`
- `Target { get; } -> EntityId`

## DamageHandler
- `Invoke(ref DamageCall call) -> void`

## DamageSetup — Damage per type, the engine's DamageSetup variable (52 bytes).
- `DamageSetup()`
- `DamageSetup(float Cut, float Pierce, float Blunt, float Fire, float Ice, float Fog, float Lightning, float Healing, float Poison, float Wood, float Stone, float Metal, float Explosion)`
- `Blunt { get; set; } -> float`
- `Cut { get; set; } -> float`
- `Explosion { get; set; } -> float`
- `Fire { get; set; } -> float`
- `static FireOnly(float f) -> DamageSetup`
- `Fog { get; set; } -> float`
- `Healing { get; set; } -> float`
- `Ice { get; set; } -> float`
- `static IceOnly(float f) -> DamageSetup`
- `Lightning { get; set; } -> float`
- `Metal { get; set; } -> float`
- `Pierce { get; set; } -> float`
- `Poison { get; set; } -> float`
- `Scaled(float k) -> DamageSetup`
- `Stone { get; set; } -> float`
- `Total { get; } -> float`
- `Wood { get; set; } -> float`

## EntityBuff `0.2.2` — A buff on an entity: a player, a creature or an NPC (the game keeps them in the entity's BuffContainer, on the server).
- `EntityBuff(uint Id, string Slot, TimeSpan? Remaining = null, int Level = 0)`
- `Id { get; init; } -> uint` — The game's BuffTypeId: FNV-1a of the buff's GUID (`GameBuffs.Ids.X`, `Abilities.IdOf`).
- `Is(Guid buff) -> bool` — True for this buff's GUID (its id is FNV-1a of the GUID).
- `Is(uint id) -> bool` — True for this buff id.
- `Level { get; init; } -> int` — Its level, when known.
- `Remaining { get; init; } -> TimeSpan?` — Time left, when known.
- `Slot { get; init; } -> string` — The game's BuffSlotType name: "Food", "Buff", "Debuff", "Rested", "TerrainDebuff".

## EquippedGem `0.2.5` — A gem whose effect runs for a player now: the game spawned its impact entity.
- `EquippedGem(EntityId Player, GemImpactSlot Slot, SocketedGem Socket, EntityId Impact)`
- `Impact { get; init; } -> EntityId`
- `Player { get; init; } -> EntityId`
- `Slot { get; init; } -> GemImpactSlot`
- `Socket { get; init; } -> SocketedGem`

## GemConfigValue `0.2.5` — One config of a gem's impact program.
- `GemConfigValue(uint Id, string? Alias, string Kind, double Value, double? ScaleFactor)`
- `Alias { get; init; } -> string?`
- `Id { get; init; } -> uint`
- `Kind { get; init; } -> string`
- `ScaleFactor { get; init; } -> double?`
- `Text { get; init; } -> string?` — The value as kiln.gamedata writes it when it is not a number: a GUID (a buff, a template), an enum name (`Big`)...
- `Value { get; init; } -> double`

## GemCost `0.2.5` — An amount of one item: what a forge action costs, or what Recycle gives.
- `GemCost()`
- `GemCost(ItemId Item, int Count)`
- `Count { get; init; } -> int` — How many; 0 is nothing.
- `static Free { get; } -> GemCost` — Nothing.
- `IsFree { get; } -> bool` — True when this is nothing.
- `Item { get; init; } -> ItemId` — The item.

## GemCraftAction `0.2.5` — A Shroud Forge's gem actions (the game's `WeaponGemActionType`).
enum: Socket, Unsocket, Upgrade, Recycle

## GemCraftedEvent `0.2.5` — A forge action happened: a player's own (after the game committed it) or one of `Gems`' calls, `Gems.ReturnSocketedGems` included (Unsocket, no forge).
- `Action { get; } -> GemCraftAction` — The action.
- `ByCode { get; } -> bool` — True for a call of `Gems`, false for a player's own forge action.
- `Carrier { get; } -> ItemInstanceRef` — The item with the socket; none for a loose gem.
- `Forge { get; } -> EntityId` — The forge; none for `Gems.ReturnSocketedGems`' forgeless return.
- `Gem { get; } -> ItemInstanceRef` — The gem.
- `Involves(EntityId entity) -> bool` — Whether the event is about `entity`: the player, the forge, the carrier or the gem.
- `LevelAfter { get; } -> int` — The gem's level after (Recycle: 0).
- `LevelBefore { get; } -> int` — The gem's level before.
- `Paid { get; } -> GemCost` — What the player paid.
- `Player { get; } -> EntityId` — The player.
- `Received { get; } -> GemCost` — What the player got (Recycle).
- `Result { get; } -> GemCraftResult` — What happened.

## GemCraftingEvent `0.2.5` — A player is about to run a forge action: socket, unsocket, upgrade or recycle a gem at a Shroud Forge.
- `GemCraftingEvent(EntityId player, EntityId forge, GemCraftAction action, ItemInstanceRef carrier, ItemInstanceRef gem, int gemLevel, GemCost cost, GemCost yield, uint version)` — Makes the event (Kiln posts it; a mod has no reason to).
- `Action { get; } -> GemCraftAction` — What the player asked for.
- `Carrier { get; } -> ItemInstanceRef` — The item with the socket; none for a loose gem (Upgrade, Recycle).
- `Cost { get; } -> GemCost` — What the game will charge the player (the game's own cost function, with this forge); nothing for Recycle.
- `Forge { get; } -> EntityId` — The forge the player uses.
- `Gem { get; } -> ItemInstanceRef` — The gem.
- `GemLevel { get; } -> int` — The gem's level now.
- `Involves(EntityId entity) -> bool` — Whether the event is about `entity`: the player, the forge, the carrier or the gem.
- `Player { get; } -> EntityId` — The player.
- `Version { get; } -> uint` — The version of the player's input (one action of one player).
- `Yield { get; } -> GemCost` — Recycle: the essence the game will give; nothing for the other actions.

## GemCraftOptions `0.2.5` — Options of the forge actions (`Gems.Socket` and the others).
- `CarrierHolder { get; init; } -> EntityId` `0.2.7` — Where the carrier (the item with the socket) is: none (the default) for `GemCraftOptions.Holder`, else a container...
- `Cost { get; init; } -> GemCost?` — Null (the default): the game's cost, paid by the player in the forge's material.
- `GemHolder { get; init; } -> EntityId` `0.2.7` — Where the gem is (Socket; a loose gem for Upgrade and Recycle): none (the default) for `GemCraftOptions.Holder`...
- `Holder { get; init; } -> EntityId` — Where the carrier and the gem are: none (the default) for the player's own inventories, or an entity with an...
- `PayFrom { get; init; } -> GemPayer` `0.2.7` — Who pays the game's cost (Socket, Upgrade; Unsocket is free on this game build): the player (the default), a...
- `ReturnTo { get; init; } -> EntityId` `0.2.7` — Where an unsocketed gem (Unsocket) and Recycle's essence go: none (the default) for the player, else a container...

## GemCraftResult `0.2.5` — What a forge action did.
- `GemCraftResult(GemCraftStatus Status, GemCraftAction Action, EntityId Player, EntityId Forge, ItemInstanceRef Carrier, ItemInstanceRef Gem, int LevelBefore, int LevelAfter, GemCost Paid, GemCost Received, string Message)`
- `Action { get; init; } -> GemCraftAction` — The action.
- `Carrier { get; init; } -> ItemInstanceRef` — The item with the socket; none for a loose gem.
- `Forge { get; init; } -> EntityId` — The forge; none for `Gems.ReturnSocketedGems`' forgeless return.
- `Gem { get; init; } -> ItemInstanceRef` — The gem.
- `LevelAfter { get; init; } -> int` — After (Upgrade: one more; Recycle: 0).
- `LevelBefore { get; init; } -> int` — The gem's level before.
- `Message { get; init; } -> string` — Why, for anything but `GemCraftStatus.Done`; a note otherwise (may be empty).
- `Ok { get; } -> bool` — True when the action happened.
- `Paid { get; init; } -> GemCost` — What the player paid.
- `Player { get; init; } -> EntityId` — The player it ran for.
- `Reason { get; } -> string?` — Why it did not happen; null when `GemCraftResult.Ok`.
- `Received { get; init; } -> GemCost` — What the player got (Recycle).
- `Status { get; init; } -> GemCraftStatus` — The outcome.

## GemCraftStatus `0.2.5` — How a forge action ended.
enum: Done, Refused, NotEnough, NoForge, NotFound, NoRoom, MaxLevel, Canceled, Unavailable, Dropped, Occupied

## GemDefinition `0.2.5` — A new gem for `Gems.Register`: a clone of a gem item (`GemDefinition.BaseOn`) with its own GUID, texts, icon, rarity and level range, and an effect: the base's...
- `BaseOn { get; init; } -> string` — The gem to clone: a game gem's debug name (`T1_Gem_GemOfBleeding`), `"0x..."` id or GUID.
- `Configs { get; init; } -> Dictionary<string, object>` — The gem's config values (its ItemInfo `impactValues`), as kiln.gamedata's item paths take them: key = a config alias...
- `Description { get; init; } -> string?` — The tooltip description (every language).
- `DisplayName { get; init; } -> string?` — The name players see (every language).
- `Guid { get; init; } -> Guid?` — The gem item's fixed GUID (saves keep its id, FNV-1a of the GUID: never change it once released).
- `Icon { get; init; } -> string?` — The icon: a PNG file of the mod (`"icons/frostbite.png"`), `"item:<name>"` or an item GUID.
- `Id { get; init; } -> string` — `"<modid>:<name>"` with the registering mod's id, unique among the registered gems.
- `LevelMax { get; init; } -> int?` — The highest level (1..105), what an upgrade at a forge can reach (the forge's own cap still applies).
- `LevelMin { get; init; } -> int?` — The level a new gem has (1..105).
- `Name { get; init; } -> string?` — The item's debug name (`A-Z a-z 0-9 _ - .`).
- `OnTrigger { get; init; } -> Action<GemTriggerContext>?` — The C# effect, on the server's tick thread, the Pre after the game's step that had the hit, as the gem's mod (its...
- `Program { get; init; } -> Guid?` — A loaded Impact program for the gem's effect: another gem's, a perk's, or one the mod cooked with tools/impact and...
- `Rarity { get; init; } -> ItemRarity?` — The rarity.
- `Texts { get; init; } -> Dictionary<string, ItemTexts>` — Texts per language (`"en"`, `"de"`, ...), as kiln.content's item texts.
- `Trigger { get; init; } -> GemTrigger?` — A Kiln trigger program for the effect: the gem calls `GemDefinition.OnTrigger`.

## GemForgeOpenedEvent `0.2.7` — A player's game has the Shroud Forge screen open (a Kiln client told the server): posted when it opens, every 5 s while it stays open, and right after the player used...
- `Forge { get; } -> EntityId` — The nearest Shroud Forge within 12 m of the player; none when Kiln found none.
- `Involves(EntityId entity) -> bool` — Whether the event is about `entity`: the player or the forge.
- `Player { get; } -> EntityId` — The player.
- `Refresh { get; } -> bool` — False when the screen just opened; true for the 5-second refresh and after an action on an offered gem.

## GemHandle `0.2.5` — A gem registration (`Gems.Register`): `GemHandle.Ok`, or why it was refused.
- `Definition { get; } -> GemDefinition` — The definition.
- `Dispose() -> void` — Stops the handler and drops the registration (the item stays until the server restarts).
- `Guid { get; } -> Guid` — The gem item's GUID.
- `Item { get; } -> ItemId` — The gem item (FNV-1a of `GemHandle.Guid`).
- `Name { get; } -> string` — The gem item's debug name.
- `Ok { get; } -> bool` — True when the gem was registered.
- `Reason { get; } -> string?` — Why not; null when `GemHandle.Ok`.

## GemImpactSlot `0.2.5` — The game's equipment impact slots: a socketed gem runs from the item in any of them.
enum: Held, Shield, Head, Chest, Hands, Legs, Feet, Ring1, Ring2, Ammo, Spell

## GemInfo `0.2.5` — A gem item definition (category WeaponGem) from the catalog: the game's own gems and any mod's.
- `GemInfo(ItemId Item, string Name, string? DisplayName, ItemRarity Rarity, int LevelMin, int LevelMax, Guid Program, IReadOnlyDictionary<uint, GemConfigValue> Configs, bool FromMod)`
- `Configs { get; init; } -> IReadOnlyDictionary<uint, GemConfigValue>`
- `DisplayName { get; init; } -> string?`
- `FromMod { get; init; } -> bool`
- `Item { get; init; } -> ItemId`
- `LevelMax { get; init; } -> int`
- `LevelMin { get; init; } -> int`
- `Name { get; init; } -> string`
- `Program { get; init; } -> Guid`
- `Rarity { get; init; } -> ItemRarity`

## GemPayer `0.2.7` — Who pays a forge action's cost (`GemCraftOptions.PayFrom`, `GemSourceOptions.PayFrom`): the player (the default), a container your mod vouches for, or the base's...
- `static BaseStorage { get; } -> GemPayer` — The base's crafting-stock containers pay.
- `Container { get; } -> EntityId` — The paying container (`GemPayerKind.Container`); none otherwise.
- `Equals(GemPayer other) -> bool`
- `static From(EntityId container) -> GemPayer` — One container pays (a chest your mod vouches for).
- `IsPlayer { get; } -> bool` — True for the player (the game's own payment).
- `Kind { get; } -> GemPayerKind` — Which payer.
- `static operator !=(GemPayer left, GemPayer right) -> bool`
- `static operator ==(GemPayer left, GemPayer right) -> bool`
- `static Player { get; } -> GemPayer` — The player pays, as the game does (the default).
- `static PlayerThenBaseStorage { get; } -> GemPayer` — The player pays what it can, the base's crafting-stock containers the rest.

## GemPayerKind `0.2.7` — Who pays a forge action's cost (`GemPayer`).
enum: Player, Container, BaseStorage, PlayerThenBaseStorage

## GemReturned `0.2.5` — One gem `Gems.ReturnSocketedGems` took out of a non-weapon socket (or could not).
- `GemReturned(ItemInstanceRef Carrier, ItemId Gem, int Level, bool Returned, string? Reason)`
- `Carrier { get; init; } -> ItemInstanceRef` — The ring, armour piece, shield or other gear that held it.
- `Gem { get; init; } -> ItemId` — The gem item.
- `Level { get; init; } -> int` — The gem's level.
- `Reason { get; init; } -> string?` — Why not, when `Returned` is false.
- `Returned { get; init; } -> bool` — True when the gem is in the owner's inventory now.

## Gems `0.2.5` — Enshrouded's gems for mods: the catalog, the gems a player's equipment runs, sockets, new gems with their own effects (`Register`), the forge's actions and gem...
- `static AddSocket(ItemInstanceRef item) -> Task<GemSocketResult>` — Gives an existing piece of gear without a socket an empty one: the game cannot add a component to an item entity, so...
- `static AllowSockets(GemSocketKinds kinds, double rollChance = 0) -> GemSocketsSetup` — Lets gear of `kinds` hold a gem, for as long as the calling mod is loaded (a later call replaces this mod's...
- `static Catalog { get; } -> IReadOnlyList<GemInfo>` — Every gem item: the game's and those mods added (items of category `ItemCategory.WeaponGem`), with the program and...
- `const CraftCapability = "gems.craft" -> string` — The capability row of the forge actions and their events.
- `static Equipped(EntityId player) -> IReadOnlyList<EquippedGem>` — The gems whose effects run for a player now, one per equipment impact slot with a socketed gem (the held item...
- `static Get(ItemId gem) -> GemInfo?` — One gem item's definition, or null when the item is not a gem.
- `static OfferChestGems(EntityId player, EntityId holder, GemSourceOptions? options = null) -> GemSourceOffer` `0.2.7` — Offers the loose gems in `holder` (a container your mod lets the player use) to `player` at the Shroud Forge: on the...
- `const PayersCapability = "gems.payers" -> string` `0.2.7` — The capability row of payers and returns other than the player (`GemCraftOptions.PayFrom`...
- `static Recycle(EntityId player, ItemInstanceRef gemOrCarrier, EntityId forge, GemCraftOptions? options = null) -> Task<GemCraftResult>` — Destroys a gem for the forge's essence, as the player's own Recycle at `forge` does (the amount by the gem's level...
- `static Register(GemDefinition definition) -> GemHandle` — Registers a new gem: a gem item cloned from `GemDefinition.BaseOn` through kiln.content (its own GUID, texts, icon...
- `static ReturnSocketedGems(EntityId owner) -> Task<IReadOnlyList<GemReturned>>` — The uninstall path: takes every gem out of the non-weapon gear of `owner`'s inventories (worn included) and puts it...
- `static Socket(EntityId player, ItemInstanceRef carrier, ItemInstanceRef gem, EntityId forge, GemCraftOptions? options = null) -> Task<GemCraftResult>` — Puts `gem` into the empty socket of `carrier`, as the player's own Insert at `forge` does: the game's action with...
- `static Socket(ItemInstanceRef carrier) -> SocketedGem?` — The gem in an item's socket, or null when it has no socket, the socket is empty, or the item is gone.
- `static SocketKinds { get; } -> GemSocketKinds` — The kinds any mod allows sockets on now (this side).
- `const SocketsCapability = "gems.sockets" -> string` — The capability row of sockets on other gear: on a server the createPide hook's...
- `const SourcesCapability = "gems.sources" -> string` `0.2.7` — The capability row of the offered chest gems (`Gems.OfferChestGems`, `GemForgeOpenedEvent`).
- `static Unsocket(EntityId player, ItemInstanceRef carrier, EntityId forge, GemCraftOptions? options = null) -> Task<GemCraftResult>` — Takes the gem out of `carrier`'s socket into `player`'s backpack, as the player's own Remove at `forge` does (the...
- `static Upgrade(EntityId player, ItemInstanceRef gemOrCarrier, EntityId forge, GemCraftOptions? options = null) -> Task<GemCraftResult>` — Raises a gem's level by one, as the player's own Upgrade at `forge` does: the game's cost in the forge's material...
- `static WithdrawChestGems(EntityId player, EntityId holder = default(EntityId)) -> int` `0.2.7` — Withdraws your mod's offers to `player` in `holder` (none: every container).

## GemSocketChange `0.2.5` — What happened to a socket (`GemSocketChangedEvent`).
enum: Socketed, Unsocketed, LevelChanged, Removed

## GemSocketChangedEvent `0.2.5` — A gem socket of one of a player's items changed: socketed, unsocketed, upgraded or removed, however it happened (the Gem Forge, a mod, a command).
- `After { get; } -> SocketedGem?` — The socket's gem now (null: empty, or the carrier is gone).
- `Before { get; } -> SocketedGem?` — The socket's gem before (null: empty).
- `Carrier { get; } -> ItemInstanceRef` — The item with the socket.
- `Change { get; } -> GemSocketChange` — What happened.
- `Involves(EntityId entity) -> bool` — Whether the event is about `entity` (the owner or the carrier's item entity).
- `Owner { get; } -> EntityId` — The player whose item it is.
- `Step { get; } -> ulong` — The step Kiln saw it in.

## GemSocketKinds `0.2.5` — Kinds of gear, besides weapons, that may hold a gem (`Gems.AllowSockets`).
enum: None, Rings, Armour, Shields, OtherGear, All

## GemSocketResult `0.2.5` — The outcome of `Gems.AddSocket`.
- `GemSocketResult(GemSocketStatus Status, ItemInstanceRef Before, ItemInstanceRef After, string Reason)`
- `After { get; init; } -> ItemInstanceRef` — With `GemSocketStatus.Done` or `GemSocketStatus.Incomplete`: the new item (a new item entity in the same slot; the...
- `Before { get; init; } -> ItemInstanceRef` — The item asked for.
- `Missing { get; init; } -> IReadOnlyList<string>` — With `GemSocketStatus.Incomplete`: what the new item lacks, one entry each (`"a socket"`, `"level 34 (it has 30)"`...
- `Reason { get; init; } -> string` — Why, in words, for anything but Done; "" for Done.
- `Status { get; init; } -> GemSocketStatus` — What happened.
- `Succeeded { get; } -> bool` — True when the new item has its socket and everything a re-creation keeps (read back).

## GemSocketsSetup `0.2.5` — What `Gems.AllowSockets` set up on this side.
- `GemSocketsSetup(GemSocketKinds Kinds, bool Server, string? ServerReason, bool ForgeLists, string? ForgeReason)`
- `ForgeLists { get; init; } -> bool` — The client part works here: the Gem Forge lists show gear of these kinds that has a socket.
- `ForgeReason { get; init; } -> string?` — Why not, when `ForgeLists` is false (a dedicated server has no forge screen).
- `Kinds { get; init; } -> GemSocketKinds` — The kinds this mod allows now (the union over mods is `Gems.SocketKinds`).
- `Server { get; init; } -> bool` — The server part works here: `Gear.GemSlot`, `Gems.AddSocket`, the rolls and data packs' `gemSlotOverride` on these...
- `ServerReason { get; init; } -> string?` — Why not, when `Server` is false (a game client without a server world, an older Kiln, an unknown game build).

## GemSocketStatus `0.2.5` — The outcome of `Gems.AddSocket`.
enum: Done, AlreadyHasSocket, KindNotAllowed, Equipped, NotFound, NotGear, Refused, Unavailable, Incomplete

## GemSourceOffer `0.2.7` — What `Gems.OfferChestGems` did.
- `GemSourceOffer(EntityId Player, EntityId Holder, IReadOnlyList<OfferedGem> Gems, string? Refusal)`
- `Gems { get; init; } -> IReadOnlyList<OfferedGem>` — The gems offered (loose gems of the container, at most 256 per player).
- `Holder { get; init; } -> EntityId` — The container.
- `Ok { get; } -> bool` — True when the offer was taken (possibly of no gems).
- `Player { get; init; } -> EntityId` — The player.
- `Refusal { get; init; } -> string?` — Why nothing was offered; null when it worked (an empty list is fine: no gems there).

## GemSourceOptions `0.2.7` — Options of `Gems.OfferChestGems`.
- `Lifetime { get; init; } -> TimeSpan` — How long the offer is good for, real time (default 30 s; 1 ms to 120 s).
- `PayFrom { get; init; } -> GemPayer` — Who pays when the player inserts or upgrades an offered gem (default: the player).
- `ReturnTo { get; init; } -> EntityId` — Where Recycle's essence goes when the player recycles an offered gem: none (the default) for the player, else a...

## GemTrigger `0.2.5` — When a gem's C# handler runs: `GemTrigger.OnHit`, `GemTrigger.OnKill`, `GemTrigger.OnBackstab`, `GemTrigger.OnHitTaken`, `GemTrigger.Passive`.
- `EnemiesOnly { get; } -> bool` — Only when the other side of the hit is an enemy faction (the game's own gems' "all enemies" mask: creatures, not...
- `Interval { get; } -> TimeSpan` — `GemTriggerKind.Passive`: how often the handler runs while the gem is worn.
- `Kind { get; } -> GemTriggerKind` — The trigger program this gem runs.
- `static OnBackstab(bool enemiesOnly = true) -> GemTrigger` — Each backstab the wearer lands.
- `static OnHit(bool crit = false, bool enemiesOnly = true) -> GemTrigger` — Each hit the wearer lands; with `crit` only critical hits.
- `static OnHitTaken(bool enemiesOnly = true) -> GemTrigger` — Each hit the wearer takes; `enemiesOnly`: only from an enemy faction.
- `static OnKill(bool enemiesOnly = true) -> GemTrigger` — Each killing blow the wearer lands.
- `static Passive(TimeSpan interval) -> GemTrigger` — Every `interval` while the gem is worn (at least `GemTrigger.PassiveStep`, at most one hour; it runs on the...
- `static readonly PassiveStep -> TimeSpan` — The shortest `GemTrigger.Passive` interval: the passive program's own step (250 ms).

## GemTriggerContext `0.2.5` — One trigger of a registered gem: who wore it, the hit, the gem and where it is socketed.
- `Backstab { get; } -> bool` — A backstab.
- `Blocked { get; } -> bool` — The hit was blocked.
- `Carrier { get; } -> ItemInstanceRef` — The item the gem is socketed in.
- `Crit { get; } -> bool` — A critical hit.
- `Damage { get; } -> int` — The health the hit took from its target (0: none, or it healed).
- `Definition { get; } -> GemDefinition` — The gem's definition.
- `Gem { get; } -> ItemId` — The gem item.
- `HealthChange { get; } -> int` — The target's health change of the hit as the game recorded it (negative: damage).
- `Impact { get; } -> EntityId` — The gem's impact entity (the running trigger program).
- `KillingBlow { get; } -> bool` — The hit killed its target.
- `Level { get; } -> int` — The gem's level (1..105): scale effects with it.
- `OtherIsEnemy { get; } -> bool` — The other side is of an enemy faction.
- `Player { get; } -> EntityId` — The wearer (a player).
- `Position { get; } -> WorldPosition` — Where the hit landed (the wearer's position is not sent for `GemTriggerKind.Passive`: 0).
- `Slot { get; } -> GemImpactSlot` — The equipment slot whose item carries the gem.
- `Source { get; } -> EntityId` — The hit's direct source (a projectile, an explosion, the attacker).
- `Step { get; } -> ulong` — The game's step of the signal.
- `Target { get; } -> EntityId` — The other side: the one hit (the target's root entity), for `GemTriggerKind.HitTaken` the attacker's root; none for...
- `TargetPart { get; } -> EntityId` — The entity the hit struck (a part of `GemTriggerContext.Target`, or the target itself); for...
- `Trigger { get; } -> GemTriggerKind` — What happened.
- `Weapon { get; } -> EntityId` — The weapon entity of the hit (none for some hits).

## GemTriggeredEvent `0.2.5` — A socketed gem did something: its impact program ran applyDamage, applyBuff or spawnEntity (or a native passed to `Natives.Observe`).
- `Gem { get; } -> EquippedGem` — The gem: its impact slot, carrier, gem item and level, and the gem impact's entity.
- `Info { get; } -> GemInfo?` — The gem's definition (`Gems.Get`; null when the catalog does not know it).
- `Involves(EntityId entity) -> bool` — Whether the event is about `entity` (the player or the target).
- `Native { get; } -> ImpactNativeEvent` — What the gem did: an `ImpactDamageEvent`, `ImpactBuffEvent`, `ImpactSpawnEvent` or another observed native.
- `Player { get; } -> EntityId` — The player whose gem it is.
- `Step { get; } -> ulong` — The step the native ran in.
- `Target { get; } -> EntityId` — Whom it did it to: the damage's or buff's target, the spawned entity; none for another native.

## GemTriggerKind `0.2.5` — What sets off a gem's C# handler (`GemDefinition.OnTrigger`).
enum: Hit, Crit, Kill, Backstab, HitTaken, Passive

## ImpactBuffEvent — applyBuff ran (args: 0 buff type, 1 target, 2 damage setup, 4 duration).
- `Buff { get; } -> Guid`
- `Duration { get; } -> TimeSpan`
- `Target { get; } -> EntityId`

## ImpactConfigIds — Config ids of the shared vocabulary (docs/re/abilities.md section 1) and the C++-read cost ids, for keys that are not program variable names.
- `const AttackCategory = 606890942 -> uint`
- `const AttackMod = 2236524794 -> uint`
- `const BuffType = 1524311620 -> uint`
- `const Damage = 1921402148 -> uint`
- `const DamageScaleAttribute = 1395916278 -> uint`
- `const Duration = 319426290 -> uint`
- `const FuelAmount = 3360236544 -> uint`
- `const FuelCharges = 3919228920 -> uint`
- `const HitReactionType = 2178607591 -> uint`
- `const IgnoreConsecutiveHits = 3602621489 -> uint`
- `const ManaCost = 2938334926 -> uint`
- `const Material = 3205040191 -> uint`
- `const NoiseRange = 2710833586 -> uint`
- `static Parse(string key) -> uint` — "0x..." text or a known alias -> config id; 0 when neither.
- `const PostWaitTime = 2854198668 -> uint`
- `const PushBackStrength = 3844426053 -> uint`
- `const SpawnTemplate = 4138173901 -> uint`
- `const SpawnTemplate2 = 699017590 -> uint`
- `const SpecialAbilityCost = 770044405 -> uint`
- `const StaminaCost = 1920827256 -> uint`
- `const WaitTime = 3793439602 -> uint`

## ImpactConfigs — Config values for `Abilities.RunImpact` and `Abilities.RunProgram`: key = a variable alias or name of the program ("NoiseRange", "Damage1", "SpawnTemplate_Pre"), a...

## ImpactDamageEvent — applyDamage ran (args: 0 wasBlocked, 1 attacker/root, 2 target, 3 position, 6 AttackMod, 12 push-back, 13 hit reaction, 14 damage setup).
- `Attacker { get; } -> EntityId`
- `AttackMod { get; } -> float`
- `Blocked { get; } -> bool`
- `Damage { get; } -> DamageSetup`
- `Position { get; } -> WorldPosition`
- `Target { get; } -> EntityId`

## ImpactEndedEvent — An instance reported by `ImpactStartedEvent` or `ImpactStartingEvent` no longer exists (its program finished or its entity was destroyed).
- `Impact { get; } -> ImpactRef`
- `Step { get; } -> ulong`

## ImpactInstance — A live instance (tick thread: Pre/Post, handlers, OnLoad).
- `Alive { get; } -> bool` — Whether the instance still exists.
- `Find(string key) -> ImpactVariable?` — A variable by alias or debug name ("NoiseRange", "NoiseRange::value"), config id text ("0xa19409b2") or a known...
- `Find(uint configId) -> ImpactVariable?` — A variable by alias or debug name ("NoiseRange", "NoiseRange::value"), config id text ("0xa19409b2") or a known...
- `Flags { get; } -> uint?` — Instance flags (Initialized 1, Executed 2, Halt 8, Error 0x10, Destroy 0x40), or null when gone.
- `GetBool(string key) -> bool?`
- `GetDamage(string key) -> DamageSetup?`
- `GetFloat(string key) -> float?`
- `GetGuid(string key) -> Guid?`
- `GetInt(string key) -> int?`
- `GetTime(string key) -> TimeSpan?`
- `ImpactId { get; } -> uint`
- `Read(ImpactVariable v) -> byte[]?`
- `Set(string key, object value) -> bool` — Sets a variable from a value (float, int, bool, TimeSpan, Guid, DamageSetup, byte[]), converted to the variable's type.
- `SetFloat(string key, float value) -> bool`
- `SetInt(string key, int value) -> bool`
- `Variables { get; } -> IReadOnlyList<ImpactVariable>`
- `Write(ImpactVariable v, ReadOnlySpan<byte> bytes) -> bool` — Writes a variable.

## ImpactNativeAccess `0.2.4` — Generated code of `ImpactNativeAttribute` methods: typed views of a call's arguments, and the registration hand-over.
- `static Connected(ref NativeCall call, int i) -> bool` — Argument `i` is a variable (not 0, not a raw value).
- `static Context(ref NativeCall call) -> NativeContext` — The call as the method's `NativeContext`.
- `static Provide(Action register) -> void` — The generated module initializer of a mod's [ImpactNative] methods: `register` runs at the next tick for the mod...
- `static RawValue(ref NativeCall call, int i) -> uint` — A `[Raw] uint` argument: the variable's value, or the raw value the program pushed.
- `static Ref<T>(ref NativeCall call, int i, uint type) -> T` — A live view of argument `i`'s variable as `T`.
- `static SetTime(ref NativeCall call, int i, TimeSpan value) -> void` — Writes a `TimeSpan` into a Time variable (ns = ticks * 100, saturated).
- `static Time(ref NativeCall call, int i) -> TimeSpan` — A Time variable (int64 nanoseconds) as a `TimeSpan` (ticks = ns / 100).

## ImpactNativeAttribute `0.2.4` — Makes a static method of a mod a synchronous Impact native "\<modid>.`ImpactNativeAttribute.Name`" (since 0.2.0-dev.29).
- `ImpactNativeAttribute(string name)`
- `Name { get; } -> string` — The native's name without the mod id.

## ImpactNativeEntryAttribute `0.2.4` — Generated code: the impact/natives.json entry of one [ImpactNative] method ("args" as JSON), which the SDK's KilnImpactNatives build task writes into the output's...
- `ImpactNativeEntryAttribute(string name, string args)`
- `Args { get; } -> string` — The entry's "args" array as JSON text.
- `Name { get; } -> string` — The native's name without the mod id.

## ImpactNativeEvent — A vanilla native ran inside a program (observe only; posted in the next Pre).
- `ArgBytes(int i) -> ReadOnlySpan<byte>` — The bytes of argument i's variable when it was larger than 4 bytes (else empty).
- `ArgCount { get; } -> int`
- `ArgEntity(int i) -> EntityId`
- `ArgFloat(int i) -> float`
- `ArgU32(int i) -> uint` — The first 4 bytes of argument i's variable after the call (or the raw value).
- `Error { get; } -> byte`
- `Impact { get; } -> ImpactRef`
- `IsConnected(int i) -> bool`
- `Native { get; } -> uint`
- `NativeName { get; } -> string`
- `RawArg(int i) -> uint` — The raw argument (a handle 0xFFFF0000|index, 0 = not connected, or a raw value).
- `Result { get; } -> byte` — The value the native pushed (applyDamage: 1 = the hit was not blocked).
- `Site { get; } -> uint`
- `Step { get; } -> ulong`

## ImpactNativeInfo — A native in the game's registry.
- `ImpactNativeInfo(uint Hash, string Name, int Argc, ImpactNativeKind Kind)`
- `Argc { get; init; } -> int`
- `Hash { get; init; } -> uint`
- `Kind { get; init; } -> ImpactNativeKind`
- `Name { get; init; } -> string`

## ImpactNativeKind
enum: Vanilla, Wrapped, Builtin, Managed, Sync `0.2.4`

## ImpactOptions — Options of `Abilities.RunImpact` / `Abilities.RunProgram`.
- `Components { get; init; } -> IReadOnlyList<SpawnComponent>?` — More spawn components (kiln.spawn's `SpawnOptions.Components`).
- `Configs { get; init; } -> ImpactConfigs?` — Config values for the instance (see `ImpactConfigs`).
- `Faction { get; init; } -> byte?` — The impact's faction (FactionType: Player 7, PlayerAlly 11, Vukah 3, ...).
- `Root { get; init; } -> EntityId` — The caster: the impact's ImpactHierarchy root and parent (kill credit, faction and same-root filters: the caster is...
- `Tag { get; init; } -> string?`
- `Track { get; init; } -> bool` — Record the entity in kiln.spawn's per-world list (default: no; impacts are short-lived and never saved: they always...
- `YawDegrees { get; init; } -> float`

## ImpactProgramInfo — A program in the game's ImpactRegistry.
- `ImpactProgramInfo(uint Id, Guid Guid, int CodeWords, int ShutdownWords, int Variables, int DataBytes, int StackSize, int Streams, bool FromKiln)`
- `CodeWords { get; init; } -> int`
- `DataBytes { get; init; } -> int`
- `FromKiln { get; init; } -> bool`
- `Guid { get; init; } -> Guid`
- `Id { get; init; } -> uint`
- `ShutdownWords { get; init; } -> int`
- `StackSize { get; init; } -> int`
- `Streams { get; init; } -> int`
- `Variables { get; init; } -> int`

## ImpactRef — A running Impact program instance: the game's impact id, its program, the entity it runs on and that entity's ImpactHierarchy root (the caster of a player ability...
- `ImpactRef()`
- `ImpactRef(uint ImpactId, uint ProgramId, EntityId Self, EntityId Root)`
- `ImpactId { get; init; } -> uint`
- `ProgramId { get; init; } -> uint`
- `Root { get; init; } -> EntityId`
- `Self { get; init; } -> EntityId`

## ImpactSignalEvent — A program called kiln.Signal(value, tag): Kiln's bridge from Impact programs to mods (fire and forget; posted in the next Pre).
- `AsEntity { get; } -> EntityId`
- `AsFloat { get; } -> float`
- `AsInt { get; } -> int`
- `Impact { get; } -> ImpactRef`
- `Site { get; } -> uint`
- `Step { get; } -> ulong`
- `Tag { get; } -> uint`
- `Value { get; } -> byte[]`
- `ValueTypeHash { get; } -> uint` — The value variable's type hash (0 = a raw u32).

## ImpactSpawnEvent — spawnEntity ran (args: 0 the spawned entity (when connected), 5 template, 6 transform).
- `Position { get; } -> WorldPosition` — Position of the transform argument (32.32 fixed point in the engine).
- `Spawned { get; } -> EntityId`
- `Template { get; } -> Guid`
- `TemplateName { get; } -> string?`

## ImpactStartedEvent — An Impact instance was created and the game copied its config values in (observe only; one step later).
- `Impact { get; } -> ImpactRef`
- `Instance { get; } -> ImpactInstance`
- `Step { get; } -> ulong`
- `TemplateName { get; } -> string?` — The entity's template name (looked up now; null when the entity is gone).

## ImpactStartingEvent — Before an instance's first run (Forge-style "per cast" tweak): the instance exists with the game's config values and has not run yet; handlers change its variables...
- `GetFloat(string key) -> float?`
- `Impact { get; } -> ImpactRef`
- `Instance { get; } -> ImpactInstance`
- `RootIsPlayer { get; } -> bool`
- `Scale(string key, float factor) -> bool` — Multiplies a float variable (e.g. "AttackMod", "NoiseRange").
- `Set(string key, object value) -> bool`
- `SetFloat(string key, float value) -> bool`
- `Step { get; } -> ulong`
- `TemplateName { get; } -> string?`

## ImpactValueType — What a variable holds, from its engine type.
enum: Bytes, UInt32, Int32, Float, Bool, Time, Entity, Float3, Guid, Damage

## ImpactVariable — A program variable.
- `ImpactVariable(int Index, uint NameHash, uint ConfigId, uint Type, int Size, int Offset, string Name)`
- `Alias { get; } -> string` — The name without the "::value" suffix and node prefixes ("NoiseRange", "Damage1").
- `ConfigId { get; init; } -> uint`
- `Index { get; init; } -> int` — Its index (handle 0xFFFF0000 | index).
- `Name { get; init; } -> string` — Debug name, e.g. "NoiseRange::value".
- `NameHash { get; init; } -> uint`
- `Offset { get; init; } -> int`
- `Size { get; init; } -> int`
- `Type { get; init; } -> uint`
- `ValueType { get; } -> ImpactValueType`

## ImpactVariableChangedEvent `0.2.2` — A mod changed a variable of a live Impact instance (`ImpactInstance.Write`, `ImpactInstance.Set` and the typed setters).
- `ImpactId { get; } -> uint` — The instance's id (`ImpactInstance.ImpactId`).
- `Instance { get; } -> ImpactInstance` — The instance, to read the value now.
- `Involves(EntityId entity) -> bool` — Whether the event is about `entity` (the instance's self).
- `Origin { get; } -> string` — The id of the mod that made the change ("" when Kiln made it for no mod).
- `ProgramId { get; } -> uint` — Its program.
- `Self { get; } -> EntityId` — Its self entity.
- `Variable { get; } -> string` — The variable's name.

## ItemUseResult `0.2.13` — What `Abilities.UseItem` did.
enum: Started, NotConsumable, NotOwnSlot, EmptySlot, SlotChanged, NotAPlayer, Dead, NotReady, Busy, ServerOnly

## NativeArg `0.2.4` — One argument of a typed native declaration (`Natives.DeclareTyped`), in ECALL order: argument 0 is the value the program pushes last.
- `NativeArg()`
- `NativeArg(string Type, NativeArgDirection Direction = NativeArgDirection.In, bool Optional = false, bool Raw = false, int Size = 0)`
- `Deconstruct(out string Type, out NativeArgDirection Direction, out bool Optional, out bool Raw, out int Size) -> void`
- `Direction { get; init; } -> NativeArgDirection` — What the native does with it (documentation; every argument is checked alike).
- `Equals(NativeArg other) -> bool`
- `static operator !=(NativeArg left, NativeArg right) -> bool`
- `static operator ==(NativeArg left, NativeArg right) -> bool`
- `Optional { get; init; } -> bool` — The program may leave it not connected (0).
- `Raw { get; init; } -> bool` — It takes a raw integer the program pushes (a literal), not only a variable.
- `Size { get; init; } -> int` — The variable's size in bytes; 0 = the size of `Type` when Kiln knows it, else any.
- `Type { get; init; } -> string` — The impact type name of the variable the program must pass: "sint32", "uint32", "float32", "bool", "Time"...

## NativeArgDirection `0.2.4` — What a native does with an argument (`NativeArg`).
enum: In, Out, InOut

## NativeAttachment `0.2.4` — A handler attached with `Natives.Attach`: its `NativeAttachment.State`; Dispose to detach.
- `Dispose() -> void` — Detaches it: from the next call on the handler is not called.
- `State { get; } -> NativeAttachmentState` — Where it stands now (any thread).

## NativeAttachmentState `0.2.4` — Where an attachment stands (`NativeAttachment.State`).
enum: Gone, Pending, Active, Off

## NativeCall — A synchronous call of a wrapped vanilla native, on the game's job thread inside the sim step.
- `ArgCount { get; } -> int`
- `Bytes(int i) -> Span<byte>` — The bytes of argument i's variable, or empty (not connected, out of range).
- `CallOriginal() -> void` — Runs the game's native now with the current arguments (once).
- `Cancel() -> void` — The native does not run and the program gets 0 (applyDamage: no hit; applyBuff: no buff).
- `Decided { get; } -> bool` — The game's native ran (or a handler returned a value): later handlers see the outcome.
- `GetEntity(int i) -> EntityId`
- `GetFloat(int i) -> float`
- `GetU32(int i) -> uint`
- `ImpactId { get; } -> uint`
- `IsConnected(int i) -> bool`
- `Native { get; } -> uint`
- `ProgramId { get; } -> uint`
- `Raw(int i) -> uint`
- `ResultValue { get; } -> byte`
- `Return(byte value) -> void` — The native does not run; the program gets `value`.
- `Self { get; } -> EntityId`
- `SetFloat(int i, float v) -> bool`
- `SetU32(int i, uint v) -> bool`
- `Site { get; } -> uint`
- `VariableType(int i) -> uint`

## NativeContext `0.2.4` — The call an `ImpactNativeAttribute` method answers (its optional first parameter, `in NativeContext ctx`): which instance, program and call site.
- `ImpactId { get; } -> uint` — The program instance's id.
- `Post(Action action) -> void` — Runs `action` on the tick thread before the next step's Pre handlers, as the native's mod (its...
- `Program { get; } -> uint` — The calling program's id (FNV-1a-32 of its GUID).
- `Root { get; } -> EntityId` — The instance's root entity (whose cast, hit or spawn it belongs to): the root of its `ImpactHierarchy`, read through...
- `Self { get; } -> EntityId` — The instance's own entity.
- `Site { get; } -> uint` — The ECALL's call-site id.
- `TryRead<T>(EntityId entity, out T value) -> bool` — Reads component `T` of another entity (or the instance's own) through impact_simulation's own accessors (its...
- `TryWrite<T>(EntityId entity, in T value) -> bool` — Writes component `T` of another entity through impact_simulation's own other_write accessor with the job's tracker...

## NativeHandler — A managed native: returns the value the program's ECALL pushes (0..255).
- `Invoke(NativeInvocation call) -> byte`

## NativeInvocation — A call of a managed Kiln native (tick thread, the step after the program called it).
- `ArgCount { get; } -> int`
- `GetBool(int i) -> bool`
- `GetBytes(int i) -> byte[]?`
- `GetEntity(int i) -> EntityId`
- `GetFloat(int i) -> float`
- `GetInt(int i) -> int`
- `Impact { get; } -> ImpactRef`
- `Instance { get; } -> ImpactInstance`
- `IsVariable(int i) -> bool`
- `Name { get; } -> string`
- `Raw(int i) -> uint` — Argument i as passed: a handle 0xFFFF0000|index, 0 (not connected) or a raw value.
- `SetBool(int i, bool value) -> bool`
- `SetEntity(int i, EntityId value) -> bool`
- `SetFloat(int i, float value) -> bool`
- `SetInt(int i, int value) -> bool`
- `Site { get; } -> uint` — The ECALL's call-site id (unique per call site; the editor node id).
- `Variable(int i) -> ImpactVariable?` — The variable argument i names (null for a raw value or a variable index out of range).

## NativeParameter `0.2.4` — One parameter of an `ImpactNativeAttribute` native (`NativeRef`): its C# name and its typed declaration.
- `NativeParameter(string Name, NativeArg Arg)`
- `Arg { get; init; } -> NativeArg` — Its type, size, direction and flags.
- `Name { get; init; } -> string` — The parameter's name in the C# method (what Blocks bind by).

## NativeRef `0.2.4` — A mod's own `ImpactNativeAttribute` native, as the build generated it (`Natives.<Name>`): its name, its parameters and, once Kiln registered it (at the first tick...
- `NativeRef(string name, NativeParameter[] parameters)` — Generated code: one per [ImpactNative] method.
- `FullName { get; } -> string?` — "\<modid>.\<Name>" once registered; null before.
- `HashFor(string modId) -> uint` — The hash a program's ECALL names for this native in mod `modId` (FNV-1a-32 of "\<modid>.\<Name>").
- `Name { get; } -> string` — The native's name without the mod id.
- `Parameters { get; } -> IReadOnlyList<NativeParameter>` — The parameters in ECALL order (argument 0 first).
- `Register(NativeSyncHandler thunk) -> NativeResult` — Generated code: registers the native for the mod whose code runs (its typed declaration, then `Natives.RegisterSync`).
- `Registration { get; } -> NativeResult?` — The registration's result; null until Kiln registered it.

## NativeRegistrationRefusedEvent `0.2.4` — A synchronous native that `Natives.RegisterSync` accepted before the server world existed, and that the world's native registry then had no room for (posted in the...
- `Hash { get; } -> uint` — FNV-1a-32 of `NativeRegistrationRefusedEvent.Name`.
- `Name { get; } -> string` — "\<modid>.\<Name>".
- `Reason { get; } -> string` — Why.

## NativeResult `0.2.4` — What `Natives.RegisterSync` did: `NativeResult.Ok` with the native's `NativeResult.Hash`, or the game-state limit that refused it (`NativeResult.Reason`: the native...
- `NativeResult(bool Ok, string? Reason, uint Hash)`
- `Hash { get; init; } -> uint` — FNV-1a-32 of the full name (what a program's ECALL names), also when refused.
- `Ok { get; init; } -> bool` — The native is registered.
- `Reason { get; init; } -> string?` — Why not; null when `Ok`.

## NativeStage `0.2.4` — Where an attached handler runs in a native call (`Natives.Attach`).
enum: Before, Instead, After

## NativeState `0.2.4` — Where a Kiln native stands (`Natives.StateOf`).
enum: None, Waiting, Live, Refused, Dead

## NativeSyncHandler `0.2.4` — A synchronous Kiln native (`Natives.RegisterSync`): runs inside the program's ECALL, on the thread that runs the game's Impact programs, and returns the value the...
- `Invoke(ref NativeCall call) -> byte`

## NativeWrapHandler
- `Invoke(ref NativeCall call) -> void`

## OfferedGem `0.2.7` — One gem an offer listed: its token (Kiln's, unguessable), the gem, its slot and level when offered.
- `OfferedGem(ulong Token, ItemInstanceRef Gem, SlotId Slot, int Level)`
- `Gem { get; init; } -> ItemInstanceRef`
- `Level { get; init; } -> int`
- `Slot { get; init; } -> SlotId`
- `Token { get; init; } -> ulong`

## `Optional<T>` `0.2.4` — An argument of an `ImpactNativeAttribute` method that the program may leave not connected (0): `Optional.HasValue` false then.
- `Optional()` — A connected argument.
- `Optional(T value)` — A connected argument.
- `GetValueOrDefault(T fallback = default(T)) -> T` — The value, or `fallback` when the argument is not connected.
- `HasValue { get; } -> bool` — The program connected the argument.
- `Value { get; } -> T` — The value.

## RawAttribute `0.2.4` — On a `uint` parameter of an `ImpactNativeAttribute` method: the program may pass a raw integer (a literal) as well as a variable.

## SequenceEndedEvent — An actor's sequence ended (replaced by none or by another sequence).
- `Actor { get; } -> EntityId`
- `Duration { get; } -> TimeSpan` — Wall time it was seen running.
- `Sequence { get; } -> uint`

## SequenceOptions — How `Abilities.PlaySequence` starts a sequence.
- `AbortCurrent { get; init; } -> bool` — Abort the actor's current sequence (trigger ExternalAbortAndOverride, as fall damage does); false = ExternalTrigger...
- `static readonly Default -> SequenceOptions` — Abort what the actor is doing (ExternalAbortAndOverride), hold the keep-alive states.
- `HoldInputs { get; init; } -> bool` — Hold the states the sequence's KeepAlive and Loop events wait for (InRangeCombat, InRangeCombatShooting, ...: what a...
- `static readonly Queue -> SequenceOptions` — ExternalTrigger (the trigger the game's emotes use), hold the keep-alive states.

## SequenceStartedEvent — An actor started an action sequence (attack, spell cast, ability, hit reaction ...): its `Actor.sequenceRuntimeId` changed.
- `Actor { get; } -> EntityId`
- `CallIndex { get; } -> int`
- `Sequence { get; } -> uint` — FNV-1a of the ActorSequenceResource GUID (`Abilities.IdOf`).
- `TriggerTime { get; } -> long`

## SocketedGem `0.2.5` — A gem in a socket: the carrier item, the gem item and its level.
- `SocketedGem()`
- `SocketedGem(ItemInstanceRef Carrier, ItemId Gem, int Level)`
- `Carrier { get; init; } -> ItemInstanceRef`
- `Gem { get; init; } -> ItemId`
- `Level { get; init; } -> int`

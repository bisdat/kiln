# Creatures (Kiln.Creatures)

## Act — Actions.
- `static Attack(Guid attack) -> RuleAction` — Starts the attack with this GUID.
- `static Attack(string name) -> RuleAction` — Starts the calling mod's attack `name` through the game's own selection, past its style, range and constraints.
- `static Custom(Action<NpcView> run) -> RuleAction` — Your own action: queued when the rule fires, run on the tick thread in the next step with the full API.
- `static Game(string type, params (string Field, object? Value)[] fields) -> RuleAction` — Any of the game's action kinds, by type name (`"SetCombatStateAction"`) and field values.
- `static Job(uint id, uint arg = 0) -> RuleAction` — A handler on the game's job thread, registered with `Arsenals.JobHandler`.
- `static Named(string name) -> RuleAction` — An action registered by name with `Arsenals.Action`.
- `static SetPhase(Guid phase) -> RuleAction` — Sets the creature's phase by GUID.
- `static SetPhase(string name) -> RuleAction` — Sets the creature's phase (see `Is.InPhase`).

## AiAccess — Private template settings of one creature: field paths of a template component, by the game's reflection ("aggroSetup.viewRange"...
- `Get(string path) -> string` — The field as this entity reads it now (its copy, else the shared template).
- `Get(string path, out bool isPrivate) -> string` — The field as this entity reads it now, and whether it comes from this entity's private copy.
- `GetNumber(string path) -> double` — The field as a number: `AiAccess.Get` parsed with the invariant culture.
- `Override(Action<AiAccess> edit) -> void` — Several fields at once.
- `Reset(string? pathOrComponent = null) -> int` — Drops the calling mod's AI overrides (one path, one component, or all).
- `Set(string path, bool value) -> void` — Sets a boolean field (as `AiAccess.Set` does, with "true" or "false").
- `Set(string path, double value) -> void` — Sets a numeric field (as `AiAccess.Set` does, with the number written as text).
- `Set(string path, string value) -> void` — Sets a field (text: numbers, true/false, enum names, flags "A|B" or "+A -B", GUIDs, seconds for keen::Time).

## ArsenalAttack — One attack of an arsenal.
- `Abort { get; init; } -> List<RuleCondition>?` — Ends it when one holds.
- `Chance { get; init; } -> double?` — The attack's `selectionChance`: the game rolls under it when it selects an attack.
- `Execution { get; init; } -> List<RuleCondition>?` — Must hold for it to start once picked.
- `Fields { get; init; } -> Dictionary<string, object?>?` — Any other field of `AttackDesscription`, by the game's name (speed, cooldownGroupID, ...).
- `Flags { get; init; } -> string[]?` — The attack's `flags` as the game's names.
- `Id { get; init; } -> Guid?` — The GUID when the attack needs a given one; else it follows from `ArsenalAttack.Name`.
- `MaxRange { get; init; } -> double?` — The longest distance to the target at which the attack is available (`maxRange`).
- `MinRange { get; init; } -> double?` — The shortest distance to the target at which the attack is available (`minRange`).
- `Modify { get; init; } -> Guid?` — Change the base arsenal's attack with this GUID instead of adding one.
- `ModifyIndex { get; init; } -> int?` — Change the base arsenal's attack with this index instead of adding one.
- `Name { get; init; } -> string?` — Its name in this mod; `Act.Attack` and `AttackControl.Force` take it.
- `Selection { get; init; } -> List<RuleCondition>?` — Must hold for the game to pick it.
- `Sequence { get; init; } -> Guid?` — The `ActorSequenceResource` it plays.
- `Style { get; init; } -> string[]?` — When the game considers it (`keen::enemy01::AttackStyle`): Normal, Retaliate, CounterAttack, StunBreak, Special...
- `Threat { get; init; } -> double?` — The attack's `threat` value.

## ArsenalHandle — A mod's additions to one arsenal.
- `Dispose() -> void` — Takes this mod's additions out; the game's arsenal comes back when nothing else adds to it.
- `Id { get; } -> Guid` — The arsenal's GUID: set it on a creature with `CreatureRef.Arsenal` or a variant's AI values.

## ArsenalInfo — An arsenal as the AI reads it now.
- `ArsenalInfo(Guid Id, bool IsKiln, ulong Address, IReadOnlyList<ArsenalInfo.Attack> Attacks, IReadOnlyList<ArsenalInfo.Rule> Rules, IReadOnlyList<string> Problems)`
- `Address { get; init; } -> ulong` — The address of the arsenal record in memory, for diagnostics.
- `Attacks { get; init; } -> IReadOnlyList<ArsenalInfo.Attack>` — Its attacks, in index order.
- `Id { get; init; } -> Guid` — The arsenal's GUID.
- `IsKiln { get; init; } -> bool` — Kiln's additions are installed in this arsenal (from `Arsenals.Define` or `Arsenals.Extend`), not only the game's...
- `Problems { get; init; } -> IReadOnlyList<string>` — What went wrong writing it, such as a limit of the base arsenal (32 attacks, 32 rules) that left some additions out.
- `Rules { get; init; } -> IReadOnlyList<ArsenalInfo.Rule>` — Its rules, in index order.

## ArsenalInfo.Attack — One attack of an arsenal, as the AI reads it.
- `Attack(int Index, Guid Id, Guid Sequence, int Style)`
- `Id { get; init; } -> Guid` — The attack's GUID.
- `Index { get; init; } -> int` — The attack's index in the arsenal (`AttackControl.Force` takes it).
- `Sequence { get; init; } -> Guid` — The GUID of the sequence it plays.
- `Style { get; init; } -> int` — The attack's style mask: bits of the game's `keen::enemy01::AttackStyle`.

## ArsenalInfo.Rule — One rule of an arsenal, as the AI reads it.
- `Rule(int Index, Guid Id, int Mode, int When, int Actions)`
- `Actions { get; init; } -> int` — How many actions the rule has.
- `Id { get; init; } -> Guid` — The rule's GUID (all zero when it has none).
- `Index { get; init; } -> int` — The rule's index in the arsenal.
- `Mode { get; init; } -> int` — The rule's mode: a `RuleMode` value.
- `When { get; init; } -> int` — How many conditions the rule has.

## ArsenalRule — One rule: when, if, then.
- `AbortIf { get; init; } -> List<RuleCondition>?` — Conditions that abort the rule when one holds.
- `Chance { get; init; } -> double` — Chance it fires when its conditions hold, 0 to 1.
- `Cooldown { get; init; } -> double` — Seconds a rule rests after it fired.
- `Do { get; init; } -> List<RuleAction>` — What the rule does when it fires, in order.
- `Every { get; init; } -> double` — Seconds between checks of an OnUpdate rule, at least.
- `EveryMax { get; init; } -> double?` — Seconds between checks, at most.
- `If { get; init; } -> List<RuleCondition>` — Conditions that must all hold for the rule to fire.
- `Modify { get; init; } -> Guid?` — Change the base arsenal's rule with this GUID instead of adding one.
- `ModifyIndex { get; init; } -> int?` — Change the base arsenal's rule with this index instead of adding one.
- `Name { get; init; } -> string?` — Its name in this mod.
- `When { get; init; } -> RuleMode` — When the game looks at the rule.

## ArsenalRulesStatus — What the arsenal rule system has resolved and done so far (`Arsenals.RulesState`).
- `ArsenalRulesStatus(bool Writer, bool Dispatcher, bool Attacks, bool AiSwitch, bool RuleHooks, bool AttackHooks, int Arsenals, int Tracked, int Overrides, int AiOff, int MapCapacity, int MapCount, ulong Constraints, ulong ConstraintsTrue, ulong Actions, ulong ActionsDropped, ulong Selects, ulong SelectsForced, ulong SelectsFiltered, ulong PicksWaiting, ulong FollowupsRefused, ulong Starts, ulong StartsOffOverride, ulong SequencesRefused, ulong PreNs, ulong PreCalls, ulong JobTicks, ulong JobCalls, ulong JobTickHz)`
- `Actions { get; init; } -> ulong` — Kiln actions fired.
- `ActionsDropped { get; init; } -> ulong` — Kiln actions lost to a full queue.
- `AiOff { get; init; } -> int` — Creatures whose AI Kiln switched off (0 off the tick).
- `AiSwitch { get; init; } -> bool` — The AI switch's field resolved.
- `Arsenals { get; init; } -> int` — Kiln's arsenals in the registry.
- `AttackHooks { get; init; } -> bool` — The attack hooks are installed now.
- `Attacks { get; init; } -> bool` — The attack override's targets resolved.
- `Constraints { get; init; } -> ulong` — Kiln constraints evaluated on the job threads.
- `ConstraintsTrue { get; init; } -> ulong` — Of those, the ones that were true.
- `Dispatcher { get; init; } -> bool` — The targets of the two rule hooks resolved.
- `FollowupsRefused { get; init; } -> ulong` — Combo follow-ups an override refused.
- `JobCalls { get; init; } -> ulong` — The job-thread side's calls so far.
- `JobTickHz { get; init; } -> ulong` — The counter's frequency: `JobTicks` / `JobTickHz` is seconds.
- `JobTicks { get; init; } -> ulong` — The job-thread side's cost so far, in counter ticks (rdtsc, while `Arsenals.Timing` is on).
- `MapCapacity { get; init; } -> int` — Capacity of the registry's lookup map.
- `MapCount { get; init; } -> int` — Entries in the registry's lookup map.
- `Overrides { get; init; } -> int` — Creatures with an attack override (0 off the tick).
- `PicksWaiting { get; init; } -> ulong` — Selections that waited for an override.
- `PreCalls { get; init; } -> ulong` — The tick side's calls so far.
- `PreNs { get; init; } -> ulong` — The tick side's cost so far, in nanoseconds.
- `RuleHooks { get; init; } -> bool` — The rule hooks are installed now.
- `Selects { get; init; } -> ulong` — The game's attack selections seen.
- `SelectsFiltered { get; init; } -> ulong` — Attacks an allow mask refused.
- `SelectsForced { get; init; } -> ulong` — Selections an override forced.
- `SequencesRefused { get; init; } -> ulong` — Attack sequences played by a rule that were refused.
- `Starts { get; init; } -> ulong` — Attacks started.
- `StartsOffOverride { get; init; } -> ulong` — Attacks started against an override (rewritten).
- `Tracked { get; init; } -> int` — Creatures in the published table (0 off the tick).
- `Writer { get; init; } -> bool` — Arsenals can be written (the registry map and the record layout were read).

## Arsenals — Arsenals as data, and rules with Kiln's own kinds.
- `static Action(string name, Action<NpcView> run) -> void` — Registers an action by name for rules that name it (`Act.Named`, `{"custom": name}` in a data pack).
- `static Available { get; } -> bool` — True when arsenals can be written on this build.
- `static Condition(string name, Func<NpcView, bool> check, double every = 1) -> void` — Registers a condition by name for rules that name it (`Is.Named`, `{"custom": name}` in a data pack).
- `static ConditionStats(RuleCondition condition) -> (ulong Evaluated, ulong True)` — How many times a condition slot was evaluated, and came out true.
- `static Define(string name, ArsenalSpec spec) -> ArsenalHandle` — A new arsenal of the calling mod: GUID `UUIDv5(<mod>:arsenal:<name>)`.
- `static Extend(Guid arsenal, ArsenalSpec spec) -> ArsenalHandle` — Adds to an arsenal the game has, for every creature that uses it, by its GUID.
- `static Extend(string template, ArsenalSpec spec) -> ArsenalHandle` — Adds to the arsenal a template names.
- `static Info(Guid id) -> ArsenalInfo?` — The arsenal with this GUID as the AI reads it now, or null.
- `static JobHandler(uint id, nint function, nint user = 0) -> void` — Registers a handler for `Is.Job` and `Act.Job`, id 1 to 63: an `[UnmanagedCallersOnly]` function...
- `static OfTemplate(string template) -> Guid?` — The arsenal a template names, or null.
- `static PumpCost { get; } -> (double Milliseconds, long Pumps)` — What the tick side of Kiln's kinds has cost so far: this mod's and other mods' conditions, actions and picks, in the...
- `static Refresh() -> int` — Rebuilds Kiln's arsenals from the game's current data; Kiln.Creatures does this after a content change.
- `static RulesState { get; } -> ArsenalRulesStatus?` — What the arsenal rule system resolved and what ran; null without kiln.creatures.
- `static SelfTest() -> (int Checked, int Bad)` — Reads every arsenal of the game and writes it back through the writer in memory: how many, and how many differ.
- `static Status { get; } -> RulesStatus?` — What resolved and what ran; null without kiln.creatures.
- `static Timing { set; }` — Times the job-thread side (`ArsenalRulesStatus.JobTicks`); a few nanoseconds per Kiln kind while on.

## ArsenalSpec — What to write into an arsenal.
- `Attacks { get; init; } -> List<ArsenalAttack>` — The attacks to write: new ones, or changes to the base arsenal's.
- `From { get; init; } -> Guid?` — A new arsenal starts with this one's attacks and rules (the low-level way; a block does not name one).
- `PickAttack { get; init; } -> Func<NpcView, string?>?` — Decides the attack for each creature, on the tick thread, every `ArsenalSpec.PickEvery` seconds: an attack name or...
- `PickEvery { get; init; } -> double` — Seconds between the calls of `ArsenalSpec.PickAttack` and `ArsenalSpec.PickTarget` for a creature.
- `PickTarget { get; init; } -> Func<NpcView, EntityId>?` — Decides the combat target for each creature, on the tick thread, every `ArsenalSpec.PickEvery` seconds; default for...
- `Rules { get; init; } -> List<ArsenalRule>` — The rules to write: new ones, or changes to the base arsenal's.

## AttackControl — Overrides one creature's attack choice.
- `Allow(params int[] indexes) -> void` — The game picks with its own rules, among these indexes only (0 to 31).
- `Clear() -> void` — Drops this mod's override.
- `Force(Guid attack) -> void` — Forces an attack by GUID, in the arsenal the creature reads now.
- `Force(int index) -> void` — Whenever the creature attacks, it starts this one, past its style, range, cooldown and constraints.
- `Force(string name) -> void` — Forces the calling mod's attack by name, in the arsenal the creature reads now.
- `Natural() -> void` — The game chooses as it does without an override, and an arsenal with `ArsenalSpec.PickAttack` stops waiting.

## CreatureAdminCommands — The `/creature` command tree (admins only; permission node `kiln.command.creature`).
- `static Register() -> CommandRegistration` — Asks for `/creature` on behalf of the calling mod.

## CreatureChangedEvent `0.2.2` — A mod changed one creature through Kiln.Creatures: a `CreatureRef` value (level, faction, scale, home, a resistance, loot, features), its AI switch, phase or held...
- `Entity { get; } -> EntityId` — The creature.
- `Origin { get; } -> string` — The id of the mod that made the change ("" outside mod code).
- `What { get; } -> string` — What was changed: the value's name as `CreatureRef` has it (`Level`, `Faction`, `Home`, ...), `AiEnabled`, `Phase`...

## CreatureCommands — Commands for one creature, sent as the game's own events in the next sim step.
- `Attack(EntityId target, float threat = 1000) -> void` — "Attack that": adds threat on the target (AddThreatTargetEvent; the threat system picks it).
- `ControlTarget(EntityId target) -> void` — A player-controlled combat target (RegisterControlledCombatTargetEvent, as companions get).
- `Guard(EntityId target) -> void` — Guard (follow and protect) an entity, a player for companions: the game's StartGuardingEvent (its Guard state walks...
- `Guard(EntityId target, GuardOptions options) -> void` — Guard with explicit options (`GuardOptions.EventOnly` = only the game's event).
- `Heal(float seconds = 3) -> void` — Restore health over `seconds` (RestoreEnemyHealthEvent).
- `MoveTo(WorldPosition position) -> void` — Move toward a position: its home zone moves there (roaming / return home walk it over).
- `Stay() -> void` — Stay here: stops guarding whom it guards and sets its home to where it stands.
- `StopGuarding(EntityId target) -> void` — Stop guarding (StopGuardingEvent, sent again until the game cleared it); ends Kiln's guard order (its home goes back).

## CreatureFlags — Template flags from the catalog.
enum: None, Boss, Phases, Saved, GuardsPlayer, Ally, Weakspot, BossHealthBar, Catalog, Loaded, Runtime

## CreatureInfo — A snapshot of one creature (`CreatureRef.Inspect`).
- `CreatureInfo(EntityId Entity, Guid Template, string TemplateName, CreatureKind Kind, bool HasAi, EnemyState State, byte AwarenessState, float Awareness, Faction Faction, int Level, int LevelMax, int Health, int HealthMax, float Scale, float XpFactor, float SpeedFactor, float DamageMultiplier, EnemyFeatures TemplateFeatures, EnemyFeatures EnabledFeatures, EnemyFeatures DisabledFeatures, EntityId Target, EntityId Guard, WorldPosition Home, Guid LootLabels, int LootSlotsMin, int LootSlotsMax, bool LootDisabled, bool Dead, bool Ready, bool ForcedReady, int Layers, int PrivateCopies)`
- `Awareness { get; init; } -> float` — The creature's awareness value (the `awareness` of its `NetworkEnemy` state).
- `AwarenessState { get; init; } -> byte` — The game's awareness level of the creature (the `awarenessState` of its `NetworkEnemy` state, as a raw byte).
- `DamageMultiplier { get; init; } -> float` — The multiplier of its damage (`DamageMod_Multiplier`, `CreatureRef.DamageFactor`).
- `Dead { get; init; } -> bool` — The actor's dead bit is set.
- `DisabledFeatures { get; init; } -> EnemyFeatures` — Features switched off for this one entity.
- `EnabledFeatures { get; init; } -> EnemyFeatures` — Features switched on for this one entity, on top of the template's.
- `Entity { get; init; } -> EntityId` — The creature's entity.
- `Faction { get; init; } -> Faction` — Its faction now (`Faction.None` when it has none).
- `ForcedReady { get; init; } -> bool` — The lab readiness switch holds this entity ready (`CreatureRef.ForceReady`, a lab-only switch).
- `Guard { get; init; } -> EntityId` — The entity it guards (`guardEntityId`); `EntityId.None` when it guards none.
- `HasAi { get; init; } -> bool` — The entity has enemy AI state.
- `Health { get; init; } -> int` — Its health now.
- `HealthMax { get; init; } -> int` — Its maximum health.
- `Home { get; init; } -> WorldPosition` — Its home zone position: where it roams around and returns to.
- `Kind { get; init; } -> CreatureKind` — The kind of its template (`CreatureKind.None` when unknown).
- `Layers { get; init; } -> int` — How many values mods changed on this entity.
- `Level { get; init; } -> int` — Its level now.
- `LevelMax { get; init; } -> int` — The maximum level it can have.
- `LootDisabled { get; init; } -> bool` — Random loot is switched off.
- `LootLabels { get; init; } -> Guid` — The GUID of the loot label collection its drops are rolled from.
- `LootSlotsMax { get; init; } -> int` — How many loot slots are rolled at most.
- `LootSlotsMin { get; init; } -> int` — How many loot slots are rolled at least.
- `PrivateCopies { get; init; } -> int` — How many private template copies the entity reads (`CreatureRef.Ai`).
- `Ready { get; init; } -> bool` — The actor is ready for gameplay: the game sets this near players.
- `Scale { get; init; } -> float` — Its uniform scale (`CreatureRef.Scale`).
- `SpeedFactor { get; init; } -> float` — The factor on its movement speeds (`CreatureRef.SpeedFactor`).
- `State { get; init; } -> EnemyState` — The enemy AI's state (`EnemyState.Invalid` without enemy AI).
- `Target { get; init; } -> EntityId` — Its combat target (`SelectedTargets[0]`); `EntityId.None` when it has none.
- `Template { get; init; } -> Guid` — The GUID of its template.
- `TemplateFeatures { get; init; } -> EnemyFeatures` — The enemy AI features its template has.
- `TemplateName { get; init; } -> string` — The template's name ("" when Kiln does not know it).
- `XpFactor { get; init; } -> float` — The XP factor of a kill (`CreatureRef.XpFactor`).

## CreatureKind — What a creature template is (by the AI program its entities run).
enum: None, Enemy, Wildlife, Animal, Villager

## CreatureRef — One creature or NPC.
- `Ai { get; } -> AiAccess` — Private template settings of this one creature (Enemy01BehaviorState by default).
- `AiEnabled { get; set; } -> bool` — The game's AI switch for this creature, as a layer of the calling mod: off while any mod wants it off, back on when...
- `Arsenal { get; set; } -> Guid` — The arsenal (attacks and behaviour rules) its enemy AI uses.
- `Attacks { get; } -> AttackControl` — Overrides the creature's attack choice, on every path the game starts an attack by.
- `Command { get; } -> CreatureCommands` — Commands sent as the game's own events (applied in the next sim step).
- `DamageFactor { get; set; } -> float` — Damage multiplier (DamageMod_Multiplier; DamageMod = base * multiplier + adder).
- `Entity { get; } -> EntityId` — The creature's entity.
- `Exists { get; } -> bool` — The entity exists now.
- `Faction { get; set; } -> Faction` — Faction: who it fights.
- `Features { get; } -> CreatureRef.FeatureAccess` — Enemy AI feature switches (per entity, on top of the template's).
- `Home { get; set; } -> WorldPosition` — Where it roams around and returns to (the AI's home zone centre).
- `Inspect() -> CreatureInfo?` — A full snapshot, or null when the entity does not exist.
- `Level { get; set; } -> int` — Level (1..its level max).
- `Loot { get; } -> CreatureRef.LootAccess` — Loot labels, slots, and the random-loot switch.
- `Phase { get; } -> uint` — The creature's phase as a rule's `Is.InPhase` sees it: FNV-1a of the phase's GUID, 0 for none.
- `PreferTarget(EntityId target, double seconds = 5, float threat = 5000) -> void` — Holds `target` as the creature's combat target for `seconds`, then the game chooses again: it sends the game's...
- `ReleaseTarget() -> void` — Lets go of a target held with `CreatureRef.PreferTarget`: the game chooses again.
- `Reset() -> int` — Drops everything the calling mod changed on this creature (values, AI overrides, its attack override, AI switch and...
- `Resistance { get; } -> CreatureRef.ResistanceAccess` — Damage susceptibility per kind (1 = normal, 0 = immune, 2 = double).
- `Scale { get; set; } -> float` — Uniform scale (ScaleComponent and the transform's scale).
- `SetPhase(Guid phase) -> void` — Sets the phase by GUID.
- `SetPhase(string name) -> void` — Sets the phase by name (see `Is.InPhase`).
- `SpeedFactor { get; set; } -> float` — Factor on all movement speeds against the values before the first change (1 = unchanged).
- `State { get; } -> EnemyState` — The enemy AI state (Invalid without enemy AI).
- `Target { get; } -> EntityId` — The AI's current target (SelectedTargets[0]); None when it has none.
- `Template { get; } -> CreatureTemplate?` — Its catalog template (null for non-creatures).
- `XpFactor { get; set; } -> float` — XP factor of a kill (ExperienceSource.xpMod; 1 = the template's).

## CreatureRef.FeatureAccess — The enemy AI feature switches of one creature (`CreatureRef.Features`).
- `Disable(EnemyFeatures f) -> void` — Switches features off for this entity (e.g. FleeHome | ReturnHome: never runs home).
- `Disabled { get; } -> EnemyFeatures` — The features switched off for this entity, against its template (see `FeatureAccess.Enabled`).
- `Enable(EnemyFeatures f) -> void` — Switches features on for this entity (the calling mod's layer; layers of several mods combine).
- `Enabled { get; } -> EnemyFeatures` — The features this entity has on top of / against its template (per-entity masks).
- `Reset() -> void` — Drops the calling mod's feature changes.

## CreatureRef.LootAccess — The random loot of one creature (`CreatureRef.Loot`).
- `Disabled { get; set; } -> bool` — No random loot at all.
- `Labels { get; set; } -> Guid` — RandomLoot.defaultLabels: the loot label collection its drops are rolled from.
- `Slots { get; set; } -> (int Min, int Max)` — How many loot slots are rolled (min, max).

## CreatureRef.ResistanceAccess — Damage susceptibility of one creature, per `DamageKind` (`CreatureRef.Resistance`).
- `Reset(DamageKind kind) -> void` — Drops the calling mod's change of one kind.
- `this[DamageKind kind] { get; set; } -> float` — The creature's susceptibility to this kind of damage: 1 = normal damage taken, 0 = immune, 2 = double.

## Creatures — What one creature or NPC is and does at runtime, server-authoritative: the creature catalog, per-creature values (level, faction, scale, resistances, speed, damage...
- `static AiFields(string component = "Enemy01BehaviorState") -> string` — The JSON field list of a template component ("Enemy01BehaviorState", "Aggro", "EnemyScaling", ...) from the game's...
- `static ApplyJson(string sectionJson, string owner, string source = "") -> CreaturesPackReport` — Applies a "creatures" section...
- `static Available { get; } -> bool` — True when kiln.creatures is loaded on this build (capability `kiln.creatures`).
- `static GuardOrders { get; } -> IReadOnlyList<GuardOrder>` — The guard orders Kiln keeps now (guards commanded with `CreatureCommands.Guard`).
- `static Near(WorldPosition center, double radius, CreatureKind? kind = null) -> IReadOnlyList<EntityId>` — Creatures (enemy AI, villagers and animals of the catalog) within `radius` metres of `center`, nearest first.
- `static Of(EntityId entity) -> CreatureRef` — A handle on one creature (any entity id; check `CreatureRef.Exists`).
- `static RefreshCopies(EntityId entity = default(EntityId)) -> int` — Rebuilds private AI copies from the shared template data (all entities, or one) and re-applies the overrides...
- `static RevertJson(string owner, string? source = null) -> int` — Removes what `owner` added (one source, or all of them; all also reverts the changes its variants made to live...
- `static Spawn(string variantOrTemplate, WorldPosition position, float yawDegrees = 0, CreatureSpawnOptions? options = null) -> Task<EntityId>` — Spawns a variant (by id) or a plain template (by name/GUID) and applies the variant's overrides in the same step...
- `static Status { get; } -> CreaturesStatus?` — What works on this build (null when unavailable).
- `static Template(Guid id) -> CreatureTemplate?` — A catalog template by GUID; null if it is not a creature/NPC template.
- `static Template(string nameOrGuid) -> CreatureTemplate?` — A catalog template by name ("Enemy_Vukah_Melee"), display name ("Vukah") or GUID text; null if none.
- `static Templates { get; } -> IReadOnlyList<CreatureTemplate>` — Every creature/NPC template Kiln knows (enemy AI, wildlife, animals, villagers), sorted by name.

## CreaturesPackReport — What `Creatures.ApplyJson` did.
- `CreaturesPackReport(int Applied, IReadOnlyList<string> Errors, IReadOnlyList<string> Warnings)`
- `Applied { get; init; } -> int` — How many variants, arsenals and spawn rules the section added.
- `Errors { get; init; } -> IReadOnlyList<string>` — What the section could not add, one message per entry (or one for a section that is not valid JSON).
- `Ok { get; } -> bool` — The section applied without errors (`CreaturesPackReport.Errors` is empty).
- `Warnings { get; init; } -> IReadOnlyList<string>` — Things the section had that were ignored, such as an unknown key.

## CreatureSpawnOptions — Options for `Creatures.Spawn`.
- `DespawnOnUnload { get; init; } -> bool` — Destroy the creature when the spawning mod unloads (default true; false leaves it, with the variant's changes reverted).
- `Flags { get; init; } -> SpawnFlags` — Spawn behaviour switches, passed to the spawn (`SpawnOptions.Flags`).
- `Tag { get; init; } -> string?` — A label stored with the tracked spawn record (`SpawnOptions.Tag`), so cleanup can select by it.

## CreatureSpawnRule — A natural-spawn rule: creatures of `CreatureSpawnRule.Template` that random spawners create become `CreatureSpawnRule.Variant` with `CreatureSpawnRule.Chance`.
- `CreatureSpawnRule(string Template, string Variant, double Chance = 1, bool NightOnly = false, bool DayOnly = false)`
- `Chance { get; init; } -> double` — The chance of the replacement, 0 to 1 (a rule outside that range is refused).
- `DayOnly { get; init; } -> bool` — The rule applies by day only.
- `NightOnly { get; init; } -> bool` — The rule applies at night only.
- `Template { get; init; } -> string` — The template the spawners create: its name, display name or GUID text.
- `Variant { get; init; } -> string` — The id of the registered variant that replaces it.

## CreaturesStatus — What kiln.creatures can do on this build (`Creatures.Status`).
- `CreaturesStatus(bool Slots, bool AiHooks, bool ReadyBit, bool Arsenals, bool Reflection, bool Commands, bool Marks, bool LabReady, int Catalog, int CatalogLoaded, int EngineCreaturesMissing, int ArsenalCount, int Entities, int Layers, int PrivateCopies, int PendingFree, ulong StateChanges, ulong EventsDelivered, ulong EventsDropped, ulong CommandsSent, ulong MarksWritten, ulong CopyReads)`
- `AiHooks { get; init; } -> bool` — The enemy AI state hooks are installed.
- `ArsenalCount { get; init; } -> int` — How many arsenals the registry has.
- `Arsenals { get; init; } -> bool` — The arsenal registry is readable.
- `Catalog { get; init; } -> int` — How many templates the catalog has.
- `CatalogLoaded { get; init; } -> int` — Of those, how many the running game has loaded.
- `Commands { get; init; } -> bool` — The command program is active.
- `CommandsSent { get; init; } -> ulong` — How many commands were sent.
- `CopyReads { get; init; } -> ulong` — How many behaviour updates read a private `Enemy01BehaviorState` copy (a diagnostic counter).
- `EngineCreaturesMissing { get; init; } -> int` — Engine templates with enemy AI or villager state that the shipped catalog lacks (clones).
- `Entities { get; init; } -> int` — How many entities have value layers or private copies.
- `EventsDelivered { get; init; } -> ulong` — How many creature events were delivered.
- `EventsDropped { get; init; } -> ulong` — How many creature events were lost to a ring overflow.
- `LabReady { get; init; } -> bool` — The lab readiness switch file exists (`kiln/data/kiln.creatures/force-ready`).
- `Layers { get; init; } -> int` — How many value layers are active.
- `Marks { get; init; } -> bool` — The replication marks program is active.
- `MarksWritten { get; init; } -> ulong` — How many components the marks program marked for replication.
- `PendingFree { get; init; } -> int` — How many private copies wait to be freed.
- `PrivateCopies { get; init; } -> int` — How many private template copies are live.
- `ReadyBit { get; init; } -> bool` — The actor readiness bit resolved.
- `Reflection { get; init; } -> bool` — Field paths resolve through the game's reflection.
- `Slots { get; init; } -> bool` — Private template copies work (the template slot layout resolved).
- `StateChanges { get; init; } -> ulong` — How many enemy AI state changes Kiln saw (only while someone listens).

## CreatureStateEvent — The enemy AI changed state (enemy01 SetState): `CreatureStateEvent.From` to `CreatureStateEvent.To`, because of `CreatureStateEvent.Condition`.
- `Condition { get; } -> StateCondition` — The StateCondition HandleCondition was given (`StateCondition.Unknown` if none).
- `Entity { get; } -> EntityId` — The creature whose AI changed state.
- `From { get; } -> EnemyState` — The state it left.
- `Step { get; } -> ulong` — The server-world step it happened in.
- `Template { get; } -> CreatureTemplate?` — Its template (null when the entity is gone already).
- `To { get; } -> EnemyState` — The state it entered.

## CreatureTargetEvent — An active enemy AI's target changed (SelectedTargets[0]): `CreatureTargetEvent.Target` is the new one (None = lost), `CreatureTargetEvent.Previous` the old one.
- `Acquired { get; } -> bool` — The creature had no target and has one now.
- `Entity { get; } -> EntityId` — The creature whose target changed.
- `Lost { get; } -> bool` — The creature has no target now (`CreatureTargetEvent.Target` is none).
- `Previous { get; } -> EntityId` — The target it had before (`EntityId.None` = it had none).
- `State { get; } -> EnemyState` — The last AI state Kiln saw for the creature.
- `Step { get; } -> ulong` — The server-world step the change was seen in.
- `Target { get; } -> EntityId` — The new target (`EntityId.None` = it lost its target).

## CreatureTemplate — One creature/NPC template of the catalog.
- `CreatureTemplate(Guid Id, string Name, string DisplayName, CreatureKind Kind, CreatureFlags Flags, Faction Faction, int Level, int LevelMax, int EnemyClass, Guid Arsenal, Guid LootLabels, int LootSlotsMin, int LootSlotsMax, float XpMod, float Scale, Guid Model, Guid AnimationGraph)`
- `AnimationGraph { get; init; } -> Guid` — The animation graph's GUID.
- `Arsenal { get; init; } -> Guid` — The GUID of the arsenal (attacks and rules) its enemy AI uses; all zero when it has none.
- `ClassName { get; } -> string` — "Melee", "Ranger", "Mage" or "".
- `DisplayName { get; init; } -> string` — The in-game name when the shipped catalog has it, else "".
- `EnemyClass { get; init; } -> int` — 0 Melee, 1 Ranger, 2 Mage; -1 when the template is not an enemy.
- `Faction { get; init; } -> Faction` — The template's faction (`Faction.None` when it has none).
- `Flags { get; init; } -> CreatureFlags` — What the catalog knows about the template (boss, phases, saved, loaded, ...).
- `Id { get; init; } -> Guid` — The template's GUID.
- `IsBoss { get; } -> bool` — The template is a boss (`CreatureFlags.Boss`).
- `IsLoaded { get; } -> bool` — The running game has the template (`CreatureFlags.Loaded`).
- `IsRuntime { get; } -> bool` — Found in the engine only (kiln.content clones): `CreatureFlags.Runtime`.
- `IsSaved { get; } -> bool` — Entities of the template can reach the save (`CreatureFlags.Saved`).
- `IsVillager { get; } -> bool` — The template is a villager or workshop NPC (`CreatureKind.Villager`).
- `Kind { get; init; } -> CreatureKind` — What AI program the template's entities run.
- `Level { get; init; } -> int` — The template's own level: its `Level` attribute.
- `LevelMax { get; init; } -> int` — The template's maximum level.
- `LootLabels { get; init; } -> Guid` — The GUID of the loot label collection its drops are rolled from (`RandomLoot.defaultLabels`).
- `LootSlotsMax { get; init; } -> int` — How many loot slots are rolled at most.
- `LootSlotsMin { get; init; } -> int` — How many loot slots are rolled at least.
- `Model { get; init; } -> Guid` — The model resource's GUID.
- `Name { get; init; } -> string` — The template name (`Enemy_Vukah_Melee` style).
- `Scale { get; init; } -> float` — The template's uniform scale (1 when it has none).
- `XpMod { get; init; } -> float` — The XP factor of a kill (`ExperienceSource.xpMod`).

## CreatureVariant — A creature variant: a base template (a vanilla template or a kiln.content clone) and the overrides Kiln applies to each of its creatures.
- `Ai { get; init; } -> Dictionary<string, string>` — Private AI settings: reflection path (Enemy01BehaviorState by default, or "Component.path") to value text, e.g....
- `Base { get; init; } -> string` — The base template: name, display name or GUID text.
- `DamageFactor { get; init; } -> float?` — Damage multiplier (`CreatureRef.DamageFactor`); null keeps the base template's.
- `DisableFeatures { get; init; } -> EnemyFeatures` — Enemy AI features to switch off for each creature (`FeatureAccess.Disable`).
- `EnableFeatures { get; init; } -> EnemyFeatures` — Enemy AI features to switch on for each creature (`FeatureAccess.Enable`).
- `Faction { get; init; } -> Faction?` — Faction of each creature (`CreatureRef.Faction`); null keeps the base template's.
- `Id { get; init; } -> string` — Unique id ("elite_vukah"); case-insensitive.
- `Level { get; init; } -> int?` — Level of each creature (`CreatureRef.Level`); null keeps the base template's.
- `LootDisabled { get; init; } -> bool?` — True switches random loot off, false switches it on (`LootAccess.Disabled`); null keeps the base template's.
- `LootLabels { get; init; } -> Guid?` — The loot label collection its drops are rolled from (`LootAccess.Labels`); null keeps the base template's.
- `LootSlots { get; init; } -> (int Min, int Max)?` — How many loot slots are rolled, as (min, max) (`LootAccess.Slots`); null keeps the base template's.
- `Name { get; init; } -> string?` — A display name for logs and Kiln clients (vanilla clients show the base template's own name).
- `Persist { get; init; } -> bool` — For templates the game saves (villagers, tamed animals): remember the variant with the entity (Kiln.Saves EntityData...
- `Resistances { get; init; } -> Dictionary<DamageKind, float>` — Damage susceptibility per kind (`CreatureRef.Resistance`: 1 = normal, 0 = immune, 2 = double).
- `Scale { get; init; } -> float?` — Uniform scale (`CreatureRef.Scale`); null keeps the base template's.
- `SpeedFactor { get; init; } -> float?` — Factor on all movement speeds (`CreatureRef.SpeedFactor`); null keeps them.
- `XpFactor { get; init; } -> float?` — XP factor of a kill (`CreatureRef.XpFactor`); null keeps the base template's.

## CreatureVariantAppliedEvent — A variant was applied to a creature (spawned by `Creatures.Spawn`, a natural spawn replaced by a spawn rule, or `CreatureVariants.Apply`).
- `Entity { get; } -> EntityId` — The creature the variant was applied to.
- `Natural { get; } -> bool` — A natural spawn (spawn rule), not a Kiln spawn.
- `Variant { get; } -> CreatureVariant` — The variant that was applied.

## CreatureVariants — The variant registry (per owner: a mod, or a data pack's "creatures" section).
- `static AddSpawnRule(CreatureSpawnRule rule) -> IDisposable` — Adds a natural-spawn rule for the calling mod (removed when it unloads, or when disposed).
- `static All { get; } -> IReadOnlyList<CreatureVariant>` — Registered variants.
- `static Apply(EntityId entity, string variantId) -> IReadOnlyList<string>` — Applies a registered variant to an existing creature (layers of the variant's owner).
- `static Get(string id) -> CreatureVariant?` — The registered variant with this id (case-insensitive), or null.
- `static Of(EntityId entity) -> CreatureVariant?` — The variant applied to an entity in this session, or null.
- `static Register(CreatureVariant variant) -> IDisposable` — Registers a variant for the calling mod (removed when it unloads, or when the result is disposed).
- `static SpawnRules { get; } -> IReadOnlyList<CreatureSpawnRule>` — Natural-spawn rules.

## DamageKind — DamageSusceptibility slots (attribute structure order).
enum: All, Physical, Cut, Pierce, Blunt, Wood, Stone, Metal, Poison, Explosion, SkillShot, Magical, MagicalBase, MagicalAdder, MagicalMultiplier, Fire, Fog, Ice, Lightning

## EnemyFeatures — ecs::EnemyFeature as a mask (bit = the game's enum value).
enum: None, UseTaunt, UseThreatSystem, UseBlocking, Move, Prowl, Rotate, RotateWhileStanding, Patrol, Roaming, ShowSpawnSequence, ReturnHome, FleeHome, Combat, Interact, TriggerAlarm, CallForHelp, Run, IdleTurn, BlockProjectiles, SearchForCover, MoveToMarker, Sleep, RunAround, Circling, StunnedState, Awareness, Investigate

## EnemyState — enemy01::State: the enemy AI's state machine.
enum: Invalid, Start, Idle, Dead, Spawning, Investigate, Combat, ReturnHome, FleeHome, WaitAtHome, ReactToNoise, Patrol, TurnAfterPatrol, WaitAfterPatrol, ControlledByAttack, Roaming, Flee, WaitAfterFlee, Falling, Hit, Guard, Interact, Landing, BlockProjectile, MoveToMarker, Sleep, Circling, AvoidWater, Revive, Stunned

## Faction — keen::FactionType.
enum: Environment, HitAll, Fogger, Vukah, Scavenger, Wildlife, WildBeast, Player, AutomatedPlayer, HitOthers, Skeleton, PlayerAlly, FarmAnimal, Cyclops, Drak, Ancients, None

## GuardOptions — How a guard follows and defends (`CreatureCommands.Guard`).
- `Assist { get; init; } -> bool` — What the target hits gets threat on the guard (default true).
- `CatchUpDistance { get; init; } -> double` — Teleport the guard next to its target when it is farther away than this (metres; 0 = never; default 30).
- `static readonly Default -> GuardOptions` — Follow, catch up and defend (the defaults below).
- `Defend { get; init; } -> bool` — Whoever hurts the target gets threat on the guard (default true).
- `static readonly EventOnly -> GuardOptions` — Only the game's StartGuardingEvent, nothing kept (the older behaviour).
- `Focus { get; init; } -> bool` — While it guards, the enemy AI features Awareness and Investigate are off for it (default true): the game's own...
- `HomeFollows { get; init; } -> bool` — Its home zone follows the target (default true): idle roaming, the leash and "return home" centre on the target...
- `Keep { get; init; } -> bool` — Send the order again when the game dropped it (default true).
- `StuckSeconds { get; init; } -> double` — Teleport it when it stayed within 1.5 m of one spot this long (seconds) while the target is out of its guard range...
- `Threat { get; init; } -> float` — Threat added per hit for `GuardOptions.Defend` / `GuardOptions.Assist` (default 2000; the Vukah's alarmed threshold...

## GuardOrder — One guard order Kiln keeps (`Creatures.GuardOrders`).
- `GuardOrder(EntityId Guard, EntityId Target, GuardOptions Options, string Owner)`
- `CatchUps { get; } -> int` — Catch-up teleports so far.
- `Guard { get; init; } -> EntityId` — The creature that guards.
- `Options { get; init; } -> GuardOptions` — The options the order was given with.
- `Owner { get; init; } -> string` — The owner of the order: the id of the mod that gave it, or "kiln.creatures" when no mod did.
- `Resent { get; } -> int` — Times the order was sent again because the game had dropped it.
- `Target { get; init; } -> EntityId` — The entity it guards.
- `ThreatsSent { get; } -> int` — Threat events sent for Defend / Assist.

## Is — Conditions.
- `static Custom(Func<NpcView, bool> check, double every = 1) -> RuleCondition` — Your own condition.
- `static Day { get; } -> RuleCondition` — It is day: 6 to 18.
- `static Game(string type, params (string Field, object? Value)[] fields) -> RuleCondition` — Any of the game's constraint kinds, by type name (`"StateConstrain"`) and field values as the game's reflection...
- `static HasBuff(Guid buff) -> RuleCondition` — The creature has this buff.
- `static HealthAbove(double fraction) -> RuleCondition` — Health at or above this fraction (0 to 1).
- `static HealthBelow(double fraction) -> RuleCondition` — Health at or below this fraction (0 to 1).
- `static InPhase(Guid phase) -> RuleCondition` — The creature is in this phase: Kiln's, or the game's on a body that has phases.
- `static InPhase(string name) -> RuleCondition` — The creature is in the calling mod's phase `name`, set by `Act.SetPhase` or `CreatureRef.SetPhase`.
- `static Job(uint id, uint arg = 0) -> RuleCondition` — A handler on the game's job thread, registered with `Arsenals.JobHandler`.
- `static Named(string name) -> RuleCondition` — A condition registered by name with `Arsenals.Condition`, here or in another mod.
- `static Night { get; } -> RuleCondition` — It is night: 18 to 6.
- `static TargetBetween(double min, double max) -> RuleCondition` — Combat target between these distances, metres.
- `static TargetBeyond(double metres) -> RuleCondition` — Combat target farther than these metres.
- `static TargetWithin(double metres) -> RuleCondition` — Combat target within these metres.
- `static TimeBetween(double from, double to) -> RuleCondition` — Time of day within [from, to) hours; wraps past midnight when from > to.

## NpcView — What a condition or action sees on the tick thread: one creature, read when asked.
- `Creature { get; } -> CreatureRef` — The creature, with every `CreatureRef` API.
- `Entity { get; } -> EntityId` — The creature.
- `Health { get; } -> int` — Its health now; 0 when it is gone.
- `HealthFraction { get; } -> double` — Health as a fraction, 0 to 1.
- `HealthMax { get; } -> int` — Its maximum health; 0 when it is gone.
- `Info { get; } -> CreatureInfo?` — A snapshot of the creature, taken once per call; null when it is gone.
- `InPhase(string name) -> bool` — Its phase is this one of the calling mod's.
- `Position { get; } -> WorldPosition?` — Where it stands, null when it is gone.
- `State { get; } -> EnemyState` — Its enemy AI state; `EnemyState.Invalid` when it has none or is gone.
- `Target { get; } -> EntityId` — The combat target, default when it has none.
- `TargetDistance { get; } -> double?` — Metres to the combat target, null without one.

## RuleAction — An action of a rule.
- `ToJson() -> string` — The JSON the arsenal writer takes.

## RuleCondition — A condition of a rule or an attack.
- `Not() -> RuleCondition` — The opposite.
- `ToJson() -> string` — The JSON the arsenal writer takes.

## RuleIds — Ids of what rules name.
- `static Attack(string name) -> Guid` — The GUID of the calling mod's attack `name`.
- `static Derive(string mod, string kind, string name) -> Guid` — The derived GUID.
- `static Fnv(Guid g) -> uint` — FNV-1a-32 over a GUID's bytes: the game's id of a sequence or buff with that GUID.
- `static Phase(string name) -> Guid` — The GUID of the calling mod's phase `name`.

## RuleMode — When a rule is looked at.
enum: OnUpdate, OnEnter, OnExit, OnStartAttack, OnEndAttack, OnEnterState, OnExitState

## RulesStatus — What the rule system has resolved and done so far.
- `RulesStatus(bool Writer, bool Dispatcher, bool Attacks, bool AiSwitch, bool RuleHooks, bool AttackHooks, int Arsenals, int Tracked, int Overrides, int AiOff, int MapCapacity, int MapCount, ulong Constraints, ulong ConstraintsTrue, ulong Actions, ulong ActionsDropped, ulong Selects, ulong SelectsForced, ulong SelectsFiltered, ulong PicksWaiting, ulong FollowupsRefused, ulong Starts, ulong StartsOffOverride, ulong SequencesRefused, ulong PreNs, ulong PreCalls, ulong JobTicks, ulong JobCalls, ulong JobTickHz)`
- `Actions { get; init; } -> ulong` — Kiln actions fired.
- `ActionsDropped { get; init; } -> ulong` — Kiln actions lost to a full queue.
- `AiOff { get; init; } -> int` — Creatures whose AI Kiln switched off (0 off the tick).
- `AiSwitch { get; init; } -> bool` — The AI switch's field resolved.
- `Arsenals { get; init; } -> int` — Kiln's arsenals in the registry.
- `AttackHooks { get; init; } -> bool` — The attack hooks are installed now.
- `Attacks { get; init; } -> bool` — The attack override's targets resolved.
- `Constraints { get; init; } -> ulong` — Kiln constraints evaluated on the job threads.
- `ConstraintsTrue { get; init; } -> ulong` — Of those, the ones that were true.
- `Dispatcher { get; init; } -> bool` — The targets of the two rule hooks resolved.
- `FollowupsRefused { get; init; } -> ulong` — Combo follow-ups an override refused.
- `JobCalls { get; init; } -> ulong` — The job-thread side's calls so far.
- `JobTickHz { get; init; } -> ulong` — The counter's frequency: `JobTicks` / `JobTickHz` is seconds.
- `JobTicks { get; init; } -> ulong` — The job-thread side's cost so far, in counter ticks (rdtsc, while `Arsenals.Timing` is on).
- `MapCapacity { get; init; } -> int` — Capacity of the registry's lookup map.
- `MapCount { get; init; } -> int` — Entries in the registry's lookup map.
- `Overrides { get; init; } -> int` — Creatures with an attack override (0 off the tick).
- `PicksWaiting { get; init; } -> ulong` — Selections that waited for an override.
- `PreCalls { get; init; } -> ulong` — The tick side's calls so far.
- `PreNs { get; init; } -> ulong` — The tick side's cost so far, in nanoseconds.
- `RuleHooks { get; init; } -> bool` — The rule hooks are installed now.
- `Selects { get; init; } -> ulong` — The game's attack selections seen.
- `SelectsFiltered { get; init; } -> ulong` — Attacks an allow mask refused.
- `SelectsForced { get; init; } -> ulong` — Selections an override forced.
- `SequencesRefused { get; init; } -> ulong` — Attack sequences played by a rule that were refused.
- `Starts { get; init; } -> ulong` — Attacks started.
- `StartsOffOverride { get; init; } -> ulong` — Attacks started against an override (rewritten).
- `Tracked { get; init; } -> int` — Creatures in the published table (0 off the tick).
- `Writer { get; init; } -> bool` — Arsenals can be written (the registry map and the record layout were read).

## StateCondition — enemy01::StateCondition: why the state machine moved.
enum: Death, TargetSelected, TargetLost, IsFinished, TargetUnreachable, TargetOutsideLeash, NoiseHeard, ProjectileHeard, HasReachedNoAwarenessState, HasReachedLowAwarenessState, HasReachedHighAwarenessState, IsReadyToPatrol, IsReadyToInteract, IsReadyToTriggerAlarm, IsControlledByAttack, IsReadyForRoaming, IsReadyForCircling, WantsToFlee, HasBeenHit, HasBeenHitRecently, WasHookshotTargetRecently, IsFalling, HitReaction, NeedsToGuard, WantsToLand, NeedsToTurn, CanMoveToMarker, WantsToSleep, WantsToAvoidWater, Revive, IsExhausted, IsHelpingEnemy, NeedsToInvestigate, Unknown

## Villager — One friendly NPC.
- `Entity { get; } -> EntityId` — The villager's entity.
- `Follow(EntityId target, double distance = 4, double catchUp = 40) -> void` — Keeps walking after `target` (a GoTo when it is farther than `distance`), and is teleported to it beyond `catchUp`...
- `GoTo(WorldPosition target) -> void` — Walks to `target` (the game's GoToPosition).
- `IsFollowing { get; } -> bool` — A `Villager.Follow` is running for it.
- `NpcId { get; } -> uint` — FNV-1a-32 of its template GUID, the id of the game's roster.
- `Position { get; } -> WorldPosition?` — Where it stands, null when it is gone.
- `State { get; } -> VillagerState` — Its state machine now (Invalid when it is gone).
- `Stay() -> void` — Stays where it stands: that becomes its home.
- `StopFollowing() -> void` — Ends a `Villager.Follow`.
- `Teleport(WorldPosition at, float yawDegrees = 0) -> void` — Teleports it to `at` and makes that its home (the game's Reset).

## Villagers — Friendly NPCs (villagers, crafters, traders, recruited NPCs, clones): find them and command them.
- `static Available { get; } -> bool` — True when kiln.creatures can send the game's NpcCommandEvent on this build.
- `static Find(Guid template) -> Villager?` — The world's villager of a template (the game keeps one per NpcId); null when it has none.
- `static List() -> IReadOnlyList<Villager>` — Every villager in the world.
- `static NpcIdOf(Guid template) -> uint` — The roster id of a template: FNV-1a-32 of its GUID bytes.
- `static Of(EntityId entity) -> Villager?` — The villager behind an entity; null when it is not one (no DynamicActiveNpcState).
- `static Watch(EntityId entity) -> void` — Posts `VillagerStateEvent` about four times a second for this villager.

## VillagerState — keen::active_npc::State.
enum: Invalid, Start, FindBed, Idle, DecideTask, FindTarget, GoToTarget, Roam, Sleep, Sitting, Interacting, NpcSocializing, Eating, WaitingCloseToPlayer, AvoidBuildingCursor, AvoidWater, InsideWater, FinishTask

## VillagerStateEvent — A villager's state machine changed (`Villager.State`, the game's `DynamicActiveNpcState`).
- `Entity { get; } -> EntityId` — The villager's entity.
- `From { get; } -> VillagerState` — The state it left.
- `NpcId { get; } -> uint` — The villager's roster id: FNV-1a-32 of its template GUID (`Villager.NpcId`).
- `To { get; } -> VillagerState` — The state it entered.

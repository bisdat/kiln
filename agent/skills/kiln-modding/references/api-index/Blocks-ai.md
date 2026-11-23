# Blocks (Kiln.Blocks): ai

## AIBlock — How it notices and chases: the game's tuning of a creature to start from (`AIs`), and what to change.
- `Aggro { get; init; } -> double?` — Within this distance a target it sees is engaged at once (`combatCommitmentSetup.gain.inRange.range`; the game's 0...
- `CallsForHelp { get; init; } -> bool?` — True: it calls the friends around it into its fights (the game's CallForHelp); false: it never does.
- `Disable { get; init; } -> EnemyFeatures?` — The game's feature switches to turn off.
- `Enable { get; init; } -> EnemyFeatures?` — The game's feature switches to turn on, beyond what the other fields say (...
- `Faction { get; init; } -> Faction?` — Its side: who it fights.
- `Fights { get; init; } -> bool?` — False: it does not fight (the game's Combat off).
- `Flee { get; init; } -> FleeBlock?` — How it runs from a fight.
- `Hearing { get; init; } -> double?` — How far it hears, as a share of the game's noise ranges, 1 as the game has it (`noiseListenerSetup.listenRangeFactor`).
- `Hears { get; init; } -> bool?` — False: it does not react to noise (`noiseListenerSetup.isEnabled`).
- `HelpRadius { get; init; } -> double?` — Metres its call reaches (`callForHelpSetup.radius`).
- `Key { get; init; } -> string` — How JSON names a premade.
- `Leash { get; init; } -> double?` — How far from home it chases before it goes back.
- `Like { get; init; } -> string?` — In JSON, the premade this one starts from.
- `Moves { get; init; } -> bool?` — False: it stands where it is (the game's Move, Roaming and Patrol off).
- `NightSight { get; init; } -> double?` — 0..4: the share of `AIBlock.Sight` it sees at night (`combatCommitmentSetup.view.nightRangeFactor`; the Vukah's 0.6).
- `PickEvery { get; init; } -> double?` — Seconds between the decisions of `AIBlock.PickTarget`, and of the behaviour's attack picker when...
- `PickTarget { get; init; } -> Func<NpcView, EntityId>?` — Decides its combat target, on the tick thread, every `AIBlock.PickEvery` seconds: an entity, or `default` for the...
- `PickTargetName { get; init; } -> string?` — In JSON: a target picker registered by name with `Blocks.Target`.
- `Relations { get; init; } -> IDictionary<Faction, Relation>?` — Who it fights, by faction, for this creature only...
- `ReturnsHome { get; init; } -> bool?` — False: it never walks home after a fight (the game's ReturnHome and FleeHome off).
- `Roam { get; init; } -> RoamBlock?` — How it wanders.
- `SenseEvery { get; init; } -> double?` — Seconds between a SenseFilter's looks.
- `SenseFilter { get; init; } -> Func<NpcView, EntityId, bool>?` — Decides, for one creature or player it could notice, whether it treats it as an enemy: true fights as the game...
- `SenseFilterName { get; init; } -> string?` — In JSON: a sense filter registered by name with `Blocks.SenseFilter`.
- `Sight { get; init; } -> double?` — How far it sees a target by day (the game's `combatCommitmentSetup.view.range`): less at night and in water, by the...
- `SightAngle { get; init; } -> double?` — Half the angle of its view, in degrees (`combatCommitmentSetup.view.coneHalfAngle`).
- `Template { get; init; } -> string?` — The game creature whose AI this starts from.
- `Threat { get; init; } -> double?` — How much it draws enemies' attention, 1 as the game has it (`threatFactor`).
- `Tune { get; init; } -> IDictionary<string, string>?` — Any other AI field, by its path in the game's AI (`Enemy01BehaviorState` unless it names another component)...

## AiRulesApi — What `AiRulesApi` adds to `Is` and `Act`.
Extension members (C# 14): call them on the receiver (`Act.X`, `Blocks.X`, `Is.X`), not on `AiRulesApi`.
- `Is.AlliesNear(int count, double metres) -> RuleCondition` — At least `count` creatures on its side are within `metres` (itself not counted).
- `Act.Apply(StatusApply status, NpcEffectOn on = NpcEffectOn.Target) -> RuleAction` — Puts a status on its target, or on itself.
- `Act.BackOff(double min, double max) -> RuleAction` — It backs away from its target, between these metres, and fights on (`RunAwayAction`).
- `Act.Cast(IDeliveryBlock delivery) -> RuleAction` — Casts a delivery at its target from where it stands, without an animation: a projectile flies at it.
- `Is.Chance(double chance) -> RuleCondition` — A roll that comes up this share of the times, 0..1 (`RandomChanceConstrain`).
- `Act.DisableFeature(EnemyFeatures feature) -> RuleAction` — Turns one of the game's AI features off (`DisableFeatureAction`).
- `Act.EnableFeature(EnemyFeatures feature) -> RuleAction` — Turns one of the game's AI features on (`EnableFeatureAction`).
- `Is.EnemiesNear(int count, double metres) -> RuleCondition` — At least `count` creatures or players it would fight are within `metres`, by the game's own check (with Kiln's...
- `Act.Flee() -> RuleAction` — It runs from the fight, as the game's creatures flee (the state condition WantsToFlee; how far: `AIBlock.Flee`).
- `Is.HasTarget { get; } -> RuleCondition` — It has a combat target (`HasCombatTargetConstrain`).
- `Act.Heal(double fraction) -> RuleAction` — Heals it by a share of its most health, 0..1.
- `Is.HitWithin(double seconds) -> RuleCondition` — It was hit within the last `seconds` (`LastHitReceivedConstrain`).
- `Act.Immune(bool immune = true) -> RuleAction` — Takes no damage, or with false takes it again (`SetImmuneAction`).
- `Is.InCombat { get; } -> RuleCondition` — It is fighting (the game's Combat state).
- `Is.InState(params EnemyState[] states) -> RuleCondition` — Its AI is in one of these states (`StateConstrain`).
- `Act.Pause(double minSeconds, double maxSeconds) -> RuleAction` — Stands still for a while, facing its target (`PauseAction`).
- `Act.Play(Guid sequence, bool wait = false) -> RuleAction` — Plays one of the game's sequences (an attack, a shout) by its GUID, without choosing an attack...
- `Blocks.SenseFilter(string name, Func<NpcView, EntityId, bool> filter) -> void` — A sense filter for data files: `"senseFilter": "name"`.
- `Act.Summon(string creature, int count = 1, double radius = 3, double seconds = 0, SummonSide side = SummonSide.Summoner) -> RuleAction` — Calls `count` creatures around it: a creature block of this mod (`"imp"`), of another (`"mod:imp"`), a...
- `Is.SummonsBelow(int count) -> RuleCondition` — Fewer than `count` of the creatures it summoned (`Act.Summon`) are alive.
- `Is.TargetAtHome { get; } -> RuleCondition` — Its target stands inside its home zone (`IsTargetInHomeZoneConstraint`).
- `Is.TargetHealthBelow(double fraction) -> RuleCondition` — Its target's health is at or below this share, 0..1 (`CurrentTargetHealthPercentageConstrain`).
- `Is.TargetIsPlayer(bool player = true) -> RuleCondition` — Its target is a player, or with false is not (`IsPlayerTargetConstrain`).

## AiStats — What the AI blocks did since the server started, for tests and admin commands.
- `static Casts { get; } -> long` — Casts started by rules.
- `static FilterCalls { get; } -> long` — Sense filter calls, and how many of them refused.
- `static FilterRefused { get; } -> long` — How many of the `AiStats.FilterCalls` refused.
- `static Rows { get; } -> long` — Creatures given their own relations.
- `static Summons { get; } -> long` — Creatures summoned by rules.
- `static SummonsOf(EntityId summoner) -> IReadOnlyList<EntityId>` — The creatures this one summoned that still live.

## Factions — Who fights whom in the world, by faction: `Factions.Relate(Faction.Vukah, Faction.Scavenger, Relation.Hostile)`.
- `static Available { get; } -> bool` — kiln.npcai is here and found the game's checks on this build.
- `static Between(EntityId a, EntityId b) -> (bool Fights, bool Friends)` — What the game's checks answer now for two entities, a about b, with Kiln's table, a's own relations and its...
- `static Fights(Faction a, Faction b) -> bool` — What the game's own check answers now: would a fight b.
- `static Friends(Faction a, Faction b) -> bool` — What the game's own check answers now: is b on a's side.
- `static Get(Faction a, Faction b) -> Relation` — The relation the world's table holds for a toward b (any mod's); `Relation.Game` when none.
- `static Relate(Faction a, Faction b, Relation relation, bool mutual = true) -> void` — a toward b, and b toward a unless `mutual` is false.
- `static Reset() -> void` — Gives every relation the calling mod set back to the game.
- `static Status { get; } -> FactionStatus?` — What kiln.npcai reports, or null without it.

## FactionStatus — What kiln.npcai reports: which hooks it has, what is set, how often the game asked and Kiln answered.
- `FactionStatus(bool Available, bool Installed, int Relations, int CreatureRows, int Pairs, int Mapped, ulong Calls, ulong Answered, ulong ByPair, ulong ByRow, ulong ByTable)`
- `Answered { get; init; } -> ulong` — How many of those calls Kiln answered itself.
- `Available { get; init; } -> bool` — kiln.npcai is here and found the game's checks on this build, as `Factions.Available`.
- `ByPair { get; init; } -> ulong` — Calls answered from a setting on one pair of entities.
- `ByRow { get; init; } -> ulong` — Calls answered from a creature's own relations.
- `ByTable { get; init; } -> ulong` — Calls answered from the world's faction table.
- `Calls { get; init; } -> ulong` — How many times the game asked its relation checks.
- `CreatureRows { get; init; } -> int` — How many creatures have relations of their own.
- `Installed { get; init; } -> bool` — Kiln's hooks on the game's relation checks are in place.
- `Mapped { get; init; } -> int` — The mapped count kiln.npcai reports (the "mapped" in `FactionStatus.ToString`).
- `Pairs { get; init; } -> int` — How many pairs of entities have a setting of their own (a sense filter's refusals set them).
- `Relations { get; init; } -> int` — How many faction relations are set in the world's table.

## FleeBlock — How a creature runs from a fight: below a share of its health, as far as the game's flee goes.
- `Below { get; init; } -> double?` — 0..1: it runs once its health is at or below this share.
- `Cooldown { get; init; } -> double` — Seconds before `FleeBlock.Below` can make it run again.
- `Max { get; init; } -> double?` — Metres it runs at most (`fleeSetup.maxFleeRadius`).
- `Min { get; init; } -> double?` — Metres it runs at least (`fleeSetup.minFleeRadius`).
- `Seconds { get; init; } -> double?` — Seconds it runs at most before it turns again (`fleeSetup.maxFleeDuration`).

## Relation — What a creature's check about another answers: does it fight it, is it on its side.
enum: Game, Hostile, Friendly, Neutral

## RoamBlock — How a creature wanders while it has nothing to do.
- `FromHome { get; init; } -> double?` — How far from home it may get while roaming (`roamingSetup.maxHomeZoneDistance`).
- `Max { get; init; } -> double?` — Metres of the longest walk.
- `Min { get; init; } -> double?` — Metres of the shortest walk.

## SummonSide — Who a summon fights for.
enum: Summoner, Own

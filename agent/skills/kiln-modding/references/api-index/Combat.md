# Combat (Kiln.Combat)

## AttributeIds — The game's attribute ids of one pair, read from its attribute resource when the first world starts.
- `AttributeIds()`
- `AttributeIds(GameAttribute Kind, uint CurrentId, uint MinId, uint MaxId, uint ComponentHash, bool GameClamp)`
- `ComponentHash { get; init; } -> uint` — FNV-1a of the component's type name (keen::ecs::Stamina is 0x92410664).
- `CurrentId { get; init; } -> uint` — The id of the current value (the one `Attributes.Set` writes).
- `GameClamp { get; init; } -> bool` — The game's own formula clamps the current value to [min, max] after every change.
- `Kind { get; init; } -> GameAttribute` — The pair.
- `MaxId { get; init; } -> uint` — The id of the maximum, 0 when the pair has none.
- `MinId { get; init; } -> uint` — The id of the minimum, 0 when the pair has none.

## AttributeReading — One attribute of an entity with its pair's current value and bounds.
- `AttributeReading()`
- `AttributeReading(uint Id, GameAttribute Kind, int Value, int Current, int? Min, int? Max, bool IsCurrent, uint CurrentId, uint MaxId)`
- `Current { get; init; } -> int` — The pair's current value.
- `CurrentId { get; init; } -> uint` — The pair's current-value id.
- `Fraction { get; } -> double` — Current / maximum, 0 without a positive maximum.
- `Id { get; init; } -> uint` — The attribute id asked for.
- `IsCurrent { get; init; } -> bool` — `Id` is the pair's current value, the one that can be set and added to.
- `Kind { get; init; } -> GameAttribute` — The pair it belongs to.
- `Max { get; init; } -> int?` — The pair's maximum, null when it has none.
- `MaxId { get; init; } -> uint` — The pair's maximum id, 0 when it has none.
- `Min { get; init; } -> int?` — The pair's minimum, null when it has none.
- `Value { get; init; } -> int` — The value of `Id` (for a current value: the same as `Current`).

## Attributes — Health, stamina, mana, body heat and oxygen of any entity on the server world, by the game's own attribute ids (`Attributes.Ids`: resolved from the game's attribute...
- `static Add(EntityId entity, GameAttribute attribute, int delta) -> int` — Adds `delta` (negative to spend) to the current value, clamped like Set.
- `static Add(EntityId entity, uint attributeId, int delta) -> int` — Add by the game's attribute id of a pair's current value.
- `static Available { get; } -> bool` — True when the attribute functions work on this build and a world has started (capability `combat.attributes`).
- `static Get(EntityId entity, GameAttribute attribute) -> AttributeReading?` — The current value of a pair with its bounds, or null when the entity has no such attribute (a creature without mana...
- `static Get(EntityId entity, uint attributeId) -> AttributeReading?` — Any attribute of the five components by its id (current, minimum, maximum, recharge...), with its pair's current...
- `static Ids(GameAttribute attribute) -> AttributeIds?` — The game's ids of a pair, or null before the first server world runs or when this build has no attribute resource...
- `static Set(EntityId entity, GameAttribute attribute, int value) -> int` — Sets the current value, clamped to the pair's [min, max].
- `static Set(EntityId entity, uint attributeId, int value) -> int` — Set by the game's attribute id of a pair's current value (see `Attributes.Ids`).
- `static TryAdd(EntityId entity, GameAttribute attribute, int delta, out AttributeReading result) -> bool` — Like `Attributes.Add`, false instead of throwing when the entity lacks the attribute or attributes are unavailable.
- `static TrySet(EntityId entity, GameAttribute attribute, int value, out AttributeReading result) -> bool` — Like `Attributes.Set`, false instead of throwing when the entity lacks the attribute or attributes are unavailable.

## Combat — Health, damage, healing, kills and god mode on the server world, through the engine's own paths: health writes go through the attribute system (the game copies Health...
- `static Available { get; } -> bool` — True when health access works on this build and a server world runs.
- `static Damage(EntityId target, int amount, EntityId attacker = default(EntityId), DamageOptions? options = null) -> Task<HitResult>` — Deals `amount` damage to `target` as an engine hit from `attacker` (kill credit and XP go to the attacker when it is...
- `static GetHealth(EntityId entity) -> Health?` — The entity's health, or null when it has none (props without Health, unknown ids).
- `static Heal(EntityId entity, int amount) -> int` — Heals up to max, silently (no HitEvent).
- `static HitsAvailable { get; } -> bool` — True when Damage/Kill (the engine hit path) work.
- `static IsInvulnerable(EntityId entity) -> bool` — Kiln god mode is on for the entity (`Combat.SetInvulnerable`, any mod).
- `static Kill(EntityId target, EntityId attacker = default(EntityId), DamageOptions? options = null) -> Task<HitResult>` — Kills `target` with a hit of exactly its current health (a killing blow; kill credit for `attacker`).
- `static SetHealth(EntityId entity, int value) -> int` — Sets current health (clamped to 0..max) silently.
- `static SetInvulnerable(EntityId entity, bool on = true) -> void` — God mode for one entity (players too): damage hits on it are dropped before they apply (no reaction either), and...
- `static Status { get; } -> CombatStatus?` — kiln.combat's state and counters, or null when it is not loaded.
- `static TryGetHealth(EntityId entity, out Health health) -> bool` — The entity's health; false when it has none.

## CombatChangedEvent `0.2.2` — A mod changed an entity through Kiln.Combat without a hit or a heal: its health set (`Combat.SetHealth`), its god mode (`Combat.SetInvulnerable`) or an attribute's...
- `After { get; } -> int` — The value after.
- `Before { get; } -> int` — The value before (health points, the attribute's current value, 1/0 for god mode on/off).
- `Entity { get; } -> EntityId` — The entity.
- `Involves(EntityId entity) -> bool` — Whether the event is about `entity`.
- `Origin { get; } -> string` — The id of the mod that made the change ("" when Kiln made it for no mod).
- `What { get; } -> string` — What changed: `Health`, `Invulnerable`, or `Attribute.<name>` (`Attribute.Stamina`; `Attribute.0x...` for an id...

## CombatOrigin — Where a hit or a death came from.
enum: Game, Kiln, Other

## CombatStatus — Counters of kiln.combat (diagnostics).
- `CombatStatus(bool Health, bool Hits, bool Deaths, bool Holding, uint Interest, ulong HitsSeen, ulong HitsHeld, ulong HitsReleased, ulong HitsCanceled, ulong HitsModified, ulong KilnHits, ulong GodBlocked, ulong GodRestored, ulong DeathsSeen, ulong EventsDelivered, uint Invulnerable, ulong GameDiedEvents, ulong HitsDeferred, ulong DropsSeen, ulong DropsCanceled, ulong RepeatHits)`
- `AttributeHeals { get; init; } -> bool` — changeAttributeValue health gains reach `EntityHealEvent`.
- `AttributeHealsCorrected { get; init; } -> ulong`
- `AttributeHealsSeen { get; init; } -> ulong`
- `Deaths { get; init; } -> bool`
- `DeathsSeen { get; init; } -> ulong`
- `DropsCanceled { get; init; } -> ulong`
- `DropsSeen { get; init; } -> ulong`
- `EventsDelivered { get; init; } -> ulong`
- `Falls { get; init; } -> bool` — Fall damage is intercepted on this build (`EntityFallEvent`).
- `FallsCanceled { get; init; } -> ulong`
- `FallsModified { get; init; } -> ulong`
- `FallsSeen { get; init; } -> ulong`
- `GameDiedEvents { get; init; } -> ulong`
- `GodBlocked { get; init; } -> ulong`
- `GodRestored { get; init; } -> ulong`
- `Health { get; init; } -> bool`
- `Hits { get; init; } -> bool`
- `HitsCanceled { get; init; } -> ulong`
- `HitsDeferred { get; init; } -> ulong`
- `HitsHeld { get; init; } -> ulong`
- `HitsModified { get; init; } -> ulong`
- `HitsReleased { get; init; } -> ulong`
- `HitsSeen { get; init; } -> ulong`
- `Holding { get; init; } -> bool`
- `Interest { get; init; } -> uint`
- `Invulnerable { get; init; } -> uint`
- `KilnHits { get; init; } -> ulong`
- `KnockbacksModified { get; init; } -> ulong`
- `KnockbacksSeen { get; init; } -> ulong`
- `RegenCanceled { get; init; } -> ulong`
- `Regeneration { get; init; } -> bool` — Natural regeneration reaches `EntityHealEvent`.
- `RegenSeen { get; init; } -> ulong`
- `RepeatHits { get; init; } -> ulong`

## DamageOptions — Options for `Combat.Damage`.
- `Aggro { get; init; } -> float` — Threat for enemy AI (the game's own hits use about 1).
- `AsGameHit { get; init; } -> bool` — Treat the hit like one the engine produced: no synchronous `EntityHurtEvent`; it is held and reported in the next...
- `static readonly Default -> DamageOptions`
- `ForceInterrupt { get; init; } -> bool` — The hit interrupts the target's current action.
- `NoCredit { get; init; } -> bool` — Clears the target's credit before the hit.
- `PushBack { get; init; } -> byte` — Push-back force 0..255.
- `Reaction { get; init; } -> HitReaction` — Stagger of the target (none by default).
- `Silent { get; init; } -> bool` — No HitEvent: health changes, but no reaction, no damage number for clients, no kill credit for this hit (a player...
- `Source { get; init; } -> EntityId` — The direct source entity (weapon, projectile); default: the attacker.

## EntityDamagedEvent — A hit changed an entity's health (after the fact; observe only).
- `Amount { get; } -> int` — Health lost (positive) or gained (negative).
- `Attacker { get; } -> EntityId`
- `Flags { get; } -> HitFlags`
- `HealthAfter { get; } -> int`
- `HealthBefore { get; } -> int`
- `HealthMax { get; } -> int`
- `Involves(EntityId entity) -> bool` `0.2.2` — For event filters (`EventFilter`): the target, the attacker or the source.
- `IsPlayer { get; } -> bool`
- `KillingBlow { get; } -> bool` — This hit took the target from above 0 to 0 or below.
- `Origin { get; } -> CombatOrigin`
- `Position { get; } -> WorldPosition`
- `PushBack { get; } -> int` — The push-back force the hit applied with (after `EntityKnockbackEvent` handlers).
- `Reaction { get; } -> HitReaction` — The hit reaction the hit applied with (after `EntityKnockbackEvent` handlers).
- `Source { get; } -> EntityId`
- `Step { get; } -> ulong`
- `Target { get; } -> EntityId`

## EntityDeathEvent — An entity died (Forge: LivingDeathEvent; here observe only): the game's EntityDiedEvent, posted in the Pre after the game's death program ran.
- `Flags { get; } -> HitFlags` — Flags of the killing blow (KillingBlow is always set when there was one).
- `Health { get; } -> int`
- `Involves(EntityId entity) -> bool` `0.2.2` — For event filters (`EventFilter`): the victim, the killer or the source.
- `IsPlayer { get; } -> bool`
- `Killer { get; } -> EntityId` — Root attacker of the killing blow (a player, a creature) when the death came from a hit Kiln saw; None for deaths...
- `Origin { get; } -> CombatOrigin` — Game/Kiln: the killing blow was a hit of that origin; Other: no hit.
- `Position { get; } -> WorldPosition` — Where the victim was when the death was reported (metres).
- `Source { get; } -> EntityId` — The killing blow's direct source (weapon, projectile).
- `Step { get; } -> ulong`
- `Victim { get; } -> EntityId`

## EntityDropsEvent — A loot container appeared at a corpse (Forge: LivingDropsEvent, one event per container): an entity of a LootContainer_* template with the game's DestroyOnLoot tag...
- `Container { get; } -> EntityId` — The loot container entity (may equal `EntityDropsEvent.Victim` when the corpse itself became loot).
- `Involves(EntityId entity) -> bool` `0.2.2` — For event filters (`EventFilter`): the victim, the container or the killer.
- `Killer { get; } -> EntityId`
- `Position { get; } -> WorldPosition`
- `Step { get; } -> ulong`
- `Victim { get; } -> EntityId`

## EntityDyingEvent `0.2.4` — An entity's health reached 0 or less and the game is about to let it die: a handler may `CancelableEvent.Cancel` to keep it alive with...
- `EntityDyingEvent(DecisionId id, EventEntityKey key, int health, int maximumHealth)` — Makes the event (Kiln posts it; a mod has no reason to).
- `Entity { get; } -> EntityId` — The dying entity.
- `Health { get; } -> int` — Its health, read in this Pre (0 or less).
- `Id { get; } -> DecisionId` — The decision; the same id is on its `DecisionCompletedEvent`.
- `Involves(EntityId entity) -> bool` — Whether the event is about `entity`.
- `Key { get; } -> EventEntityKey` — Which life of the entity id is dying.
- `MaximumHealth { get; } -> int` — Its maximum health, read in this Pre (0 when it could not be read).
- `SurvivalHealth { get; set; } -> int` — The health a canceled death leaves the entity with: positive, 1 by default, clamped to the entity's maximum when it...

## EntityFallEvent — An entity is about to take fall damage (Forge: LivingFallEvent).
- `Damage { get; set; } -> int` — Health points the fall takes.
- `DamageFraction { get; } -> float` — The game's damage as a fraction of max health (1 = lethal from full health).
- `Distance { get; } -> float` — Metres fallen (the game's detected fall distance).
- `Entity { get; } -> EntityId`
- `HealthBefore { get; } -> int`
- `HealthMax { get; } -> int`
- `Involves(EntityId entity) -> bool` `0.2.2` — For event filters (`EventFilter`): the entity that fell.
- `IsPlayer { get; } -> bool`
- `Lethal { get; } -> bool` — The original damage would kill.
- `OriginalDamage { get; } -> int`
- `Position { get; } -> WorldPosition`
- `Step { get; } -> ulong`

## EntityHealEvent — An entity is about to be healed (Forge: LivingHealEvent): healing hits (spells), natural regeneration, effects that add health directly (potions, food), and...
- `Cause { get; } -> HealCause`

## EntityHurtEvent — An entity is about to take damage from a hit (Forge: LivingAttackEvent + LivingHurtEvent + LivingDamageEvent in one).
- `Lethal { get; } -> bool` — The original amount would kill the target (cancel or lower it to keep the target alive).

## EntityKnockbackEvent — A hit is about to push an entity back or make it stagger (Forge: LivingKnockBackEvent).
- `Attacker { get; } -> EntityId`
- `Direction { get; set; } -> Vector3` — The hit direction the push follows (attacker -> target).
- `Flags { get; } -> HitFlags`
- `ImpactId { get; } -> uint`
- `Involves(EntityId entity) -> bool` `0.2.2` — For event filters (`EventFilter`): the target, the part hit, the attacker or the source.
- `IsPlayer { get; } -> bool`
- `Origin { get; } -> CombatOrigin`
- `OriginalDirection { get; } -> Vector3`
- `OriginalReaction { get; } -> HitReaction`
- `OriginalStrength { get; } -> int`
- `Part { get; } -> EntityId`
- `Position { get; } -> WorldPosition`
- `Reaction { get; set; } -> HitReaction` — How hard the target staggers.
- `Source { get; } -> EntityId`
- `Step { get; } -> ulong`
- `Strength { get; set; } -> int` — Push-back force 0..255 (the engine's pushBackForce).
- `Target { get; } -> EntityId`

## GameAttribute — The attribute pairs `Attributes` handles: a current value with a minimum and a maximum that the game computes (level, gear, buffs).
enum: Health, Stamina, Mana, BodyHeat, Oxygen

## HealCause — What healed an entity (`EntityHealEvent.Cause`).
enum: Hit, Kiln, Regeneration, Attribute

## Health — An entity's health (engine health points).
- `Health()`
- `Health(int Current, int Max, bool Dead, bool HasActor, bool Invulnerable)`
- `Current { get; init; } -> int` — Health points now.
- `Dead { get; init; } -> bool` — The game's dead state (Actor state 7, read through the game's own state test) as its death check leaves it at the...
- `Fraction { get; } -> double`
- `HasActor { get; init; } -> bool` — The entity has an Actor (creatures, players, NPCs).
- `Invulnerable { get; init; } -> bool` — Kiln god mode is on for it.
- `Max { get; init; } -> int` — The computed maximum.

## HitFlags — The engine's HitEventFlags (what the hit was).
enum: None, Blocked, Parried, BlockBroken, Crit, Visceral, Backstab, Weakspot, SkillShot, Resistance, Immune, Ineffective, KillingBlow, Healing, Terrain, Building, ForceInterrupt

## HitReaction — The engine's hit reaction (impact::HitReactionType): how hard the target staggers.
enum: None, VeryLight, Light, Medium, Heavy, Boss, LongStun, TrapStun, ThrowIntoAir

## HitResult — Outcome of one `Combat.Damage` / `Combat.Kill` hit.
- `HitResult(HitState State, EntityId Target, int Amount, int HealthBefore, int HealthAfter, bool KillingBlow, int Steps)`
- `Amount { get; init; } -> int`
- `Applied { get; } -> bool`
- `HealthAfter { get; init; } -> int`
- `HealthBefore { get; init; } -> int`
- `KillingBlow { get; init; } -> bool`
- `Ok { get; } -> bool` `0.2.2` — The engine applied the hit (`HitResult.Applied`).
- `Reason { get; } -> string?` `0.2.2` — Why the hit was not applied; null when it was.
- `State { get; init; } -> HitState`
- `Steps { get; init; } -> int`
- `Target { get; init; } -> EntityId`

## HitState
enum: Pending, Applied, Dropped

## PendingHitEvent — Common fields of a hit that is about to apply (`EntityHurtEvent`, `EntityHealEvent`).
- `Amount { get; set; } -> int` — Health points the hit changes, as a positive number.
- `AttackCategory { get; } -> byte`
- `Attacker { get; } -> EntityId` — The root attacker (the player for player hits, the creature for creature hits); None if unknown.
- `DamageTypes { get; } -> ushort` — The engine's damage type mask (Cut, Pierce, Blunt, Fire, Ice, ...; bit order of DamageSetup).
- `Flags { get; } -> HitFlags`
- `HealthBefore { get; } -> int` — The target's health when the event was built (-1 unknown, e.g. a destructible prop without Health yet).
- `HealthMax { get; } -> int`
- `ImpactId { get; } -> uint` — The engine's impact instance id (one swing or projectile).
- `Involves(EntityId entity) -> bool` `0.2.2` — For event filters (`EventFilter`): the target, the part hit, the attacker or the source, the four entities...
- `IsPlayer { get; } -> bool`
- `Origin { get; } -> CombatOrigin`
- `OriginalAmount { get; } -> int` — The amount the engine (or the Combat call) produced, before any handler.
- `Part { get; } -> EntityId` — The entity the hit landed on (a weakspot or body part); equals `PendingHitEvent.Target` otherwise.
- `Position { get; } -> WorldPosition` — Hit point (metres).
- `Reaction { get; } -> HitReaction`
- `Source { get; } -> EntityId` — The direct source: weapon, projectile, spell or the attacker itself.
- `Step { get; } -> ulong` — Sim step in which the engine produced the hit.
- `Target { get; } -> EntityId` — The entity whose health changes (a creature, not its weakspot).
- `WeaponCategory { get; } -> byte`

## PlayerRespawnEvent — A dead player is alive again (Forge: PlayerRespawnEvent): respawned at a flame altar / bed or revived by another player.
- `Health { get; } -> int`
- `HealthMax { get; } -> int`
- `Involves(EntityId entity) -> bool` `0.2.2` — For event filters (`EventFilter`): the player.
- `Player { get; } -> EntityId`
- `Position { get; } -> WorldPosition`
- `Step { get; } -> ulong`

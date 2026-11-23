// SPDX-License-Identifier: MIT
// Combat events on Kiln's event bus (Forge: LivingAttackEvent/LivingHurtEvent/LivingDamageEvent, LivingHealEvent,
// LivingDeathEvent, PlayerEvent.PlayerRespawnEvent). All are posted on the tick thread, in Pre (before the sim step),
// and only built while someone subscribes. docs/kiln/services/combat.md has the timing details.
namespace Kiln;

/// <summary>Where a hit or a death came from.</summary>
public enum CombatOrigin
{
    /// <summary>The engine's own hits: weapons, spells, AI attacks, traps.</summary>
    Game = 0,
    /// <summary>A <see cref="Combat"/> call (Damage, Kill, Heal).</summary>
    Kiln = 1,
    /// <summary>No hit Kiln saw: fall damage, drowning, <see cref="Combat.SetHealth"/>, the game's own scripts.</summary>
    Other = 2,
}

/// <summary>The engine's HitEventFlags (what the hit was).</summary>
[Flags]
public enum HitFlags : uint
{
    /// <summary>No flags set.</summary>
    None = 0,
    /// <summary>The target blocked the hit.</summary>
    Blocked = 0x1,
    /// <summary>The target parried the hit.</summary>
    Parried = 0x2,
    /// <summary>The hit broke the target's block.</summary>
    BlockBroken = 0x4,
    /// <summary>A critical hit.</summary>
    Crit = 0x8,
    /// <summary>A visceral hit (the engine's Visceral flag).</summary>
    Visceral = 0x10,
    /// <summary>A hit from behind.</summary>
    Backstab = 0x20,
    /// <summary>The hit landed on a weak spot.</summary>
    Weakspot = 0x40,
    /// <summary>A skill shot, as the engine flags it.</summary>
    SkillShot = 0x80,
    /// <summary>The hit met a resistance of the target (the engine's Resistance flag).</summary>
    Resistance = 0x100,
    /// <summary>The target is immune to the hit.</summary>
    Immune = 0x200,
    /// <summary>The hit was ineffective (the engine's Ineffective flag).</summary>
    Ineffective = 0x400,
    /// <summary>The hit took the target from above 0 health to 0 or below.</summary>
    KillingBlow = 0x2000,
    /// <summary>The hit heals instead of damaging.</summary>
    Healing = 0x4000,
    /// <summary>The hit landed on terrain.</summary>
    Terrain = 0x8000,
    /// <summary>The hit landed on a building.</summary>
    Building = 0x10000,
    /// <summary>The hit interrupts the target's current action.</summary>
    ForceInterrupt = 0x800000,
}

/// <summary>The engine's hit reaction (impact::HitReactionType): how hard the target staggers.</summary>
public enum HitReaction : ushort
{
    /// <summary>No reaction: the target does not stagger.</summary>
    None = 0,
    /// <summary>A very light stagger.</summary>
    VeryLight = 1,
    /// <summary>A light stagger.</summary>
    Light = 2,
    /// <summary>A medium stagger.</summary>
    Medium = 3,
    /// <summary>A heavy stagger.</summary>
    Heavy = 4,
    /// <summary>The stagger of a boss hit.</summary>
    Boss = 5,
    /// <summary>A long stun.</summary>
    LongStun = 6,
    /// <summary>The stun of a trap.</summary>
    TrapStun = 7,
    /// <summary>The target is thrown into the air.</summary>
    ThrowIntoAir = 9,
}

/// <summary>Common fields of a hit that is about to apply (<see cref="EntityHurtEvent"/>, <see cref="EntityHealEvent"/>).</summary>
public abstract class PendingHitEvent : CancelableEvent, IEntityEvent
{
    /// <summary>For event filters (<see cref="EventFilter"/>): the target, the part hit, the attacker or the source, the
    /// four entities kiln.combat's hold also tests (a hit an entity deals is about it too). Since 0.2.0-dev.27.</summary>
    public bool Involves(EntityId entity) => !entity.IsNone && (Target == entity || Part == entity || Attacker == entity || Source == entity);

    private int _amount;

    internal PendingHitEvent(in CombatNative.Event e)
    {
        Target = new EntityId(e.Target);
        Part = new EntityId(e.Part);
        Attacker = new EntityId(e.Attacker);
        Source = new EntityId(e.Source);
        _amount = OriginalAmount = e.Amount;
        HealthBefore = e.HealthBefore;
        HealthMax = e.HealthMax;
        Flags = (HitFlags)e.HitFlags;
        DamageTypes = e.DamageTypes;
        Reaction = (HitReaction)e.Reaction;
        AttackCategory = e.AttackCategory;
        WeaponCategory = e.WeaponCategory;
        Origin = (CombatOrigin)e.Origin;
        IsPlayer = e.IsPlayer != 0;
        ImpactId = e.ImpactId;
        Position = new WorldPosition(e.Position.X, e.Position.Y, e.Position.Z);
        Step = e.Step;
    }

    // Kiln's own hit (Combat.Damage, Kill, Heal), posted inside the call: the entities kiln.combat's record of it names
    // (t_damage: target and root target the target, root source the attacker, source the given one or else the attacker)
    internal PendingHitEvent(EntityId target, EntityId attacker, EntityId source, int amount, int healthBefore, int healthMax, bool isPlayer)
    {
        Target = Part = target;
        Attacker = attacker;
        Source = source.IsNone ? attacker : source;
        _amount = OriginalAmount = amount;
        HealthBefore = healthBefore;
        HealthMax = healthMax;
        Origin = CombatOrigin.Kiln;
        IsPlayer = isPlayer;
    }

    /// <summary>The entity whose health changes (a creature, not its weakspot).</summary>
    public EntityId Target { get; }
    /// <summary>The entity the hit landed on (a weakspot or body part); equals <see cref="Target"/> otherwise.</summary>
    public EntityId Part { get; }
    /// <summary>The root attacker (the player for player hits, the creature for creature hits); None if unknown.</summary>
    public EntityId Attacker { get; }
    /// <summary>The direct source: weapon, projectile, spell or the attacker itself.</summary>
    public EntityId Source { get; }

    /// <summary>Health points the hit changes, as a positive number. Handlers may change it (0 = a hit without
    /// effect: the target still reacts). Negative values count as 0.</summary>
    public int Amount
    {
        get => _amount;
        set => _amount = Math.Max(0, value);
    }

    /// <summary>The amount the engine (or the Combat call) produced, before any handler.</summary>
    public int OriginalAmount { get; }
    /// <summary>The target's health when the event was built (-1 unknown, e.g. a destructible prop without Health yet).</summary>
    public int HealthBefore { get; }
    /// <summary>The target's maximum health when the event was built (-1 when unknown).</summary>
    public int HealthMax { get; }
    /// <summary>The engine's hit flags: what kind of hit this is (blocked, critical, backstab and so on).</summary>
    public HitFlags Flags { get; }
    /// <summary>The engine's damage type mask (Cut, Pierce, Blunt, Fire, Ice, ...; bit order of DamageSetup).</summary>
    public ushort DamageTypes { get; }
    /// <summary>How hard the hit makes the target stagger, as the engine decided it. Hits that push or stagger also raise an
    /// <see cref="EntityKnockbackEvent"/>, where the reaction can be changed.</summary>
    public HitReaction Reaction { get; }
    /// <summary>The engine's attack category of the hit, as the raw number the game uses.</summary>
    public byte AttackCategory { get; }
    /// <summary>The engine's weapon category of the hit, as the raw number the game uses.</summary>
    public byte WeaponCategory { get; }
    /// <summary>Where the hit came from: the engine or a <see cref="Combat"/> call.</summary>
    public CombatOrigin Origin { get; }
    /// <summary>True when the target is a player.</summary>
    public bool IsPlayer { get; }
    /// <summary>The engine's impact instance id (one swing or projectile).</summary>
    public uint ImpactId { get; }
    /// <summary>Hit point (metres).</summary>
    public WorldPosition Position { get; }
    /// <summary>Sim step in which the engine produced the hit.</summary>
    public ulong Step { get; }
}

/// <summary>An entity is about to take damage from a hit (Forge: LivingAttackEvent + LivingHurtEvent +
/// LivingDamageEvent in one). Cancel it and the hit never happens (no health change, no reaction, no HitEvent);
/// change <see cref="PendingHitEvent.Amount"/> to change the damage.</summary>
/// <remarks>
/// Game hits: while anything subscribes, Kiln holds every entity hit for one sim step (about 41 ms on a dedicated
/// server) so the handlers run on the tick thread before it applies; with no subscriber hits are untouched.
/// <see cref="Combat.Damage"/> hits fire this synchronously inside the call. Hits that bypass the hit pipeline are not
/// seen: fall damage has its own <see cref="EntityFallEvent"/>; drowning and other direct losses only
/// <see cref="EntityDeathEvent"/> / <see cref="Combat.SetInvulnerable"/>.
/// Damage is "true damage" here: armour and resistances were already applied by the engine.
/// </remarks>
public sealed class EntityHurtEvent : PendingHitEvent
{
    internal EntityHurtEvent(in CombatNative.Event e) : base(e) => Lethal = e.Lethal != 0;

    internal EntityHurtEvent(EntityId target, EntityId attacker, EntityId source, int amount, int healthBefore, int healthMax, bool isPlayer)
        : base(target, attacker, source, amount, healthBefore, healthMax, isPlayer) =>
        Lethal = healthBefore > 0 && amount >= healthBefore;

    /// <summary>The original amount would kill the target (cancel or lower it to keep the target alive).</summary>
    public bool Lethal { get; }
}

/// <summary>What healed an entity (<see cref="EntityHealEvent.Cause"/>).</summary>
public enum HealCause
{
    /// <summary>A healing hit through the hit pipeline (spells, healing projectiles). Held one step like hurt hits.</summary>
    Hit = 0,
    /// <summary><see cref="Combat.Heal"/>.</summary>
    Kiln = 1,
    /// <summary>The game's natural regeneration (health_recharge, also regen buffs that raise its rate). Held one step:
    /// cancel = the points are not added, Amount writable.</summary>
    Regeneration = 2,
    /// <summary>An effect that adds Health directly (the impact native changeAttributeValue: potions, food, skills).
    /// Reported in the Pre after it applied (the client saw it): cancel = Kiln takes it back, a lower or higher Amount
    /// is corrected; never below 1 health for an entity that was alive.</summary>
    Attribute = 3,
}

/// <summary>An entity is about to be healed (Forge: LivingHealEvent): healing hits (spells), natural regeneration,
/// effects that add health directly (potions, food), and <see cref="Combat.Heal"/>; see <see cref="Cause"/>.
/// Cancelable; <see cref="PendingHitEvent.Amount"/> is writable.</summary>
public sealed class EntityHealEvent : PendingHitEvent
{
    internal EntityHealEvent(in CombatNative.Event e) : base(e) => Cause = (HealCause)e.Cause;

    internal EntityHealEvent(EntityId target, int amount, int healthBefore, int healthMax, bool isPlayer)
        : base(target, EntityId.None, EntityId.None, amount, healthBefore, healthMax, isPlayer) => Cause = HealCause.Kiln;

    /// <summary>What heals the entity: a healing hit, <see cref="Combat.Heal"/>, natural regeneration or an effect that adds
    /// health directly.</summary>
    public HealCause Cause { get; }
}

/// <summary>A hit is about to push an entity back or make it stagger (Forge: LivingKnockBackEvent). Posted after the
/// hit's <see cref="EntityHurtEvent"/> (not at all when that was canceled), for the same held game hits, and inside
/// <see cref="Combat.Damage"/> for Kiln hits with a push-back or reaction. Cancel: no push-back and no hit reaction
/// (the damage still applies). <see cref="Strength"/>, <see cref="Direction"/> and <see cref="Reaction"/> are
/// writable.</summary>
/// <remarks>How far a strength pushes is up to the target's own logic (enemies turn it into an impulse, players into
/// a stagger); without a player near, creatures are not simulated and do not move.</remarks>
public sealed class EntityKnockbackEvent : CancelableEvent, IEntityEvent
{
    /// <summary>For event filters (<see cref="EventFilter"/>): the target, the part hit, the attacker or the source. Since 0.2.0-dev.27.</summary>
    public bool Involves(EntityId entity) => !entity.IsNone && (Target == entity || Part == entity || Attacker == entity || Source == entity);

    private int _strength;

    internal EntityKnockbackEvent(in CombatNative.Event e)
    {
        Target = new EntityId(e.Target);
        Part = new EntityId(e.Part);
        Attacker = new EntityId(e.Attacker);
        Source = new EntityId(e.Source);
        _strength = OriginalStrength = e.PushBack;
        Direction = OriginalDirection = new System.Numerics.Vector3((float)e.Direction.X, (float)e.Direction.Y, (float)e.Direction.Z);
        Reaction = OriginalReaction = (HitReaction)e.Reaction;
        Flags = (HitFlags)e.HitFlags;
        Origin = (CombatOrigin)e.Origin;
        IsPlayer = e.IsPlayer != 0;
        ImpactId = e.ImpactId;
        Position = new WorldPosition(e.Position.X, e.Position.Y, e.Position.Z);
        Step = e.Step;
        Id = e.Id;
    }

    internal EntityKnockbackEvent(EntityId target, EntityId attacker, EntityId source, int strength, HitReaction reaction, bool isPlayer)
    {
        Target = Part = target;
        Attacker = attacker;
        Source = source.IsNone ? attacker : source;
        _strength = OriginalStrength = strength;
        Reaction = OriginalReaction = reaction;
        Origin = CombatOrigin.Kiln;
        IsPlayer = isPlayer;
    }

    internal ulong Id { get; }

    /// <summary>The entity whose health the hit changes (a creature, not its weakspot).</summary>
    public EntityId Target { get; }
    /// <summary>The entity the hit landed on (a weakspot or body part); equals <see cref="Target"/> otherwise.</summary>
    public EntityId Part { get; }
    /// <summary>The root attacker (the player for player hits, the creature for creature hits); None if unknown.</summary>
    public EntityId Attacker { get; }
    /// <summary>The direct source: weapon, projectile, spell or the attacker itself.</summary>
    public EntityId Source { get; }

    /// <summary>Push-back force 0..255 (the engine's pushBackForce). Writable; clamped.</summary>
    public int Strength
    {
        get => _strength;
        set => _strength = Math.Clamp(value, 0, 255);
    }

    /// <summary>The push-back force the engine produced, before any handler changed <see cref="Strength"/>.</summary>
    public int OriginalStrength { get; }

    /// <summary>The hit direction the push follows (attacker → target). Writable; normalised when applied (a zero
    /// vector keeps the original). Not used by <see cref="Combat.Damage"/> hits (they push away from the attacker).</summary>
    public System.Numerics.Vector3 Direction { get; set; }

    /// <summary>The hit direction the engine produced, before any handler changed <see cref="Direction"/>.</summary>
    public System.Numerics.Vector3 OriginalDirection { get; }

    /// <summary>How hard the target staggers. Writable.</summary>
    public HitReaction Reaction { get; set; }

    /// <summary>The hit reaction the engine produced, before any handler changed <see cref="Reaction"/>.</summary>
    public HitReaction OriginalReaction { get; }
    /// <summary>The engine's hit flags of the hit.</summary>
    public HitFlags Flags { get; }
    /// <summary>Where the hit came from: the engine or a <see cref="Combat"/> call.</summary>
    public CombatOrigin Origin { get; }
    /// <summary>True when the target is a player.</summary>
    public bool IsPlayer { get; }
    /// <summary>The engine's impact instance id (one swing or projectile).</summary>
    public uint ImpactId { get; }
    /// <summary>The hit point (metres).</summary>
    public WorldPosition Position { get; }
    /// <summary>The sim step the engine produced the hit in.</summary>
    public ulong Step { get; }
}

/// <summary>An entity is about to take fall damage (Forge: LivingFallEvent). The game's fall damage bypasses hits
/// (fall_damage_infliction writes Health directly), so <see cref="EntityHurtEvent"/> never sees it. Kiln holds it for
/// one step (about 41 ms on a dedicated server) while anything subscribes; the game then applies the decided damage.
/// Cancel: no damage and no hard-landing stagger. <see cref="Damage"/> is writable (0 = cancel).</summary>
/// <remarks>Entities in god mode (<see cref="Combat.SetInvulnerable"/>) arrive canceled. The game computes the damage
/// as a fraction of max health from the fall height between the entity's own start and lethal distances.</remarks>
public sealed class EntityFallEvent : CancelableEvent, IEntityEvent
{
    /// <summary>For event filters (<see cref="EventFilter"/>): the entity that fell. Since 0.2.0-dev.27.</summary>
    public bool Involves(EntityId entity) => !entity.IsNone && (Entity == entity);

    private int _damage;

    internal EntityFallEvent(in CombatNative.Event e)
    {
        Entity = new EntityId(e.Target);
        Distance = e.FallDistance;
        DamageFraction = e.FallFraction;
        _damage = OriginalDamage = e.Amount;
        HealthBefore = e.HealthBefore;
        HealthMax = e.HealthMax;
        Lethal = e.Lethal != 0;
        IsPlayer = e.IsPlayer != 0;
        Position = new WorldPosition(e.Position.X, e.Position.Y, e.Position.Z);
        Step = e.Step;
    }

    /// <summary>The entity that fell.</summary>
    public EntityId Entity { get; }
    /// <summary>Metres fallen (the game's detected fall distance).</summary>
    public float Distance { get; }
    /// <summary>The game's damage as a fraction of max health (1 = lethal from full health).</summary>
    public float DamageFraction { get; }

    /// <summary>Health points the fall takes. Writable (>= 0; 0 cancels).</summary>
    public int Damage
    {
        get => _damage;
        set => _damage = Math.Max(0, value);
    }

    /// <summary>The damage the game computed, before any handler changed <see cref="Damage"/>.</summary>
    public int OriginalDamage { get; }
    /// <summary>The entity's health when the event was built.</summary>
    public int HealthBefore { get; }
    /// <summary>The entity's maximum health when the event was built.</summary>
    public int HealthMax { get; }
    /// <summary>The original damage would kill.</summary>
    public bool Lethal { get; }
    /// <summary>True when the entity is a player.</summary>
    public bool IsPlayer { get; }
    /// <summary>Where the entity is (metres).</summary>
    public WorldPosition Position { get; }
    /// <summary>The sim step the fall was reported for.</summary>
    public ulong Step { get; }
}

/// <summary>A hit changed an entity's health (after the fact; observe only). One event per entity whose Health
/// changed: a hit on a weakspot that propagates to its creature reports the creature.</summary>
public sealed class EntityDamagedEvent : KilnEvent, IEntityEvent
{
    /// <summary>For event filters (<see cref="EventFilter"/>): the target, the attacker or the source. Since 0.2.0-dev.27.</summary>
    public bool Involves(EntityId entity) => !entity.IsNone && (Target == entity || Attacker == entity || Source == entity);

    internal EntityDamagedEvent(in CombatNative.Event e)
    {
        Target = new EntityId(e.Target);
        Attacker = new EntityId(e.Attacker);
        Source = new EntityId(e.Source);
        Amount = e.Amount;
        HealthBefore = e.HealthBefore;
        HealthAfter = e.HealthAfter;
        HealthMax = e.HealthMax;
        Flags = (HitFlags)e.HitFlags;
        Origin = (CombatOrigin)e.Origin;
        IsPlayer = e.IsPlayer != 0;
        Position = new WorldPosition(e.Position.X, e.Position.Y, e.Position.Z);
        Step = e.Step;
        Reaction = (HitReaction)e.Reaction;
        PushBack = e.PushBack;
    }

    /// <summary>The entity whose health changed (a creature, not its weakspot).</summary>
    public EntityId Target { get; }
    /// <summary>The root attacker (the player for player hits, the creature for creature hits); None if unknown.</summary>
    public EntityId Attacker { get; }
    /// <summary>The direct source: weapon, projectile, spell or the attacker itself.</summary>
    public EntityId Source { get; }
    /// <summary>The hit reaction the hit applied with (after <see cref="EntityKnockbackEvent"/> handlers).</summary>
    public HitReaction Reaction { get; }
    /// <summary>The push-back force the hit applied with (after <see cref="EntityKnockbackEvent"/> handlers).</summary>
    public int PushBack { get; }
    /// <summary>Health lost (positive) or gained (negative).</summary>
    public int Amount { get; }
    /// <summary>The target's health before the hit.</summary>
    public int HealthBefore { get; }
    /// <summary>The target's health after the hit.</summary>
    public int HealthAfter { get; }
    /// <summary>The target's maximum health.</summary>
    public int HealthMax { get; }
    /// <summary>The engine's hit flags of the hit.</summary>
    public HitFlags Flags { get; }
    /// <summary>This hit took the target from above 0 to 0 or below.</summary>
    public bool KillingBlow => (Flags & HitFlags.KillingBlow) != 0;
    /// <summary>Where the hit came from: the engine or a <see cref="Combat"/> call.</summary>
    public CombatOrigin Origin { get; }
    /// <summary>True when the target is a player.</summary>
    public bool IsPlayer { get; }
    /// <summary>The hit point (metres).</summary>
    public WorldPosition Position { get; }
    /// <summary>The sim step the hit was reported for.</summary>
    public ulong Step { get; }
}

/// <summary>An entity died (Forge: LivingDeathEvent; here observe only): the game's EntityDiedEvent, posted in the
/// Pre after the game's death program ran. To keep something alive, cancel the lethal <see cref="EntityHurtEvent"/>
/// (<see cref="EntityHurtEvent.Lethal"/>) or use <see cref="Combat.SetInvulnerable"/>: by the time a death is reported
/// the game has already marked the actor dead and other programs (XP, quests, loot) consume the same event.</summary>
public sealed class EntityDeathEvent : KilnEvent, IEntityEvent
{
    /// <summary>For event filters (<see cref="EventFilter"/>): the victim, the killer or the source. Since 0.2.0-dev.27.</summary>
    public bool Involves(EntityId entity) => !entity.IsNone && (Victim == entity || Killer == entity || Source == entity);

    internal EntityDeathEvent(in CombatNative.Event e)
    {
        Victim = new EntityId(e.Target);
        Killer = new EntityId(e.Attacker);
        Source = new EntityId(e.Source);
        Origin = (CombatOrigin)e.Origin;
        Flags = (HitFlags)e.HitFlags;
        IsPlayer = e.IsPlayer != 0;
        Position = new WorldPosition(e.Position.X, e.Position.Y, e.Position.Z);
        Health = e.HealthAfter;
        Step = e.Step;
    }

    /// <summary>The entity that died.</summary>
    public EntityId Victim { get; }
    /// <summary>Root attacker of the killing blow (a player, a creature) when the death came from a hit Kiln saw;
    /// None for deaths without a hit (fall damage, SetHealth(0), scripts).</summary>
    public EntityId Killer { get; }
    /// <summary>The killing blow's direct source (weapon, projectile).</summary>
    public EntityId Source { get; }
    /// <summary>Game/Kiln: the killing blow was a hit of that origin; Other: no hit.</summary>
    public CombatOrigin Origin { get; }
    /// <summary>Flags of the killing blow (KillingBlow is always set when there was one).</summary>
    public HitFlags Flags { get; }
    /// <summary>True when the victim is a player.</summary>
    public bool IsPlayer { get; }
    /// <summary>Where the victim was when the death was reported (metres).</summary>
    public WorldPosition Position { get; }
    /// <summary>The victim's health when the death was reported.</summary>
    public int Health { get; }
    /// <summary>The sim step the death was reported for.</summary>
    public ulong Step { get; }
}

/// <summary>A dead player is alive again (Forge: PlayerRespawnEvent): respawned at a flame altar / bed or revived by
/// another player. Detected when the game clears the player's dead state (its Health is back above 0).</summary>
public sealed class PlayerRespawnEvent : KilnEvent, IEntityEvent
{
    /// <summary>For event filters (<see cref="EventFilter"/>): the player. Since 0.2.0-dev.27.</summary>
    public bool Involves(EntityId entity) => !entity.IsNone && (Player == entity);

    internal PlayerRespawnEvent(in CombatNative.Event e)
    {
        Player = new EntityId(e.Target);
        Health = e.HealthAfter;
        HealthMax = e.HealthMax;
        Position = new WorldPosition(e.Position.X, e.Position.Y, e.Position.Z);
        Step = e.Step;
    }

    /// <summary>The player that is alive again.</summary>
    public EntityId Player { get; }
    /// <summary>The player's health after the respawn.</summary>
    public int Health { get; }
    /// <summary>The player's maximum health.</summary>
    public int HealthMax { get; }
    /// <summary>Where the player is (metres).</summary>
    public WorldPosition Position { get; }
    /// <summary>The sim step the respawn was reported for.</summary>
    public ulong Step { get; }
}

/// <summary>A loot container appeared at a corpse (Forge: LivingDropsEvent, one event per container): an entity of a
/// LootContainer_* template with the game's DestroyOnLoot tag (the loot spot / bag the game spawns for a killed creature) within 4 m of
/// the victim within 30 s of its death, or the corpse itself once it turned into loot (it has an Inventory).
/// Cancel to remove the container (it is destroyed, with its items). To change the items, use Kiln.Items on
/// <see cref="Container"/> (read, give, take: it is an ordinary inventory). Posted in Pre, about 1 s after the container
/// appeared, so the game has filled it.</summary>
/// <remarks>The game spawns loot from the creature's death sequence, which only runs near a player: a creature killed
/// where no player is has no loot (and no game EntityDiedEvent) until one comes near.</remarks>
public sealed class EntityDropsEvent : CancelableEvent, IEntityEvent
{
    /// <summary>For event filters (<see cref="EventFilter"/>): the victim, the container or the killer. Since 0.2.0-dev.27.</summary>
    public bool Involves(EntityId entity) => !entity.IsNone && (Victim == entity || Container == entity || Killer == entity);

    internal EntityDropsEvent(in CombatNative.Event e)
    {
        Victim = new EntityId(e.Target);
        Container = new EntityId(e.Part);
        Killer = new EntityId(e.Attacker);
        Position = new WorldPosition(e.Position.X, e.Position.Y, e.Position.Z);
        Step = e.Step;
    }

    /// <summary>The entity whose death produced the container.</summary>
    public EntityId Victim { get; }
    /// <summary>The loot container entity (may equal <see cref="Victim"/> when the corpse itself became loot).</summary>
    public EntityId Container { get; }
    /// <summary>The root attacker of the killing blow when Kiln saw one; None otherwise.</summary>
    public EntityId Killer { get; }
    /// <summary>Where the container is (metres).</summary>
    public WorldPosition Position { get; }
    /// <summary>The sim step the container was reported for.</summary>
    public ulong Step { get; }
}

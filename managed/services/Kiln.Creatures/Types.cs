// SPDX-License-Identifier: MIT
// Public value types of Kiln.Creatures. Enum values are the game's own (keen::enemy01::State, StateCondition,
// keen::ecs::EnemyFeature, keen::FactionType), from the reflection of build 1285601.
namespace Kiln;

/// <summary>What a creature template is (by the AI program its entities run).</summary>
public enum CreatureKind
{
    /// <summary>Not a creature template Kiln knows (the kind is unknown).</summary>
    None = 0,
    /// <summary>Enemy AI (enemy01): monsters, bosses, summons and companions.</summary>
    Enemy = 1,
    /// <summary>Wildlife (the game's wildlife AI program).</summary>
    Wildlife = 2,
    /// <summary>Farm animals and pets.</summary>
    Animal = 3,
    /// <summary>Villagers and workshop NPCs.</summary>
    Villager = 4,
}

/// <summary>keen::FactionType.</summary>
public enum Faction : byte
{
    /// <summary>The environment: never hostile to anyone.</summary>
    Environment = 0,
    /// <summary>Hostile to every faction.</summary>
    HitAll = 1,
    /// <summary>Fogger creatures.</summary>
    Fogger = 2,
    /// <summary>Vukah.</summary>
    Vukah = 3,
    /// <summary>Scavengers.</summary>
    Scavenger = 4,
    /// <summary>Wildlife.</summary>
    Wildlife = 5,
    /// <summary>Wild beasts.</summary>
    WildBeast = 6,
    /// <summary>Players. Players, <see cref="AutomatedPlayer"/> and <see cref="PlayerAlly"/> are all on the players' side
    /// and never hostile to each other.</summary>
    Player = 7,
    /// <summary>Automated players (the game's AutomatedPlayer template). On the players' side.</summary>
    AutomatedPlayer = 8,
    /// <summary>The game's HitOthers faction.</summary>
    HitOthers = 9,
    /// <summary>Skeletons.</summary>
    Skeleton = 10,
    /// <summary>Fights on the players' side (summons, companions).</summary>
    PlayerAlly = 11,
    /// <summary>Farm animals.</summary>
    FarmAnimal = 12,
    /// <summary>Cyclops.</summary>
    Cyclops = 13,
    /// <summary>Drak.</summary>
    Drak = 14,
    /// <summary>The Ancients.</summary>
    Ancients = 15,
    /// <summary>No faction (the entity has no Faction component).</summary>
    None = 255,
}

/// <summary>enemy01::State: the enemy AI's state machine.</summary>
public enum EnemyState : byte
{
    /// <summary>No state: the creature has no enemy AI, or it is gone.</summary>
    Invalid = 0,
    /// <summary>The state machine's first state.</summary>
    Start = 1,
    /// <summary>At rest with nothing to react to. A selected target moves it to <see cref="Combat"/>.</summary>
    Idle = 2,
    /// <summary>Dead.</summary>
    Dead = 3,
    /// <summary>Waiting for the game to mark the actor ready (only near players on a dedicated server).</summary>
    Spawning = 4,
    /// <summary>Going to look into something it noticed (<see cref="StateCondition.NeedsToInvestigate"/>).</summary>
    Investigate = 5,
    /// <summary>Fighting its target.</summary>
    Combat = 6,
    /// <summary>Walking back to its home zone.</summary>
    ReturnHome = 7,
    /// <summary>Running home out of a fight (for example when the target is outside its leash).</summary>
    FleeHome = 8,
    /// <summary>Waiting at its home position.</summary>
    WaitAtHome = 9,
    /// <summary>Reacting to a noise it heard (<see cref="StateCondition.NoiseHeard"/>).</summary>
    ReactToNoise = 10,
    /// <summary>Patrolling.</summary>
    Patrol = 11,
    /// <summary>Turning around after a patrol.</summary>
    TurnAfterPatrol = 12,
    /// <summary>Waiting after a patrol.</summary>
    WaitAfterPatrol = 13,
    /// <summary>An attack is in control of the creature (<see cref="StateCondition.IsControlledByAttack"/>).</summary>
    ControlledByAttack = 14,
    /// <summary>Roaming around its home zone.</summary>
    Roaming = 15,
    /// <summary>Fleeing.</summary>
    Flee = 16,
    /// <summary>Waiting after fleeing.</summary>
    WaitAfterFlee = 17,
    /// <summary>Falling.</summary>
    Falling = 18,
    /// <summary>Playing a hit reaction.</summary>
    Hit = 19,
    /// <summary>Guarding an entity: walks to the guarded entity and stays with it (see <see cref="CreatureCommands.Guard(EntityId)"/>).</summary>
    Guard = 20,
    /// <summary>Interacting with something.</summary>
    Interact = 21,
    /// <summary>Landing after a fall.</summary>
    Landing = 22,
    /// <summary>Blocking a projectile.</summary>
    BlockProjectile = 23,
    /// <summary>Moving to a marker.</summary>
    MoveToMarker = 24,
    /// <summary>Sleeping.</summary>
    Sleep = 25,
    /// <summary>Circling its target.</summary>
    Circling = 26,
    /// <summary>Moving away from water.</summary>
    AvoidWater = 27,
    /// <summary>Being revived.</summary>
    Revive = 28,
    /// <summary>Stunned (for example when exhausted: <see cref="StateCondition.IsExhausted"/>).</summary>
    Stunned = 29,
}

/// <summary>enemy01::StateCondition: why the state machine moved.</summary>
public enum StateCondition : byte
{
    /// <summary>The creature died.</summary>
    Death = 0,
    /// <summary>The AI selected a combat target.</summary>
    TargetSelected = 1,
    /// <summary>The AI lost its combat target.</summary>
    TargetLost = 2,
    /// <summary>The current state finished.</summary>
    IsFinished = 3,
    /// <summary>The target cannot be reached.</summary>
    TargetUnreachable = 4,
    /// <summary>The target is outside the creature's leash.</summary>
    TargetOutsideLeash = 5,
    /// <summary>The creature heard a noise.</summary>
    NoiseHeard = 6,
    /// <summary>The creature heard a projectile.</summary>
    ProjectileHeard = 7,
    /// <summary>Its awareness fell to the no-awareness level.</summary>
    HasReachedNoAwarenessState = 8,
    /// <summary>Its awareness reached the low level.</summary>
    HasReachedLowAwarenessState = 9,
    /// <summary>Its awareness reached the high level.</summary>
    HasReachedHighAwarenessState = 10,
    /// <summary>The creature is ready to patrol.</summary>
    IsReadyToPatrol = 11,
    /// <summary>The creature is ready to interact.</summary>
    IsReadyToInteract = 12,
    /// <summary>The creature is ready to trigger an alarm.</summary>
    IsReadyToTriggerAlarm = 13,
    /// <summary>An attack took control of the creature.</summary>
    IsControlledByAttack = 14,
    /// <summary>The creature is ready to roam.</summary>
    IsReadyForRoaming = 15,
    /// <summary>The creature is ready to circle its target.</summary>
    IsReadyForCircling = 16,
    /// <summary>The creature wants to flee.</summary>
    WantsToFlee = 17,
    /// <summary>The creature has been hit.</summary>
    HasBeenHit = 18,
    /// <summary>The creature has been hit recently.</summary>
    HasBeenHitRecently = 19,
    /// <summary>A hookshot targeted the creature recently.</summary>
    WasHookshotTargetRecently = 20,
    /// <summary>The creature is falling.</summary>
    IsFalling = 21,
    /// <summary>The creature plays a hit reaction.</summary>
    HitReaction = 22,
    /// <summary>The creature needs to guard (follow and protect) an entity.</summary>
    NeedsToGuard = 23,
    /// <summary>The creature wants to land.</summary>
    WantsToLand = 24,
    /// <summary>The creature needs to turn.</summary>
    NeedsToTurn = 25,
    /// <summary>The creature can move to a marker.</summary>
    CanMoveToMarker = 26,
    /// <summary>The creature wants to sleep.</summary>
    WantsToSleep = 27,
    /// <summary>The creature wants to move away from water.</summary>
    WantsToAvoidWater = 28,
    /// <summary>The creature is to be revived.</summary>
    Revive = 29,
    /// <summary>The creature is exhausted (moves it to <see cref="EnemyState.Stunned"/>).</summary>
    IsExhausted = 30,
    /// <summary>The creature is helping another enemy.</summary>
    IsHelpingEnemy = 31,
    /// <summary>The creature needs to investigate something.</summary>
    NeedsToInvestigate = 32,
    /// <summary>The state was set without a condition Kiln saw.</summary>
    Unknown = 255,
}

/// <summary>ecs::EnemyFeature as a mask (bit = the game's enum value).</summary>
[Flags]
public enum EnemyFeatures : uint
{
    /// <summary>No features.</summary>
    None = 0,
    /// <summary>The creature uses taunts.</summary>
    UseTaunt = 1u << 0,
    /// <summary>The creature uses the game's threat system to choose its target.</summary>
    UseThreatSystem = 1u << 1,
    /// <summary>The creature can block.</summary>
    UseBlocking = 1u << 2,
    /// <summary>The creature can move.</summary>
    Move = 1u << 3,
    /// <summary>The creature can prowl.</summary>
    Prowl = 1u << 4,
    /// <summary>The creature can rotate.</summary>
    Rotate = 1u << 5,
    /// <summary>The creature can rotate while standing.</summary>
    RotateWhileStanding = 1u << 6,
    /// <summary>The creature can patrol (<see cref="EnemyState.Patrol"/>).</summary>
    Patrol = 1u << 7,
    /// <summary>The creature can roam (<see cref="EnemyState.Roaming"/>).</summary>
    Roaming = 1u << 8,
    /// <summary>The creature plays a spawn sequence when it spawns.</summary>
    ShowSpawnSequence = 1u << 9,
    /// <summary>The creature can walk back to its home zone (<see cref="EnemyState.ReturnHome"/>).</summary>
    ReturnHome = 1u << 10,
    /// <summary>The creature can run home out of a fight (<see cref="EnemyState.FleeHome"/>).</summary>
    FleeHome = 1u << 11,
    /// <summary>The creature can fight (<see cref="EnemyState.Combat"/>).</summary>
    Combat = 1u << 12,
    /// <summary>The creature can interact (<see cref="EnemyState.Interact"/>).</summary>
    Interact = 1u << 13,
    /// <summary>The creature can trigger an alarm.</summary>
    TriggerAlarm = 1u << 14,
    /// <summary>The creature can call other creatures for help.</summary>
    CallForHelp = 1u << 15,
    /// <summary>The creature can run.</summary>
    Run = 1u << 16,
    /// <summary>The creature can turn while idle.</summary>
    IdleTurn = 1u << 17,
    /// <summary>The creature can block projectiles (<see cref="EnemyState.BlockProjectile"/>).</summary>
    BlockProjectiles = 1u << 18,
    /// <summary>The creature can search for cover.</summary>
    SearchForCover = 1u << 19,
    /// <summary>The creature can move to a marker (<see cref="EnemyState.MoveToMarker"/>).</summary>
    MoveToMarker = 1u << 20,
    /// <summary>The creature can sleep (<see cref="EnemyState.Sleep"/>).</summary>
    Sleep = 1u << 21,
    /// <summary>The creature can run around.</summary>
    RunAround = 1u << 22,
    /// <summary>The creature can circle its target (<see cref="EnemyState.Circling"/>).</summary>
    Circling = 1u << 23,
    /// <summary>The creature can be stunned (<see cref="EnemyState.Stunned"/>).</summary>
    StunnedState = 1u << 24,
    /// <summary>The creature has awareness of what is around it (<see cref="CreatureInfo.Awareness"/>).</summary>
    Awareness = 1u << 25,
    /// <summary>The creature can investigate (<see cref="EnemyState.Investigate"/>).</summary>
    Investigate = 1u << 26,
}

/// <summary>DamageSusceptibility slots (attribute structure order). 1.0 = normal damage taken, 0 = immune, 2 = double.
/// Fire/Fog/Ice/Lightning sit under Magical; Magical itself is computed from its base/multiplier/adder.</summary>
public enum DamageKind
{
    /// <summary>The all-damage slot (slot 0).</summary>
    All = 0,
    /// <summary>Physical damage.</summary>
    Physical = 1,
    /// <summary>Cutting damage.</summary>
    Cut = 2,
    /// <summary>Piercing damage.</summary>
    Pierce = 3,
    /// <summary>Blunt damage.</summary>
    Blunt = 4,
    /// <summary>Wood damage.</summary>
    Wood = 5,
    /// <summary>Stone damage.</summary>
    Stone = 6,
    /// <summary>Metal damage.</summary>
    Metal = 7,
    /// <summary>Poison damage.</summary>
    Poison = 8,
    /// <summary>Explosion damage.</summary>
    Explosion = 9,
    /// <summary>Skill-shot damage.</summary>
    SkillShot = 10,
    /// <summary>Magical damage as a whole, computed from <see cref="MagicalBase"/>, <see cref="MagicalMultiplier"/> and
    /// <see cref="MagicalAdder"/>.</summary>
    Magical = 11,
    /// <summary>The base value of <see cref="Magical"/>.</summary>
    MagicalBase = 12,
    /// <summary>The adder of <see cref="Magical"/>.</summary>
    MagicalAdder = 13,
    /// <summary>The multiplier of <see cref="Magical"/>.</summary>
    MagicalMultiplier = 14,
    /// <summary>Fire damage (under <see cref="Magical"/>).</summary>
    Fire = 15,
    /// <summary>Fog damage (under <see cref="Magical"/>).</summary>
    Fog = 16,
    /// <summary>Ice damage (under <see cref="Magical"/>).</summary>
    Ice = 17,
    /// <summary>Lightning damage (under <see cref="Magical"/>).</summary>
    Lightning = 18,
}

/// <summary>Template flags from the catalog.</summary>
[Flags]
public enum CreatureFlags : uint
{
    /// <summary>No flags.</summary>
    None = 0,
    /// <summary>The template has the game's <c>Boss</c> component.</summary>
    Boss = 1,
    /// <summary>The template has <c>EnemyPhases</c>: the game's own phases.</summary>
    Phases = 2,
    /// <summary>Entities of the template can reach the save (no DoNotSave).</summary>
    Saved = 4,
    /// <summary>The template can guard players (<c>guardSetup.canGuardPlayer</c>).</summary>
    GuardsPlayer = 8,
    /// <summary>The template's faction is <see cref="Faction.Player"/> or <see cref="Faction.PlayerAlly"/>.</summary>
    Ally = 16,
    /// <summary>The template is a weakspot child (<c>Enemy_Weakspot_*</c>).</summary>
    Weakspot = 32,
    /// <summary>The template has a boss health bar.</summary>
    BossHealthBar = 64,
    /// <summary>The template is listed in Kiln's shipped catalog.</summary>
    Catalog = 1u << 8,
    /// <summary>The running game has the template.</summary>
    Loaded = 1u << 9,
    /// <summary>Not in Kiln's shipped catalog: found in the engine (a kiln.content clone, or a newer game build). Only
    /// the name, GUID and kind are known.</summary>
    Runtime = 1u << 10,
}

/// <summary>One creature/NPC template of the catalog.</summary>
/// <param name="Id">The template's GUID.</param>
/// <param name="Name">The template name (<c>Enemy_Vukah_Melee</c> style).</param>
/// <param name="DisplayName">The in-game name when the shipped catalog has it, else "".</param>
/// <param name="Kind">What AI program the template's entities run.</param>
/// <param name="Flags">What the catalog knows about the template (boss, phases, saved, loaded, ...).</param>
/// <param name="Faction">The template's faction (<see cref="Faction.None"/> when it has none).</param>
/// <param name="Level">The template's own level: its <c>Level</c> attribute. Spawners and level overrides change the level of an entity.</param>
/// <param name="LevelMax">The template's maximum level.</param>
/// <param name="EnemyClass">0 Melee, 1 Ranger, 2 Mage; -1 when the template is not an enemy. <see cref="CreatureTemplate.ClassName"/> has the name.</param>
/// <param name="Arsenal">The GUID of the arsenal (attacks and rules) its enemy AI uses; all zero when it has none.</param>
/// <param name="LootLabels">The GUID of the loot label collection its drops are rolled from (<c>RandomLoot.defaultLabels</c>).</param>
/// <param name="LootSlotsMin">How many loot slots are rolled at least.</param>
/// <param name="LootSlotsMax">How many loot slots are rolled at most.</param>
/// <param name="XpMod">The XP factor of a kill (<c>ExperienceSource.xpMod</c>).</param>
/// <param name="Scale">The template's uniform scale (1 when it has none).</param>
/// <param name="Model">The model resource's GUID. Client data, given for reference.</param>
/// <param name="AnimationGraph">The animation graph's GUID. Templates that share it share a skeleton family.</param>
public sealed record CreatureTemplate(
    Guid Id, string Name, string DisplayName, CreatureKind Kind, CreatureFlags Flags, Faction Faction, int Level,
    int LevelMax, int EnemyClass, Guid Arsenal, Guid LootLabels, int LootSlotsMin, int LootSlotsMax, float XpMod,
    float Scale, Guid Model, Guid AnimationGraph)
{
    /// <summary>The template is a boss (<see cref="CreatureFlags.Boss"/>).</summary>
    public bool IsBoss => Flags.HasFlag(CreatureFlags.Boss);
    /// <summary>The template is a villager or workshop NPC (<see cref="CreatureKind.Villager"/>).</summary>
    public bool IsVillager => Kind == CreatureKind.Villager;
    /// <summary>Entities of the template can reach the save (<see cref="CreatureFlags.Saved"/>).</summary>
    public bool IsSaved => Flags.HasFlag(CreatureFlags.Saved);
    /// <summary>The running game has the template (<see cref="CreatureFlags.Loaded"/>).</summary>
    public bool IsLoaded => Flags.HasFlag(CreatureFlags.Loaded);
    /// <summary>Found in the engine only (kiln.content clones): <see cref="CreatureFlags.Runtime"/>.</summary>
    public bool IsRuntime => Flags.HasFlag(CreatureFlags.Runtime);
    /// <summary>"Melee", "Ranger", "Mage" or "".</summary>
    public string ClassName => EnemyClass switch { 0 => "Melee", 1 => "Ranger", 2 => "Mage", _ => "" };
    /// <summary>The template name, with the display name in brackets when there is one.</summary>
    public override string ToString() => DisplayName.Length > 0 ? $"{Name} ({DisplayName})" : Name;
}

/// <summary>A snapshot of one creature (<see cref="CreatureRef.Inspect"/>).</summary>
/// <param name="Entity">The creature's entity.</param>
/// <param name="Template">The GUID of its template.</param>
/// <param name="TemplateName">The template's name ("" when Kiln does not know it).</param>
/// <param name="Kind">The kind of its template (<see cref="CreatureKind.None"/> when unknown).</param>
/// <param name="HasAi">The entity has enemy AI state. <paramref name="State"/> is <see cref="EnemyState.Invalid"/> when it has not.</param>
/// <param name="State">The enemy AI's state (<see cref="EnemyState.Invalid"/> without enemy AI).</param>
/// <param name="AwarenessState">The game's awareness level of the creature (the <c>awarenessState</c> of its <c>NetworkEnemy</c> state, as a raw byte).</param>
/// <param name="Awareness">The creature's awareness value (the <c>awareness</c> of its <c>NetworkEnemy</c> state).</param>
/// <param name="Faction">Its faction now (<see cref="Faction.None"/> when it has none).</param>
/// <param name="Level">Its level now.</param>
/// <param name="LevelMax">The maximum level it can have.</param>
/// <param name="Health">Its health now.</param>
/// <param name="HealthMax">Its maximum health.</param>
/// <param name="Scale">Its uniform scale (<see cref="CreatureRef.Scale"/>).</param>
/// <param name="XpFactor">The XP factor of a kill (<see cref="CreatureRef.XpFactor"/>).</param>
/// <param name="SpeedFactor">The factor on its movement speeds (<see cref="CreatureRef.SpeedFactor"/>).</param>
/// <param name="DamageMultiplier">The multiplier of its damage (<c>DamageMod_Multiplier</c>, <see cref="CreatureRef.DamageFactor"/>).</param>
/// <param name="TemplateFeatures">The enemy AI features its template has.</param>
/// <param name="EnabledFeatures">Features switched on for this one entity, on top of the template's.</param>
/// <param name="DisabledFeatures">Features switched off for this one entity.</param>
/// <param name="Target">Its combat target (<c>SelectedTargets[0]</c>); <see cref="EntityId.None"/> when it has none.</param>
/// <param name="Guard">The entity it guards (<c>guardEntityId</c>); <see cref="EntityId.None"/> when it guards none.</param>
/// <param name="Home">Its home zone position: where it roams around and returns to.</param>
/// <param name="LootLabels">The GUID of the loot label collection its drops are rolled from.</param>
/// <param name="LootSlotsMin">How many loot slots are rolled at least.</param>
/// <param name="LootSlotsMax">How many loot slots are rolled at most.</param>
/// <param name="LootDisabled">Random loot is switched off.</param>
/// <param name="Dead">The actor's dead bit is set.</param>
/// <param name="Ready">The actor is ready for gameplay: the game sets this near players.</param>
/// <param name="ForcedReady">The lab readiness switch holds this entity ready (<c>CreatureRef.ForceReady</c>, a lab-only switch).</param>
/// <param name="Layers">How many values mods changed on this entity.</param>
/// <param name="PrivateCopies">How many private template copies the entity reads (<see cref="CreatureRef.Ai"/>).</param>
public sealed record CreatureInfo(
    EntityId Entity, Guid Template, string TemplateName, CreatureKind Kind, bool HasAi, EnemyState State,
    byte AwarenessState, float Awareness, Faction Faction, int Level, int LevelMax, int Health, int HealthMax,
    float Scale, float XpFactor, float SpeedFactor, float DamageMultiplier, EnemyFeatures TemplateFeatures,
    EnemyFeatures EnabledFeatures, EnemyFeatures DisabledFeatures, EntityId Target, EntityId Guard, WorldPosition Home,
    Guid LootLabels, int LootSlotsMin, int LootSlotsMax, bool LootDisabled, bool Dead, bool Ready, bool ForcedReady,
    int Layers, int PrivateCopies);

/// <summary>What kiln.creatures can do on this build (<see cref="Creatures.Status"/>).</summary>
/// <param name="Slots">Private template copies work (the template slot layout resolved).</param>
/// <param name="AiHooks">The enemy AI state hooks are installed.</param>
/// <param name="ReadyBit">The actor readiness bit resolved.</param>
/// <param name="Arsenals">The arsenal registry is readable.</param>
/// <param name="Reflection">Field paths resolve through the game's reflection.</param>
/// <param name="Commands">The command program is active.</param>
/// <param name="Marks">The replication marks program is active.</param>
/// <param name="LabReady">The lab readiness switch file exists (<c>kiln/data/kiln.creatures/force-ready</c>).</param>
/// <param name="Catalog">How many templates the catalog has.</param>
/// <param name="CatalogLoaded">Of those, how many the running game has loaded.</param>
/// <param name="EngineCreaturesMissing">Engine templates with enemy AI or villager state that the shipped catalog lacks (clones). They are added to the catalog as <see cref="CreatureFlags.Runtime"/> entries.</param>
/// <param name="ArsenalCount">How many arsenals the registry has.</param>
/// <param name="Entities">How many entities have value layers or private copies.</param>
/// <param name="Layers">How many value layers are active.</param>
/// <param name="PrivateCopies">How many private template copies are live.</param>
/// <param name="PendingFree">How many private copies wait to be freed.</param>
/// <param name="StateChanges">How many enemy AI state changes Kiln saw (only while someone listens).</param>
/// <param name="EventsDelivered">How many creature events were delivered.</param>
/// <param name="EventsDropped">How many creature events were lost to a ring overflow.</param>
/// <param name="CommandsSent">How many commands were sent.</param>
/// <param name="MarksWritten">How many components the marks program marked for replication.</param>
/// <param name="CopyReads">How many behaviour updates read a private <c>Enemy01BehaviorState</c> copy (a diagnostic counter).</param>
public sealed record CreaturesStatus(
    bool Slots, bool AiHooks, bool ReadyBit, bool Arsenals, bool Reflection, bool Commands, bool Marks, bool LabReady,
    int Catalog, int CatalogLoaded, int EngineCreaturesMissing, int ArsenalCount, int Entities, int Layers,
    int PrivateCopies, int PendingFree, ulong StateChanges, ulong EventsDelivered, ulong EventsDropped,
    ulong CommandsSent, ulong MarksWritten, ulong CopyReads);

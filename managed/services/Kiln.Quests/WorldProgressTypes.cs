// SPDX-License-Identifier: MIT
// The records of world progress (WorldProgress.cs): what the game stores about the world's flame, its difficulty, Ancient
// Spires, Flame Shrines and Sanctums, Shroud Roots and Elixir Wells, Ancient Obelisks, bosses and chests, and how to
// read each fact for the world or for one character. Usage: docs/kiln/services/world-progress.md.
namespace Kiln;

/// <summary>Where the game keeps a fact.</summary>
public enum ProgressScope : byte
{
    /// <summary>The world's knowledge only: one value for everyone.</summary>
    World = 0,
    /// <summary>Each character's own knowledge (saved with the character).</summary>
    Character = 1,
    /// <summary>The world's knowledge, with a per-character twin that a character in personal quest progression reads instead.</summary>
    Both = 2,
}

/// <summary>A fact as the game stores it: a requirement (a knowledge id compared to a value, or a named query) and where
/// it lives. <see cref="Holds(EntityId)"/> evaluates it with the game's own evaluator, so personal progression and the game's
/// rules for world and player objects apply.</summary>
/// <param name="Query">The requirement as the game stores it: a knowledge object compared to a value, or a named query.</param>
/// <param name="Scope">Where the game keeps the fact: the world, each character, or both.</param>
public sealed record ProgressFact(KnowledgeQuery Query, ProgressScope Scope)
{
    /// <summary>The knowledge id (or the named query's id).</summary>
    public uint Id => Query.Id;

    /// <summary>For the world (<paramref name="target"/> None) or a player in game. Tick thread. Null when it cannot be read:
    /// kiln.knowledge is not loaded, the player is not in game, or a character fact asked of the world.</summary>
    [TickThreadOnly]
    public bool? Holds(EntityId target)
    {
        if (!Knowledge.Available) return null;
        if (target.IsNone && Scope == ProgressScope.Character) return null;
        Services.RequireTickThread("ProgressFact.Holds");
        return Knowledge.TryQuery(target, Query, target.IsNone ? KnowledgeMode.Shared : KnowledgeMode.Player);
    }

    /// <summary>For a player in game, as <see cref="Holds(EntityId)"/>.</summary>
    [TickThreadOnly]
    public bool? Holds(Player player) => Holds(player.Entity);

    /// <summary>The requirement and its scope.</summary>
    public override string ToString() => $"{Query} ({Scope})";
}

/// <summary>The world's flame: the Mother Flame's level and the flame altars (the game's NetworkServerProgress on the
/// world's progress entity, the level the Flame Altar menu shows).</summary>
/// <param name="Level">The Mother Flame's level as the game replicates it (the level the Flame Altar menu shows).</param>
/// <param name="Altars">The number of Flame Altars that exist now (not the permitted maximum: see
/// <see cref="WorldProgress.LimitsAt(int)"/>).</param>
/// <param name="HighestAltarLevel">The highest level among the existing Flame Altars.</param>
/// <param name="WaterSources">The number of water sources that exist now (not the permitted maximum).</param>
/// <param name="Entity">The world's progress entity.</param>
public sealed record FlameStatus(int Level, int Altars, int HighestAltarLevel, int WaterSources, EntityId Entity)
{
    /// <summary>The <c>Level</c> attribute of the progress entity: the store the game writes when the flame levels up (and its
    /// admin command writes); <see cref="Level"/> is its replicated copy. Null when kiln.knowledge's flame part is off.</summary>
    public int? Stored { get; init; }

    /// <summary>The game's maximum flame level (the second value of the <c>Level</c> attribute, <c>flameLevelMax</c>).</summary>
    public int? Max { get; init; }

    /// <summary>What the game's own altar, water-source and fog rules read (<c>ServerProgressSystem.progressLevel</c>).</summary>
    public int? SystemLevel { get; init; }

    /// <summary>The flame level, the altars and the water sources.</summary>
    public override string ToString() => $"flame level {Level}, {Altars} altar(s) (highest level {HighestAltarLevel}), {WaterSources} water source(s)";
}

/// <summary>What the game derives from a flame level (the balancing table's per-level lists and the fog rule).</summary>
/// <param name="Level">The flame level asked about.</param>
/// <param name="Altars">How many Flame Altars the world may have (<c>altarsPerFlameLevel</c>).</param>
/// <param name="WaterSources">How many water sources it may have (<c>waterSourcesPerFlameLevel</c>).</param>
/// <param name="FogResistance">The Shroud resistance of a player: <c>playerBaseFogResistance + playerFogResistancePerFlameLevel * level</c>.</param>
/// <param name="FogRemovalLevel">The server setting <c>flameLevelFogRemoval</c> (null when kiln.settings is not ready).</param>
/// <param name="FogRemoved">The level reaches <see cref="FogRemovalLevel"/>.</param>
public sealed record FlameLimits(int Level, int Altars, int WaterSources, int FogResistance, int? FogRemovalLevel, bool? FogRemoved)
{
    /// <summary>The level and its limits as text.</summary>
    public override string ToString() =>
        $"flame level {Level}: {Altars} altar(s), {WaterSources} water source(s), fog resistance {FogResistance}" +
        (FogRemovalLevel is { } f ? $", fog removal from level {f} ({(FogRemoved == true ? "yes" : "no")})" : "");
}

/// <summary>The outcome of <see cref="WorldProgress.SetFlameLevel"/>.</summary>
/// <param name="Request">The native request id (0 when refused at once).</param>
/// <param name="Ok">The requested write completed (see <see cref="Mirrored"/>). False with <see cref="Reason"/> otherwise;
/// a failed rollback can leave changed state, reported explicitly in the reason.</param>
/// <param name="Status">"ok", "unavailable", "internal", ...</param>
/// <param name="Requested">What was asked for.</param>
/// <param name="Before">The stored level before and after the write (after: once the game's attribute formula ran).</param>
/// <param name="Max">The game's maximum level (0 when unknown).</param>
/// <param name="Clamped">The game's formula limited the value.</param>
/// <param name="Marked">The component was marked written (so the game saves and replicates it).</param>
/// <param name="Mirrored"><c>NetworkServerProgress.level</c> followed (what players see).</param>
/// <param name="Steps">Steps from the write until it was mirrored.</param>
/// <param name="After">The stored level after the write (once the game's attribute formula ran).</param>
/// <param name="Reason">Why the write failed or was limited; empty on a plain success.</param>
public sealed record FlameChange(ulong Request, bool Ok, string Status, int Requested, int Before, int After, int Max, bool Clamped,
    bool Marked, bool Mirrored, int Steps, string Reason)
{
    internal static FlameChange Failed(int requested, string status, string reason) =>
        new(0, false, status, requested, 0, 0, 0, false, false, false, 0, reason);

    /// <summary>The outcome as text.</summary>
    public override string ToString() => Ok
        ? $"flame level {Before} -> {After}{(Reason.Length > 0 ? $" ({Reason})" : "")}{(Mirrored ? $", mirrored after {Steps} step(s)" : ", not mirrored yet")}"
        : $"flame level {Requested} refused: {Reason}";
}

/// <summary>The world's difficulty: the server's preset and the game settings in effect (kiln.settings).</summary>
/// <param name="Preset">Default, Relaxed, Hard, Survival or Custom (the game's GameSettingsPreset); null when unknown.</param>
/// <param name="Settings">Every game setting's current value by name, as <c>enshrouded_server.json</c> spells it.</param>
public sealed record WorldDifficulty(string? Preset, IReadOnlyDictionary<string, object> Settings)
{
    /// <summary>True when <see cref="Preset"/> is Custom: the settings were changed by hand.</summary>
    public bool IsCustom => Preset == "Custom";

    /// <summary>A setting as a number (factors, durations in ns, option values); null when absent or not numeric.</summary>
    public double? Number(string name) => Settings.TryGetValue(name, out var v) ? v switch
    {
        double d => d,
        long l => l,
        bool b => b ? 1 : 0,
        _ => null,
    } : null;

    /// <summary>The preset and the number of settings.</summary>
    public override string ToString() => $"preset {Preset ?? "?"}, {Settings.Count} settings";
}

/// <summary>An Ancient Spire: its fast-travel point, the knowledge the game sets when it is unlocked, the per-character
/// "top reached" knowledge and the beacon on top.</summary>
/// <param name="Id">Lower-case, from the fast-travel marker's template name (<c>grassland</c>, <c>capital</c>, ...).</param>
/// <param name="NameTag">Loca tag of the map marker's name (the catalog's displayName).</param>
/// <param name="Template">The fast-travel marker's template name.</param>
/// <param name="Position">Where the spire's fast-travel point is.</param>
/// <param name="Unlock">The fact that the spire's fast travel is unlocked.</param>
/// <param name="TopReached">The fact that a character reached the top; null when the game keeps none for this spire.</param>
/// <param name="Beacon">The fact that the beacon on top is lit; null when the game keeps none for this spire.</param>
public sealed record Spire(string Id, string Template, uint NameTag, WorldPosition Position, ProgressFact Unlock,
    ProgressFact? TopReached, ProgressFact? Beacon)
{
    /// <summary>The spire's fast travel is unlocked in this world (tick thread; null when unreadable).</summary>
    [TickThreadOnly]
    public bool? Unlocked => Unlock.Holds(EntityId.None);
    /// <summary>Unlocked as this character sees it (the world's value, or its own twin in personal progression).</summary>
    [TickThreadOnly]
    public bool? UnlockedFor(Player player) => Unlock.Holds(player.Entity);
    /// <summary>The character reached the top (its own knowledge; the game's spire achievement reads it).</summary>
    [TickThreadOnly]
    public bool? TopReachedBy(Player player) => TopReached?.Holds(player.Entity);
    /// <summary>The beacon on top is lit in this world.</summary>
    [TickThreadOnly]
    public bool? BeaconLit => Beacon?.Holds(EntityId.None);
    /// <summary>The spire's id, template and position.</summary>
    public override string ToString() => $"spire {Id} ({Template}) at {Position}";
}

/// <summary>What kind of flame structure a <see cref="FlameShrine"/> is.</summary>
public enum ShrineKind : byte
{
    /// <summary>A Flame Shrine.</summary>
    Shrine = 0,
    /// <summary>A Flame Sanctum (a flame temple).</summary>
    Sanctum = 1,
}

/// <summary>A Flame Shrine or Flame Sanctum. The game keeps one fact per shrine: a character found it (came in range of
/// its marker; exploration XP is paid then and the game's "all shrines found" achievement counts it).</summary>
/// <param name="Id">Lower-case, from the game's achievement query (<c>grassland_flameshrine_01</c>).</param>
/// <param name="Kind">A Flame Shrine or a Flame Sanctum.</param>
/// <param name="Region">The region, from the game's achievement name (such as <c>grassland</c>).</param>
/// <param name="Template">The marker's template name; null when it is not known.</param>
/// <param name="NameTag">Loca tag of the map marker's name.</param>
/// <param name="Position">Where it is; null when it is not known.</param>
/// <param name="Discovery">The fact that a character found it.</param>
public sealed record FlameShrine(string Id, ShrineKind Kind, string Region, string? Template, uint NameTag, WorldPosition? Position,
    ProgressFact Discovery)
{
    /// <summary>Whether this character found it (tick thread; null when it cannot be read).</summary>
    [TickThreadOnly]
    public bool? FoundBy(Player player) => Discovery.Holds(player.Entity);
    /// <summary>The kind and the id.</summary>
    public override string ToString() => $"{Kind.ToString().ToLowerInvariant()} {Id}";
}

/// <summary>A Shroud Root. Destroying it gives skill points (the root of an Elixir Well 3, the others 1); the game keeps
/// one fact per character.</summary>
/// <param name="ElixirWell">This root is the one at an Elixir Well (the well counts as cleared when it is destroyed).</param>
/// <param name="Id">Lower-case, from the game's achievement name (<c>desert_well_09</c>).</param>
/// <param name="Region">The region, from the game's achievement name.</param>
/// <param name="SkillPoints">The skill points destroying it gives.</param>
/// <param name="Position">Where it is; null until the bosses and roots are read (see
/// <see cref="WorldProgress.BossesReady"/>).</param>
/// <param name="Destruction">The fact that a character destroyed it.</param>
public sealed record ShroudRoot(string Id, string Region, bool ElixirWell, int SkillPoints, WorldPosition? Position, ProgressFact Destruction)
{
    /// <summary>Whether this character destroyed it (tick thread; null when it cannot be read).</summary>
    [TickThreadOnly]
    public bool? DestroyedBy(Player player) => Destruction.Holds(player.Entity);
    /// <summary>Whether it is an Elixir Well root, and the id.</summary>
    public override string ToString() => $"{(ElixirWell ? "elixir well root" : "shroud root")} {Id}";
}

/// <summary>An Ancient Obelisk (reading one reveals its region's shrines).</summary>
/// <param name="Id">Lower-case, from the game's achievement name (<c>wetlands_obelisk_above_fog_read</c>).</param>
/// <param name="Region">The region, from the game's achievement name.</param>
/// <param name="Position">Where it is; null when it is not known.</param>
/// <param name="Reading">The fact that a character read it.</param>
public sealed record Obelisk(string Id, string Region, WorldPosition? Position, ProgressFact Reading)
{
    /// <summary>Whether this character read it (tick thread; null when it cannot be read).</summary>
    [TickThreadOnly]
    public bool? ReadBy(Player player) => Reading.Holds(player.Entity);
    /// <summary>The id.</summary>
    public override string ToString() => $"obelisk {Id}";
}

/// <summary>A boss placed in the world: a creature the game gives a boss health bar, placed directly or by a spawner. The
/// game records a kill for some of them (<see cref="Kill"/>); for the others only the live <see cref="BossKilledEvent"/> tells.</summary>
/// <param name="Id">The boss template's name, lower-case, with <c>_n</c> when the world has several of it.</param>
/// <param name="Spawner">The placed spawner's template, when a spawner places it.</param>
/// <param name="NameTag">Loca tag of the health bar's name.</param>
/// <param name="Template">The boss template's name.</param>
/// <param name="Position">Where it is placed.</param>
/// <param name="Kill">The fact that records its kill; null when the game keeps none (see <see cref="Tracked"/>).</param>
public sealed record Boss(string Id, string Template, string? Spawner, uint NameTag, WorldPosition Position, ProgressFact? Kill)
{
    /// <summary>The game records this boss's kill.</summary>
    public bool Tracked => Kill is not null;
    /// <summary>Killed in this world (tick thread); null when the game keeps no record or it is a character fact.</summary>
    [TickThreadOnly]
    public bool? Killed => Kill?.Holds(EntityId.None);
    /// <summary>Whether this character killed it, for a boss whose kill is a character fact (tick thread; null when the
    /// game keeps no record or it cannot be read).</summary>
    [TickThreadOnly]
    public bool? KilledBy(Player player) => Kill?.Holds(player.Entity);
    /// <summary>The id and the position.</summary>
    public override string ToString() => $"boss {Id} at {Position}";
}

/// <summary>A loot chest placed in the world and its state. <see cref="LootId"/> is the game's loot container id of the
/// placement (stable for a game build). The state is read from the chest's entity, which exists only while its area is
/// loaded (near a player or a base); otherwise <see cref="Loaded"/> is false and the state is null.</summary>
/// <param name="Opened">The chest's loot was generated (it was opened at least once).</param>
/// <param name="Looted">Opened and emptied.</param>
/// <param name="LootId">The game's loot container id of the placement (stable for a game build).</param>
/// <param name="Template">The chest's template name.</param>
/// <param name="Position">Where the chest is placed.</param>
/// <param name="Entity">The chest's entity while its area is loaded; <see cref="EntityId.None"/> otherwise.</param>
public sealed record ChestStatus(uint LootId, string Template, WorldPosition Position, EntityId Entity, bool? Opened, bool? Looted)
{
    /// <summary>True when the chest's entity exists (its area is loaded), so the state can be read.</summary>
    public bool Loaded => !Entity.IsNone;
    /// <summary>The loot id, the template, the position and the state.</summary>
    public override string ToString() =>
        $"chest 0x{LootId:x8} {Template} at {Position}: {(Loaded ? Looted == true ? "looted" : Opened == true ? "opened" : "untouched" : "not loaded")}";
}

/// <summary>A survivor (an NPC of the game's roster with a rescue requirement).</summary>
/// <param name="Id">The NPC's debug name.</param>
/// <param name="NameTag">Loca tag of the NPC's name.</param>
/// <param name="Rescue">The fact that the survivor is rescued.</param>
public sealed record Survivor(string Id, uint NameTag, ProgressFact Rescue)
{
    /// <summary>Whether the survivor is rescued in this world (tick thread; null when it cannot be read).</summary>
    [TickThreadOnly]
    public bool? Rescued => Rescue.Holds(EntityId.None);
    /// <summary>Whether the survivor is rescued as this character sees it (the world's value, or its own twin in personal
    /// progression; tick thread).</summary>
    [TickThreadOnly]
    public bool? RescuedFor(Player player) => Rescue.Holds(player.Entity);
}

/// <summary>How far the index of the game's data is.</summary>
public enum WorldIndexState : byte
{
    /// <summary>The index has not started to build.</summary>
    NotStarted = 0,
    /// <summary>The index is being built in the background.</summary>
    Building = 1,
    /// <summary>The index is built.</summary>
    Ready = 2,
    /// <summary>The index could not be built (see <see cref="WorldProgressStatus.Problems"/>).</summary>
    Failed = 3,
}

/// <summary>What world progress can read on this server, and how far its index of the game's data is.</summary>
/// <param name="Places">Spires, shrines, roots, obelisks (the query db and the scene's global chunk).</param>
/// <param name="Bosses">Bosses and root positions (every chunk of the scene and every template).</param>
/// <param name="Knowledge">kiln.knowledge is loaded.</param>
/// <param name="World">kiln.world is loaded.</param>
/// <param name="Settings">kiln.settings is loaded.</param>
/// <param name="Assets">kiln.assets is loaded.</param>
/// <param name="Spires">The number of spires found.</param>
/// <param name="Shrines">The number of Flame Shrines and Sanctums found.</param>
/// <param name="Roots">The number of Shroud Roots found.</param>
/// <param name="Obelisks">The number of obelisks found.</param>
/// <param name="BossCount">The number of bosses found.</param>
/// <param name="TrackedBosses">The number of bosses whose kill the game records.</param>
/// <param name="PlacesSeconds">How long reading the places took, in seconds.</param>
/// <param name="BossesSeconds">How long reading the bosses took, in seconds.</param>
/// <param name="Problems">Problems found while reading the game's data, as text lines.</param>
public sealed record WorldProgressStatus(bool Knowledge, bool World, bool Settings, bool Assets, WorldIndexState Places,
    WorldIndexState Bosses, int Spires, int Shrines, int Roots, int Obelisks, int BossCount, int TrackedBosses, double PlacesSeconds,
    double BossesSeconds, IReadOnlyList<string> Problems)
{
    /// <summary>The index state, the counts, the services in use and any problems, as text.</summary>
    public override string ToString() =>
        $"places {Places} ({Spires} spires, {Shrines} shrines, {Roots} roots, {Obelisks} obelisks, {PlacesSeconds:0.00} s), " +
        $"bosses {Bosses} ({BossCount}, {TrackedBosses} with a kill record, {BossesSeconds:0.00} s); knowledge {(Knowledge ? "on" : "OFF")}, " +
        $"world {(World ? "on" : "OFF")}, settings {(Settings ? "on" : "OFF")}, assets {(Assets ? "on" : "OFF")}" +
        (Problems.Count > 0 ? $"; {Problems.Count} problem(s): {string.Join("; ", Problems)}" : "");
}

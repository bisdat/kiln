// SPDX-License-Identifier: MIT
// New gems (gem wave G2): Gems.Register makes a gem item (a kiln.content clone of a gem) whose effect is either an
// existing Impact program with the gem's own configs, or a C# handler reached through a Kiln trigger program. Kiln
// ships one trigger program per kind (Gems/programs/*.kimp, assembled with tools/impact; the .bin next to each is
// embedded here), served through kiln.assets under the registering mod's owner. The game runs it like its own gem
// programs (equipment_impacts spawns it for the socketed gem); it walks the wearer's hits and sends each one with
// kiln.Signal. Here the signal's impact entity is matched to the gem through Gems.Equipped (the wearer's equipment
// impacts), and the gem's handler runs on the tick thread with the hit, the gem, its level and its carrier.
// docs/kiln/services/gems-new.md.
using System.Globalization;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace Kiln;

/// <summary>What sets off a gem's C# handler (<see cref="GemDefinition.OnTrigger"/>). Each kind is one Kiln program that
/// every gem of that kind shares.</summary>
public enum GemTriggerKind : byte
{
    /// <summary>A hit the wearer lands (not with a skill's or a gem's own damage).</summary>
    Hit = 1,
    /// <summary>A critical hit the wearer lands.</summary>
    Crit = 2,
    /// <summary>A killing blow the wearer lands.</summary>
    Kill = 3,
    /// <summary>A backstab the wearer lands.</summary>
    Backstab = 4,
    /// <summary>A hit the wearer takes.</summary>
    HitTaken = 5,
    /// <summary>While the gem is worn, at an interval.</summary>
    Passive = 6,
}

/// <summary>When a gem's C# handler runs: <see cref="OnHit"/>, <see cref="OnKill"/>, <see cref="OnBackstab"/>,
/// <see cref="OnHitTaken"/>, <see cref="Passive"/>.</summary>
public sealed class GemTrigger
{
    /// <summary>The shortest <see cref="Passive"/> interval: the passive program's own step (250 ms).</summary>
    public static readonly TimeSpan PassiveStep = TimeSpan.FromMilliseconds(250);

    private GemTrigger(GemTriggerKind kind, bool enemiesOnly, TimeSpan interval)
    {
        Kind = kind;
        EnemiesOnly = enemiesOnly;
        Interval = interval;
    }

    /// <summary>The trigger program this gem runs.</summary>
    public GemTriggerKind Kind { get; }

    /// <summary>Only when the other side of the hit is an enemy faction (the game's own gems' "all enemies" mask:
    /// creatures, not players, props or buildings).</summary>
    public bool EnemiesOnly { get; }

    /// <summary><see cref="GemTriggerKind.Passive"/>: how often the handler runs while the gem is worn.</summary>
    public TimeSpan Interval { get; }

    /// <summary>Each hit the wearer lands; with <paramref name="crit"/> only critical hits.</summary>
    public static GemTrigger OnHit(bool crit = false, bool enemiesOnly = true) =>
        new(crit ? GemTriggerKind.Crit : GemTriggerKind.Hit, enemiesOnly, TimeSpan.Zero);

    /// <summary>Each killing blow the wearer lands.</summary>
    public static GemTrigger OnKill(bool enemiesOnly = true) => new(GemTriggerKind.Kill, enemiesOnly, TimeSpan.Zero);

    /// <summary>Each backstab the wearer lands.</summary>
    public static GemTrigger OnBackstab(bool enemiesOnly = true) => new(GemTriggerKind.Backstab, enemiesOnly, TimeSpan.Zero);

    /// <summary>Each hit the wearer takes; <paramref name="enemiesOnly"/>: only from an enemy faction.</summary>
    public static GemTrigger OnHitTaken(bool enemiesOnly = true) => new(GemTriggerKind.HitTaken, enemiesOnly, TimeSpan.Zero);

    /// <summary>Every <paramref name="interval"/> while the gem is worn (at least <see cref="PassiveStep"/>, at most one
    /// hour; it runs on the program's 250 ms steps, so a multiple of 250 ms is exact). The first run comes about 250 ms
    /// after the gem is worn. The clock belongs to the gem in its carrier: a hotbar switch away and back (the game starts
    /// the gem's program again) does not run it early.</summary>
    /// <exception cref="ArgumentOutOfRangeException">The interval is below 250 ms or above one hour.</exception>
    public static GemTrigger Passive(TimeSpan interval)
    {
        if (interval < PassiveStep || interval > TimeSpan.FromHours(1))
            throw new ArgumentOutOfRangeException(nameof(interval), interval, "a passive gem's interval is 250 ms to 1 hour");
        return new(GemTriggerKind.Passive, false, interval);
    }

    /// <inheritdoc/>
    public override string ToString() => Kind switch
    {
        GemTriggerKind.Passive => $"Passive({Interval.TotalSeconds.ToString("0.###", CultureInfo.InvariantCulture)} s)",
        _ => EnemiesOnly ? $"{Kind} (enemies)" : Kind.ToString(),
    };
}

/// <summary>A new gem for <see cref="Gems.Register"/>: a clone of a gem item (<see cref="BaseOn"/>) with its own GUID,
/// texts, icon, rarity and level range, and an effect: the base's program, another loaded program
/// (<see cref="Program"/>) with the gem's own <see cref="Configs"/>, or a C# handler (<see cref="Trigger"/> +
/// <see cref="OnTrigger"/>).</summary>
public sealed class GemDefinition
{
    /// <summary><c>"&lt;modid&gt;:&lt;name&gt;"</c> with the registering mod's id, unique among the registered gems.</summary>
    public required string Id { get; init; }

    /// <summary>The gem item's fixed GUID (saves keep its id, FNV-1a of the GUID: never change it once released).
    /// Null: one made from <see cref="Id"/> (the same every start).</summary>
    public Guid? Guid { get; init; }

    /// <summary>The item's debug name (<c>A-Z a-z 0-9 _ - .</c>). Null: from <see cref="Id"/>
    /// (<c>"mymod:frostbite"</c> gives <c>Gem_mymod_frostbite</c>).</summary>
    public string? Name { get; init; }

    /// <summary>The gem to clone: a game gem's debug name (<c>T1_Gem_GemOfBleeding</c>), <c>"0x…"</c> id or GUID. Its
    /// model, icon, category (WeaponGem: the forge lists it) and, without <see cref="Program"/> or
    /// <see cref="Trigger"/>, its program come with it.</summary>
    public required string BaseOn { get; init; }

    /// <summary>The name players see (every language). Null: the base's.</summary>
    public string? DisplayName { get; init; }

    /// <summary>The tooltip description (every language). Null: the base's.</summary>
    public string? Description { get; init; }

    /// <summary>Texts per language (<c>"en"</c>, <c>"de"</c>, ...), as kiln.content's item texts.</summary>
    public Dictionary<string, ItemTexts> Texts { get; init; } = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>The icon: a PNG file of the mod (<c>"icons/frostbite.png"</c>), <c>"item:&lt;name&gt;"</c> or an item
    /// GUID. Null: the base's.</summary>
    public string? Icon { get; init; }

    /// <summary>The rarity. Null: the base's.</summary>
    public ItemRarity? Rarity { get; init; }

    /// <summary>The level a new gem has (1..105). Null: 1.</summary>
    public int? LevelMin { get; init; }

    /// <summary>The highest level (1..105), what an upgrade at a forge can reach (the forge's own cap still applies).
    /// Null: 105, as the game's gems.</summary>
    public int? LevelMax { get; init; }

    /// <summary>A loaded Impact program for the gem's effect: another gem's, a perk's, or one the mod cooked with
    /// tools/impact and serves through kiln.assets. Null: the base's. Not with <see cref="Trigger"/>.</summary>
    public Guid? Program { get; init; }

    /// <summary>The gem's config values (its ItemInfo <c>impactValues</c>), as kiln.gamedata's item paths take them:
    /// key = a config alias (<c>"BuffType"</c>), <c>"0x5e34f09b"</c>, a program's own string id
    /// (<c>"mymod.Radius"</c>), optionally followed by <c>.scaleFactor</c>; value = a number, bool, string (enum name,
    /// a template or buff by name), <see cref="System.Guid"/> or <see cref="TimeSpan"/>. Applied at every world load
    /// as the mod's layer. Not with <see cref="Trigger"/> (a Kiln trigger program reads none).</summary>
    public Dictionary<string, object> Configs { get; init; } = new(StringComparer.Ordinal);

    /// <summary>A Kiln trigger program for the effect: the gem calls <see cref="OnTrigger"/>.</summary>
    public GemTrigger? Trigger { get; init; }

    /// <summary>The C# effect, on the server's tick thread, the Pre after the game's step that had the hit, as the gem's
    /// mod (its SynchronizationContext too: an <c>await</c> comes back to the tick thread). It runs where the server world
    /// runs (a dedicated server, or the hosting player's game); a joined client never calls it.</summary>
    public Action<GemTriggerContext>? OnTrigger { get; init; }
}

/// <summary>One trigger of a registered gem: who wore it, the hit, the gem and where it is socketed.</summary>
public sealed class GemTriggerContext
{
    internal GemTriggerContext() { }

    /// <summary>The gem's definition.</summary>
    public GemDefinition Definition { get; internal init; } = null!;

    /// <summary>What happened.</summary>
    public GemTriggerKind Trigger { get; internal init; }

    /// <summary>The wearer (a player).</summary>
    public EntityId Player { get; internal init; }

    /// <summary>The other side: the one hit (the target's root entity), for <see cref="GemTriggerKind.HitTaken"/> the
    /// attacker's root; none for <see cref="GemTriggerKind.Passive"/>.</summary>
    public EntityId Target { get; internal init; }

    /// <summary>The entity the hit struck (a part of <see cref="Target"/>, or the target itself); for
    /// <see cref="GemTriggerKind.HitTaken"/> the wearer's own part (or the wearer).</summary>
    public EntityId TargetPart { get; internal init; }

    /// <summary>The gem item.</summary>
    public ItemId Gem { get; internal init; }

    /// <summary>The gem's level (1..105): scale effects with it.</summary>
    public int Level { get; internal init; }

    /// <summary>The item the gem is socketed in.</summary>
    public ItemInstanceRef Carrier { get; internal init; }

    /// <summary>The equipment slot whose item carries the gem.</summary>
    public GemImpactSlot Slot { get; internal init; }

    /// <summary>The gem's impact entity (the running trigger program).</summary>
    public EntityId Impact { get; internal init; }

    /// <summary>The health the hit took from its target (0: none, or it healed).</summary>
    public int Damage => HealthChange < 0 ? -HealthChange : 0;

    /// <summary>The target's health change of the hit as the game recorded it (negative: damage).</summary>
    public int HealthChange { get; internal init; }

    /// <summary>Where the hit landed (the wearer's position is not sent for <see cref="GemTriggerKind.Passive"/>: 0).</summary>
    public WorldPosition Position { get; internal init; }

    /// <summary>The weapon entity of the hit (none for some hits).</summary>
    public EntityId Weapon { get; internal init; }

    /// <summary>The hit's direct source (a projectile, an explosion, the attacker).</summary>
    public EntityId Source { get; internal init; }

    /// <summary>A critical hit.</summary>
    public bool Crit { get; internal init; }

    /// <summary>The hit killed its target.</summary>
    public bool KillingBlow { get; internal init; }

    /// <summary>A backstab.</summary>
    public bool Backstab { get; internal init; }

    /// <summary>The hit was blocked.</summary>
    public bool Blocked { get; internal init; }

    /// <summary>The other side is of an enemy faction.</summary>
    public bool OtherIsEnemy { get; internal init; }

    /// <summary>The game's step of the signal.</summary>
    public ulong Step { get; internal init; }
}

/// <summary>A gem registration (<see cref="Gems.Register"/>): <see cref="Ok"/>, or why it was refused. Dispose to stop
/// its handler; the item itself stays registered until the server restarts (saved gems stay valid), as every
/// kiln.content item does. A mod's gems also stop when it unloads.</summary>
public sealed class GemHandle : IKilnResult, IDisposable
{
    internal GemHandle(GemDefinition definition, bool ok, string? reason, Guid guid, string name)
    {
        Definition = definition;
        Ok = ok;
        Reason = ok ? null : reason;
        Guid = guid;
        Name = name;
    }

    /// <summary>True when the gem was registered.</summary>
    public bool Ok { get; }

    /// <summary>Why not; null when <see cref="Ok"/>.</summary>
    public string? Reason { get; }

    /// <summary>The definition.</summary>
    public GemDefinition Definition { get; }

    /// <summary>The gem item's GUID.</summary>
    public Guid Guid { get; }

    /// <summary>The gem item's debug name.</summary>
    public string Name { get; }

    /// <summary>The gem item (FNV-1a of <see cref="Guid"/>).</summary>
    public ItemId Item => new(GemRegistry.Fnv(Guid));

    /// <summary>Stops the handler and drops the registration (the item stays until the server restarts).</summary>
    public void Dispose()
    {
        if (Ok) GemRegistry.Remove(Definition.Id, this);
    }

    /// <inheritdoc/>
    public override string ToString() => Ok ? $"gem {Definition.Id} ({Name}, 0x{Item.Value:x8})" : $"gem {Definition.Id} refused: {Reason}";
}

public static partial class Gems
{
    /// <summary>Registers a new gem: a gem item cloned from <see cref="GemDefinition.BaseOn"/> through kiln.content (its
    /// own GUID, texts, icon, rarity, level range) with the base's program, another loaded program and its own configs,
    /// or a C# handler run by a Kiln trigger program (<see cref="GemDefinition.Trigger"/>). Call it in <c>OnLoad</c> on
    /// every side that has the mod (a client needs the item to show it); the handler runs on the server. Removed when
    /// the mod unloads (the item stays until the server restarts). docs/kiln/services/gems-new.md.</summary>
    /// <returns><see cref="GemHandle.Ok"/> false with a reason when the id, GUID or name is registered already.</returns>
    /// <exception cref="ArgumentException">The definition is wrong (id, an id under another mod's id, base, levels, a
    /// program and a trigger at once, a trigger without a handler, configs with a trigger, a name kiln.content refuses).</exception>
    /// <exception cref="InvalidOperationException">Not called from mod code.</exception>
    /// <exception cref="KilnUnavailableException"><c>gems.triggers</c>: kiln.content's new items or (for a trigger)
    /// kiln.assets (its resources) are not here. The <c>gems.triggers</c> row does not cover these two: check for this
    /// exception too.</exception>
    [TickThreadOnly]
    [RequiresCapability(GemRegistry.Row)]
    public static GemHandle Register(GemDefinition definition) => GemRegistry.Add(definition);
}

/// <summary>A hit as the trigger programs send it (their <c>$hit</c> block, 48 bytes).</summary>
internal readonly record struct GemHit(uint Attacker, uint Target, uint Part, int Health, float X, float Y, float Z,
    uint Weapon, uint Source, bool Crit, bool Kill, bool Backstab, bool Blocked, bool Enemy)
{
    public const int Size = 48;

    public static GemHit? Parse(ReadOnlySpan<byte> v)
    {
        if (v.Length < 41) return null;
        static uint U(ReadOnlySpan<byte> b, int o) => BitConverter.ToUInt32(b[o..(o + 4)]);
        static float F(ReadOnlySpan<byte> b, int o) => BitConverter.ToSingle(b[o..(o + 4)]);
        return new GemHit(U(v, 0), U(v, 4), U(v, 8), (int)U(v, 12), F(v, 16), F(v, 20), F(v, 24), U(v, 28), U(v, 32),
            v[36] != 0, v[37] != 0, v[38] != 0, v[39] != 0, v[40] != 0);
    }
}

/// <summary>What the registry needs from the game (kiln.content, kiln.assets, kiln.abilities, G1's equipped gems);
/// replaced in tests.</summary>
internal interface IGemBackend
{
    /// <summary>Null when new items can be defined here, else why not.</summary>
    string? ContentMissing { get; }
    /// <summary>Null when kiln.assets can serve the trigger programs, else why not.</summary>
    string? AssetsMissing { get; }
    void ServeProgram(ModContext mod, Guid guid, byte[] blob);
    void DefineItem(ItemDefinition item);
    void ApplyConfigs(ModContext mod, string documentJson, Func<bool> stillWanted);
    /// <summary>The wearer's equipped gems now (G1's <see cref="Gems.Equipped"/>); the registry reads them once per step.</summary>
    IReadOnlyList<EquippedGem> Equipped(EntityId player);
    long NowMs { get; }
    IDisposable SubscribeSignals(Action<ImpactSignalEvent> handler);
}

internal static class GemRegistry
{
    /// <summary>The capability row of <see cref="Gems.Register"/> (G1's <c>GemTriggeredEvent</c> shares it).</summary>
    public const string Row = "gems.triggers";

    /// <summary>keen::ImpactProgram.</summary>
    internal const uint ProgramType = 0xd4922d88u;

    internal sealed record Program(GemTriggerKind Kind, Guid Guid, uint Tag, string Stem)
    {
        public uint Id { get; } = Fnv(Guid);
    }

    /// <summary>The trigger programs (Gems/programs/&lt;stem&gt;.kimp): GUIDs and tags fixed forever.</summary>
    internal static readonly Program[] Programs =
    [
        new(GemTriggerKind.Hit, new Guid("6b696c6e-6765-6d73-8000-000000000001"), 0x6b470001, "on_hit"),
        new(GemTriggerKind.Crit, new Guid("6b696c6e-6765-6d73-8000-000000000002"), 0x6b470002, "on_crit"),
        new(GemTriggerKind.Kill, new Guid("6b696c6e-6765-6d73-8000-000000000003"), 0x6b470003, "on_kill"),
        new(GemTriggerKind.Backstab, new Guid("6b696c6e-6765-6d73-8000-000000000004"), 0x6b470004, "on_backstab"),
        new(GemTriggerKind.HitTaken, new Guid("6b696c6e-6765-6d73-8000-000000000005"), 0x6b470005, "on_hit_taken"),
        new(GemTriggerKind.Passive, new Guid("6b696c6e-6765-6d73-8000-000000000006"), 0x6b470006, "passive"),
    ];

    internal static Program ProgramOf(GemTriggerKind kind) => Programs.First(p => p.Kind == kind);

    /// <summary>The cooked blob of a trigger program (embedded from Gems/programs/&lt;stem&gt;.bin).</summary>
    internal static byte[] Blob(Program p)
    {
        using var s = typeof(GemRegistry).Assembly.GetManifestResourceStream($"Kiln.Gems.{p.Stem}.bin")
                      ?? throw new InvalidOperationException($"Kiln.Abilities has no gem program {p.Stem}.bin");
        var b = new byte[s.Length];
        s.ReadExactly(b);
        return b;
    }

    internal sealed class Entry
    {
        public required GemDefinition Def;
        public required ModContext Mod;
        public required GemHandle Handle;
        public required uint Item;
        public int Faults;
    }

    private static readonly object Lock = new();
    private static readonly Dictionary<string, Entry> ById = new(StringComparer.OrdinalIgnoreCase);
    private static readonly Dictionary<uint, Entry> ByItem = new();
    private static readonly Dictionary<string, Entry> ByName = new(StringComparer.OrdinalIgnoreCase);
    private static readonly HashSet<(ModContext, GemTriggerKind)> Served = new();
    private static readonly HashSet<ModContext> Watched = new(ReferenceEqualityComparer.Instance);
    // a passive gem's clock: the worn gem (wearer, gem item, carrier entity), not its impact entity, which the game spawns
    // again on every hotbar switch to the carrier (review); each with its own interval, which its clean-up follows (review)
    private static readonly Dictionary<(uint Player, uint Gem, uint Carrier), (long Last, long IntervalMs)> PassiveLast = new();
    // the wearers' equipped gems of one step: all of a step's signals come in one Pre (review: one read per wearer and step)
    private static readonly Dictionary<uint, IReadOnlyList<EquippedGem>?> WornThisStep = new();
    private static ulong _wornStep = ulong.MaxValue;
    private static IDisposable? _signals;
    private static long _lastPrune;
    private static int _lookupWarned;
    private static int _lookupFaults;

    internal static IGemBackend Backend = new LiveGemBackend();

    // ------------------------------------------------------------------------------------------- registration ----

    private static readonly Regex IdShape = new("^[a-z0-9][a-z0-9_.-]*:[A-Za-z0-9][A-Za-z0-9_.-]*$", RegexOptions.CultureInvariant);
    private static readonly Regex NameShape = new("^[A-Za-z0-9_.-]{1,96}$", RegexOptions.CultureInvariant);

    internal static GemHandle Add(GemDefinition d)
    {
        ArgumentNullException.ThrowIfNull(d);
        var mod = ModContext.CurrentMod ?? throw new InvalidOperationException("Gems.Register must be called from mod code (OnLoad or a Kiln callback)");
        Validate(d);
        // review: the id's mod part is the caller's: another mod could otherwise take the id, and the GUID made from it
        // (the item id saved gems keep), before the mod that owns it loads
        if (!string.Equals(d.Id[..d.Id.IndexOf(':')], mod.Info.Id, StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException($"gem id '{d.Id}' must start with the registering mod's id: \"{mod.Info.Id}:<name>\"", nameof(d));
        var guid = d.Guid ?? GuidOf(d.Id);
        var name = d.Name ?? NameOf(d.Id);
        if (!NameShape.IsMatch(name)) throw new ArgumentException($"gem {d.Id}: debug name '{name}' is not A-Z a-z 0-9 _ - . (at most 96)", nameof(d));
        if (Backend.ContentMissing is { } why) throw new KilnUnavailableException(Row, $"new gems need kiln.content's new items: {why}");
        if (d.Trigger is not null && Backend.AssetsMissing is { } no)
            throw new KilnUnavailableException(Row, $"a gem with a Kiln trigger needs kiln.assets to serve its program: {no}");
        var item = Fnv(guid);
        Entry e;
        lock (Lock)
        {
            if (ById.TryGetValue(d.Id, out var had)) return Refused(d, guid, name, $"gem {d.Id} is registered already (by {had.Mod.Info.Id})");
            if (ByItem.TryGetValue(item, out had)) return Refused(d, guid, name, $"GUID {guid} is gem {had.Def.Id}'s (by {had.Mod.Info.Id})");
            if (ByName.TryGetValue(name, out had)) return Refused(d, guid, name, $"debug name {name} is gem {had.Def.Id}'s (by {had.Mod.Info.Id})");
            e = new Entry { Def = d, Mod = mod, Handle = new GemHandle(d, true, null, guid, name), Item = item };
        }
        Guid program;
        if (d.Trigger is { } trigger)
        {
            var p = ProgramOf(trigger.Kind);
            bool serve;
            lock (Lock) serve = Served.Add((mod, trigger.Kind));
            if (serve)
            {
                try
                {
                    Backend.ServeProgram(mod, p.Guid, Blob(p));
                }
                catch (KilnUnavailableException x) when (x.Capability != Row)
                {
                    // review: kiln.assets is here but refuses resources (its hooks are off): the documented refusal names gems.triggers
                    lock (Lock) Served.Remove((mod, trigger.Kind));
                    throw new KilnUnavailableException(Row, $"a gem with a Kiln trigger needs kiln.assets to serve its program: {x.Message}", x);
                }
                catch
                {
                    lock (Lock) Served.Remove((mod, trigger.Kind));
                    throw;
                }
            }
            program = p.Guid;
        }
        else program = d.Program ?? System.Guid.Empty;
        Backend.DefineItem(ItemOf(d, guid, name, program));
        lock (Lock)
        {
            ById[d.Id] = e;
            ByItem[item] = e;
            ByName[name] = e;
        }
        Watch(mod);
        if (d.Configs.Count > 0)
        {
            var json = ConfigsJson(name, d.Configs);
            Backend.ApplyConfigs(mod, json, () => { lock (Lock) return ById.TryGetValue(d.Id, out var now) && ReferenceEquals(now, e); });
        }
        if (d.Trigger is not null) EnsureSignals();
        mod.Log.Info($"gems: {d.Id} registered: item {name} (0x{item:x8}) from {d.BaseOn}, " +
                     (d.Trigger is { } t ? $"trigger {t}" : d.Program is { } g ? $"program {g}" : "the base's program") +
                     (d.Configs.Count > 0 ? $", {d.Configs.Count} config(s)" : ""));
        return e.Handle;
    }

    private static GemHandle Refused(GemDefinition d, Guid guid, string name, string reason) => new(d, false, reason, guid, name);

    internal static void Validate(GemDefinition d)
    {
        if (string.IsNullOrWhiteSpace(d.Id) || !IdShape.IsMatch(d.Id))
            throw new ArgumentException($"gem id '{d.Id}' must be \"<modid>:<name>\" (lowercase mod id; name A-Z a-z 0-9 _ - .)", nameof(d));
        if (string.IsNullOrWhiteSpace(d.BaseOn)) throw new ArgumentException($"gem {d.Id}: BaseOn (the gem to clone) is required", nameof(d));
        if (d.Guid == System.Guid.Empty) throw new ArgumentException($"gem {d.Id}: Guid must not be empty", nameof(d));
        if (d.Program == System.Guid.Empty) throw new ArgumentException($"gem {d.Id}: Program must not be empty (null keeps the base's)", nameof(d));
        if (d.LevelMin is < 1 or > 105) throw new ArgumentException($"gem {d.Id}: LevelMin {d.LevelMin} is outside 1..105", nameof(d));
        if (d.LevelMax is < 1 or > 105) throw new ArgumentException($"gem {d.Id}: LevelMax {d.LevelMax} is outside 1..105", nameof(d));
        if ((d.LevelMin ?? 1) > (d.LevelMax ?? 105)) throw new ArgumentException($"gem {d.Id}: LevelMin {d.LevelMin ?? 1} is above LevelMax {d.LevelMax ?? 105}", nameof(d));
        if (d.Trigger is not null && d.Program is not null) throw new ArgumentException($"gem {d.Id}: a Program and a Trigger at once (choose one)", nameof(d));
        if (d.Trigger is null && d.OnTrigger is not null) throw new ArgumentException($"gem {d.Id}: OnTrigger needs a Trigger (GemTrigger.OnHit, ...)", nameof(d));
        if (d.Trigger is not null && d.OnTrigger is null) throw new ArgumentException($"gem {d.Id}: a Trigger needs OnTrigger (the C# effect)", nameof(d));
        if (d.Trigger is not null && d.Configs.Count > 0)
            throw new ArgumentException($"gem {d.Id}: a Kiln trigger program reads no configs: keep the gem's numbers in the handler", nameof(d));
        foreach (var (k, v) in d.Configs)
        {
            if (string.IsNullOrWhiteSpace(k) || k.Contains('"')) throw new ArgumentException($"gem {d.Id}: config key '{k}'", nameof(d));
            if (v is null) throw new ArgumentException($"gem {d.Id}: config {k} has no value", nameof(d));
            _ = ConfigValue(d.Id, k, v);
        }
    }

    /// <summary>The kiln.content item of a gem definition.</summary>
    internal static ItemDefinition ItemOf(GemDefinition d, Guid guid, string name, Guid program)
    {
        var fields = new Dictionary<string, object>(StringComparer.Ordinal)
        {
            ["itemLevelRange.minLevel"] = d.LevelMin ?? 1,
            ["itemLevelRange.maxLevel"] = d.LevelMax ?? 105,
        };
        if (d.Rarity is { } r) fields["rarity"] = r.ToString();
        var refs = program == System.Guid.Empty ? null : new Dictionary<string, string> { ["equipment.program"] = program.ToString() };
        return new ItemDefinition
        {
            Guid = guid,
            Name = name,
            BaseOn = d.BaseOn,
            DisplayName = d.DisplayName,
            Description = d.Description,
            Texts = d.Texts.Count > 0 ? new Dictionary<string, ItemTexts>(d.Texts, StringComparer.OrdinalIgnoreCase) : null,
            Icon = d.Icon,
            Fields = fields,
            Refs = refs,
        };
    }

    /// <summary>A pack document with the gem's configs as kiln.gamedata item rules (owned by the mod, applied at every
    /// world load).</summary>
    internal static string ConfigsJson(string itemName, IReadOnlyDictionary<string, object> configs)
    {
        var rules = new JsonObject();
        foreach (var (k, v) in configs) rules[ConfigPath(k)] = ConfigValue(itemName, k, v);
        return new JsonObject { ["format"] = 1, ["items"] = new JsonObject { [itemName] = rules } }.ToJsonString();
    }

    /// <summary>"BuffType" → impactValues.BuffType; "0x5e34f09b.scaleFactor" stays a path; a name with dots
    /// ("mymod.Radius") goes in brackets, as kiln.gamedata reads it.</summary>
    internal static string ConfigPath(string key)
    {
        foreach (var field in new[] { ".scaleFactor", ".function", ".value" })
            if (key.EndsWith(field, StringComparison.Ordinal))
                return ConfigPath(key[..^field.Length]) + field;
        if (key.StartsWith('[')) return "impactValues" + key;
        return key.Contains('.') ? $"impactValues[{key}]" : "impactValues." + key;
    }

    internal static JsonNode ConfigValue(string owner, string key, object v) => v switch
    {
        bool b => JsonValue.Create(b),
        string s => JsonValue.Create(s),
        Guid g => JsonValue.Create(g.ToString()),
        TimeSpan t => JsonValue.Create(t.TotalSeconds),
        Enum en => JsonValue.Create(en.ToString()),
        sbyte or byte or short or ushort or int or uint or long or ulong => JsonValue.Create(Convert.ToInt64(v, CultureInfo.InvariantCulture)),
        float or double or decimal => JsonValue.Create(Convert.ToDouble(v, CultureInfo.InvariantCulture)),
        JsonNode n => n.DeepClone(),
        _ => throw new ArgumentException($"gem {owner}: config {key}: a {v.GetType().Name} is no config value (number, bool, string, Guid, TimeSpan)"),
    };

    internal static Guid GuidOf(string id)
    {
        // RFC 4122 name-based (SHA-1, version 5) in Kiln's gem namespace: the same id gives the same GUID on every start and side
        var ns = new Guid("6b696c6e-6765-6d73-8000-00000000006e").ToByteArray(bigEndian: true);
        var name = Encoding.UTF8.GetBytes(id.ToLowerInvariant());
        var h = SHA1.HashData([.. ns, .. name]);
        h[6] = (byte)((h[6] & 0x0F) | 0x50);
        h[8] = (byte)((h[8] & 0x3F) | 0x80);
        return new Guid(h.AsSpan(0, 16), bigEndian: true);
    }

    internal static string NameOf(string id) => "Gem_" + Regex.Replace(id, "[^A-Za-z0-9_.-]", "_");

    /// <summary>FNV-1a-32 over a GUID's 16 bytes in the game's byte order (.NET's little-endian layout): item and program ids.</summary>
    internal static uint Fnv(Guid g)
    {
        Span<byte> b = stackalloc byte[16];
        g.TryWriteBytes(b);
        var h = 0x811c9dc5u;
        foreach (var x in b) h = (h ^ x) * 0x01000193u;
        return h;
    }

    private static void Watch(ModContext mod)
    {
        lock (Lock)
            if (!Watched.Add(mod)) return;
        mod.Retiring += () => Retire(mod);
    }

    internal static void Retire(ModContext mod)
    {
        lock (Lock)
        {
            foreach (var e in ById.Values.Where(e => ReferenceEquals(e.Mod, mod)).ToArray()) Drop(e);
            Served.RemoveWhere(s => ReferenceEquals(s.Item1, mod));
            Watched.Remove(mod);
        }
        CheckSignals();
    }

    internal static void Remove(string id, GemHandle handle)
    {
        lock (Lock)
            if (ById.TryGetValue(id, out var e) && ReferenceEquals(e.Handle, handle)) Drop(e);
        CheckSignals();
    }

    private static void Drop(Entry e)
    {
        ById.Remove(e.Def.Id);
        ByItem.Remove(e.Item);
        ByName.Remove(e.Handle.Name);
    }

    internal static IReadOnlyList<GemDefinition> Definitions { get { lock (Lock) return ById.Values.Select(e => e.Def).ToArray(); } }

    // --------------------------------------------------------------------------------------------- dispatch -------

    private static void EnsureSignals()
    {
        lock (Lock)
        {
            if (_signals is not null) return;
            var outer = ModContext.CurrentMod;
            ModContext.CurrentMod = null;  // Kiln's own subscription, not the registering mod's
            try { _signals = Backend.SubscribeSignals(OnSignal); }
            finally { ModContext.CurrentMod = outer; }
        }
    }

    private static void CheckSignals()
    {
        IDisposable? off = null;
        lock (Lock)
        {
            if (_signals is null || ById.Values.Any(e => e.Def.Trigger is not null)) return;
            off = _signals;
            _signals = null;
            PassiveLast.Clear();
            WornThisStep.Clear();
            _wornStep = ulong.MaxValue;
        }
        off.Dispose();
    }

    internal static bool Listening { get { lock (Lock) return _signals is not null; } }

    // Kiln's own ImpactSignalEvent handler. Nothing may throw from here into the event bus (review): the bus counts a
    // throw as a fault of this subscription and removes it after five, and every gem's handler would then stay silent
    // (Listening still true, so no registration subscribes again).
    internal static void OnSignal(ImpactSignalEvent ev)
    {
        try
        {
            Dispatch(ev.Impact.ProgramId, ev.Tag, ev.Impact.Self, ev.Value, ev.Step);
        }
        catch (Exception x)
        {
            LookupFault(x);
        }
    }

    private static void LookupFault(Exception x)
    {
        var n = Interlocked.Increment(ref _lookupFaults);
        if (n <= 3) ModLog.Error($"gems: a gem trigger could not be dispatched ({n} of 3 logged): {x}");
    }

    // The wearer's gem whose impact sent the signal: one read of the wearer's equipped gems per step; a read that fails
    // counts as no gem for the rest of the step and never throws.
    private static EquippedGem? Worn(EntityId player, EntityId impact, ulong step)
    {
        IReadOnlyList<EquippedGem>? gems;
        lock (Lock)
        {
            if (step != _wornStep)
            {
                _wornStep = step;
                WornThisStep.Clear();
            }
            WornThisStep.TryGetValue(player.Value, out gems);
            if (gems is null && WornThisStep.ContainsKey(player.Value)) return null;  // failed in this step already
        }
        if (gems is null)
        {
            try
            {
                gems = Backend.Equipped(player);
            }
            catch (Exception x) when (x is NotImplementedException or KilnUnavailableException or InvalidOperationException)
            {
                if (Interlocked.Exchange(ref _lookupWarned, 1) == 0)
                    ModLog.Warn($"gems: a gem trigger came, but the wearer's gems cannot be read here ({x.Message}): gem handlers do not run");
            }
            catch (Exception x)
            {
                LookupFault(x);
            }
            lock (Lock) WornThisStep[player.Value] = gems;
            if (gems is null) return null;
        }
        foreach (var g in gems)
            if (g.Impact == impact) return g;
        return null;
    }

    /// <summary>One signal of a trigger program: find the gem, filter, call its handler. Returns the number of handlers
    /// called (0 or 1). Tick thread.</summary>
    internal static int Dispatch(uint programId, uint tag, EntityId impact, byte[] value, ulong step)
    {
        Program? p = null;
        foreach (var x in Programs)
            if (x.Id == programId && x.Tag == tag) p = x;
        if (p is null) return 0;
        EntityId player;
        GemHit hit = default;
        if (p.Kind == GemTriggerKind.Passive)
        {
            if (value.Length < 4) return 0;
            player = new EntityId(BitConverter.ToUInt32(value));
        }
        else
        {
            if (GemHit.Parse(value) is not { } h) return 0;
            hit = h;
            player = new EntityId(p.Kind == GemTriggerKind.HitTaken ? h.Target : h.Attacker);
        }
        if (player.IsNone) return 0;
        if (Worn(player, impact, step) is not { } worn) return 0;
        Entry? e;
        lock (Lock) ByItem.TryGetValue(worn.Socket.Gem.Value, out e);
        if (e?.Def.Trigger is not { } trigger || trigger.Kind != p.Kind || e.Def.OnTrigger is not { } handler) return 0;
        if (trigger.EnemiesOnly && p.Kind != GemTriggerKind.Passive && !hit.Enemy) return 0;
        if (p.Kind == GemTriggerKind.Passive && !PassiveDue((player.Value, worn.Socket.Gem.Value, worn.Socket.Carrier.Entity.Id.Value), trigger.Interval))
            return 0;
        var ctx = new GemTriggerContext
        {
            Definition = e.Def,
            Trigger = p.Kind,
            Player = player,
            Target = p.Kind switch
            {
                GemTriggerKind.Passive => EntityId.None,
                GemTriggerKind.HitTaken => new EntityId(hit.Attacker),
                _ => new EntityId(hit.Target),
            },
            TargetPart = new EntityId(hit.Part),
            Gem = worn.Socket.Gem,
            Level = worn.Socket.Level,
            Carrier = worn.Socket.Carrier,
            Slot = worn.Slot,
            Impact = impact,
            HealthChange = hit.Health,
            Position = new WorldPosition(hit.X, hit.Y, hit.Z),
            Weapon = new EntityId(hit.Weapon),
            Source = new EntityId(hit.Source),
            Crit = hit.Crit,
            KillingBlow = hit.Kill,
            Backstab = hit.Backstab,
            Blocked = hit.Blocked,
            OtherIsEnemy = hit.Enemy,
            Step = step,
        };
        // as the mod, as an event handler runs (Events.Post): its CurrentMod, and its SynchronizationContext, so an async
        // handler's await comes back to the tick thread through the mod's scheduler, which drops it when the mod unloads (review)
        var outer = ModContext.CurrentMod;
        var outerSync = SynchronizationContext.Current;
        ModContext.CurrentMod = e.Mod;
        SynchronizationContext.SetSynchronizationContext(e.Mod.Scheduler.SynchronizationContext);
        try
        {
            handler(ctx);
        }
        catch (Exception x)
        {
            var n = Interlocked.Increment(ref e.Faults);
            if (n <= 3) e.Mod.Log.Error(x, $"gems: the handler of {e.Def.Id} threw ({n} of 3 logged)");
        }
        finally
        {
            ModContext.CurrentMod = outer;
            SynchronizationContext.SetSynchronizationContext(outerSync);
        }
        return 1;
    }

    // The passive program signals every 250 ms per gem impact; the handler runs at the gem's interval (half a step of slack).
    // The clock is the worn gem's: a new impact of the same gem in the same carrier (a hotbar switch) keeps it (review). A
    // clock is dropped once a minute has passed beyond its own interval: its next signal runs the handler either way (review:
    // the caller's interval dropped a long gem's clock after a minute).
    private static bool PassiveDue((uint Player, uint Gem, uint Carrier) gem, TimeSpan interval)
    {
        var now = Backend.NowMs;
        var every = (long)interval.TotalMilliseconds;
        lock (Lock)
        {
            if (now - _lastPrune > 30_000)
            {
                _lastPrune = now;
                foreach (var k in PassiveLast.Where(kv => now - kv.Value.Last > 60_000 + kv.Value.IntervalMs).Select(kv => kv.Key).ToArray())
                    PassiveLast.Remove(k);
            }
            if (PassiveLast.TryGetValue(gem, out var clock) && now - clock.Last < every - 125) return false;
            PassiveLast[gem] = (now, every);
            return true;
        }
    }

    // ---------------------------------------------------------------------------------------------- the row -------

    /// <summary>What <see cref="Gems.Register"/> needs, as a row. Since the merge with G1 the <c>gems.triggers</c> row is
    /// G1's alone (GemsCaps: <c>gems.read</c>, kiln.abilities, <c>abilities.natives</c>) and <see cref="ReportRow"/> is not
    /// called; Register checks kiln.content and kiln.assets itself and throws with its own reason. Pure.</summary>
    internal static (CapabilityState State, CapabilityReason Reason, string Text) DecideRow(string? contentMissing, string? assetsMissing,
        bool abilities)
    {
        if (!abilities) return (CapabilityState.Unavailable, CapabilityReason.ServiceMissing, "kiln.abilities is not loaded");
        if (contentMissing is not null) return (CapabilityState.Unavailable, CapabilityReason.ServiceMissing, $"needs kiln.content's new items: {contentMissing}");
        if (assetsMissing is not null) return (CapabilityState.Unavailable, CapabilityReason.ServiceMissing, $"needs kiln.assets: {assetsMissing}");
        return (CapabilityState.Available, CapabilityReason.None, "");
    }

    internal static unsafe void ReportRow()
    {
        try
        {
            var (s, r, t) = DecideRow(Backend.ContentMissing, Backend.AssetsMissing, AbilitiesNative.T != null);
            ServiceCaps.Report(Row, s, r, t);
        }
        catch (Exception e) when (e is ArgumentException or InvalidOperationException or TypeInitializationException
                                      or FileNotFoundException or FileLoadException)
        {
            ModLog.Warn($"kiln.abilities: {Row} was not reported: {e.Message}");
        }
    }

    // test seam: back to an empty registry
    internal static void ResetForTests()
    {
        lock (Lock)
        {
            ById.Clear();
            ByItem.Clear();
            ByName.Clear();
            Served.Clear();
            Watched.Clear();
            PassiveLast.Clear();
            WornThisStep.Clear();
            _wornStep = ulong.MaxValue;
            _signals?.Dispose();
            _signals = null;
            _lookupWarned = 0;
            _lookupFaults = 0;
            _lastPrune = 0;
        }
    }
}

/// <summary>The game side of the registry.</summary>
internal sealed class LiveGemBackend : IGemBackend
{
    public string? ContentMissing => NewContent.Available ? null : "kiln.content is not loaded or older than its new items (v3)";

    public string? AssetsMissing => Assets.Available ? null : "kiln.assets is not loaded";

    public void ServeProgram(ModContext mod, Guid guid, byte[] blob)
    {
        Assets.RegisterAs(mod.Owner, new AssetKey(guid, GemRegistry.ProgramType), blob);
        // a world that runs already registers what kiln.assets serves only at its start: ask kiln.abilities now
        try
        {
            if (Abilities.Available && Game.IsTickThread) Abilities.Programs.Refresh();
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or ArgumentException)
        {
            mod.Log.Warn($"gems: the trigger program {guid} waits for the next world: {e.Message}");
        }
    }

    public void DefineItem(ItemDefinition item) => NewContent.Register(item);

    public void ApplyConfigs(ModContext mod, string documentJson, Func<bool> stillWanted) =>
        Content.OnReady(_ =>
        {
            if (!stillWanted()) return;
            var r = Content.Apply(documentJson);
            foreach (var e in r.Errors) mod.Log.Warn($"gems: config: {e}");
        });

    public IReadOnlyList<EquippedGem> Equipped(EntityId player) => Gems.Equipped(player);

    public long NowMs => Environment.TickCount64;

    public IDisposable SubscribeSignals(Action<ImpactSignalEvent> handler) => Events.On(handler);
}

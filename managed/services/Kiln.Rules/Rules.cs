// SPDX-License-Identifier: MIT
using System.Text.Json;

namespace Kiln;

/// <summary>
/// Per-player rules: what the game lets a player do (fly, build where, at what cost, how fast they mine) and what they
/// never run out of (health, stamina, mana, water). Mods apply rule sets to a scope (a player, everyone, an area) for a
/// while or until they say stop; Kiln combines every rule set that applies to a player (see <see cref="PlayerRules"/>)
/// and applies the result where the game decides each thing.
/// </summary>
/// <remarks>
/// Server side (a dedicated server, or a game client hosting a world); tick thread. A rule set belongs to the mod that
/// applied it and ends when that mod unloads; one for a single player also ends when the player leaves. Flight and
/// placement need Kiln in the player's game: <see cref="ClientOf"/> says whether it has it. Rules are session state:
/// nothing is saved, so a restart starts from the game's rules.
/// </remarks>
[ServerOnly]
public static unsafe class Rules
{
    /// <summary>The network channel a player's game applies its rules from (flight and the build cursor).</summary>
    public const string ChannelName = "kiln:rules";

    /// <summary>True when the native kiln.rules service is loaded (its hooks may still be off: see <see cref="Status"/>).</summary>
    public static bool Available => RulesNative.T != null;

    /// <summary>Applies <paramref name="rules"/> to <paramref name="scope"/> until the returned handle is disposed, the
    /// calling mod unloads, <paramref name="duration"/> runs out, or (for a single player) the player leaves.</summary>
    /// <exception cref="ArgumentException">A value is out of range (named in the message).</exception>
    [TickThreadOnly]
    public static RuleSet Apply(RuleScope scope, PlayerRules rules, TimeSpan? duration = null, string? label = null) =>
        Apply(scope, rules, duration, label, null);

    // `made` gets the rule set before it is registered and resolved: resolving runs other code, and a caller that
    // must take back exactly its own set when that throws has no return value to take it from.
    internal static RuleSet Apply(RuleScope scope, PlayerRules rules, TimeSpan? duration, string? label, Action<RuleSet>? made)
    {
        ArgumentNullException.ThrowIfNull(scope);
        ArgumentNullException.ThrowIfNull(rules);
        Game.AssertTickThread();
        var problems = rules.Problems();
        if (problems.Count > 0) throw new ArgumentException(string.Join("; ", problems), nameof(rules));
        RulesRuntime.Start();
        var mod = ModContext.CurrentMod;
        var set = new RuleSet(scope, rules, mod?.Info.Id ?? "kiln", label, duration is { } d ? DateTime.UtcNow + d : null);
        made?.Invoke(set);
        RulesRuntime.Add(set, mod);
        return set;
    }

    /// <summary>Applies <paramref name="rules"/> to one player (<see cref="RuleScope.ForPlayer"/>).</summary>
    [TickThreadOnly]
    public static RuleSet Apply(EntityId player, PlayerRules rules, TimeSpan? duration = null, string? label = null) =>
        Apply(RuleScope.ForPlayer(player), rules, duration, label);

    /// <summary>A player's rules as they stand now (every rule set that applies, combined, the server's limits held).</summary>
    [TickThreadOnly]
    public static EffectiveRules Of(EntityId player)
    {
        Game.AssertTickThread();
        return RulesRuntime.Effective(player);
    }

    /// <summary>Every active rule set, in the order they were applied.</summary>
    public static IReadOnlyList<RuleSet> Active => RulesRuntime.Sets();

    /// <summary>
    /// A named set of rules: <c>&lt;configDir&gt;/rules/&lt;name&gt;.json</c> on this server first, then one a mod
    /// defined (<see cref="DefinePreset"/>), then Kiln's own (<c>creative</c>). Null when there is none.
    /// </summary>
    /// <exception cref="JsonException">The file is not valid rules (the message names the member).</exception>
    public static PlayerRules? Preset(string name) => RulesPresets.Get(name);

    /// <summary>Names of every preset: the server's files, the mods', Kiln's.</summary>
    public static IReadOnlyList<string> Presets => RulesPresets.Names();

    /// <summary>Defines a preset for as long as the calling mod is loaded (a server file of the same name wins).</summary>
    public static IDisposable DefinePreset(string name, PlayerRules rules) => RulesPresets.Define(name, rules, ModContext.CurrentMod);

    /// <summary>The server's limits on everyone's rules (<c>&lt;configDir&gt;/rules.json</c>; edits apply within a second).</summary>
    public static RulesPolicy Policy => RulesRuntime.Policy;

    /// <summary>Treats a player character that is not a joined player (a <c>1_Player</c> a lab test spawned) as a player:
    /// rule sets for it apply as they would to a player (vitals, water, flight, the native hooks). For tests without a
    /// client; never needed for real players. Up to 16 at once for the native hooks.</summary>
    [System.Diagnostics.CodeAnalysis.Experimental("KILNX001", UrlFormat = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/analyzers.md#kilnx001")]
    [System.ComponentModel.EditorBrowsable(System.ComponentModel.EditorBrowsableState.Never)]
    [TickThreadOnly]
    public static void Simulate(EntityId character, bool on = true)
    {
        Game.AssertTickThread();
        RulesRuntime.TrackCharacter(character, on);
    }

    /// <summary>Whether a player's game applies the client-side half of the rules (flight, the build cursor).</summary>
    public static RulesClient ClientOf(EntityId player) => RulesRuntime.ClientOf(player);

    /// <summary>The native hooks' state and counters (null when kiln.rules is not loaded).</summary>
    public static RulesStatus? Status()
    {
        var t = RulesNative.T;
        if (t == null) return null;
        RulesNative.Status s = default;
        s.Size = (uint)sizeof(RulesNative.Status);
        if (t->GetStatus(&s) != 0) return null;
        return new RulesStatus((s.Hooks & RulesNative.HookFly) != 0, (s.Hooks & RulesNative.HookCursor) != 0,
            (s.Hooks & RulesNative.HookCosts) != 0, (s.Hooks & RulesNative.HookDig) != 0, (int)s.Players, s.FlyAllowed,
            s.CursorCleared, s.CostsScaled, s.RefundsScaled, s.DigsScaled, s.Faults, RulesNative.Text(s.Detail, 160))
        {
            BuildingZone = (s.Hooks & RulesNative.HookZone) != 0, ShroudTimer = (s.Hooks & RulesNative.HookShroud) != 0,
            Gliding = (s.Hooks & RulesNative.HookGlide) != 0, ZoneForced = s.ZoneForced, ShroudHeld = s.ShroudHeld,
            GlideChanged = s.GlideChanged, FreeCrafting = (s.Hooks & RulesNative.HookCraft) != 0,
            KeptConsumables = (s.Hooks & RulesNative.HookConsume) != 0, KeptFoodAndPotions = (s.Hooks & RulesNative.HookConsumeBuff) != 0,
            CharacterFlight = (s.Hooks & RulesNative.HookFlight) != 0,
            GlideSteer = (s.Hooks & RulesNative.HookGlideSteer) != 0, GlideSteered = s.GlideSteered,
            NoClip = (s.Hooks & RulesNative.HookNoClip) != 0,
            MapReveal = (s.Hooks & RulesNative.HookMap) != 0,
            CraftButtons = (s.Hooks & RulesNative.HookCraftUi) != 0,
            CraftsFree = RulesCrafting.Counters.Crafts, ConsumablesKept = RulesCrafting.Counters.Kept,
        };
    }
}

/// <summary>One mod's rules for a scope. Dispose it to take them away.</summary>
public sealed class RuleSet : IDisposable
{
    private static long _next;

    internal RuleSet(RuleScope scope, PlayerRules rules, string owner, string? label, DateTime? expires)
    {
        Id = Interlocked.Increment(ref _next);
        Scope = scope;
        Rules = rules;
        Owner = owner;
        Label = label ?? "";
        Expires = expires;
    }

    /// <summary>A number that identifies this rule set, unique in the process.</summary>
    public long Id { get; }
    /// <summary>Who the rules apply to.</summary>
    public RuleScope Scope { get; }
    /// <summary>The rules this set asks for. <see cref="Update"/> replaces them.</summary>
    public PlayerRules Rules { get; private set; }

    /// <summary>Id of the mod that applied it ("kiln" outside mod code).</summary>
    public string Owner { get; }

    /// <summary>A name for the set, for logs and listings; empty when none was given.</summary>
    public string Label { get; }

    /// <summary>When it ends by itself (UTC), or null.</summary>
    public DateTime? Expires { get; }

    /// <summary>True until the set ends: it was disposed, it expired, or it was removed with its mod or its player.</summary>
    public bool Active { get; private set; } = true;

    /// <summary>Counts <see cref="Update"/> calls: a request a death or a teleport used up (NoClip) is asked anew by
    /// the next one.</summary>
    internal int Version { get; private set; }

    internal Action? Detach;

    /// <summary>Changes the rules (same scope, owner and end). They count at once: <see cref="Kiln.Rules.Of"/> can be
    /// read right after, as after <see cref="Kiln.Rules.Apply(RuleScope, PlayerRules, TimeSpan?, string?)"/>. Updating
    /// a set with the rules it already has asks again for what a death or a teleport used up
    /// (<see cref="PlayerRules.NoClip"/>).</summary>
    [TickThreadOnly]
    public void Update(PlayerRules rules)
    {
        ArgumentNullException.ThrowIfNull(rules);
        Game.AssertTickThread();
        var problems = rules.Problems();
        if (problems.Count > 0) throw new ArgumentException(string.Join("; ", problems), nameof(rules));
        Rules = rules;
        Version++;
        RulesRuntime.Changed(this);
    }

    /// <summary>Takes the rules away. Disposing a set that has already ended does nothing.</summary>
    public void Dispose()
    {
        if (!Active) return;
        Active = false;
        Detach?.Invoke();
        Detach = null;
        RulesRuntime.Remove(this);
    }

    /// <summary>A short description for logs: the id, the owner, the label and the scope.</summary>
    public override string ToString() => $"#{Id} {Owner}{(Label.Length > 0 ? " '" + Label + "'" : "")} for {Scope}";
}

/// <summary>Whether a player's game applies the client-side half of the rules.</summary>
public enum RulesClient
{
    /// <summary>No network peer is known for the player (yet).</summary>
    Unknown,

    /// <summary>The player is the host of this world: their game is this process.</summary>
    Host,

    /// <summary>A Kiln game with the rules channel: flight and placement work for them.</summary>
    Kiln,

    /// <summary>A Kiln game without the rules channel (an older Kiln): server-side rules only.</summary>
    KilnWithoutRules,

    /// <summary>A game without Kiln: server-side rules only.</summary>
    Vanilla,

    /// <summary>The Kiln handshake has not finished yet.</summary>
    Pending,

    /// <summary>A character <see cref="Rules.Simulate"/> tracks (a lab test): it has no game, so its rules apply on the
    /// server only, and creative mode is allowed for it.</summary>
    Simulated,
}

/// <summary>The state of kiln.rules' hooks and counters (diagnostics).</summary>
/// <param name="Flight">The fly-gate hook works (FlyAnywhere).</param>
/// <param name="Placement">The build cursor's hook works (the placement rules).</param>
/// <param name="Costs">The material cost hooks work (BuildCost, BuildRefund).</param>
/// <param name="Mining">The dig damage hook works (DigDamage).</param>
/// <param name="Players">Server entities that have rules set.</param>
/// <param name="FlyAllowed">Fly-gate decisions turned to allowed.</param>
/// <param name="CursorCleared">Cursor evaluations where a refusal flag was cleared.</param>
/// <param name="CostsScaled">Material lists scaled when placing.</param>
/// <param name="RefundsScaled">Material lists scaled when dismantling and undoing.</param>
/// <param name="DigsScaled">Damage values scaled.</param>
/// <param name="Faults">Hook bodies that faulted; the game's own path ran instead.</param>
/// <param name="Detail">What is switched off and why, as text; empty when nothing is.</param>
public sealed record RulesStatus(bool Flight, bool Placement, bool Costs, bool Mining, int Players, ulong FlyAllowed,
    ulong CursorCleared, ulong CostsScaled, ulong RefundsScaled, ulong DigsScaled, ulong Faults, string Detail)
{
    /// <summary>The server cursor's building-zone hook (BuildOutsideBases: the hammer's "Needs a Flame Altar nearby").</summary>
    public bool BuildingZone { get; init; }

    /// <summary>The Shroud timer's hook (FreezeShroudTimer, MinShroudTime).</summary>
    public bool ShroudTimer { get; init; }

    /// <summary>The glider's hooks (UnlimitedGliding, GlideSink, GlideBoost).</summary>
    public bool Gliding { get; init; }

    /// <summary>Building-zone tests turned to "near a base".</summary>
    public ulong ZoneForced { get; init; }
    /// <summary>Shroud timer steps held or recovered instead of draining.</summary>
    public ulong ShroudHeld { get; init; }
    /// <summary>Glider steps changed (sink, stamina, boost).</summary>
    public ulong GlideChanged { get; init; }

    /// <summary>The recipe-payment hooks (FreeCrafting): crafts pay nothing inside the game's own transaction.</summary>
    public bool FreeCrafting { get; init; }

    /// <summary>The two consume removals (KeepConsumables: fired arrows, thrown items, staff ammunition).</summary>
    public bool KeptConsumables { get; init; }

    /// <summary>The buff item's consume removal (KeepConsumables: food, drinks, potions, bandages).</summary>
    public bool KeptFoodAndPotions { get; init; }

    /// <summary>Character flight's hooks (CharacterFlight): the locomotion state, gravity and the flying mover.</summary>
    public bool CharacterFlight { get; init; }

    /// <summary>The glider's pitch step (GlideSteer; actor_rotation).</summary>
    public bool GlideSteer { get; init; }

    /// <summary>Glider pitch steps steered so far.</summary>
    public ulong GlideSteered { get; init; }

    /// <summary>NoClip's hook on the character's collision step (NoClip; only ever on with <see cref="CharacterFlight"/>).</summary>
    public bool NoClip { get; init; }

    /// <summary>Game client: the fog of war's discover call (the creative menu's map reveal).</summary>
    public bool MapReveal { get; init; }

    /// <summary>Game client: the crafting menu's craftable amount (craft buttons stay enabled with FreeCrafting).</summary>
    public bool CraftButtons { get; init; }

    /// <summary>Recipe payments answered "paid" so far.</summary>
    public ulong CraftsFree { get; init; }

    /// <summary>Consume removals skipped so far.</summary>
    public ulong ConsumablesKept { get; init; }
}

/// <summary>A player's rules changed (a rule set applied, updated, ended, or an area entered or left). Posted on the tick
/// thread; not cancelable.</summary>
public sealed class PlayerRulesChangedEvent : KilnEvent
{
    internal PlayerRulesChangedEvent(EffectiveRules before, EffectiveRules after)
    {
        Before = before;
        After = after;
    }

    /// <summary>The player entity whose rules changed.</summary>
    public EntityId Player => After.Player;
    /// <summary>The player's rules before the change.</summary>
    public EffectiveRules Before { get; }
    /// <summary>The player's rules after the change.</summary>
    public EffectiveRules After { get; }
    /// <summary>A short description for logs: the player and the new rules.</summary>
    public override string ToString() => $"rules of {Player}: {After}";
}

/// <summary>The server's limits on everyone's rules: <c>&lt;configDir&gt;/rules.json</c>.</summary>
public sealed class RulesPolicy
{
    /// <summary>Players may get the building fly mode outside bases (<see cref="PlayerRules.FlyAnywhere"/>). Default true.</summary>
    [Comment("Players may get the building fly mode outside bases (FlyAnywhere).")]
    public bool AllowFlyAnywhere { get; set; } = true;

    /// <summary>Players may build outside bases (<see cref="PlayerRules.BuildOutsideBases"/>). Default true.</summary>
    [Comment("Players may build outside bases (BuildOutsideBases).")]
    public bool AllowBuildOutsideBases { get; set; } = true;

    /// <summary>Players may build in the game's no-build zones around points of interest, dungeons and altars (<see cref="PlayerRules.IgnoreNoBuildZones"/>). Default true.</summary>
    [Comment("Players may build in the game's no-build zones around points of interest, dungeons and altars (IgnoreNoBuildZones).")]
    public bool AllowIgnoreNoBuildZones { get; set; } = true;

    /// <summary>Players may build with enemies nearby (<see cref="PlayerRules.IgnoreEnemiesNearby"/>). Default true.</summary>
    [Comment("Players may build with enemies nearby (IgnoreEnemiesNearby).")]
    public bool AllowIgnoreEnemiesNearby { get; set; } = true;

    /// <summary>Players may build inside the Shroud (<see cref="PlayerRules.BuildInShroud"/>). Default true.</summary>
    [Comment("Players may build inside the Shroud (BuildInShroud).")]
    public bool AllowBuildInShroud { get; set; } = true;

    /// <summary>Players may build in other players' bases without their permission (<see cref="PlayerRules.IgnoreBasePermissions"/>). Default false: other players' bases stay theirs.</summary>
    [Comment("Players may build in other players' bases without their permission (IgnoreBasePermissions). Off by default: other players' bases stay theirs.")]
    public bool AllowIgnoreBasePermissions { get; set; }

    /// <summary>Players may be made invulnerable (<see cref="PlayerRules.Invulnerable"/>). Default true.</summary>
    [Comment("Players may be made invulnerable (Invulnerable).")]
    public bool AllowInvulnerable { get; set; } = true;

    /// <summary>Water containers may get water back (<see cref="PlayerRules.Water"/>). Default true.</summary>
    [Comment("Water containers may get water back (Water).")]
    public bool AllowWater { get; set; } = true;

    /// <summary>The lowest building cost multiplier any rule may give, 0 to 1000 (0 allows free building). Default 0.</summary>
    [Comment("The lowest building cost multiplier any rule may give (0 = free building allowed)."), Range(0, 1000)]
    public double MinBuildCost { get; set; }

    /// <summary>The highest dig damage multiplier any rule may give, 1 to 1000000 (1000 breaks anything breakable in one hit). Default 1000.</summary>
    [Comment("The highest dig damage multiplier any rule may give (1000 = one hit breaks anything breakable)."), Range(1, 1000000)]
    public double MaxDigDamage { get; set; } = 1000;

    /// <summary>The highest health floor any rule may keep, 0 to 1 (1 is full). Default 1.</summary>
    [Comment("The highest health floor any rule may keep (1 = full)."), Range(0, 1)]
    public double MaxHealthFloor { get; set; } = 1;

    /// <summary>The highest stamina floor any rule may keep, 0 to 1 (1 is full). Default 1.</summary>
    [Comment("The highest stamina floor any rule may keep (1 = full)."), Range(0, 1)]
    public double MaxStaminaFloor { get; set; } = 1;

    /// <summary>The highest mana floor any rule may keep, 0 to 1 (1 is full). Default 1.</summary>
    [Comment("The highest mana floor any rule may keep (1 = full)."), Range(0, 1)]
    public double MaxManaFloor { get; set; } = 1;

    /// <summary>The Shroud timer may be frozen for a player (<see cref="PlayerRules.FreezeShroudTimer"/>). Default true.</summary>
    [Comment("The Shroud timer may be frozen for a player (FreezeShroudTimer).")]
    public bool AllowFreezeShroudTimer { get; set; } = true;

    /// <summary>The highest Shroud timer floor any rule may keep, 0 to 1 (1 is full). Default 1.</summary>
    [Comment("The highest Shroud timer floor any rule may keep (1 = full)."), Range(0, 1)]
    public double MaxShroudFloor { get; set; } = 1;

    /// <summary>Gliding may cost no stamina for a player (<see cref="PlayerRules.UnlimitedGliding"/>). Default true.</summary>
    [Comment("Gliding may cost no stamina for a player (UnlimitedGliding).")]
    public bool AllowUnlimitedGliding { get; set; } = true;

    /// <summary>Holding sprint may push a glider along the view (<see cref="PlayerRules.GlideBoost"/>). Default true.</summary>
    [Comment("Holding sprint may push a glider along the view (GlideBoost).")]
    public bool AllowGlideBoost { get; set; } = true;

    /// <summary>The fastest a boosted glider may go, m/s, 0 to 120. Default 60.</summary>
    [Comment("The fastest a boosted glider may go, m/s."), Range(0, 120)]
    public double MaxGlideSpeed { get; set; } = 60;

    /// <summary>The glider's own nose may be steered for a player (<see cref="PlayerRules.GlideSteer"/>). It applies only for a player whose game confirms it steers too; others glide as before. Default true.</summary>
    [Comment("The glider's own nose may be steered for a player (GlideSteer): S raises it, letting go holds it, W dives. Applied only for a player whose game confirms it steers too; others glide as before.")]
    public bool AllowGlideSteer { get; set; } = true;

    /// <summary>How far above level a steered glider's nose may rise, degrees, 0 to 85 (85 is almost straight up, 0 is level at most). Default 85.</summary>
    [Comment("How far above level a steered glider's nose may rise, degrees (85 = almost straight up, 0 = level at most)."), Range(0, 85)]
    public double MaxGlideSteerAngle { get; set; } = 85;

    /// <summary>Falls may do no damage for a player (<see cref="PlayerRules.NoFallDamage"/>). Default true.</summary>
    [Comment("Falls may do no damage for a player (NoFallDamage).")]
    public bool AllowNoFallDamage { get; set; } = true;

    /// <summary>Allows requesting <see cref="PlayerRules.FreeCrafting"/>. Currently unavailable without an authoritative transaction hook. Default true.</summary>
    [Comment("Allows requesting FreeCrafting; currently unavailable without an authoritative transaction hook.")]
    public bool AllowFreeCrafting { get; set; } = true;

    /// <summary>Allows requesting <see cref="PlayerRules.KeepConsumables"/>: arrows, ammunition, food and potions are not used up. Default true.</summary>
    [Comment("Allows requesting KeepConsumables (arrows, ammunition, food, potions are not used up).")]
    public bool AllowKeepConsumables { get; set; } = true;

    /// <summary>The character itself may fly for a player (<see cref="PlayerRules.CharacterFlight"/>). Default false: off until the server turns it on.</summary>
    [Comment("The character itself may fly for a player (CharacterFlight). Off until the server turns it on: new, and tested on fewer setups than the rest.")]
    public bool AllowCharacterFlight { get; set; }

    /// <summary>A player may move through terrain, blocks and props (<see cref="PlayerRules.NoClip"/>, also <c>/noclip</c>). Default false: off until the server turns it on; a tool for admins and for debugging, not for normal play.</summary>
    [Comment("A player may move through terrain, blocks and props (NoClip, also /noclip). Off until the server turns it on: a tool for admins and for debugging, not for normal play.")]
    public bool AllowNoClip { get; set; }

    internal RulesLimits Limits => new(AllowFlyAnywhere, AllowBuildOutsideBases, AllowIgnoreNoBuildZones, AllowIgnoreEnemiesNearby,
        AllowBuildInShroud, AllowIgnoreBasePermissions, AllowInvulnerable, MinBuildCost, MaxDigDamage, MaxStaminaFloor, MaxHealthFloor,
        MaxManaFloor, AllowWater, AllowFreezeShroudTimer, MaxShroudFloor, AllowUnlimitedGliding, AllowGlideBoost, MaxGlideSpeed,
        AllowNoFallDamage, AllowFreeCrafting, AllowKeepConsumables, AllowCharacterFlight, AllowGlideSteer, MaxGlideSteerAngle,
        AllowNoClip);
}

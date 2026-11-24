// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Text.Json;

namespace Kiln;

/// <summary>One Creative contribution. Effective and Restore are display text, not a parseable rules API.
/// Requested describes Creative's own granting fields, not the merged result. Locked is null when restoring is allowed,
/// otherwise a reason; an existing contribution can still be withdrawn. OtherOwners reports other scoped grants.</summary>
/// <param name="Name">The effect's stable name, as /creative effect and <c>Creative.SetEffect</c> take it.</param>
/// <param name="Label">The effect's display name for menus.</param>
/// <param name="Requested">Whether Creative itself currently grants this effect for the player.</param>
/// <param name="Effective">Display text for the merged result the player gets now, after other rules and server limits.</param>
/// <param name="Restore">Display text for what switching the effect back on would grant.</param>
/// <param name="Locked">Null when the effect can be switched back on; otherwise the reason it can't (server preset or
/// policy, a limit that cancels it out, another owner's rule).</param>
/// <param name="OtherOwners">Whether other mods or rule sets also grant this effect for the player.</param>
public sealed record CreativeEffectState(string Name, string Label, bool Requested, string Effective, string Restore,
    string? Locked, bool OtherOwners);

// The explicit byte IDs are a private kiln:creative v1 contract. Never derive them from display order or reuse an ID.
// All optional PlayerRules instance properties belong to exactly one group (covered by a completeness test).
internal sealed class CreativeEffect(byte id, string name, string label, params string[] fields)
{
    public byte Id { get; } = id;
    public string Name { get; } = name;
    public string Label { get; } = label;
    public PropertyInfo[] Fields { get; } = fields.Select(f => typeof(PlayerRules).GetProperty(f)!).ToArray();

    public bool Requested(PlayerRules rules) => Fields.Any(f => Grants(f, f.GetValue(rules)));
    internal static bool Grants(PropertyInfo field, object? value) => value switch
    {
        null => false,
        bool b => b,
        CharacterFlightMode m => m != CharacterFlightMode.Off,
        double d => field.Name is "BuildCost" or "BuildRefund" or "GlideSink" ? d != 1 :
            field.Name == "DigDamage" ? d != 1 : d > 0,
        _ => true,
    };

    public PlayerRules Edit(PlayerRules current, PlayerRules restore, bool on)
    {
        var next = current with { };
        foreach (var f in Fields)
        {
            // Off withdraws grants, never adds false/zero/Off to deny other owners. Existing preset denials remain.
            var value = on ? f.GetValue(restore) : f.GetValue(current);
            f.SetValue(next, on || !Grants(f, value) ? value : null);
        }
        return next;
    }

    public string Display(object rules) => Short(string.Join(", ", Fields.Select(f =>
        $"{char.ToLowerInvariant(f.Name[0])}{f.Name[1..]}={Value(rules.GetType().GetProperty(f.Name)!.GetValue(rules))}")));

    private static string Short(string text) => text.Length <= 380 ? text : text[..377] + "...";

    private static string Value(object? value) => value is null ? "game" : value switch
    {
        bool b => b ? "on" : "off",
        double d => d.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture),
        CharacterFlightMode m => m.ToString(),
        _ => JsonSerializer.Serialize(value, PlayerRules.Json).ReplaceLineEndings("").Replace("  ", ""),
    };

    public string? Denial(PlayerRules restore, EffectiveRules preview)
    {
        if (!Requested(restore)) return "Excluded by the Creative preset (no granting value to restore).";
        foreach (var f in Fields)
        {
            var asked = f.GetValue(restore);
            if (!Grants(f, asked)) continue;
            var key = char.ToLowerInvariant(f.Name[0]) + f.Name[1..];
            if (preview.Denied.Contains(key + "Unavailable"))
                return $"Required execution support is unavailable for {key}.";
            if (preview.Denied.Contains(key)) return $"Server policy denies {key}.";
            var effective = typeof(EffectiveRules).GetProperty(f.Name)!.GetValue(preview);
            if (asked is true && effective is false || asked is CharacterFlightMode && effective is CharacterFlightMode.Off ||
                asked is not null && effective is null)
                return $"Server limits or another rule deny {key}.";
        }
        return null;
    }

    public string? NumericPolicyDenial(EntityId player, PlayerRules restore, RulesLimits limits)
    {
        var asked = Fields.Where(f => Grants(f, f.GetValue(restore))).ToArray();
        if (!asked.Any(f => f.GetValue(restore) is double)) return null;
        // Compare this group's request in isolation with the policy-held game baseline, not the combined owners.
        // A capped cost can still have a refund effect, and another owner saturating a cap is not an exclusion.
        var held = RulesMath.Combine(player, [(null, Edit(new PlayerRules(), restore, true))], limits);
        var baseline = RulesMath.Combine(player, [], limits);
        return asked.All(f => Equals(typeof(EffectiveRules).GetProperty(f.Name)!.GetValue(held),
                typeof(EffectiveRules).GetProperty(f.Name)!.GetValue(baseline)))
            ? "Server numeric limits remove this entire Creative effect." : null;
    }
}

internal readonly record struct CreativeManagedSupport(bool Vitals, bool Invulnerability, bool Falls, bool Water);

internal static class CreativeEffects
{
    public static readonly CreativeEffect[] All =
    [
        new(1, "building-flight", "Building flight + carry body", "FlyAnywhere", "FlightCarriesBody"),
        new(2, "build-outside", "Build outside bases + keep builds", "BuildOutsideBases", "KeepBuildsOutsideBases"),
        new(3, "no-build-zones", "Ignore no-build zones", "IgnoreNoBuildZones"),
        new(4, "near-enemies", "Build near enemies", "IgnoreEnemiesNearby"),
        new(5, "build-in-shroud", "Build in Shroud", "BuildInShroud"),
        new(6, "base-permissions", "Ignore base permissions", "IgnoreBasePermissions"),
        new(7, "building-costs", "Building costs + refunds", "BuildCost", "BuildRefund"),
        new(8, "free-crafting", "Free crafting", "FreeCrafting"),
        new(9, "no-consume", "Keep consumables / ammunition", "KeepConsumables"),
        new(10, "mining", "Mining damage", "DigDamage"),
        new(11, "invulnerability", "Invulnerability + health floor", "Invulnerable", "MinHealth"),
        new(12, "no-fall", "No fall damage (not landing grace)", "NoFallDamage"),
        new(13, "stamina", "Stamina floor", "MinStamina"),
        new(14, "mana", "Mana floor", "MinMana"),
        new(15, "shroud-timer", "Shroud timer freeze + floor", "FreezeShroudTimer", "MinShroudTime"),
        new(16, "unlimited-gliding", "Unlimited glider duration", "UnlimitedGliding"),
        new(17, "glider-sink", "Glider sink multiplier", "GlideSink"),
        new(18, "glider-boost", "Glider boost", "GlideBoost"),
        new(19, "glider-steering", "Glider steering", "GlideSteer"),
        new(20, "water", "Water refill", "Water"),
        new(21, "character-flight", "Character flight (not building/glider flight)", "CharacterFlight"),
        new(22, "no-clip", "No clip", "NoClip"),
    ];

    internal static uint RequiredHooks(CreativeEffect effect) => effect.Id switch
    {
        1 => RulesNative.HookFly,
        2 => RulesNative.HookZone | RulesNative.HookCursor,
        3 or 4 or 5 or 6 => RulesNative.HookCursor,
        7 => RulesNative.HookCosts,
        10 => RulesNative.HookDig,
        15 => RulesNative.HookShroud,
        16 or 17 or 18 => RulesNative.HookGlide,
        19 => RulesNative.HookGlide | RulesNative.HookGlideSteer,
        21 => RulesNative.HookFlight,
        22 => RulesNative.HookNoClip | RulesNative.HookFlight,
        _ => 0, // craft/consume have their existing alternative-hook rules in RulesCrafting.Supported
    };

    // Legacy rules still merge as before. The effect UI/preflight additionally distinguishes a merged request from
    // an actual native grant when an original (fly/costs/dig/glide/etc.) hook is absent on this build.
    internal static EffectiveRules Supported(EffectiveRules rules, uint hooks)
    {
        var result = rules with { };
        var denied = rules.Denied.ToList();
        var game = EffectiveRules.None(rules.Player);
        foreach (var effect in All)
        {
            var required = RequiredHooks(effect);
            if ((hooks & required) == required) continue;
            foreach (var field in effect.Fields)
            {
                var target = typeof(EffectiveRules).GetProperty(field.Name)!;
                if (CreativeEffect.Grants(field, target.GetValue(result)))
                    denied.Add(char.ToLowerInvariant(field.Name[0]) + field.Name[1..] + "Unavailable");
                target.SetValue(result, target.GetValue(game));
            }
        }
        return result with { Denied = denied };
    }

    // Synthetic tests may replace the availability snapshot; production always uses the existing service/status APIs.
    internal static Func<CreativeManagedSupport>? ManagedProbe;
    internal static CreativeManagedSupport ManagedSupport()
    {
        if (ManagedProbe is { } probe) return probe();
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null; // service probes must not acquire listener/capability lifetime for the caller's mod
        try
        {
            return new(
                Available(() => Abilities.Available && Abilities.Status is { Attributes: true }),
                Available(() => Combat.Available),
                Available(() => Combat.Available && Combat.Status is { Falls: true }),
                Available(() => Items.Fuel.Available));
        }
        finally { ModContext.CurrentMod = outer; }
    }

    private static bool Available(Func<bool> read)
    {
        try { return read(); }
        catch (Exception e) when (e is FileNotFoundException or FileLoadException or TypeLoadException or MissingMethodException or KilnUnavailableException)
        { return false; }
    }

    internal static EffectiveRules SupportedManaged(EffectiveRules rules, CreativeManagedSupport support)
    {
        var denied = rules.Denied.ToList();
        if (!support.Vitals)
        {
            if (rules.MinHealth > 0) denied.Add("minHealthUnavailable");
            if (rules.MinStamina > 0) denied.Add("minStaminaUnavailable");
            if (rules.MinMana > 0) denied.Add("minManaUnavailable");
        }
        if (!support.Invulnerability && rules.Invulnerable) denied.Add("invulnerableUnavailable");
        if (!support.Falls && rules.NoFallDamage) denied.Add("noFallDamageUnavailable");
        if (!support.Water && rules.Water is not null) denied.Add("waterUnavailable");
        return rules with
        {
            MinHealth = support.Vitals ? rules.MinHealth : 0,
            MinStamina = support.Vitals ? rules.MinStamina : 0,
            MinMana = support.Vitals ? rules.MinMana : 0,
            Invulnerable = support.Invulnerability && rules.Invulnerable,
            NoFallDamage = support.Falls && rules.NoFallDamage,
            Water = support.Water ? rules.Water : null,
            Denied = denied,
        };
    }

    public static CreativeEffect? Find(string? name) => name is { Length: > 0 and <= 64 }
        ? All.FirstOrDefault(e => string.Equals(e.Name, name, StringComparison.OrdinalIgnoreCase)) : null;
    public static CreativeEffect? Find(uint id) => All.FirstOrDefault(e => e.Id == id);
}

public static partial class Creative
{
    private static readonly Dictionary<EntityId, PlayerRules> Defaults = new();

    /// <summary>Restores or withdraws one named effect on Creative's own live rule set. On restores the captured entry
    /// preset (or the last explicit /creative flight selection); it never fills missing preset fields with built-in
    /// defaults. Refused inputs, exclusions and denials do not mutate rules. Trusted server API, like Set; callers
    /// must enforce their existing authorization. Off leaves other owners and temporary landing safety alone.</summary>
    [TickThreadOnly]
    public static CreativeResult SetEffect(EntityId player, string name, bool on)
    {
        Game.AssertTickThread();
        var effect = CreativeEffects.Find(name);
        if (effect is null) return new(false, IsOn(player), "Unknown Creative effect. Use /creative effect for names.");
        if (CreativeFlame.Running) return new(false, false, CreativeFlame.Note + ".");
        if (!Sets.TryGetValue(player, out var set) || !set.Active)
            return new(false, false, "Creative mode must be on before changing its effects.");
        var restore = Defaults.GetValueOrDefault(player) ?? set.Rules;
        if (on && LockReason(player, set, effect, restore) is { } why) return new(false, true, why);
        var next = effect.Edit(set.Rules, restore, on);
        if (next != set.Rules) Update(set, next);
        CreativeServer.Push(player);
        var state = Effects(player).Single(e => e.Name == effect.Name);
        return new(true, true, $"{effect.Label}: Creative {(state.Requested ? "on" : "off")}; effective {state.Effective}." +
            (state.OtherOwners ? " Other owners also grant this effect." : ""));
    }

    /// <summary>Fresh status of every named effect, including locked/excluded effects. Requested and Locked describe
    /// Creative, not other owners. Effective and Restore are human-readable display values, not parseable API.</summary>
    [TickThreadOnly]
    public static IReadOnlyList<CreativeEffectState> Effects(EntityId player)
    {
        Game.AssertTickThread();
        Sets.TryGetValue(player, out var set);
        var active = set is { Active: true } && !CreativeFlame.Running;
        var current = active ? set!.Rules : new PlayerRules();
        var restore = active ? Defaults.GetValueOrDefault(player) ?? current : new PlayerRules();
        var support = CreativeEffects.ManagedSupport();
        var effective = EffectEffective(player, RulesRuntime.Fresh(player), support);
        var others = EffectEffective(player, RulesRuntime.Preview(player, set, new PlayerRules()), support);
        return CreativeEffects.All.Select(e => new CreativeEffectState(e.Name, e.Label, active && e.Requested(current),
            e.Display(effective) + (e.Name is "invulnerability" or "no-fall" && RulesRuntime.LandingGrace(player)
                ? support.Invulnerability ? "; temporary safe-landing protection active (separate)" : "; safe-landing grace active but Combat protection unavailable" : "") +
            (e.Name == "character-flight" && effective.CharacterFlight == CharacterFlightMode.BuildingArea && !effective.FlightAreaAllowed
                ? "; outside a verified building area: flight inactive" : ""), e.Display(restore),
            active ? LockReason(player, set!, e, restore, support) : "Creative mode is not active.",
            e.Fields.Any(f => CreativeEffect.Grants(f, typeof(EffectiveRules).GetProperty(f.Name)!.GetValue(effective)) &&
                CreativeEffect.Grants(f, typeof(EffectiveRules).GetProperty(f.Name)!.GetValue(others)) &&
                others.From.Any(s => !ReferenceEquals(s, set) && CreativeEffect.Grants(f, f.GetValue(s.Rules))))))
            .ToArray();
    }

    internal static string EffectStatus(EntityId player, string? name = null)
    {
        if (name is not null && CreativeEffects.Find(name) is null) return "Unknown Creative effect. Use /creative effect for names.";
        return string.Join("\n", Effects(player).Where(e => name is null || e.Name.Equals(name, StringComparison.OrdinalIgnoreCase))
            .Select(e => $"{e.Name}: Creative {(e.Requested ? "on" : "off")}; effective {e.Effective}; on restores {e.Restore}" +
                (e.Locked is { } why ? $"; locked: {why}" : "") + (e.OtherOwners ? "; other owners also grant it" : "")));
    }

    private static string? LockReason(EntityId player, RuleSet set, CreativeEffect effect, PlayerRules restore,
        CreativeManagedSupport? support = null) =>
        effect.Denial(restore, EffectEffective(player, RulesRuntime.Preview(player, set, effect.Edit(set.Rules, restore, true)), support)) ??
        effect.NumericPolicyDenial(player, restore, RulesRuntime.Limits);

    // These fields are predicted in the player's client. Fresh includes server hooks and policy; report the matching
    // client gate too, rather than describing a request as effective when the player's native snapshot strips it.
    private static EffectiveRules EffectEffective(EntityId player, EffectiveRules rules, CreativeManagedSupport? support = null)
    {
        rules = CreativeEffects.Supported(rules, RulesNative.Hooks);
        rules = CreativeEffects.SupportedManaged(rules, support ?? CreativeEffects.ManagedSupport());
        var denied = rules.Denied.ToList();
        if (rules.CharacterFlight != CharacterFlightMode.Off && !RulesRuntime.CanPredictFlight(player))
        { rules = rules with { CharacterFlight = CharacterFlightMode.Off }; denied.Add("characterFlightUnavailable"); }
        if (rules.GlideSteer is not null && !RulesRuntime.CanPredictSteer(player))
        { rules = rules with { GlideSteer = null }; denied.Add("glideSteerUnavailable"); }
        if (rules.NoClip && !RulesRuntime.CanPredictNoClip(player))
        { rules = rules with { NoClip = false }; denied.Add("noClipUnavailable"); }
        return rules with { Denied = denied };
    }

    private static void Update(RuleSet set, PlayerRules next)
    {
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;
        try { RulesRuntime.UpdateCreative(set, next); }
        finally { ModContext.CurrentMod = outer; }
    }

    internal static void Forget(EntityId player) { Sets.Remove(player); Defaults.Remove(player); }
}

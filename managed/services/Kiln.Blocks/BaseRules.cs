// SPDX-License-Identifier: MIT
// Kiln's base rules (since 0.2.0-dev.31; docs/kiln/services/build.md "Base rules and base limits"): how many Flame Altars
// a world may have and how large an altar's build zone is, in Kiln's own names. Staged in Mod.OnBoot like a block, or
// written in blocks/*.json ("baseRules"); kiln.content checks them in the boot generation's prepare and composes every
// mod's rules into one raise-only change of the game's balancing table (native/services/content/baserules.hpp). Not
// compiled by Kiln.Blocks: the record is the document kiln.content reads.
using System.Numerics;
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>Base rules: room for more Flame Altars and larger build zones, without the game's names. Stage it in
/// <see cref="Mod.OnBoot"/>: <c>BootDefinitions.For(boot).Define("roomy", new BaseRulesBlock { MinimumAltars = 8 })</c>, or
/// write it in a blocks file under <c>"baseRules"</c>. Every member is optional; a rule never lowers the game's numbers.
/// Kiln combines every mod's rules: at each Flame level the world gets the largest count any rule asks for, each altar
/// level the largest size on each axis. Two mods with base rules never conflict, and a mod that changes the same numbers
/// with its own resource patch (by the game's names) is not refused either: Kiln's rules are applied after every patch and
/// only ever raise. The change is made before the game starts and holds until it stops; <see cref="Build.BaseLimits"/>
/// reads the result back. Needs <c>"boot": true</c> in kiln.mod.json and the capability <c>build.base_rules</c>.</summary>
/// <example>
/// <code>
/// public override void OnBoot(BootContext boot) =>
///     BootDefinitions.For(boot).Define("roomy", new BaseRulesBlock
///     {
///         MinimumAltars = 6, ExtraAltars = 2, BuildAreaPercent = 150,
///         Sizes = [new AltarSize(1, new Vector3(60, 60, 60))],
///     });
/// </code>
/// </example>
public sealed record BaseRulesBlock : IBlock
{
    /// <summary>The most Flame Altars a rule can ask for: the game counts no more than 16.</summary>
    public const int MaxAltars = 16;

    /// <summary>At least this many Flame Altars at every Flame level (1 to 16); null leaves the game's numbers as the
    /// floor. Above 16 is refused: the game counts no more.</summary>
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public int? MinimumAltars { get; init; }

    /// <summary>This many more Flame Altars than the game's own number at every Flame level (0 to 15); the result is at
    /// most 16.</summary>
    public int ExtraAltars { get; init; }

    /// <summary>Every altar level's build zone at this share of the game's own size (100 to 200 percent), at most 256
    /// metres on an axis; null keeps the game's sizes.</summary>
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public int? BuildAreaPercent { get; init; }

    /// <summary>At least this full size for an altar of the given level (1 to 5), on each axis: finite, 0.5 to 256 metres, a
    /// multiple of 0.5. One entry per level.</summary>
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public IReadOnlyList<AltarSize>? Sizes { get; init; }

    /// <summary>What kiln.content would refuse in the rule's values, as Kiln.Blocks can see it first; null when nothing.</summary>
    internal string? Problem()
    {
        if (MinimumAltars is > MaxAltars)
            return $"MinimumAltars is {MinimumAltars}: at most {MaxAltars} Flame Altars (the game counts no more than 16; above 16 is not a base rule)";
        if (MinimumAltars is < 1) return $"MinimumAltars is {MinimumAltars}: from 1 to {MaxAltars}";
        if (ExtraAltars is < 0 or > MaxAltars - 1) return $"ExtraAltars is {ExtraAltars}: from 0 to {MaxAltars - 1}";
        if (BuildAreaPercent is < 100 or > 200) return $"BuildAreaPercent is {BuildAreaPercent}: from 100 to 200";
        var seen = new HashSet<int>();
        for (var i = 0; i < (Sizes?.Count ?? 0); i++)
        {
            var s = Sizes![i];
            if (s is null) return $"Sizes[{i}] is null";
            if (s.AltarLevel is < 1 or > 5) return $"Sizes[{i}]: AltarLevel is {s.AltarLevel}: from 1 to 5 (an altar level the game has, not 0)";
            if (!seen.Add(s.AltarLevel)) return $"Sizes[{i}]: altar level {s.AltarLevel} is named twice";
            foreach (var (axis, v) in new[] { ("X", s.Size.X), ("Y", s.Size.Y), ("Z", s.Size.Z) })
            {
                if (!float.IsFinite(v) || v < 0.5f || v > 256f) return $"Sizes[{i}]: {axis} is {v}: each axis is from 0.5 to 256 metres";
                if (v * 2 != MathF.Floor(v * 2)) return $"Sizes[{i}]: {axis} is {v}: each axis is a multiple of 0.5 metres";
            }
        }
        return null;
    }
}

// SPDX-License-Identifier: MIT
// dev.31 package 18, larger bases (out/18-survivors/CLIENT-CHECKS.md): Kiln's base rules in a real game, with the
// data-only fixture roomy-bases-sample (mods/samples/RoomyBases: at least 6 Flame Altars, 2 more than the game at every
// level, zones at 150%, a level-1 zone of at least 64 m). 18-c18.2: a third altar past the game's count at Flame level 1,
// placed by the player through the game's own input. 18-c18.3: the bigger zone's border, building inside it, and what a
// joined vanilla player (W) sees. L hosts, Kiln on. Orders 2482-2483.
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    private const string RoomyBases = "roomy-bases-sample";

    [ClientCheck("18-c18.2", "Base rules: more Flame Altars than the game allows at this Flame level, placed with the game's own input",
        Area = SurvivorsArea, Since = Dev31, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2482)]
    public async Task BaseRulesCount(CheckContext c)
    {
        if (!await Need(c, RoomyBases) || !await NeedPlayer(c) || !await NeedOpen(c, "build.base_limits")) return;
        var limits = Build.BaseLimits;
        if (!c.Expect(limits is not null, "the world's base limits read", "no progress entity yet")) { await c.Verdict(); return; }
        c.Log.Info($"client-pass: 18-c18.2 flame {limits!.FlameLevel}, altars {limits.CurrentAltars}, the game's {limits.OriginalCount}, " +
                   $"this start's {limits.EffectiveCount}, rules {string.Join(", ", limits.AppliedRules)}");
        c.Expect(limits.EffectiveCount > limits.OriginalCount, "the base rules raise the count at this Flame level",
            $"{limits.OriginalCount} -> {limits.EffectiveCount}");
        c.Instruct($"Use a world at Flame level 1 (the game allows {limits.OriginalCount} Flame Altars there; this start allows " +
                   $"{limits.EffectiveCount}). Place Flame Altars the normal way (build menu, place with the mouse) far apart, " +
                   $"until you have {Math.Min(limits.OriginalCount + 1, limits.EffectiveCount)}. Does the game let you place the one past its own " +
                   "number, with no error message and the usual altar menu on it?");
        await c.Verdict();
        var after = Build.BaseLimits;
        c.Log.Info($"client-pass: 18-c18.2 altars now {after?.CurrentAltars}");
        c.Expect(after is not null && after.CurrentAltars > limits.OriginalCount, "the world counts more altars than the game's own number",
            $"{after?.CurrentAltars}");
        c.Instruct($"Keep placing until you have {limits.EffectiveCount}, then try one more. Is the last one refused with the game's own " +
                   "message (the limit is the base rules' number, not the game's)?");
        await c.Verdict();
        c.Instruct("Open the Flame Altar's menu and the map's base list. Do they show every altar, with no wrong count anywhere?");
        await c.Verdict();
        c.Instruct("Second player: W joins this world with Kiln and roomy-bases-sample (the modpack hands it over). Can W place an " +
                   "altar past the game's number too? Then W joins WITHOUT Kiln (Kiln moved aside on W): is W refused at the " +
                   "join (the mod is for every player), or what does W see when it tries to place one? Write what happened.");
        await c.Verdict();
        c.Instruct("Save, quit, host again: are all altars still there?");
        await c.Verdict();
    }

    [ClientCheck("18-c18.3", "Base rules: the bigger build zone's border, building inside it, a vanilla player's view",
        Area = SurvivorsArea, Since = Dev31, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2483)]
    public async Task BaseRulesSize(CheckContext c)
    {
        if (!await Need(c, RoomyBases) || !await NeedPlayer(c) || !await NeedOpen(c, "build.base_limits")) return;
        var limits = Build.BaseLimits;
        var level1 = limits?.EffectiveSizes.FirstOrDefault(s => s.AltarLevel == 1);
        var game1 = limits?.OriginalSizes.FirstOrDefault(s => s.AltarLevel == 1);
        if (!c.Expect(level1 is not null && game1 is not null && level1.Size.X > game1.Size.X, "the base rules enlarge the level-1 zone",
                $"{game1?.Size} -> {level1?.Size}")) { await c.Verdict(); return; }
        c.Instruct($"Place a new Flame Altar (level 1) on open ground and stand next to it. Press continue.");
        await c.Verdict();
        var me = World.Position(c.Player);
        var zone = me is { } p ? Build.Bases.Where(b => !b.Abandoned).OrderBy(b => Math.Abs(b.Center.X - p.X) + Math.Abs(b.Center.Z - p.Z)).FirstOrDefault() : null;
        c.Log.Info($"client-pass: 18-c18.3 zone {zone?.Center} half {zone?.Half}, want half {level1!.Size / 2}");
        c.Expect(zone is not null && Math.Abs(zone.Half.X * 2 - level1.Size.X) < 0.01, "the server's zone of the new altar has the rules' size",
            $"{zone?.Half}");
        var inside = (level1.Size.X / 2 + game1!.Size.X / 2) / 2;  // between the game's border and the new one
        c.Instruct($"Take the build hammer. Is the zone outline about {level1.Size.X:0} m on a side (the game's is {game1.Size.X:0} m)? " +
                   $"Walk about {inside:0} m from the altar (past the game's border, inside the new one) and place a block and a prop " +
                   "there. Are both allowed and still there after you leave and come back?");
        await c.Verdict();
        c.Instruct("Walk past the new border and try to build: is it refused there, as outside any base?");
        await c.Verdict();
        c.Instruct("Vanilla player: W joins WITHOUT Kiln if the join lets it (else write 'refused at the join'). What outline does W's " +
                   "game draw, and can W build at the same spot past the game's border? Write what W sees: it decides whether the " +
                   "rules can ever be for the server alone.");
        await c.Verdict();
        c.Instruct("Save, quit, host again: is the block past the game's border still there?");
        await c.Verdict();
    }
}

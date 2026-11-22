// SPDX-License-Identifier: MIT
// dev.31 package 18 (out/18-survivors/CLIENT-CHECKS.md): Survivors in a real game. 18-c18.1 recruit, tool, summon and the
// personal-progression record; 18-c18.4 beds: labels, displacement, the game's own tool on a Kiln bed, a player without the
// base-edit permission (machine W joined to L's world). Server half in the hosted world (L hosts, Kiln on). The checks
// recruit a survivor of a fresh copied world: use a copy, never a world you play. Orders 2480-2489.
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    private const string SurvivorsArea = "Survivors";
    private static readonly SurvivorId LabSurvivor = new(0x672da6c9);  // NPC_Workshop_Blacksmith_Assistant_01

    private static string Show(SurvivorResult r) => $"{r.Completion} effects={r.Effects} reason={r.Reason ?? "-"}";

    [ClientCheck("18-c18.1", "Recruit from code: the survivor without its rescue scene, its summon tool, summoned into your base",
        Area = SurvivorsArea, Since = Dev31, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2480)]
    public async Task SurvivorRecruit(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedOpen(c, Survivors.RecruitCapability) || !await NeedOpen(c, Survivors.SummonCapability)) return;
        var s = Survivors.Find(LabSurvivor);
        if (!c.Expect(s is not null, "the roster has the Blacksmith's assistant", "not in this game's roster")) { await c.Verdict(); return; }
        if (s!.Unlocked)
        {
            c.Instruct("The Blacksmith's assistant is already rescued in this world. Host a copy of a fresh world (before the " +
                       "Blacksmith's rescue) and run this check again.");
            await c.Skip("needs a world with the survivor still locked");
            return;
        }
        c.Instruct("Open the Survivors page of your journal and note that the Blacksmith's assistant is not rescued. Stand inside " +
                   "your own base (near its Flame Altar). Press continue.");
        await c.Verdict();
        var me = Players.Find(c.Player);
        var rec = await Survivors.RecruitWorld(LabSurvivor, new SurvivorRecruitOptions(me));
        c.Log.Info($"client-pass: 18-c18.1 recruit {Show(rec)}");
        c.Expect(rec.Ok && rec.Effects.HasFlag(SurvivorEffects.KnowledgeWritten | SurvivorEffects.RegistryConfirmed | SurvivorEffects.ToolGiven),
            "recruited for the world, the registry followed, one summon tool given", Show(rec));
        c.Instruct("Open the Survivors page again. Is the Blacksmith's assistant shown as rescued (no rescue scene was played)?");
        await c.Verdict();
        c.Instruct("Open your inventory. Is there exactly one new summon tool for the Blacksmith's assistant?");
        await c.Verdict();
        var here = World.Position(c.Player);
        var baseId = here is { } p ? Survivors.BaseAt(p) : null;
        if (!c.Expect(baseId is not null, "you stand in a base", $"no base zone holds {here}")) { await c.Verdict(); return; }
        var spot = Ahead(c, 3) ?? here!.Value;
        var sum = await Survivors.Summon(LabSurvivor, baseId!.Value, spot);
        c.Log.Info($"client-pass: 18-c18.1 summon {Show(sum)} home {sum.After.Home}");
        c.Expect(sum.Ok && sum.After.BaseId == baseId, "summoned into your base", Show(sum));
        c.Instruct("Is the Blacksmith's assistant standing about 3 m in front of you, on the ground (not floating, not inside " +
                   "the ground)? Does it stay around there and talk to you like a rescued survivor?");
        await c.Verdict();
        c.Instruct("Use the new summon tool on a free spot of your base, as the game lets you. Does it move the assistant there " +
                   "(the game's own tool works on a survivor Kiln recruited)? The tool is not used up in vanilla either.");
        await c.Verdict();
        c.Instruct("Personal progression: in the world settings, is personal progression on? Write on or off in the notes. With " +
                   "it on, does your character's own Survivors page count the assistant as found? Record what you see; Kiln " +
                   "promises only the world's rescue.");
        await c.Verdict();
        c.Instruct("Save and quit to the main menu, host the same world again. Is the assistant still rescued, in your base, " +
                   "and is the summon tool still in your inventory?");
        await c.Verdict();
    }

    [ClientCheck("18-c18.4", "Beds from code: the bed's name label, moving to another bed, displacement, the game's tool, a player without rights",
        Area = SurvivorsArea, Since = Dev31, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 2481)]
    public async Task SurvivorBeds(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedOpen(c, Survivors.AssignCapability)) return;
        var a = Survivors.Find(LabSurvivor);
        if (a is not { Unlocked: true, Entity.IsNone: false })
        {
            c.Instruct("Run 18-c18.1 first: the Blacksmith's assistant must be rescued and in your base.");
            await c.Skip("needs the survivor of 18-c18.1 in the world");
            return;
        }
        c.Instruct("Place two single beds and one double bed in your base, within 20 m of the assistant (build them as usual). " +
                   "Stand next to the first single bed. Press continue.");
        await c.Verdict();
        var near = World.Position(c.Player);
        List<(EntityId Bed, double D)> beds = [];
        foreach (var e in Spawn.FindByName("Bed", near, 40))
            if (World.Position(e) is { } bp && near is { } np)
            {
                try
                {
                    _ = Survivors.BedOccupant(e);
                    beds.Add((e, Math.Sqrt((bp.X - np.X) * (bp.X - np.X) + (bp.Z - np.Z) * (bp.Z - np.Z))));
                }
                catch (ArgumentException) { }  // a double bed's frame: not a sleeping place
            }
        beds.Sort((x, y) => x.D.CompareTo(y.D));
        c.Log.Info($"client-pass: 18-c18.4 sleeping places near you: {string.Join(", ", beds.Select(b => $"{b.Bed} {b.D:0.0} m"))}");
        if (!c.Expect(beds.Count >= 3, "three or more sleeping places found near you", $"{beds.Count}")) { await c.Verdict(); return; }
        var bed1 = beds[0].Bed;
        var me = new SurvivorAssignmentOptions(c.Player);
        var r1 = await Survivors.AssignBed(LabSurvivor, bed1, me);
        c.Log.Info($"client-pass: 18-c18.4 assign 1 {Show(r1)}");
        c.Expect(r1.Ok && Survivors.BedOccupant(bed1) == LabSurvivor, "assigned to the bed next to you (with your rights)", Show(r1));
        c.Instruct("Look at the bed next to you. Does it show the assistant's name as the bed's owner (the game's bed label)? " +
                   "Does the assistant's entry on the Survivors page show it has a bed?");
        await c.Verdict();
        c.Instruct("Stand next to one sleeping place of the double bed. Press continue.");
        await c.Verdict();
        var dbl = beds.Skip(1).OrderBy(b => World.Position(b.Bed) is { } q && World.Position(c.Player) is { } m
            ? (q.X - m.X) * (q.X - m.X) + (q.Z - m.Z) * (q.Z - m.Z) : double.MaxValue).First().Bed;
        var r2 = await Survivors.AssignBed(LabSurvivor, dbl, me);
        c.Log.Info($"client-pass: 18-c18.4 assign double {Show(r2)}");
        c.Expect(r2.Ok && Survivors.BedOccupant(bed1) is null, "moved to the double bed; the first bed is free", Show(r2));
        c.Instruct("Does the first bed no longer show the assistant, and the double bed's sleeping place show it? Is the other " +
                   "half of the double bed still free?");
        await c.Verdict();
        c.Instruct("Now use the game's own way: point the assistant's summon tool at the FIRST bed and assign it there as you " +
                   "normally would. Press continue when done.");
        await c.Verdict();
        var game = await c.Until(() => Survivors.Find(LabSurvivor)?.Bed == bed1, TimeSpan.FromSeconds(20),
            "the game's own tool moved it back to the first bed (Kiln reads it)");
        c.Expect(game && Survivors.BedOccupant(dbl) is null, "the game's tool cleared the double bed", $"double bed: {Survivors.BedOccupant(dbl)}");
        c.Instruct("Night check: let night come (or sleep) and watch the assistant. Does it walk to the first bed and lie down?");
        await c.Verdict();
        c.Instruct("Second player (machine W): join this world from W with an account whose role cannot edit your base (set the " +
                   "role in the game's player settings, or skip this step and write 'skipped' if the game gives no such role). " +
                   "Press continue when W is in the world.");
        await c.Verdict();
        var other = Players.All.FirstOrDefault(p => p.InGame && p.Entity != c.Player);
        if (other is null) c.Log.Info("client-pass: 18-c18.4 no second player: the permission step is skipped");
        else
        {
            var r3 = await Survivors.AssignBed(LabSurvivor, beds[1].Bed, new SurvivorAssignmentOptions(other.Entity));
            c.Log.Info($"client-pass: 18-c18.4 assign as {other.Name} (permissions {other.Permissions}) {Show(r3)}");
            c.Expect(other.Permissions.HasFlag(PlayerPermissions.EditBase) ? r3.Ok : !r3.Ok && r3.Reason!.StartsWith("PermissionDenied"),
                "the second player's rights decide, as the game's own (refused without the base-edit permission)", Show(r3));
        }
        c.Instruct("Save and quit, host again. Is the assistant still the first bed's owner (label) and does it still go there at night?");
        await c.Verdict();
    }
}

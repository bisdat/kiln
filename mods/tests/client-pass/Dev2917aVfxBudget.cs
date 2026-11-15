// SPDX-License-Identifier: MIT
// A non-passing preparation and a separate restart/registration/visual proof. No game-client run is claimed.
using System.Text.Json.Nodes;
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;
namespace ClientPass;
public sealed partial class ClientPassMod
{
    private const int VfxCapOrder = 2000 + (28320 - 28000) * 10;
    private const string VfxTest = "vfx-test";
    private static JsonObject? VfxEvidence(string file)
    {
        try { return JsonNode.Parse(File.ReadAllText(file)) as JsonObject; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException) { return null; }
    }
    private static string VfxEvidenceFile(CheckContext c) => Path.Combine(c.Mod.DataDirectory,"17-vfx-budget-before.json");
    private static JsonObject? VfxFixture(CheckContext c) => VfxEvidence(Path.Combine(c.Mod.DataDirectory,"..",VfxTest,"budget.json"));

    [ClientCheck("17-vfx-budget.before", "VFX restart proof (1 of 2): record three accepted definitions, then restart",
        Area = "Limits", Since = Dev29, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = VfxCapOrder + 2)]
    public async Task VfxBudgetBefore(CheckContext c)
    {
        if (!await Need(c,VfxTest) || !await NeedPlayer(c)) return;
        var file = VfxEvidenceFile(c);
        File.Delete(file); // an unsuccessful new preparation must not leave an earlier proof usable
        var b = Vfx.ResourceBudget;
        c.Expect(b.Ok,"the first start's budget is ready",b.Reason);
        var fixture = VfxFixture(c);
        var valid = VfxBudgetProof.Before(fixture,VfxBudgetProof.ProcessKey,b.Budget.KilnUsed,out var evidence);
        if (valid)
            foreach(var entry in fixture!["effects"]!.AsArray().OfType<JsonObject>())
            {
                var name = (string)entry["name"]!;
                var r = Blocks.GetVfxRegistration(Guid.Parse((string)entry["registrationId"]!));
                valid &= c.Expect(r.Ok && r.State == DefinitionRegistrationState.AcceptedNextStart && r.SlotsConsumed == 0 &&
                    r.Definition == VfxId.Named(VfxTest,name).Guid,
                    $"{name} is still accepted for the next start, with no live slot",$"{r.State}: {r.Reason}");
            }
        c.Expect(valid,"this process's fixture has exactly budget_red/green/blue accepted with zero slots, and budget_never_stored refused");
        if (!b.Ok || !valid) { await c.Verdict(); return; }
        var tmp = file + ".tmp";
        File.WriteAllText(tmp,evidence!.ToJsonString()); File.Move(tmp,file,true);
        c.Instruct("Preparation recorded. Quit to the desktop, restart with the same mods/profile, host this world, then run 17-vfx-budget.after. This preparation cannot pass the visual/restart proof.");
        await c.Skip("Preparation recorded; restart the game and run 17-vfx-budget.after.");
    }

    [ClientCheck("17-vfx-budget.after", "VFX restart proof (2 of 2): exact registrations, three more slots and three visible bursts",
        Area = "Limits", Since = Dev29, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = VfxCapOrder + 3)]
    public async Task VfxBudgetAfter(CheckContext c)
    {
        if (!await Need(c,VfxTest) || !await NeedPlayer(c)) return;
        var evidence = VfxEvidence(VfxEvidenceFile(c));
        if (evidence == null) { await c.Skip("Run 17-vfx-budget.before in the first start before attempting the restart proof."); return; }
        var current = VfxFixture(c);
        var before = Vfx.ResourceBudget; var b = before.Budget;
        c.Expect(before.Ok,"the second start's budget is ready",before.Reason);
        var valid = VfxBudgetProof.After(evidence,current,VfxBudgetProof.ProcessKey,b.KilnUsed);
        c.Expect(valid,"a different game process has the same exact fixture receipts and exactly three more Kiln slots than the first start",
            $"KilnUsed now {b.KilnUsed}; first start {evidence["kilnUsed"]}");
        if (!before.Ok || !valid) { await c.Verdict(); return; }
        var effects = current!["effects"]!.AsArray().OfType<JsonObject>().ToDictionary(e => (string)e["name"]!);
        var registered = true;
        foreach(var name in VfxBudgetProof.Names)
        {
            var id = Guid.Parse((string)effects[name]["registrationId"]!);
            var r = Blocks.GetVfxRegistration(id);
            registered &= c.Expect(r.Ok && r.State == DefinitionRegistrationState.Registered && r.SlotsConsumed == 1 &&
                r.RegistrationId == id && r.Definition == VfxId.Named(VfxTest,name).Guid,
                $"{name} is this exact definition, registered with one slot",$"{r.State} ({r.Code}): {r.Reason}");
        }
        if (!registered) { await c.Verdict(); return; }
        if (Ahead(c,5,right:-2,up:1) is not { } left || Ahead(c,5,up:1) is not { } middle || Ahead(c,5,right:2,up:1) is not { } right)
        { c.Expect(false,"the player's place is known"); await c.Verdict(); return; }
        var at = new[] {left,middle,right};
        c.Instruct("Look 5 m ahead: three bursts repeat every 3 seconds, left to right red, green and blue, about 40 sprites each flying up and fading. Pass only when you see all three in their colours.");
        await RepeatUntilVerdict(c,TimeSpan.FromSeconds(3),() => {
            for(var i=0;i<VfxBudgetProof.Names.Length;++i) PlayVfx(c,VfxId.Named(VfxTest,VfxBudgetProof.Names[i]),at[i]);
        });
        var after = Vfx.ResourceBudget;
        c.Expect(after.Ok && after.Budget with { Revision=0 } == b with { Revision=0 },"playing the three bursts did not change the budget",after.Reason);
    }
}

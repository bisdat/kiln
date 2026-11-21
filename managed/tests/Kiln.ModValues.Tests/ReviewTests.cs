// SPDX-License-Identifier: MIT
// Review round 1 of 05b: JSON values follow the server role and the mod's load generation (2, 7), a Describe off the
// tick thread (6), the reserved CharacterData keys (11), a new world's provisional id (5).
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace ModValuesTests;

[Collection("ModValuesService")]
public sealed class ReviewTests
{
    private static readonly IReadOnlyList<Def> Json = ModValuesAuthoring.Parse("jsonmod", AuthoringTests.Example.Replace("\"xp\"", "\"xp\""), "x.json");

    [Fact]
    public void JsonValuesAreDescribedInTheMenuAndActivatedWhenTheServerRoleComesOn()
    {
        using var kiln = new KilnHarness();
        _ = kiln.ModValues;  // the real registry, fresh
        var gen = new object();
        kiln.Run(() => ModValuesAuthoring.EnsureForTests("jsonmod", Json, gen, server: false));
        var key = new ModValueKey("jsonmod", "tokens");
        Assert.NotNull(ModValuesService.Registry.Described(key));
        Assert.Null(ModValuesService.Registry.Active(key));
        kiln.Run(() => ModValuesAuthoring.EnsureForTests("jsonmod", Json, gen, server: true));  // a client starts hosting
        Assert.NotNull(ModValuesService.Registry.Active(key));
    }

    [Fact]
    public void AReloadBetweenPollsDefinesTheJsonValuesAgain()
    {
        using var kiln = new KilnHarness();
        _ = kiln.ModValues;
        var key = new ModValueKey("jsonmod", "xp");
        var first = new object();
        kiln.Run(() => ModValuesAuthoring.EnsureForTests("jsonmod", Json, first, server: true));
        kiln.Run(() => ModValuesService.Retire("jsonmod", first));  // the mod's unload retired its generation
        Assert.Null(ModValuesService.Registry.Active(key));
        Assert.True(ModValuesService.Registry.IsRetired(key));
        kiln.Run(() => ModValuesAuthoring.EnsureForTests("jsonmod", Json, new object(), server: true));  // the next generation
        Assert.NotNull(ModValuesService.Registry.Active(key));
        kiln.Run(() => ModValuesService.Retire("jsonmod", first));  // the old generation's late retire takes nothing now
        Assert.NotNull(ModValuesService.Registry.Active(key));
    }

    [Fact]
    public void ARetireTakesOnlyThatOwnersDefinitions()
    {
        var reg = new Registry();
        var a = new object();
        var b = 7UL;  // a native owner's handle, boxed: equal by value
        Assert.Null(reg.Activate(Def.FromOptions("ns", "one", new ModValueOptions(ModValueKind.Counter, "1")), a, out _));
        Assert.Null(reg.Activate(Def.FromOptions("ns", "two", new ModValueOptions(ModValueKind.Counter, "2")), b, out _));
        Assert.Single(reg.Retire("ns", 7UL));
        Assert.NotNull(reg.Active(new ModValueKey("ns", "one")));
        Assert.Null(reg.Active(new ModValueKey("ns", "two")));
    }

    [Fact]
    public async Task DescribeOffTheTickThreadQueuesItsEvent()
    {
        using var kiln = new KilnHarness();
        var fish = kiln.Load<FishTestMod>(new ModManifest("fish-kit"));
        kiln.Step(2);
        _ = kiln.ModValues;
        var seen = new List<ModValueDefinitionChangedEvent>();
        kiln.As(fish, () => Events.On<ModValueDefinitionChangedEvent>(seen.Add));
        var r = await Task.Run(() => ModValuesService.Describe(Def.FromOptions("fish-kit", "late", new ModValueOptions(ModValueKind.Counter, "L")), null));
        Assert.True(r.Ok, r.Reason);
        kiln.Step(2);
        Assert.Contains(seen, e => e.Key.Id == "late" && e.Change == "described");
    }

    [Fact]
    public void ModsCannotUseTheReservedCharacterDataKeys()
    {
        foreach (var k in CharacterStore.ReservedKeys) Assert.Throws<ArgumentException>(() => CharacterStore.CheckModKey(k));
        Assert.Equal("mod-values.mine", CharacterStore.CheckModKey("mod-values.mine"));
    }

    [Fact]
    public void ANewWorldsProvisionalIdIsReplacedWithoutLosingValues()
    {
        var r = new LedgerTests.Rig();
        r.Define("fish", "coins", new ModValueOptions(ModValueKind.Currency, "Coins"));
        var a = r.Join(1);
        var before = r.Ledger.Change("fish", a, ModValueKey.TryParse("fish:coins", out var k) ? k : default, ChangeOp.Add, 5, null, 0).After!;
        r.Store.World = Guid.NewGuid();  // the first save gave the real id
        var after = r.Ledger.Read("fish", a, k);
        Assert.Equal((5L, before.ValueRevision), (after.Value!.Value, after.Value.ValueRevision));
        Assert.Equal(r.Store.World, after.Value.World);
        Assert.Equal(ModValueReasons.StaleRevision, r.Ledger.Change("fish", a, k, ChangeOp.Add, 1, before.Version, 0).Reason);
        Assert.True(r.Ledger.Change("fish", a, k, ChangeOp.Add, 1, after.Value.Version, 0).Ok);
    }

    // Resume ruling (DECISIONS.md): an id belongs to the owner of its namespace; a second owner's description of it is
    // refused for that id and reported naming both.
    [Fact]
    public void ASecondOwnersDescriptionOfAnIdIsRefusedAndReported()
    {
        var reg = new Registry();
        var reports = new List<string>();
        reg.Report = reports.Add;
        var first = 11UL;   // native owner handles, boxed
        var second = 12UL;
        var d = Def.FromOptions("native.0123456789abcdef0123456789abcdef", "coins", new ModValueOptions(ModValueKind.Currency, "Coins"));
        Assert.Null(reg.Describe(d, first, out _));
        Assert.Equal(ModValueReasons.DefinitionConflict, reg.Describe(d, second, out _));
        Assert.Equal(ModValueReasons.DefinitionConflict, reg.Activate(d, second, out _));
        var report = Assert.Single(reports.Distinct());
        Assert.Contains("11", report);
        Assert.Contains("12", report);
        Assert.Contains("native.0123456789abcdef0123456789abcdef:coins", report);
        // the first owner's own activation still works, and its retire frees the id for the next owner
        Assert.Null(reg.Activate(d, first, out var activated));
        Assert.True(activated);
        reg.Retire(d.Owner, first);
        Assert.Null(reg.Describe(d, second, out _));
        Assert.Null(reg.Activate(d, second, out _));
        Assert.NotNull(reg.Active(d.Key));
    }

    [Fact]
    public void KilnsOwnReadOfAModsFilesAndTheModsOwnCodeShareAnId()
    {
        var reg = new Registry();
        var d = Def.FromOptions("fish", "coins", new ModValueOptions(ModValueKind.Currency, "Coins"));
        var generation = new object();
        Assert.Null(reg.Describe(d, null, out _));           // mod-values/*.json read without a live context
        Assert.Null(reg.Activate(d, generation, out _));     // the mod's own Define of the same value
        Assert.Null(reg.Describe(d, generation, out _));
    }
}

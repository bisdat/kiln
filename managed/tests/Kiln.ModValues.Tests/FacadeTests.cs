// SPDX-License-Identifier: MIT
// The facade ModValues in a test-kit harness (the real ledger on the fake store): the calling mod's namespace, the
// one error rule, events on the bus, and what a test needs to write a mod's own tests.
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace ModValuesTests;

[Collection("ModValuesService")]
public sealed class FacadeTests
{
    [Fact]
    public void AModDefinesAwardsSpendsAndReadsThroughTheFacade()
    {
        using var kiln = new KilnHarness();
        var fish = kiln.Load<FishTestMod>(new ModManifest("fish-kit"));
        kiln.Step(2);
        var a = HarnessModValues.Character(1);
        var mv = kiln.ModValues;
        var changes = new List<ModValueChangedEvent>();
        kiln.As(fish, () => Events.On<ModValueChangedEvent>(changes.Add));
        Assert.True(kiln.As(fish, () => ModValues.Define("coins", new ModValueOptions(ModValueKind.Currency, "Coins", Initial: 5))).Ok);
        mv.Join(a);
        var after = kiln.As(fish, () => ModValues.Add(a, ModValues.Key("coins"), 10));
        Assert.True(after.Ok, after.Reason);
        Assert.Equal(15, after.After!.Value);
        Assert.Equal(ModValueReasons.InsufficientFunds, kiln.As(fish, () => ModValues.Spend(a, ModValues.Key("coins"), 100)).Reason);
        Assert.Equal(["initialized", "changed"], changes.Select(c => c.Cause));
        mv.Leave(a);
        Assert.Equal(ModValueReasons.CharacterUnavailable, kiln.As(fish, () => ModValues.Add(a, ModValues.Key("coins"), 1)).Reason);
        Assert.Equal(15, kiln.As(fish, () => ModValues.Read(a, ModValues.Key("coins"))).Value!.Value);
        Assert.Contains("\"coins\"", mv.Record(a, after.After.Key.Mod));
        Assert.Throws<ArgumentException>(() => kiln.As(fish, () => ModValues.Define("Bad Id", new ModValueOptions(ModValueKind.Counter, "x"))));
        Assert.Throws<ArgumentException>(() => kiln.As(fish, () => ModValues.Key("a:b:c")));
    }
}

public sealed class FishTestMod : Mod;

[CollectionDefinition("ModValuesService", DisableParallelization = true)]
public sealed class ModValuesServiceCollection;

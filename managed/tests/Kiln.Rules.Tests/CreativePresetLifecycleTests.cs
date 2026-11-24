// SPDX-License-Identifier: MIT
extern alias rulesapi;
using System.Reflection;
using System.Runtime.Loader;
using Kiln.TestKit;
using Xunit;

namespace Kiln.Rules.Tests;

public sealed class CreativePresetLifecycleTests
{
    private sealed class Isolated() : AssemblyLoadContext(isCollectible: true)
    {
        protected override Assembly? Load(AssemblyName name) => name.Name!.StartsWith("Kiln.", StringComparison.Ordinal)
            ? LoadFromAssemblyPath(Path.Combine(Path.GetDirectoryName(typeof(CreativePresetLifecycleTests).Assembly.Location)!, name.Name + ".dll"))
            : null;
    }

    [Theory]
    [InlineData(false)] [InlineData(true)]
    public void ActualCreativeEntryReadsSyntheticServerPresetAndRetiresOnLeaveEvenWithoutEffectiveGrants(bool noGrants)
    {
        var context = new Isolated();
        try
        {
            context.LoadFromAssemblyPath(typeof(CreativePresetLifecycleTests).Assembly.Location)
                .GetType(typeof(CreativePresetScenario).FullName!)!.GetMethod(nameof(CreativePresetScenario.Run))!.Invoke(null, [noGrants]);
        }
        catch (TargetInvocationException e) { Assert.Fail(e.InnerException!.ToString()); }
        finally { context.Unload(); }
    }
}

public static class CreativePresetScenario
{
    public static void Run(bool noGrants)
    {
        using var k = new KilnHarness();
        var host = k.Players.Join("SyntheticHost", rights: PlayerPermissions.KickBan);
        k.Step();
        // Reuse the existing synthetic native-rule and hosting-client tables; never start a real game/server.
        var environment = typeof(UnlimitedFlightScenario).GetNestedType("Environment", BindingFlags.NonPublic)!;
        using var env = (IDisposable)Activator.CreateInstance(environment, [host.Entity, uint.MaxValue, ClientState.Hosting])!;
        var folder = Path.Combine(k.ConfigDirectory, "rules");
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, "creative.json"), noGrants
            ? """{"freeCrafting":false,"keepConsumables":false}"""
            : """{"minStamina":0.25,"buildCost":0.5,"freeCrafting":false,"keepConsumables":false}""");
        var entered = k.Run(() => Creative.Set(host.Entity, true));
        Assert.True(entered.Ok, entered.Message);
        var set = k.Run(() => Assert.Single(rulesapi::Kiln.Rules.Active, s => s.Label == "creative"));
        Assert.Equal("kiln", set.Owner);
        Assert.Null(set.Rules.Invulnerable); // a custom preset must not be filled from the built-in preset
        if (!noGrants)
        {
            // The synthetic native environment supplies cost hooks, not an abilities attribute service.
            Assert.True(k.Run(() => Creative.SetEffect(host.Entity, "building-costs", false)).Ok);
            Assert.True(k.Run(() => Creative.SetEffect(host.Entity, "building-costs", true)).Ok);
            Assert.Equal(0.25, set.Rules.MinStamina);
            Assert.Equal(0.5, set.Rules.BuildCost);
        }
        host.Leave();
        k.Step(24);
        Assert.False(set.Active);
        Assert.False(Creative.IsOn(host.Entity));
    }
}

// SPDX-License-Identifier: MIT
#pragma warning disable KILNX031
using System.Text.Json.Nodes;
using TestMods;
using Xunit;
using DataRegistry = Kiln.Registry.Registry;

namespace Kiln.TestKit.Tests;

public sealed class RegistryAuthoringTests
{
    [Fact]
    public void ASeededAuthorRecordsIrAndExpiresCellsWithoutClaimingNativeAvailability()
    {
        using var kiln = new KilnHarness();
        var mod = kiln.Load<TkRegistryMod>(new ModManifest("tk-registry")); kiln.Boot();
        kiln.Registry.Seed("items", "game:wood", "maxStack", "10");
        var text = kiln.Registry.Author(mod);
        var ir = JsonNode.Parse(text!)!;
        Assert.Equal(15, ir["edits"]![0]!["op"]!.GetValue<int>());
        Assert.Equal("alter", ir["stage"]!.GetValue<string>());
        Assert.Null(kiln.Registry.Author(mod, phase: RegistryPhase.Add));
        Assert.False(DataRegistry.Available);
        Assert.Contains(Coverage.All, row => row.Area == "Registry authoring" && row.Kind == CoverageKind.RecordingFake && row.Capabilities.Count == 0);
        kiln.Unload(mod); // the mod verifies that a retained cell failed closed and releases it
    }
    [Fact]
    public void MissingSeedsThrowInsteadOfInventingGameValues()
    {
        using var kiln = new KilnHarness();
        var mod = kiln.Load<TkRegistryMod>(new ModManifest("tk-registry")); kiln.Boot();
        MissingSeed(kiln, mod);
        kiln.Unload(mod);
    }
    // A caught exception's stack itself references the collectible author's method. Release the assertion's local first.
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    private static void MissingSeed(KilnHarness kiln, ModHandle mod)
        => Assert.Throws<KeyNotFoundException>(() => kiln.Registry.Author(mod));
}

// SPDX-License-Identifier: MIT
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

// The registry stage's order from Kiln.Host (RegistryHost.cs): the JSON set_order takes, the OnBoot difference line, kiln.json's
// registry.stage.
public sealed class RegistryHostTests
{
    [Fact]
    public void TheOrderIsEveryModOnceInOrder()
    {
        var json = RegistryHost.OrderJson([("late", "1.0.0"), ("bigmod", "2.0.0"), ("late", "9.9.9")]);
        Assert.Equal("[{\"mod\":\"late\",\"version\":\"1.0.0\"},{\"mod\":\"bigmod\",\"version\":\"2.0.0\"}]", json);
        Assert.Equal("[]", RegistryHost.OrderJson([]));
    }

    // review4 NS1: mods whose code uses the registry API are flagged, so the stage decodes the game's base before packs write.
    [Fact(Timeout = 30_000)]
    public async Task RegistryUsersAreFlaggedInTheOrder()
    {
        await Task.Yield();
        var json = RegistryHost.OrderJson([("late", "1.0.0"), ("author", "2.0.0")], new HashSet<string>(StringComparer.Ordinal) { "author" });
        Assert.Equal("""[{"mod":"late","version":"1.0.0"},{"mod":"author","version":"2.0.0","registry":true}]""", json);
    }
    [Fact]
    public void ADifferentOnBootOrderIsNamed()
    {
        Assert.Null(RegistryHost.OnBootDifference(["a", "b", "c"], ["a", "c"]));  // the mods in both run in the same order
        Assert.Null(RegistryHost.OnBootDifference(["a", "b"], []));
        var line = RegistryHost.OnBootDifference(["zeta", "alpha", "mid"], ["alpha", "zeta"]);
        Assert.NotNull(line);
        Assert.Contains("runs zeta before alpha", line);
        Assert.Contains("their OnBoot ran alpha first", line);
    }

    [Fact]
    public void TheStageModeComesFromKilnJson()
    {
        var dir = Directory.CreateTempSubdirectory("kiln-registry-host-").FullName;
        try
        {
            var file = Path.Combine(dir, "kiln.json");
            Assert.Null(RegistryHost.ModeOf(file));  // no file
            File.WriteAllText(file, "{ // comments as kiln.json allows\n \"registry\": { \"stage\": \"onBoot\" }, }");
            Assert.Equal("onBoot", RegistryHost.ModeOf(file));
            File.WriteAllText(file, "{\"mods\": {}}");
            Assert.Null(RegistryHost.ModeOf(file));
            File.WriteAllText(file, "not json");
            Assert.Null(RegistryHost.ModeOf(file));
        }
        finally
        {
            Directory.Delete(dir, true);
        }
    }
}

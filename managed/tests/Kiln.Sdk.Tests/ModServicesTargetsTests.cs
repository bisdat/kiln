// SPDX-License-Identifier: MIT
// KilnServices "Stations" (0.2.0-dev.29.15): a mod that only automates production stations references Kiln.Blocks' assembly
// without the defaults listing Blocks sets (side both, clients kiln), so a server does not refuse players without Kiln for it.
// Listing Blocks keeps both defaults. Kiln.Mod.targets is evaluated on a stub project (dotnet msbuild -getProperty / -getItem).
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.SdkTests;

public sealed class ModServicesTargetsTests
{
    private static string Targets => Path.Combine(AppContext.BaseDirectory, "Kiln.Mod.targets");

    private static (string Side, string ClientsFallback, string[] Services, string[] References) Evaluate(string services)
    {
        var dir = Directory.CreateTempSubdirectory("kiln-services-").FullName;
        try
        {
            File.WriteAllText(Path.Combine(dir, "stub.proj"), $"""
                <Project>
                  <PropertyGroup>
                    <AssemblyName>StationHelper</AssemblyName>
                    <KilnServices>{services}</KilnServices>
                    <KilnRepoMode>true</KilnRepoMode>
                    <KilnRepoRoot>{dir}/</KilnRepoRoot>
                    <OutDir>{dir}/out/</OutDir>
                    <IntermediateOutputPath>{dir}/obj/</IntermediateOutputPath>
                    <KilnModApiVersion>0.2</KilnModApiVersion>
                  </PropertyGroup>
                  <Import Project="{Targets}" />
                </Project>
                """);
            var (code, text) = StubMsBuild.Run(Path.Combine(dir, "stub.proj"), "-getProperty:KilnSide", "-getProperty:_KilnClientsFallback",
                "-getItem:_KilnService", "-getItem:ProjectReference");
            Assert.True(code == 0, text);
            var json = JsonNode.Parse(text[text.IndexOf('{')..])!.AsObject();
            string[] Items(string name) => json["Items"]?[name]?.AsArray().Select(i => i!["Identity"]!.GetValue<string>()).ToArray() ?? [];
            return (json["Properties"]!["KilnSide"]!.GetValue<string>(), json["Properties"]!["_KilnClientsFallback"]!.GetValue<string>(),
                Items("_KilnService"), Items("ProjectReference"));
        }
        finally
        {
            Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void StationsReferencesKilnBlocksWithoutBlocksDefaults()
    {
        var (side, clients, services, refs) = Evaluate("Stations;Items");
        Assert.Equal("server", side);
        Assert.Equal("", clients);                                           // no "kiln": players without Kiln may join
        Assert.Equal(["Items", "Blocks"], services);
        Assert.Contains(refs, r => r.EndsWith("managed/services/Kiln.Blocks/Kiln.Blocks.csproj", StringComparison.Ordinal));
        Assert.DoesNotContain(refs, r => r.Contains("Kiln.Stations", StringComparison.Ordinal));
    }

    [Fact]
    public void BlocksKeepsItsDefaults()
    {
        var (side, clients, services, _) = Evaluate("Blocks;Items");
        Assert.Equal("both", side);
        Assert.Equal("kiln", clients);
        Assert.Equal(["Blocks", "Items"], services);
        // both listed: Blocks' defaults, one reference
        (side, clients, services, _) = Evaluate("Stations;Blocks");
        Assert.Equal(("both", "kiln"), (side, clients));
        Assert.Equal(["Blocks"], services);
    }
}

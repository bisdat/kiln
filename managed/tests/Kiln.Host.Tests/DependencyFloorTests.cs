// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Text.Json;
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

public sealed class DependencyFloorTests
{
    private static string Fixture([CallerFilePath] string self = "") =>
        Path.GetFullPath(Path.Combine(Path.GetDirectoryName(self)!, "..", "..", "..", "schema", "fixtures", "dependency-floor.json"));

    public static IEnumerable<object[]> Cases()
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(Fixture()));
        foreach (var row in doc.RootElement.GetProperty("cases").EnumerateArray())
            foreach (var id in new[] { "kiln", "lib" })
                yield return [id, row[0].GetString()!, row[1].GetString()!, row[id == "kiln" ? 2 : 3].GetBoolean()];
    }

    private static ModCandidate Mod(string id, string version, string extra = "") => new()
    {
        Directory = "/mods/" + id,
        Manifest = Manifest.Parse($$"""{"id":"{{id}}","version":"{{version}}","apiVersion":"0.2","entryAssembly":"M.dll"{{extra}}}"""),
    };

    [Theory]
    [MemberData(nameof(Cases))]
    public void Dependency_ranges_distinguish_Kiln_from_mods(string id, string range, string installed, bool accepts)
    {
        var app = Mod("app", "1.0.0", $$""","dependencies":{"{{id}}":"{{range}}"}""");
        var lib = Mod("lib", installed);
        // A normal dependency has the same meaning whether it is queued or already running.
        foreach (var loaded in new[] { false, true })
        {
            var env = new ResolveEnvironment(SemVersion.Parse(id == "kiln" ? installed : "0.2.0-dev.31"),
                SemVersion.Parse("0.2.0"), ManifestSide.Server, _ => true,
                loaded && id != "kiln" ? new Dictionary<string, Manifest> { ["lib"] = lib.Manifest! } : ResolveEnvironment.NoneLoaded);
            ModCandidate[] candidates = id == "kiln" || loaded ? [app] : [app, lib];
            var result = Resolver.Resolve(candidates, env);
            Assert.Equal(accepts, result.Order.Any(m => m.Id == "app"));
            if (accepts) Assert.Empty(result.Rejected);
            else
            {
                var text = app.Manifest!.Dependencies.Single().RangeText;
                Assert.Equal(id == "kiln" ? $"needs Kiln {text}; this is Kiln {installed}"
                    : $"needs lib {text}, found lib {installed}", result.Rejected.Single().Reason);
            }
        }
    }

    [Fact]
    public void Malformed_prerelease_requirements_remain_manifest_errors_for_every_dependency()
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(Fixture()));
        foreach (var range in doc.RootElement.GetProperty("invalid").EnumerateArray())
            foreach (var id in new[] { "kiln", "lib" })
                Assert.Contains($"dependency '{id}'", Assert.Throws<InvalidDataException>(() =>
                    Mod("app", "1.0.0", $$""","dependencies":{"{{id}}":"{{range.GetString()}}"}""")).Message);
    }
}

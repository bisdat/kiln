// SPDX-License-Identifier: MIT
// The kit's FakePhysicsCookHost (Fakes/Assets/FakePhysicsCookHost.cs): a refused cook commits and registers nothing, a cook that may goes whole,
// and its features are the cook's own (tools/modelcook/cook/features.py, read when the test runs in the repository).
using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using Kiln.TestKit;
using Xunit;

namespace Kiln.TestKit.Tests;

public sealed class PhysicsCookHostTests
{
    private static readonly Dictionary<string, byte[]> Banner = new()
    {
        ["assets/cooked/banner.blob"] = [1, 2, 3, 4],
        ["assets/cooked/banner.bin"] = [5, 6, 7],
    };

    [Fact]
    public void A_feature_that_is_off_commits_and_registers_nothing()
    {
        var host = new FakePhysicsCookHost();
        var r = host.Cook("models/banner.gltf", ["modelcook.cloth"], Banner);
        Assert.False(r.Ok);
        Assert.Contains("needs cook feature modelcook.cloth, which is off (game test pending: 16e-cc16e)", r.Reason, StringComparison.Ordinal);
        Assert.Contains("\"experimental\": [\"modelcook.cloth\"]", r.Reason, StringComparison.Ordinal);
        Assert.Empty(r.Files);
        Assert.Empty(host.Files);
        Assert.Empty(host.Registrations);
        // the other row on is not enough
        Assert.False(new FakePhysicsCookHost("modelcook.chains2").Cook("models/banner.gltf", ["modelcook.cloth"], Banner).Ok);
    }

    [Fact]
    public void A_refused_input_commits_and_registers_nothing()
    {
        var host = new FakePhysicsCookHost("modelcook.cloth");
        var r = host.Cook("models/banner.gltf", ["modelcook.cloth"], Banner, "cloth[0].pin[2]: 2.0 is outside [0, 1]");
        Assert.False(r.Ok);
        Assert.Equal("models/banner.gltf: cloth[0].pin[2]: 2.0 is outside [0, 1]", r.Reason);
        Assert.Empty(host.Files);
        Assert.Empty(host.Registrations);
    }

    [Fact]
    public void A_cook_that_may_commits_every_file_then_registers_them()
    {
        var host = new FakePhysicsCookHost("modelcook.cloth", "modelcook.chains2");
        var r = host.Cook("models/banner.gltf", ["modelcook.cloth"], Banner);
        Assert.True(r.Ok);
        Assert.Null(r.Reason);
        Assert.Equal(["assets/cooked/banner.blob", "assets/cooked/banner.bin"], r.Files);
        Assert.Equal(Banner["assets/cooked/banner.bin"], host.Files["assets/cooked/banner.bin"]);
        Assert.Equal([$"blob {FakePhysicsCookHost.ContentHashOf([1, 2, 3, 4])}", "resource models/banner.gltf keen::RenderModel"], host.Registrations);
        Assert.StartsWith("4-", FakePhysicsCookHost.ContentHashOf([1, 2, 3, 4]), StringComparison.Ordinal);
        // a later refusal leaves what was committed as it was
        Assert.False(host.Cook("models/cape.gltf", ["modelcook.cloth"], new Dictionary<string, byte[]> { ["assets/cooked/cape.bin"] = [9] }, "cloth[0]: no area").Ok);
        Assert.Equal(2, host.Files.Count);
        Assert.Equal(2, host.Registrations.Count);
    }

    [Fact]
    public void Unknown_names_are_refused()
    {
        Assert.Throws<ArgumentException>(() => new FakePhysicsCookHost("modelcook.clth"));
        Assert.Throws<ArgumentException>(() => new FakePhysicsCookHost().IsOn("modelcook.sleeves"));
        Assert.Throws<ArgumentException>(() => new FakePhysicsCookHost().Cook("m.gltf", [], new Dictionary<string, byte[]>()));
    }

    private static string? Features([CallerFilePath] string self = "")
    {
        for (var d = new DirectoryInfo(Path.GetDirectoryName(self) ?? ""); d != null; d = d.Parent)
        {
            var f = Path.Combine(d.FullName, "tools", "modelcook", "cook", "features.py");
            if (File.Exists(f)) return File.ReadAllText(f);
        }
        return null;
    }

    [Fact]
    public void Its_features_are_the_cooks()
    {
        var text = Features();
        if (text == null) return;  // a packed SDK has no cook sources: the repository checks
        var rows = Regex.Matches(text, @"Feature\(""(?<name>[a-z0-9.]+)"",\s*(?<on>True|False),\s*\((?<checks>[^)]*)\)")
            .Select(m => (Name: m.Groups["name"].Value, On: m.Groups["on"].Value == "True", Checks: m.Groups["checks"].Value)).ToList();
        Assert.Equal(FakePhysicsCookHost.Features, rows.Select(r => r.Name));
        Assert.All(rows, r => Assert.False(r.On));
        Assert.All(rows, r => Assert.Equal(FakePhysicsCookHost.PendingReason, "game test pending: " + string.Join(", ", Regex.Matches(r.Checks, "\"([^\"]+)\"").Select(m => m.Groups[1].Value))));
    }
}

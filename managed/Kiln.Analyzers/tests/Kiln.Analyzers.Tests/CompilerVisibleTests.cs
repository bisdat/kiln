// SPDX-License-Identifier: MIT
using System.Text.RegularExpressions;
using Xunit;

namespace Kiln.Analyzers.Tests;

/// <summary>Every MSBuild property a rule or the game-id generator reads (<c>build_property.X</c>) is one Kiln.Sdk passes to the
/// compiler (<c>&lt;CompilerVisibleProperty Include="X" /&gt;</c> in Sdk/*.targets). A property the compiler cannot see reads as unset in
/// every mod build, while the analyzer tests (which set it themselves) pass: KilnBoot was missing, and a Kiln.Sdk mod with
/// <c>&lt;KilnBoot&gt;true&lt;/KilnBoot&gt;</c> and an OnBoot failed to build with KILN034.</summary>
public sealed class CompilerVisibleTests
{
    private static readonly Regex Read = new(@"""build_property\.(\w+)""", RegexOptions.CultureInvariant);
    private static readonly Regex Visible = new(@"<CompilerVisibleProperty\s+Include=""(\w+)""", RegexOptions.CultureInvariant);

    [Fact]
    public void Every_build_property_a_rule_reads_is_compiler_visible()
    {
        var repo = Repo();
        var visible = Directory.GetFiles(Path.Combine(repo, "managed/Kiln.Sdk/Sdk"), "*.targets")
            .SelectMany(f => Visible.Matches(File.ReadAllText(f)).Select(m => m.Groups[1].Value))
            .ToHashSet(StringComparer.Ordinal);
        Assert.Contains("KilnBoot", visible);
        var reads = Directory.GetFiles(Path.Combine(repo, "managed/Kiln.Analyzers/src"), "*.cs", SearchOption.AllDirectories)
            .Where(f => !f.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}", StringComparison.Ordinal))
            .SelectMany(f => Read.Matches(File.ReadAllText(f)).Select(m => (File: Path.GetFileName(f), Property: m.Groups[1].Value)))
            .ToList();
        Assert.Contains(reads, r => r.Property == "KilnBoot");
        // X read as the fallback of XList (KilnServices, KilnFeatures, KilnGameIds: the SDK passes the comma list, a project of its own
        // may pass the property itself) is covered by XList.
        var missing = reads.Where(r => !visible.Contains(r.Property) && !visible.Contains(r.Property + "List"))
            .Select(r => $"{r.File}: build_property.{r.Property}").Distinct().ToList();
        Assert.True(missing.Count == 0, "read by a rule but not a CompilerVisibleProperty in managed/Kiln.Sdk/Sdk/*.targets:\n" +
                                        string.Join("\n", missing));
    }

    private static string Repo([System.Runtime.CompilerServices.CallerFilePath] string self = "")
    {
        // from the test binaries, else from this source file (an --artifacts-path outside the tree)
        foreach (var start in new[] { AppContext.BaseDirectory, Path.GetDirectoryName(self) ?? "" })
            for (var d = new DirectoryInfo(start); d != null; d = d.Parent)
                if (File.Exists(Path.Combine(d.FullName, "VERSION")) && Directory.Exists(Path.Combine(d.FullName, "managed/Kiln.Sdk/Sdk")))
                    return d.FullName;
        throw new DirectoryNotFoundException("repository root not found from " + AppContext.BaseDirectory);
    }
}

// SPDX-License-Identifier: MIT
using Kiln.TestKit;
using Xunit;

namespace Kiln.Rules.Tests;

/// <summary>
/// Regression for hosted worlds with /noclip but no mod that called Rules.Apply:
/// the NoClip command must register/load rules.json before reading its policy.
/// </summary>
public sealed class NoClipPolicyStartupTests
{
    [Fact]
    public void NoClipStatusLoadsDiskPolicyBeforeAnyRuleSetAndTracksConfigChanges()
    {
        using var kiln = new KilnHarness();
        var path = Path.Combine(kiln.ConfigDirectory, "rules.json");
        File.WriteAllText(path, """
            {
              "allowNoClip": true
            }
            """);

        // No Rules.Apply call: just Kiln's built-in NoClip status command.
        var allowed = kiln.Run(NoClip.Status);
        Assert.Contains("rules.json allows it", allowed);
        Assert.True(Rules.Policy.AllowNoClip);
        Assert.Empty(Rules.Active);
        var file = Assert.IsType<ModConfig<RulesPolicy>>(Config.Find("rules"));
        Assert.Equal(path, file.FilePath);
        Assert.False(file.LastError is not null, file.LastError);

        // The registration must also have installed the policy's Changed
        // callback, otherwise the in-memory Limits remain stuck at startup.
        File.WriteAllText(path, """{ "allowNoClip": false }""");
        kiln.Run(file.Reload);
        Assert.Contains("rules.json does not allow it", kiln.Run(NoClip.Status));

        File.WriteAllText(path, """{ "allowNoClip": true }""");
        kiln.Run(file.Reload);
        Assert.Contains("rules.json allows it", kiln.Run(NoClip.Status));
        Assert.True(Rules.Policy.AllowNoClip);
    }
}

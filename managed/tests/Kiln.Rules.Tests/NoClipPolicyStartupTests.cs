// SPDX-License-Identifier: MIT
extern alias rulesapi;
using System.Reflection;
using Kiln.TestKit;
using Xunit;

namespace Kiln.Rules.Tests;

/// <summary>
/// Regression for issue #37: a world with /noclip but no mod that ever called Rules.Apply. NoClip's own entry points
/// must load the server's rules.json before they read allowNoClip, and follow the file's changes after that.
/// </summary>
/// <remarks>RulesRuntime is process-wide and starts once: this is the only test here that starts it, and it must be
/// the first thing in the process that does.</remarks>
public sealed class NoClipPolicyStartupTests : IDisposable
{
    // A KilnHarness leaves the test kit's backend in place for the rest of the process, and with no harness running that
    // backend calls no thread the tick thread. The other tests here run on Kiln.Api's placeholder backend (every thread
    // is the tick thread), so it goes back when this test ends. Kiln.Rules.Tests is no friend of Kiln.Api: reflection.
    private static readonly PropertyInfo Backend =
        typeof(Game).Assembly.GetType("Kiln.Backend", throwOnError: true)!.GetProperty("Current", BindingFlags.Public | BindingFlags.Static)!;

    private readonly object _backend = Backend.GetValue(null)!;

    public void Dispose() => Backend.SetValue(null, _backend);

    private static string Said(CommandResult r) => string.Join("\n", r.Output.Append(r.Error ?? ""));

    [Fact]
    public void NoClipReadsTheServersRulesJsonBeforeAnyRuleSetAndFollowsItsChanges()
    {
        using var kiln = new KilnHarness();
        var path = Path.Combine(kiln.ConfigDirectory, "rules.json");
        File.WriteAllText(path, """{ "allowNoClip": true }""");
        // the harness has no collision hook: switched on, NoClip.Set goes on to the server's policy, and after that to
        // the player's game (Ada's has no Kiln: refused with reason "client", which is past the policy)
        kiln.Capabilities.Override(NoClip.Capability, true);
        var ada = kiln.Players.Join("Ada");

        // No Rules.Apply anywhere: the first to ask is NoClip.Set (the hotkey mods, /noclip on), then /noclip status.
        var on = kiln.Run(() => NoClip.Set(ada.Entity, true));
        Assert.True(on.Reason == "client", on.Message);
        Assert.Contains("rules.json allows it", Said(kiln.RunCommand("noclip status")));
        Assert.True(rulesapi::Kiln.Rules.Policy.AllowNoClip);
        Assert.Empty(rulesapi::Kiln.Rules.Active);
        var file = Assert.IsType<ModConfig<RulesPolicy>>(Config.Find("rules"));
        Assert.Equal(path, file.FilePath);
        Assert.Null(file.LastError);

        // The registration also installed the policy's Changed callback: the limits follow the file.
        File.WriteAllText(path, """{ "allowNoClip": false }""");
        kiln.Run(file.Reload);
        Assert.Equal("policy", kiln.Run(() => NoClip.Set(ada.Entity, true)).Reason);
        Assert.Contains("rules.json does not allow it", Said(kiln.RunCommand("noclip status")));

        File.WriteAllText(path, """{ "allowNoClip": true }""");
        kiln.Run(file.Reload);
        Assert.Equal("client", kiln.Run(() => NoClip.Set(ada.Entity, true)).Reason);
        Assert.Contains("rules.json allows it", Said(kiln.RunCommand("noclip status")));
    }
}

// SPDX-License-Identifier: MIT
extern alias rulesapi;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using Kiln.TestKit;
using Xunit;

namespace Kiln.Rules.Tests;

/// <summary>
/// Regression for issue #37: a world with /noclip but no mod that ever called Rules.Apply. NoClip's own entry points
/// must load the server's rules.json before they read allowNoClip, and follow the file's changes after that.
/// </summary>
public sealed class NoClipPolicyStartupTests
{
    // Rules starts once, and the harness changes more than Backend.Current (service tables, config, ticks).
    // Give each entry point fresh Kiln assemblies, leaving the other unit tests' state alone in every xunit order.
    private sealed class StartupContext() : AssemblyLoadContext(isCollectible: true)
    {
        protected override Assembly? Load(AssemblyName name) => name.Name!.StartsWith("Kiln.", StringComparison.Ordinal)
            ? LoadFromAssemblyPath(Path.Combine(Path.GetDirectoryName(typeof(NoClipPolicyStartupTests).Assembly.Location)!, name.Name + ".dll"))
            : null;
    }

    private static void Isolated(string scenario, params object[] args)
    {
        var context = new StartupContext();
        try
        {
            var assembly = context.LoadFromAssemblyPath(typeof(NoClipPolicyStartupTests).Assembly.Location);
            assembly.GetType(typeof(NoClipPolicyStartupScenario).FullName!)!.GetMethod(scenario)!.Invoke(null, args);
        }
        catch (TargetInvocationException e)
        {
            Assert.Fail(e.InnerException!.ToString());
        }
        finally { context.Unload(); }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void NoClipReadsTheServersRulesJsonBeforeAnyRuleSetAndFollowsItsChanges(bool statusFirst) =>
        Isolated(nameof(NoClipPolicyStartupScenario.ReadsPolicy), statusFirst);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void MissingOrMalformedPolicyDeniesNoClipAndCanBeRepaired(bool malformed) =>
        Isolated(nameof(NoClipPolicyStartupScenario.MissingOrMalformed), malformed);

    [Fact]
    public void PolicyAndCallbackBelongToKilnAndSurviveFullStartupAndWorldClosing() =>
        Isolated(nameof(NoClipPolicyStartupScenario.ModAndWorldLifetime));

    // dev.29: the game's build hooks install on first use. A rule set asks for building events only when it keeps builds
    // outside bases, and where they are not produced that one rule is not kept while every other rule still applies.
    [Fact]
    public void OnlyKeepBuildsOutsideBasesAsksForBuildingEventsAndTheirAbsenceBreaksNothing() =>
        Isolated(nameof(NoClipPolicyStartupScenario.BuildEventsOnDemand));

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void JoinedClientStatusDoesNotReportItsLocalFileAsTheServerPolicy(bool localAllows) =>
        Isolated(nameof(NoClipPolicyStartupScenario.JoinedClient), localAllows);
}

// This type runs only in StartupContext; do not touch the harness in the default load context.
public static class NoClipPolicyStartupScenario
{

    private static string Said(CommandResult r) => string.Join("\n", r.Output.Append(r.Error ?? ""));

    public static void ReadsPolicy(bool statusFirst)
    {
        using var kiln = new KilnHarness();
        var path = Path.Combine(kiln.ConfigDirectory, "rules.json");
        File.WriteAllText(path, """{ "allowNoClip": true }""");
        // the harness has no collision hook: switched on, NoClip.Set goes on to the server's policy, and after that to
        // the player's game (Ada's has no Kiln: refused with reason "client", which is past the policy)
        kiln.Capabilities.Override(NoClip.Capability, true);
        var ada = kiln.Players.Join("Ada");

        Assert.Empty(rulesapi::Kiln.Rules.Active);
        Assert.Throws<InvalidOperationException>(() => NoClip.Set(ada.Entity, true));  // off the harness's tick thread
        Assert.Null(Config.Find("rules"));
        // No Rules.Apply anywhere: each entry point must load the policy on its own.
        if (statusFirst) Assert.Contains("rules.json allows it", Said(kiln.RunCommand("noclip status")));
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

        // Reading the policy starts no more of the rules runtime: no listener for building events, which can make
        // kiln.gameplay hook five of the game's building functions.
        Assert.False(Events.HasSubscribers<BuildEvent>());
    }

    public static void MissingOrMalformed(bool malformed)
    {
        using var kiln = new KilnHarness();
        var path = Path.Combine(kiln.ConfigDirectory, "rules.json");
        if (malformed) File.WriteAllText(path, "{ invalid JSON");
        kiln.Capabilities.Override(NoClip.Capability, true);
        var ada = kiln.Players.Join("Ada");
        Assert.Equal("policy", kiln.Run(() => NoClip.Set(ada.Entity, true)).Reason);
        var file = Assert.IsType<ModConfig<RulesPolicy>>(Config.Find("rules"));
        Assert.Equal(malformed, file.LastError is not null);
        Assert.True(File.Exists(path));
        if (malformed) Assert.Equal("{ invalid JSON", File.ReadAllText(path));
        File.WriteAllText(path, """{ "allowNoClip": true }""");
        kiln.Run(file.Reload);
        Assert.Null(file.LastError);
        Assert.Equal("client", kiln.Run(() => NoClip.Set(ada.Entity, true)).Reason);
        Assert.False(Events.HasSubscribers<BuildEvent>());
    }

    public static void ModAndWorldLifetime()
    {
        using var kiln = new KilnHarness();
        var path = Path.Combine(kiln.ConfigDirectory, "rules.json");
        File.WriteAllText(path, """{ "allowNoClip": true }""");
        kiln.Capabilities.Override(NoClip.Capability, true);
        var ada = kiln.Players.Join("Ada");
        // Use the host's real mod context and retirement, without loading a test mod (which would start the world).
        var host = typeof(Game).Assembly.GetType("Kiln.HostAccess", throwOnError: true)!;
        var current = host.GetProperty("Current")!;
        var mod = host.GetMethod("CreateContext")!.Invoke(null,
            [new ModInfo("policy-caller", "Policy caller", "1.0.0", kiln.ModsDirectory), 0UL, kiln.DataDirectory]);
        kiln.Run(() =>
        {
            var outer = current.GetValue(null);
            current.SetValue(null, mod);
            try
            {
                Assert.Equal("client", NoClip.Set(ada.Entity, true).Reason);
                Assert.Same(mod, current.GetValue(null));
                Assert.False(Events.HasSubscribers<BuildEvent>());
                var file = Assert.IsType<ModConfig<RulesPolicy>>(Config.Find("rules"));
                Assert.Equal("kiln", file.ModId);
                using var set = rulesapi::Kiln.Rules.Apply(RuleScope.Everyone, new PlayerRules());
                Assert.Same(mod, current.GetValue(null));
                Assert.Equal("policy-caller", set.Owner);
                Assert.Same(file, Config.Find("rules"));
                Assert.Single(Config.All, f => f.Id == "rules");
                var handlers = (System.Collections.ICollection)typeof(ConfigFile)
                    .GetField("_handlers", BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(file)!;
                Assert.Single(handlers.Cast<object>());
            }
            finally { current.SetValue(null, outer); }
            typeof(ModContext).GetMethod("Retire", BindingFlags.NonPublic | BindingFlags.Instance)!.Invoke(mod, null);
            var policy = Assert.IsType<ModConfig<RulesPolicy>>(Config.Find("rules"));
            File.WriteAllText(path, """{ "allowNoClip": false }""");
            policy.Reload();
            Assert.False(RulesRuntime.Limits.AllowNoClip);
            using var personal = rulesapi::Kiln.Rules.Apply(ada.Entity, new PlayerRules());
            var stopping = (ServerStoppingEvent)Activator.CreateInstance(typeof(ServerStoppingEvent),
                BindingFlags.NonPublic | BindingFlags.Instance, null, [1UL, "world changed"], null)!;
            Events.Post(stopping);
            Assert.False(personal.Active);
            // The config is profile-wide, not per world: a second hosted world uses the same file and callback.
            Assert.Same(policy, Config.Find("rules"));
            File.WriteAllText(path, """{ "allowNoClip": true }""");
            policy.Reload();
            Assert.True(RulesRuntime.Limits.AllowNoClip);
            Assert.Equal("client", NoClip.Set(ada.Entity, true).Reason);
        });
    }

    public static void BuildEventsOnDemand()
    {
        using var kiln = new KilnHarness();
        kiln.Boot();
        var ada = kiln.Players.Join("Ada");
        kiln.Run(() =>
        {
            using var plain = rulesapi::Kiln.Rules.Apply(RuleScope.Everyone, new PlayerRules { FlyAnywhere = true });
            Assert.True(plain.Active);
        });
        kiln.Step(3);
        kiln.Run(() => Assert.False(Events.HasSubscribers<BuildEvent>()));
        kiln.Run(() =>
        {
            // The test kit produces no BuildEvent (gameplay.build unavailable): asking for it must not throw.
            using var keep = rulesapi::Kiln.Rules.Apply(ada.Entity, new PlayerRules { BuildOutsideBases = true, KeepBuildsOutsideBases = true });
            Assert.True(keep.Active);
        });
        kiln.Step(3);
        kiln.Run(() => Assert.False(Events.HasSubscribers<BuildEvent>()));
    }

    public static void JoinedClient(bool localAllows)
    {
        using var kiln = new KilnHarness();
        kiln.Capabilities.Override(NoClip.Capability, true);
        var path = Path.Combine(kiln.ConfigDirectory, "rules.json");
        var text = localAllows ? """{ "allowNoClip": true }""" : """{ "allowNoClip": false }""";
        File.WriteAllText(path, text);
        WithRemoteClient(() =>
        {
            var reply = Said(kiln.RunCommand("noclip status"));
            Assert.True(reply.Contains("server", StringComparison.Ordinal), reply);
            Assert.Contains("chat", reply);
            Assert.DoesNotContain("rules.json allows it", reply);
            Assert.DoesNotContain("rules.json does not allow it", reply);
            Assert.Null(Config.Find("rules"));
            Assert.Equal(text, File.ReadAllText(path));
        });
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ClientStateData
    {
        public uint Size, Side, Session, Generation, LocalPlayer, Flags, ClientSession, ServerSession;
        public ulong ClientSteps, ServerSteps;
    }

    [StructLayout(LayoutKind.Sequential)]
    private unsafe struct ClientTable
    {
        public uint Size, Version;
        public delegate* unmanaged[Cdecl]<ClientStateData*, int> State;
        public nint SubscribeTick, Unsubscribe, Post, OnClientThread, ClientWorld;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int RemoteState(ClientStateData* state)
    {
        state->Side = 1;
        state->Session = (uint)ClientState.Remote;
        return 0;
    }

    private static unsafe void WithRemoteClient(Action check)
    {
        var field = typeof(Client).GetField("_table", BindingFlags.NonPublic | BindingFlags.Static)!;
        var previous = field.GetValue(null);
        var table = new ClientTable { Size = (uint)sizeof(ClientTable), Version = 1, State = &RemoteState };
        try
        {
            field.SetValue(null, Pointer.Box(&table, field.FieldType));
            Assert.True(Client.IsRemote);
            check();
        }
        finally { field.SetValue(null, previous); }
    }
}

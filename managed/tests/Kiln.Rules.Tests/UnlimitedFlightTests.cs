// SPDX-License-Identifier: MIT
extern alias rulesapi;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using Kiln.TestKit;
using Xunit;
using RulesApi = rulesapi::Kiln.Rules;

namespace Kiln.Rules.Tests;

public sealed class UnlimitedFlightTests
{
    // Startup and rule state are process-wide. Each scenario gets fresh service assemblies, as the policy tests do.
    private sealed class StartupContext() : AssemblyLoadContext(isCollectible: true)
    {
        protected override Assembly? Load(AssemblyName name) => name.Name!.StartsWith("Kiln.", StringComparison.Ordinal)
            ? LoadFromAssemblyPath(Path.Combine(Path.GetDirectoryName(typeof(UnlimitedFlightTests).Assembly.Location)!, name.Name + ".dll"))
            : null;
    }

    [Theory]
    [InlineData(nameof(UnlimitedFlightScenario.SyntaxAndPermissions))]
    [InlineData(nameof(UnlimitedFlightScenario.HostRulesAndOwnership))]
    [InlineData(nameof(UnlimitedFlightScenario.OffPreservesCreativeAndMods))]
    [InlineData(nameof(UnlimitedFlightScenario.PolicyAndOtherRules))]
    [InlineData(nameof(UnlimitedFlightScenario.NoHooks))]
    [InlineData(nameof(UnlimitedFlightScenario.NoGlideHook))]
    [InlineData(nameof(UnlimitedFlightScenario.NoSteeringHook))]
    [InlineData(nameof(UnlimitedFlightScenario.ClientRefusals))]
    [InlineData(nameof(UnlimitedFlightScenario.ClientConfirmation))]
    [InlineData(nameof(UnlimitedFlightScenario.PlayerLeaves))]
    [InlineData(nameof(UnlimitedFlightScenario.WorldCloses))]
    [InlineData(nameof(UnlimitedFlightScenario.JoinedClientConsole))]
    public void CommandScenario(string scenario)
    {
        var context = new StartupContext();
        try
        {
            context.LoadFromAssemblyPath(typeof(UnlimitedFlightTests).Assembly.Location)
                .GetType(typeof(UnlimitedFlightScenario).FullName!)!.GetMethod(scenario)!.Invoke(null, null);
        }
        catch (TargetInvocationException e) { Assert.Fail(e.InnerException!.ToString()); }
        finally { context.Unload(); }
    }
}

public static unsafe class UnlimitedFlightScenario
{
    private const BindingFlags PrivateStatic = BindingFlags.NonPublic | BindingFlags.Static;
    private const uint BothHooks = RulesNative.HookGlide | RulesNative.HookGlideSteer;
    private const uint BothFlags = RulesNative.GlideUnlimited | RulesNative.GlideSteer;
    private static string Said(CommandResult r) => string.Join("\n", r.Output.Append(r.Error ?? ""));
    private static RuleSet Own() => Assert.Single(RulesApi.Active, s => s.Label == "unlimitedflight");
    private static void NoOwn() => Assert.DoesNotContain(RulesApi.Active, s => s.Label == "unlimitedflight");
    private static CommandResult Run(KilnHarness k, FakePlayer p, string action) => k.RunCommand(p, "unlimitedflight " + action);
    private static void Ok(CommandResult r) => Assert.True(r.Ok, Said(r));

    public static void SyntaxAndPermissions()
    {
        using var k = new KilnHarness();
        var admin = k.Players.Join("Admin", rights: PlayerPermissions.KickBan);
        var guest = k.Players.Join("Guest");
        k.Step();
        var info = Assert.IsType<CommandInfo>(Commands.Find("unlimitedflight"));
        Assert.Equal("kiln", info.Owner);
        Assert.Equal("kiln.command.unlimitedflight", info.PermissionNode);
        Assert.Equal(CommandPermission.Admin, info.Permission);
        Assert.Equal(new[] { "unlimitedflight off", "unlimitedflight on", "unlimitedflight status" }, info.UsageLines.Order().ToArray());
        Assert.Empty(info.Aliases);
        foreach (var action in new[] { "on", "off", "status" }) Assert.True(Run(k, guest, action).Denied);
        var help = k.RunCommand(admin, "help unlimitedflight");
        Ok(help);
        Assert.Contains("glid", Said(help));
        Assert.DoesNotContain(Commands.List(CommandSource.ForPlayer(guest.Entity)), c => c.Name == "unlimitedflight");
        Assert.Contains("is off", Said(Run(k, admin, "status")));
        foreach (var line in new[] { "unlimitedflight", "unlimitedflight toggle", "unlimitedflight on Guest", "unlimitedflight status Guest" })
            Assert.False(k.RunCommand(admin, line).Ok);
        Assert.Contains("your player", Said(k.RunCommand("unlimitedflight on")));
        NoOwn();
    }

    public static void HostRulesAndOwnership()
    {
        using var k = new KilnHarness();
        var host = k.Players.Join("Host");
        var other = k.Players.Join("Other");
        k.Step();
        using var env = new Environment(host.Entity, BothHooks);
        var source = (CommandSource)typeof(CommandSource).GetMethod("HostConsole", PrivateStatic)!.Invoke(null, [host.Entity, null])!;
        Ok(k.RunCommand(source, "unlimitedflight on"));
        var set = Own();
        Assert.Equal("kiln", set.Owner);
        Assert.Equal(host.Entity, set.Scope.Player);
        Assert.Equal(new PlayerRules { UnlimitedGliding = true, GlideSteer = GlideSteerRule.Default }, set.Rules);
        k.Step();  // the existing rules path sends prediction on the next tick
        Assert.Equal(BothFlags, env.ServerFlags);
        Assert.Equal(BothFlags, env.LocalFlags);
        Assert.True(k.Run(() => RulesApi.Of(other.Entity).IsDefault));
        Assert.False(Creative.IsOn(host.Entity));
        Assert.False(new RulesPolicy().AllowCharacterFlight);
        Ok(k.RunCommand(source, "unlimitedflight on"));
        Assert.Same(set, Own());

        var hostAccess = typeof(Game).Assembly.GetType("Kiln.HostAccess", true)!;
        var current = hostAccess.GetProperty("Current")!;
        var mod = hostAccess.GetMethod("CreateContext")!.Invoke(null,
            [new ModInfo("command-caller", "Command caller", "1.0.0", k.ModsDirectory), 0UL, k.DataDirectory]);
        k.Run(() =>
        {
            set.Dispose();
            var outer = current.GetValue(null);
            current.SetValue(null, mod);
            try
            {
                Ok(Commands.Execute(source, "unlimitedflight on"));
                Assert.Same(mod, current.GetValue(null));
                Assert.Equal("kiln", Own().Owner);
            }
            finally { current.SetValue(null, outer); }
            typeof(ModContext).GetMethod("Retire", BindingFlags.NonPublic | BindingFlags.Instance)!.Invoke(mod, null);
        });
        Assert.True(Own().Active);
        Assert.Contains("S pitch up, W dive", Said(k.RunCommand(source, "unlimitedflight status")));
        Ok(k.RunCommand(source, "unlimitedflight off"));
        Ok(k.RunCommand(source, "unlimitedflight off"));
        NoOwn();
        k.Step();
        Assert.Equal(0u, env.ServerFlags);
        Assert.Equal(0u, env.LocalFlags);
    }

    public static void OffPreservesCreativeAndMods()
    {
        using var k = new KilnHarness();
        var host = k.Players.Join("Host", rights: PlayerPermissions.KickBan);
        k.Step();
        using var env = new Environment(host.Entity, BothHooks);
        Assert.True(k.Run(() => Creative.Set(host.Entity, true)).Ok);
        var creative = Assert.Single(RulesApi.Active);
        using var mod = k.Run(() => RulesApi.Apply(host.Entity, new PlayerRules { UnlimitedGliding = true, MinStamina = 1 }, label: "another-rule"));
        Ok(Run(k, host, "on"));
        Ok(Run(k, host, "off"));
        Assert.True(creative.Active);
        Assert.True(mod.Active);
        Assert.True(Creative.IsOn(host.Entity));
        Assert.True(k.Run(() => RulesApi.Of(host.Entity).UnlimitedGliding));
        Assert.Contains("other rule sets", Said(Run(k, host, "off")));
        NoOwn();
        k.Run(() => Creative.Set(host.Entity, false));
    }

    public static void PolicyAndOtherRules()
    {
        using var k = new KilnHarness();
        var path = Path.Combine(k.ConfigDirectory, "rules.json");
        File.WriteAllText(path, """{ "allowUnlimitedGliding": false, "allowGlideSteer": false }""");
        var host = k.Players.Join("Host", rights: PlayerPermissions.KickBan);
        k.Step();
        using var env = new Environment(host.Entity, BothHooks);
        var denied = Run(k, host, "on");
        Assert.False(denied.Ok);
        Assert.Contains("allowUnlimitedGliding", Said(denied));
        Assert.Contains("allowGlideSteer", Said(denied));
        NoOwn();
        var file = Assert.IsType<ModConfig<RulesPolicy>>(Config.Find("rules"));
        File.WriteAllText(path, """{ "allowGlideSteer": false }""");
        k.Run(file.Reload);
        var partial = Run(k, host, "on");
        Ok(partial);
        Assert.Contains("partial", Said(partial));
        Assert.Contains("stamina drain off", Said(partial));
        Assert.Equal(RulesNative.GlideUnlimited, env.ServerFlags);
        File.WriteAllText(path, """{ "maxGlideSteerAngle": 30 }""");
        k.Run(file.Reload);
        Assert.Contains("30", Said(Run(k, host, "status")));
        var own = Own();
        Ok(Run(k, host, "on"));
        Assert.Same(own, Own());
        Ok(Run(k, host, "off"));
        using var blocker = k.Run(() => RulesApi.Apply(host.Entity, new PlayerRules { UnlimitedGliding = false }));
        partial = Run(k, host, "on");
        Ok(partial);
        Assert.Contains("another rule set", Said(partial));
        Assert.Contains("partial", Said(partial));
        Ok(Run(k, host, "off"));
        Assert.True(blocker.Active);
    }

    public static void NoHooks() => HookCase(0, false, "unavailable");
    public static void NoGlideHook() => HookCase(RulesNative.HookGlideSteer, true, "glider stamina unavailable");
    public static void NoSteeringHook() => HookCase(RulesNative.HookGlide, true, "S/W steering unavailable");
    private static void HookCase(uint hooks, bool ok, string expected)
    {
        using var k = new KilnHarness();
        var host = k.Players.Join("Host", rights: PlayerPermissions.KickBan);
        k.Step();
        using var env = new Environment(host.Entity, hooks);
        var result = Run(k, host, "on");
        Assert.Equal(ok, result.Ok);
        Assert.Contains(expected, Said(result));
        if (ok)
        {
            Assert.Contains("partial", Said(result));
            Assert.True(Own().Active);
            Ok(Run(k, host, "off"));
        }
        NoOwn();
    }

    public static void ClientRefusals()
    {
        using var k = new KilnHarness();
        var admin = k.Players.Join("Admin", rights: PlayerPermissions.KickBan);
        k.Step();
        using var env = new Environment(EntityId.None, BothHooks);
        foreach (var (state, note) in new[] { (NetPeerState.Connecting, "still joining"), (NetPeerState.Vanilla, "no Kiln"), (NetPeerState.Ready, "rules channel") })
        {
            env.Peer(admin.Entity, state, false);
            var result = Run(k, admin, "on");
            Assert.False(result.Ok);
            Assert.Contains(note, Said(result));
            NoOwn();
        }
    }

    public static void ClientConfirmation()
    {
        using var k = new KilnHarness();
        var admin = k.Players.Join("Admin", rights: PlayerPermissions.KickBan);
        k.Step();
        using var env = new Environment(EntityId.None, BothHooks);
        k.Run(RulesNet.Start);  // autostart had no native transport when the harness booted
        var peer = env.Peer(admin.Entity, NetPeerState.Ready, true);
        var result = Run(k, admin, "on");
        Ok(result);
        Assert.Contains("waiting", Said(result));
        Assert.Contains("partial", Said(result));
        k.Step();
        Assert.Equal(RulesNative.GlideUnlimited, env.ServerFlags);
        Assert.Equal(RulesNative.GlideUnlimited, env.SentFlags);
        RulesNet.ClientHooksOf(peer, RulesNative.HookGlideSteer);
        k.Step(6);
        Assert.Contains("S pitch up, W dive", Said(Run(k, admin, "status")));
        Assert.Equal(BothFlags, env.ServerFlags);
        Assert.Equal(BothFlags, env.SentFlags);
        // Losing confirmation is visible without toggling the command again.
        RulesNet.ClientHooksOf(peer, 0);
        k.Step(6);
        Assert.Contains("waiting", Said(Run(k, admin, "status")));
        Assert.Equal(RulesNative.GlideUnlimited, env.ServerFlags);
        Ok(Run(k, admin, "off"));
    }

    public static void PlayerLeaves() => Cleanup(false);
    public static void WorldCloses() => Cleanup(true);
    private static void Cleanup(bool world)
    {
        using var k = new KilnHarness();
        var host = k.Players.Join("Host", rights: PlayerPermissions.KickBan);
        k.Step();
        using var env = new Environment(host.Entity, BothHooks);
        Ok(Run(k, host, "on"));
        var set = Own();
        if (world)
            k.Run(() => Events.Post((ServerStoppingEvent)Activator.CreateInstance(typeof(ServerStoppingEvent), BindingFlags.NonPublic | BindingFlags.Instance,
                null, [1UL, "world changed"], null)!));
        else { host.Leave(); k.Step(24); }
        Assert.False(set.Active);
        NoOwn();
        if (world) Assert.Contains("is off", Said(Run(k, host, "status")));
        else
        {
            var next = k.Players.Join("Next", rights: PlayerPermissions.KickBan);
            k.Step(6);
            Assert.Equal(host.Entity, next.Entity);
            Assert.Contains("is off", Said(Run(k, next, "status")));
        }
    }

    public static void JoinedClientConsole()
    {
        using var k = new KilnHarness();
        using var env = new Environment(new EntityId(1), BothHooks, ClientState.Remote);
        foreach (var action in new[] { "on", "off", "status" })
        {
            var result = k.RunCommand("unlimitedflight " + action);
            Assert.Contains("server chat", Said(result));
        }
        Assert.Null(Config.Find("rules"));
        NoOwn();
    }

    // Record the existing native tables; no game, no hooks or packets are installed by these tests.
    private sealed class Environment : IDisposable
    {
        private readonly List<(FieldInfo Field, object? Before, nint Memory)> _fields = [];
        private readonly Type _engine = typeof(Network).Assembly.GetType("Kiln.NetEngine", true)!;
        private readonly object? _snapshot;
        private readonly List<NetPeer> _peers = [];
        private static uint _hooks, _host, _session, _serverFlags, _localFlags, _sentFlags;
        public uint ServerFlags => _serverFlags;
        public uint LocalFlags => _localFlags;
        public uint SentFlags => _sentFlags;

        public Environment(EntityId host, uint hooks, ClientState state = ClientState.Hosting)
        {
            _hooks = hooks; _host = host.Value; _session = host.IsNone ? (uint)ClientState.DedicatedServer : (uint)state;
            _serverFlags = _localFlags = _sentFlags = 0;
            var rules = (RulesNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(RulesNative.Table));
            rules->Set = &Set; rules->Clear = &Clear; rules->GetStatus = &Status; rules->SetLocal = &Local;
            Replace(typeof(RulesNative), "_t", rules);
            var client = (nint*)NativeMemory.AllocZeroed(256);
            for (var i = 1; i < 8; i++) client[i] = (nint)(delegate* unmanaged[Cdecl]<int>)&NetOk;
            client[1] = (nint)(delegate* unmanaged[Cdecl]<uint*, int>)&State;
            Replace(typeof(Client), "_table", client);
            var native = typeof(Network).Assembly.GetType("Kiln.NetNative", true)!;
            var table = native.GetNestedType("Table", BindingFlags.NonPublic)!;
            var size = Marshal.SizeOf(table);
            var net = (nint*)NativeMemory.AllocZeroed((nuint)size);
            for (var i = 0; i < size / sizeof(nint); i++) net[i] = (nint)(delegate* unmanaged[Cdecl]<int>)&NetOk;
            net[Marshal.OffsetOf(table, "Send") / sizeof(nint)] = (nint)(delegate* unmanaged[Cdecl]<uint, byte*, uint, int>)&Send;
            Replace(native, "_table", net);
            var snapshot = _engine.GetField("_serverSnapshot", PrivateStatic)!;
            _snapshot = snapshot.GetValue(null);
            snapshot.SetValue(null, Array.Empty<NetPeer>());
        }

        private void Replace(Type type, string name, void* memory)
        {
            var field = type.GetField(name, PrivateStatic)!;
            _fields.Add((field, field.GetValue(null), (nint)memory));
            field.SetValue(null, Pointer.Box(memory, field.FieldType));
        }

        public NetPeer Peer(EntityId player, NetPeerState state, bool channel)
        {
            var p = (NetPeer)Activator.CreateInstance(typeof(NetPeer), BindingFlags.NonPublic | BindingFlags.Instance, null, [200u], null)!;
            typeof(NetPeer).GetProperty(nameof(NetPeer.Player))!.SetValue(p, player);
            typeof(NetPeer).GetProperty(nameof(NetPeer.State))!.SetValue(p, state);
            if (channel)
            {
                ((Dictionary<string, string>)typeof(NetPeer).GetField("Declared", BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(p)!)[RulesApi.ChannelName] = RulesNet.ChannelVersion;
                _engine.GetMethod("RemapServerChannels", PrivateStatic)!.Invoke(null, [p]);
            }
            _engine.GetField("_serverSnapshot", PrivateStatic)!.SetValue(null, new[] { p });
            _peers.Add(p);
            return p;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int NetOk() => 0;
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int Send(uint peer, byte* frame, uint length)
        {
            // The real channel codec frames rules; inspect its body rather than inventing another wire path.
            if (length == 56 && frame[3] == 4) _sentFlags = *(uint*)(frame + 8);
            return 0;
        }
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int State(uint* p)
        {
            p[1] = 1; p[2] = _session; p[4] = _host; p[5] = _host == 0 ? 0u : 1u;
            return 0;
        }
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int Set(uint entity, RulesNative.Player* p) { _serverFlags = p == null ? 0 : p->Flags; return 0; }
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static void Clear() { }
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int Local(RulesNative.Player* p) { _localFlags = p == null ? 0 : p->Flags; return 0; }
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        private static int Status(RulesNative.Status* p) { p->Hooks = _hooks; return 0; }
        public void Dispose()
        {
            foreach (var p in _peers) RulesNet.ClientHooksOf(p, 0);
            _engine.GetField("_serverSnapshot", PrivateStatic)!.SetValue(null, _snapshot);
            foreach (var (field, before, memory) in _fields.AsEnumerable().Reverse())
            {
                field.SetValue(null, before);
                NativeMemory.Free((void*)memory);
            }
        }
    }
}

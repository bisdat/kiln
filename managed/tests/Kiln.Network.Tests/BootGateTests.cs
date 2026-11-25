// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.Network.Tests;

/// <summary>Boot content at the join (docs/kiln/network.md "Boot content at the join"): the <c>kiln.boot</c> handshake property, peers
/// of 0.2.0-dev.28 in both directions (frames recorded as that release writes them, and its decoder), and the server's decision for
/// each <c>modSync</c> value and each side that sends no fingerprint. Hellos go through the engine's frame entry on a fake kiln.net.</summary>
[Collection(nameof(SteamGuardRelayTests))]  // the engine's statics and ModsBackend: never at once with the other engine tests
public sealed class BootGateTests : IDisposable
{
    private const uint P = 0xB7;
    private const string Altars = "more-flame-altars";
    private const string ServerFp = "1111111111111111222222222222222233333333333333334444444444444444";
    private const string Same = "1111111111111111";
    private const string Other = "9999999999999999";

    // HELLO and WELCOME as Kiln 0.2.0-dev.28 writes them (Wire.cs at the dev.28 cut 8e0303af; out/bootfp/scripts/dev28frames.py):
    // a client with more-flame-altars 1.0.0 (both) and minimap 2.1.0 (client), the channel kiln:content 1, the world-table and
    // player-graph hashes; a server under refuse with more-flame-altars, kiln:content and its advertised properties.
    private const string Dev28Hello =
        "4b4e010101000c302e322e302d6465762e32380128000002116d6f72652d666c616d652d616c7461727305312e302e3002076d696e696d617005322e312e30" +
        "010100000c6b696c6e3a636f6e74656e7401310002106b696c6e2e776f726c647461626c657310303030303030303030303030303030300f6b696c6e2e70" +
        "6c61796572616e696d1030303030303030303030303030303030";
    private const string Dev28Welcome =
        "4b4e010201000c302e322e302d6465762e323801280000067265667573650000040001116d6f72652d666c616d652d616c7461727305312e302e3002010100" +
        "0c6b696c6e3a636f6e74656e740131000005106b696c6e2e776f726c647461626c657310303030303030303030303030303030300f6b696c6e2e706c617965" +
        "72616e696d10303030303030303030303030303030300e6b696c6e2e7365727665722e6964107372762d303132333435363738396162106b696c6e2e73657276" +
        "65722e6b696c6e0c302e322e302d6465762e3238136b696c6e2e72657175697265642e636f756e740130";

    private readonly AdmissionTests.FakeGate _gate = new();
    private readonly AdmissionTests.FakeNet _net = new();
    private readonly LogBackend _log = new();
    private readonly IKilnBackend _previousBackend = Backend.Current;
    private Dictionary<string, string> _serverBoot = new(StringComparer.Ordinal);

    public BootGateTests()
    {
        Backend.Current = _log;
        NetNative.TestTransport = _net;
        NetEngine.TestServerRole = true;
        NetAdmission.ResetForTests();
        NetAdmission.TestGate = _gate;
        BootGate.TestSource = () => _serverBoot;
        ModsBackend.Current = new FakeMods(new ModEntry(Altars, "More Flame Altars", "1.0.0", "/mods/altars", ModSide.Both, ModState.Loaded, null, [], 0, null));
    }

    public void Dispose()
    {
        NetEngine.OnPeer(P, false);
        NetAdmission.ResetForTests();
        NetAdmission.TestGate = null;
        NetEngine.UseConfigForTests(new NetworkConfig());
        NetNative.TestTransport = null;
        NetEngine.TestServerRole = null;
        BootGate.TestSource = null;
        global::Kiln.Host.BootFingerprint.RegistrySource = null;
        ModsBackend.Current = null;
        Backend.Current = _previousBackend;
    }

    private sealed class LogBackend : IKilnBackend
    {
        public readonly List<string> Lines = [];
        public string Version => "0.2.0-dev.29";
        public bool IsTickThread => true;
        public bool HasFeature(string name) => false;
        public void Log(ulong owner, LogLevel level, string text) { lock (Lines) Lines.Add($"[{level}] {text}"); }
        public nint Service(string name, uint minVersion) => 0;
    }

    private sealed class FakeMods(params ModEntry[] mods) : IModsBackend
    {
        public IReadOnlyList<ModEntry> All => mods;
        public ModSide Side => ModSide.Server;
        public ModEntry? Get(string id) => mods.FirstOrDefault(m => m.Id == id);
        public ModStats Stats(string id) => default;
        public bool Send(ModContext? sender, string targetId, string method, object? payload) => false;
        public Task<ModOperationResult> Request(ModOperation op, string arg) => throw new NotSupportedException();
        public void Request(ModOperation op, string arg, Action<ModOperationResult> done, ModContext? caller) => throw new NotSupportedException();
    }

    private static byte[] Hex(string s) => Convert.FromHexString(s);

    private static NetworkConfig Policy(ModSyncPolicy? policy) => policy is { } p
        ? new NetworkConfig { ModSync = p, ModSyncStated = true, Advertise = false }
        : new NetworkConfig { Advertise = false };  // modSync not set: the default (warn, plus the stated clients: "mod" rule)

    private NetPeer Up()
    {
        NetEngine.OnPeer(P, true);
        return NetEngine.ServerPeers().Single(x => x.Id == P);
    }

    private void Hello(string? boot, string version = "1.0.0", bool withMod = true)
    {
        var props = new Dictionary<string, string>(StringComparer.Ordinal);
        if (boot != null) props[BootGate.Key] = boot;
        var mods = withMod ? new List<Wire.ModEntry> { new(Altars, version, ModSide.Both) } : [];
        NetEngine.OnFrame(P, Wire.EncodeHello(new Wire.Hello(Wire.Protocol, "0.2.0-dev.29", 7, mods, [], props)));
    }

    [Theory]
    [InlineData(ModSyncPolicy.Refuse, true, true)]
    [InlineData(ModSyncPolicy.Warn, false, true)]
    [InlineData(ModSyncPolicy.Off, false, false)]
    public void Registry_boot_values_follow_modSync_and_report_the_difference(ModSyncPolicy policy, bool rejected, bool warned)
    {
        RegistryHello(policy, "1:" + new string('c', 64) + ":" + new string('b', 64));
        Assert.Equal(rejected, LastReject() != null);
        var text = rejected ? LastReject()!.Reason : string.Join(" ", LastWelcome()!.Warnings);
        Assert.Equal(warned, text.Contains("registry", StringComparison.Ordinal));
        if (warned) { Assert.Contains("restart", text); Assert.Contains("registry", Log()); }
    }

    // The values part covers every compared field's resolved value, so equal values mean the client runs what the server runs: another
    // order or other intermediate contributions (a setting a later mod overrides) change nothing the player could see or fix.
    [Theory]
    [InlineData(ModSyncPolicy.Refuse)]
    [InlineData(ModSyncPolicy.Warn)]
    [InlineData(null)]
    public void Registry_order_difference_alone_neither_refuses_nor_warns(ModSyncPolicy? policy)
    {
        RegistryHello(policy, "1:" + new string('a', 64) + ":" + new string('d', 64));
        Assert.Null(LastReject());
        Assert.Empty(LastWelcome()!.Warnings);
        Assert.Contains("registry boot edits or load order differ", Log());  // the operator's log still says it
    }

    [Fact]
    public void Registry_order_difference_stays_out_of_the_handshake_events_missing_list()
    {
        IReadOnlyList<string>? missing = null;
        using var sub = Events.On<NetClientHandshakeEvent>(e => missing = e.Missing);
        RegistryHello(ModSyncPolicy.Refuse, "1:" + new string('a', 64) + ":" + new string('d', 64));
        Assert.NotNull(missing);
        Assert.DoesNotContain(missing!, m => m.Contains("registry", StringComparison.Ordinal));
        Assert.Null(LastReject());
    }

    [Theory]
    [InlineData(null)]
    [InlineData("malformed")]
    [InlineData("2:future")]
    public void Older_or_unknown_registry_fingerprints_do_not_change_admission(string? remote)
    {
        RegistryHello(ModSyncPolicy.Refuse, remote);
        Assert.Null(LastReject());
        Assert.Empty(LastWelcome()!.Warnings);
    }

    [Fact]
    public void Matching_registry_fingerprints_join_without_warning()
    {
        RegistryHello(ModSyncPolicy.Refuse, "1:" + new string('a', 64) + ":" + new string('b', 64));
        Assert.Null(LastReject());
        Assert.Empty(LastWelcome()!.Warnings);
    }

    [Fact]
    public void A_new_client_keeps_joining_a_server_without_the_extension()
    {
        RegistryHello(ModSyncPolicy.Refuse, "1:" + new string('c', 64) + ":" + new string('d', 64), own: false);
        Assert.Null(LastReject());
        Assert.Empty(LastWelcome()!.Warnings);
    }

    // The registry part is one hash over every participating mod, so it cannot honour the mod-list rules per mod. When the
    // side-both mod lists differ, those rules (missing, versionMatch, optionalMods, extra) decide alone, as they did before.
    private const string Pack = "balance-pack";
    private static readonly string OtherValues = "1:" + new string('c', 64) + ":" + new string('b', 64);

    [Fact]
    public void Registry_part_is_not_compared_when_the_client_lacks_an_optional_server_mod()
    {
        ModsBackend.Current = new FakeMods(Loaded(Altars, "1.0.0"), Loaded(Pack, "1.0.0"));
        RegistryHello(ModSyncPolicy.Refuse, OtherValues, config: c => c with { OptionalMods = [Pack] });
        Assert.Null(LastReject());
        Assert.DoesNotContain(LastWelcome()!.Warnings, w => w.Contains("registry", StringComparison.Ordinal));
    }

    [Fact]
    public void Registry_part_is_not_compared_for_another_version_versionMatch_accepts()
    {
        RegistryHello(ModSyncPolicy.Refuse, OtherValues, [new(Altars, "1.4.0", ModSide.Both)], c => c with { VersionMatch = ModVersionMatch.Major });
        Assert.Null(LastReject());
        Assert.DoesNotContain(LastWelcome()!.Warnings, w => w.Contains("registry", StringComparison.Ordinal));
    }

    [Fact]
    public void Registry_part_is_not_compared_when_the_client_has_an_extra_shared_mod()
    {
        RegistryHello(ModSyncPolicy.Refuse, OtherValues, [new(Altars, "1.0.0", ModSide.Both), new(Pack, "1.0.0", ModSide.Both)]);
        Assert.Null(LastReject());
        Assert.Equal([$"client mod not on the server: {Pack} 1.0.0"], LastWelcome()!.Warnings);
    }

    [Fact]
    public void A_missing_required_mod_is_named_alone_without_a_registry_line()
    {
        RegistryHello(ModSyncPolicy.Refuse, OtherValues, []);
        Assert.Contains(Altars, LastReject()!.Reason);
        Assert.DoesNotContain("registry", LastReject()!.Reason);
    }

    [Fact]
    public void Client_side_mods_do_not_stop_the_registry_comparison()
    {
        RegistryHello(ModSyncPolicy.Refuse, OtherValues, [new(Altars, "1.0.0", ModSide.Both), new("minimap", "2.1.0", ModSide.Client)]);
        Assert.Contains("registry boot values differ", LastReject()!.Reason);
    }

    private static ModEntry Loaded(string id, string version) =>
        new(id, id, version, "/mods/" + id, ModSide.Both, ModState.Loaded, null, [], 0, null);

    private void RegistryHello(ModSyncPolicy? policy, string? remote, List<Wire.ModEntry>? mods = null, Func<NetworkConfig, NetworkConfig>? config = null, bool own = true)
    {
        // Resolve the same private Host seam the real handshake reads; no boot mod is required for a data-only pack.
        global::Kiln.Host.BootFingerprint.RegistrySource = () =>
            "{\"bootSync\":{\"values\":\"" + new string('a', 64) + "\",\"order\":\"" + new string('b', 64) + "\"}}";
        if (!own) global::Kiln.Host.BootFingerprint.RegistrySource = null;
        NetEngine.UseConfigForTests(config is null ? Policy(policy) : config(Policy(policy)));
        Up();
        var props = new Dictionary<string, string>();
        if (remote != null) props["kiln.registry.boot"] = remote;
        NetEngine.OnFrame(P, Wire.EncodeHello(new Wire.Hello(Wire.Protocol, "test", 7, mods ?? [new(Altars, "1.0.0", ModSide.Both)], [], props)));
        global::Kiln.Host.BootFingerprint.RegistrySource = null;
    }

    private Wire.Reject? LastReject() => _net.Sent.Where(s => s.Peer == P && Wire.TryHeader(s.Frame, out var t) && t == Wire.Type.Reject)
        .Select(s => Wire.DecodeReject(s.Frame)).LastOrDefault();

    private Wire.Welcome? LastWelcome() => _net.Sent.Where(s => s.Peer == P && Wire.TryHeader(s.Frame, out var t) && t == Wire.Type.Welcome)
        .Select(s => Wire.DecodeWelcome(s.Frame)).LastOrDefault();

    private string Log() { lock (_log.Lines) return string.Join("\n", _log.Lines); }

    // ------------------------------------------------------------------------------------------------ the property

    [Fact]
    public void The_property_sends_16_hex_digits_per_mod_in_id_order_and_out_for_one_left_out()
    {
        _serverBoot = new(StringComparer.Ordinal) { ["zz-mod"] = ServerFp, [Altars] = ServerFp.Replace('1', 'a'), ["broken"] = BootGate.Out };
        var p = BootGate.Properties();
        Assert.Equal($"broken=out,{Altars}=aaaaaaaaaaaaaaaa,zz-mod=1111111111111111", p[BootGate.Key]);
        Assert.Equal(_serverBoot.ToDictionary(kv => kv.Key, kv => kv.Value == BootGate.Out ? BootGate.Out : kv.Value[..16]), BootGate.Decode(p[BootGate.Key]));
    }

    [Fact]
    public void Without_a_boot_generation_nothing_is_sent()
    {
        _serverBoot = new(StringComparer.Ordinal);
        Assert.Empty(BootGate.Properties());
        BootGate.TestSource = null;  // Kiln.Host's own view, with no published generation
        Assert.Empty(BootGate.Properties());
    }

    [Fact]
    public void Without_boot_mods_recorded_frames_remain_byte_for_byte_identical()
    {
        _serverBoot = new(StringComparer.Ordinal);
        var hello = Wire.DecodeHello(Hex(Dev28Hello));
        var welcome = Wire.DecodeWelcome(Hex(Dev28Welcome));
        foreach (var (key, value) in BootGate.Properties())
        {
            ((Dictionary<string, string>)hello.Properties)[key] = value;
            ((Dictionary<string, string>)welcome.Properties)[key] = value;
        }
        Assert.Equal(Hex(Dev28Hello), Wire.EncodeHello(hello));
        Assert.Equal(Hex(Dev28Welcome), Wire.EncodeWelcome(welcome));
    }

    [Fact]
    public void Kiln_Hosts_published_fingerprints_reach_the_handshake_through_the_seam()
    {
        BootGate.TestSource = null;  // the game's path: BootGate finds Kiln.Host's BootFingerprint.Current by reflection
        try
        {
            BootGeneration.Record(4, [new BootModReport(Altars, 4, BootModState.Committed, [], []), new BootModReport("broken", 4, BootModState.ExcludedCallback, [], [])],
                new Dictionary<string, IReadOnlyList<BootStaged>>());
            global::Kiln.Host.BootFingerprint.Publish(4, new Dictionary<string, string?> { [Altars] = ServerFp, ["broken"] = null });
            Assert.Equal($"broken=out,{Altars}={Same}", BootGate.Properties()[BootGate.Key]);
            BootGeneration.Record(5, [], new Dictionary<string, IReadOnlyList<BootStaged>>());  // not the generation they were made for
            Assert.Empty(BootGate.Properties());
        }
        finally
        {
            global::Kiln.Host.BootFingerprint.Publish(0, new Dictionary<string, string?>());
            BootGeneration.Record(0, [], new Dictionary<string, IReadOnlyList<BootStaged>>());
        }
    }

    [Fact]
    public void A_value_that_is_no_fingerprint_is_not_sent_and_not_read()
    {
        _serverBoot = new(StringComparer.Ordinal) { ["a"] = "not-hex-at-all-xx", ["Bad Id"] = ServerFp, ["b"] = "12" };
        Assert.Empty(BootGate.Properties());
        var d = BootGate.Decode($"a=ABCDEF0123456789,b=12,c=zzzzzzzzzzzzzzzz,=x,d,Bad Id=1111111111111111,a=2222222222222222, e = out ");
        Assert.Equal(new Dictionary<string, string> { ["a"] = "abcdef0123456789", ["e"] = "out" }, d);
        Assert.Empty(BootGate.Decode(null));
        Assert.Empty(BootGate.Decode(""));
    }

    [Fact]
    public void Many_boot_mods_stay_within_the_budget_and_a_dev28_decoder_reads_the_hello()
    {
        _serverBoot = Enumerable.Range(0, 300).ToDictionary(i => $"mod-{i:D3}-" + new string('x', 52), _ => ServerFp, StringComparer.Ordinal);
        var p = BootGate.Properties();
        Assert.True(p[BootGate.Key].Length <= BootGate.MaxValueBytes, p[BootGate.Key].Length.ToString());
        var sent = BootGate.Decode(p[BootGate.Key]);
        Assert.InRange(sent.Count, 20, 299);  // as many as fit; the rest are not compared
        Assert.Contains("do not fit the handshake", Log());
        var hello = Wire.EncodeHello(new Wire.Hello(Wire.Protocol, "0.2.0-dev.29", 1, [], [], p));
        var old = Dev28Wire.DecodeHello(hello);
        Assert.Equal(p[BootGate.Key], old.Properties[BootGate.Key]);
    }

    // ------------------------------------------------------------------------------------------------ peers of 0.2.0-dev.28

    [Fact]
    public void A_recorded_dev28_hello_decodes_and_carries_no_fingerprint()
    {
        var h = Wire.DecodeHello(Hex(Dev28Hello));
        Assert.Equal("0.2.0-dev.28", h.KilnVersion);
        Assert.Equal(new[] { new Wire.ModEntry(Altars, "1.0.0", ModSide.Both), new Wire.ModEntry("minimap", "2.1.0", ModSide.Client) }, h.Mods);
        Assert.Equal("kiln:content", Assert.Single(h.Channels).Name);
        Assert.Equal(2, h.Properties.Count);
        Assert.Empty(BootGate.Decode(h.Properties.GetValueOrDefault(BootGate.Key)));
    }

    [Fact]
    public void A_recorded_dev28_welcome_decodes_and_carries_no_fingerprint()
    {
        var w = Wire.DecodeWelcome(Hex(Dev28Welcome));
        Assert.Equal("refuse", w.Policy);
        Assert.Equal(new Wire.ModEntry(Altars, "1.0.0", ModSide.Both), Assert.Single(w.Mods));
        Assert.Equal("srv-0123456789ab", w.Properties["kiln.server.id"]);
        Assert.Empty(BootGate.Decode(w.Properties.GetValueOrDefault(BootGate.Key)));
    }

    [Fact]
    public void A_dev28_decoder_reads_our_hello_and_welcome_with_the_fingerprints()
    {
        _serverBoot = new(StringComparer.Ordinal) { [Altars] = ServerFp };
        var props = new Dictionary<string, string>(StringComparer.Ordinal) { ["kiln.worldtables"] = "0000000000000000" };
        foreach (var (k, v) in BootGate.Properties()) props[k] = v;
        List<Wire.ModEntry> mods = [new(Altars, "1.0.0", ModSide.Both)];
        var hello = Wire.EncodeHello(new Wire.Hello(Wire.Protocol, "0.2.0-dev.29", 9, mods, [new(0, "kiln:content", "1", false)], props));
        var oldHello = Dev28Wire.DecodeHello(hello);
        Assert.Equal(mods, oldHello.Mods);
        Assert.Equal("0.2.0-dev.29", oldHello.KilnVersion);
        Assert.Equal($"{Altars}={Same}", oldHello.Properties[BootGate.Key]);  // an unknown property to it: kept in NetPeer.Properties, unread

        var welcome = Wire.EncodeWelcome(new Wire.Welcome(Wire.Protocol, "0.2.0-dev.29", 9, "refuse", 262144, mods, [new(1, "kiln:content", "1", false)],
            ["a warning"], props));
        var oldWelcome = Dev28Wire.DecodeWelcome(welcome);
        Assert.Equal(mods, oldWelcome.Mods);
        Assert.Equal(new[] { "a warning" }, oldWelcome.Warnings);
        Assert.Equal(props.OrderBy(kv => kv.Key, StringComparer.Ordinal), oldWelcome.Properties.OrderBy(kv => kv.Key, StringComparer.Ordinal));
    }

    [Fact]
    public void A_dev28_client_without_fingerprints_joins_a_server_with_them_under_refuse()
    {
        NetEngine.UseConfigForTests(Policy(ModSyncPolicy.Refuse));
        _serverBoot = new(StringComparer.Ordinal) { [Altars] = ServerFp };
        var p = Up();
        NetEngine.OnFrame(P, Hex(Dev28Hello));
        Assert.Equal(NetPeerState.Ready, p.State);
        Assert.Empty(LastWelcome()!.Warnings);
        Assert.Equal($"{Altars}={Same}", LastWelcome()!.Properties[BootGate.Key]);  // the server still sends its own
    }

    // ------------------------------------------------------------------------------------------------ the decision

    [Fact]
    public void Under_refuse_another_boot_content_of_the_same_version_is_refused_with_the_mods_name_and_what_to_do()
    {
        NetEngine.UseConfigForTests(Policy(ModSyncPolicy.Refuse));
        _serverBoot = new(StringComparer.Ordinal) { [Altars] = ServerFp };
        var p = Up();
        Hello($"{Altars}={Other}");
        Assert.Equal(NetPeerState.Rejected, p.State);
        var reject = LastReject()!;
        Assert.Contains("More Flame Altars", reject.Reason);
        Assert.Contains("settings", reject.Reason);
        Assert.Contains("restart the game", reject.Reason);
        Assert.DoesNotContain("do not have", reject.Reason);
        var detail = Assert.Single(reject.Details);
        Assert.StartsWith($"{Altars} 1.0.0 (boot content differs", detail);
        Assert.Empty(NetRefusal.NamedMods(reject.Details));  // not a mod to install: the notice shows the reason
        var notice = NetRefusal.Notice("Ember Isles", reject.Reason, reject.Details, modpack: true);
        Assert.Contains("More Flame Altars", notice);
        Assert.DoesNotContain("modpack", notice);
        Assert.Contains($"refusing {p.Name}", Log());
        Assert.Contains("boot content differs: its settings or files differ from the server's", Log());
    }

    [Fact]
    public void Under_refuse_the_same_boot_content_joins()
    {
        NetEngine.UseConfigForTests(Policy(ModSyncPolicy.Refuse));
        _serverBoot = new(StringComparer.Ordinal) { [Altars] = ServerFp };
        var p = Up();
        Hello($"{Altars}={Same}");
        Assert.Equal(NetPeerState.Ready, p.State);
        Assert.Empty(LastWelcome()!.Warnings);
    }

    [Theory]
    [InlineData(ModSyncPolicy.Warn)]
    [InlineData(null)]  // modSync not set: a version difference only warns there, so does this
    public void Under_warn_and_the_default_it_joins_with_a_warning_that_says_what_to_do(ModSyncPolicy? policy)
    {
        NetEngine.UseConfigForTests(Policy(policy));
        _serverBoot = new(StringComparer.Ordinal) { [Altars] = ServerFp };
        var p = Up();
        Hello($"{Altars}={Other}");
        Assert.Equal(NetPeerState.Ready, p.State);
        var warning = Assert.Single(LastWelcome()!.Warnings);
        Assert.StartsWith("boot content of More Flame Altars (more-flame-altars 1.0.0) differs from the server's", warning);
        Assert.Contains("restart the game", warning);
        Assert.Contains("1 warning(s): boot content of More Flame Altars", Log());
    }

    [Fact]
    public void Under_off_nothing_is_checked()
    {
        NetEngine.UseConfigForTests(Policy(ModSyncPolicy.Off));
        _serverBoot = new(StringComparer.Ordinal) { [Altars] = ServerFp };
        var p = Up();
        Hello($"{Altars}={Other}");
        Assert.Equal(NetPeerState.Ready, p.State);
        Assert.Empty(LastWelcome()!.Warnings);
    }

    [Theory]
    [InlineData("server-none")]     // not a boot mod on the server (or a server without a boot generation)
    [InlineData("client-none")]     // an older Kiln on the client, or not a boot mod there
    [InlineData("client-other-mod")]
    [InlineData("client-out")]      // left out of the client's generation (it then has not loaded the mod: see the next test)
    [InlineData("server-out")]
    public void A_fingerprint_missing_on_either_side_is_no_mismatch(string side)
    {
        NetEngine.UseConfigForTests(Policy(ModSyncPolicy.Refuse));
        _serverBoot = side switch
        {
            "server-none" => new(StringComparer.Ordinal),
            "server-out" => new(StringComparer.Ordinal) { [Altars] = BootGate.Out },
            _ => new(StringComparer.Ordinal) { [Altars] = ServerFp },
        };
        var client = side switch
        {
            "client-none" => null,
            "client-other-mod" => $"another-mod={Other}",
            "client-out" => $"{Altars}=out",
            _ => $"{Altars}={Other}",
        };
        var p = Up();
        Hello(client);
        Assert.Equal(NetPeerState.Ready, p.State);
        Assert.Empty(LastWelcome()!.Warnings);
    }

    [Fact]
    public void Left_out_on_the_client_and_committed_here_the_mod_rules_decide_and_the_log_says_why()
    {
        NetEngine.UseConfigForTests(Policy(ModSyncPolicy.Refuse));
        _serverBoot = new(StringComparer.Ordinal) { [Altars] = ServerFp };
        var p = Up();
        Hello($"{Altars}=out", withMod: false);  // its OnBoot failed there: the mod did not load on that game
        Assert.Equal(NetPeerState.Rejected, p.State);
        Assert.StartsWith("This server requires mods you do not have: more-flame-altars 1.0.0 (missing)", LastReject()!.Reason);
        Assert.Contains("more-flame-altars is left out of the client's boot generation and committed here", Log());
    }

    [Fact]
    public void Left_out_on_the_server_and_committed_on_the_client_is_said_in_the_joined_line()
    {
        NetEngine.UseConfigForTests(Policy(ModSyncPolicy.Warn));
        ModsBackend.Current = new FakeMods();  // the server's own copy did not load
        _serverBoot = new(StringComparer.Ordinal) { [Altars] = BootGate.Out };
        var p = Up();
        Hello($"{Altars}={Other}");
        Assert.Equal(NetPeerState.Ready, p.State);
        Assert.Equal("client mod not on the server: more-flame-altars 1.0.0", Assert.Single(LastWelcome()!.Warnings));
        Assert.Contains("more-flame-altars is left out of this server's boot generation and committed on the client", Log());
    }

    [Fact]
    public void Another_version_is_the_version_rule_alone()
    {
        // versionMatch any lets 1.1.0 in: its boot content may differ for that reason, and the operator accepted other versions
        NetEngine.UseConfigForTests(Policy(ModSyncPolicy.Refuse) with { VersionMatch = ModVersionMatch.Any });
        _serverBoot = new(StringComparer.Ordinal) { [Altars] = ServerFp };
        var p = Up();
        Hello($"{Altars}={Other}", version: "1.1.0");
        Assert.Equal(NetPeerState.Ready, p.State);
        NetEngine.OnPeer(P, false);
        NetEngine.UseConfigForTests(Policy(ModSyncPolicy.Refuse));
        p = Up();
        Hello($"{Altars}={Other}", version: "1.1.0");
        Assert.Equal(NetPeerState.Rejected, p.State);
        Assert.Equal(new[] { "more-flame-altars 1.0.0 (client has 1.1.0)" }, LastReject()!.Details);
    }

    [Theory]
    [InlineData(ModSide.Server)]
    [InlineData(ModSide.Client)]
    public void A_server_only_or_client_only_mod_is_not_compared(ModSide side)
    {
        NetEngine.UseConfigForTests(Policy(ModSyncPolicy.Refuse));
        ModsBackend.Current = new FakeMods(new ModEntry(Altars, "More Flame Altars", "1.0.0", "/mods/altars", side, ModState.Loaded, null, [], 0, null));
        _serverBoot = new(StringComparer.Ordinal) { [Altars] = ServerFp };
        var p = Up();
        var props = new Dictionary<string, string> { [BootGate.Key] = $"{Altars}={Other}" };
        NetEngine.OnFrame(P, Wire.EncodeHello(new Wire.Hello(Wire.Protocol, "0.2.0-dev.29", 7, [new(Altars, "1.0.0", side)], [], props)));
        Assert.Equal(NetPeerState.Ready, p.State);
        Assert.Empty(LastWelcome()!.Warnings);
    }

    [Fact]
    public void An_optional_mod_is_not_compared_as_its_version_is_not()
    {
        NetEngine.UseConfigForTests(Policy(ModSyncPolicy.Refuse) with { OptionalMods = [Altars] });
        _serverBoot = new(StringComparer.Ordinal) { [Altars] = ServerFp };
        var p = Up();
        Hello($"{Altars}={Other}");
        Assert.Equal(NetPeerState.Ready, p.State);
    }

    [Fact]
    public void A_missing_mod_and_another_boot_content_are_both_named_in_the_reason()
    {
        NetEngine.UseConfigForTests(Policy(ModSyncPolicy.Refuse));
        ModsBackend.Current = new FakeMods(
            new ModEntry(Altars, "More Flame Altars", "1.0.0", "/mods/altars", ModSide.Both, ModState.Loaded, null, [], 0, null),
            new ModEntry("lanterns", "Lanterns", "2.0.0", "/mods/lanterns", ModSide.Both, ModState.Loaded, null, [], 1, null));
        _serverBoot = new(StringComparer.Ordinal) { [Altars] = ServerFp };
        var p = Up();
        Hello($"{Altars}={Other}");
        Assert.Equal(NetPeerState.Rejected, p.State);
        var reason = LastReject()!.Reason;
        Assert.StartsWith("This server requires mods you do not have: lanterns 2.0.0 (missing)", reason);
        Assert.Contains("The boot content of More Flame Altars on your game differs from this server's", reason);
    }

    [Fact]
    public void The_reason_names_several_mods_and_cuts_a_long_list()
    {
        Assert.Contains("of A, B and C on your game", BootGate.Reason(["A", "B", "C"]));
        Assert.Contains("their settings", BootGate.Reason(["A", "B"]));
        Assert.Contains("of A, B, C and D (and 2 more)", BootGate.Reason(["A", "B", "C", "D", "E", "F"]));
    }
}

/// <summary>The HELLO and WELCOME decoders of Kiln 0.2.0-dev.28 (Wire.cs at the dev.28 cut 8e0303af), frozen here, so a change of
/// today's decoder cannot hide that a frame of ours no longer decodes on a peer of that release.</summary>
internal static class Dev28Wire
{
    private static List<Wire.ModEntry> ReadMods(ref NetReader r)
    {
        var n = (int)Math.Min(r.VarUInt(), 4097UL);
        if (n > 4096) throw new NetFormatException("too many mods");
        var list = new List<Wire.ModEntry>(n);
        for (var i = 0; i < n; i++)
        {
            var id = r.String(64);
            var ver = r.String(64);
            var side = r.Byte();
            list.Add(new Wire.ModEntry(id, ver, side <= 2 ? (ModSide)side : ModSide.Server));
        }
        return list;
    }

    private static List<Wire.ChannelEntry> ReadChannels(ref NetReader r)
    {
        var n = (int)Math.Min(r.VarUInt(), 4097UL);
        if (n > 4096) throw new NetFormatException("too many channels");
        var list = new List<Wire.ChannelEntry>(n);
        for (var i = 0; i < n; i++) list.Add(new Wire.ChannelEntry(r.UInt16(), r.String(128), r.String(64), r.Bool()));
        return list;
    }

    private static Dictionary<string, string> ReadProps(ref NetReader r)
    {
        var n = (int)Math.Min(r.VarUInt(), 257UL);
        if (n > 256) throw new NetFormatException("too many properties");
        var d = new Dictionary<string, string>(n, StringComparer.Ordinal);
        for (var i = 0; i < n; i++) d[r.String(128)] = r.String(4096);
        return d;
    }

    public static Wire.Hello DecodeHello(ReadOnlySpan<byte> frame)
    {
        var r = new NetReader(frame[Wire.HeaderSize..]);
        var proto = r.UInt16();
        var ver = r.String(64);
        var nonce = r.UInt32();
        var mods = ReadMods(ref r);
        var chans = ReadChannels(ref r);
        var props = r.End ? new Dictionary<string, string>() : ReadProps(ref r);
        return new Wire.Hello(proto, ver, nonce, mods, chans, props);
    }

    public static Wire.Welcome DecodeWelcome(ReadOnlySpan<byte> frame)
    {
        var r = new NetReader(frame[Wire.HeaderSize..]);
        var proto = r.UInt16();
        var ver = r.String(64);
        var nonce = r.UInt32();
        var policy = r.String(32);
        var max = r.UInt32();
        var mods = ReadMods(ref r);
        var chans = ReadChannels(ref r);
        var nw = (int)Math.Min(r.VarUInt(), 257UL);
        var warns = new List<string>();
        for (var i = 0; i < nw && i < 256; i++) warns.Add(r.String(2048));
        var props = r.End ? new Dictionary<string, string>() : ReadProps(ref r);
        return new Wire.Welcome(proto, ver, nonce, policy, max, mods, chans, warns, props);
    }
}

// SPDX-License-Identifier: MIT
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Network.Tests;

/// <summary>The kiln.npcbodies handshake property (chapter 16f §4.5): the server admits a client whose Ready bodies and closure hashes are
/// its own, refuses one that differs (naming the first body), counts an older peer as having none, and refuses a client with a body
/// staged after its start. Pure parts first, then hellos through the engine's frame entry on a fake kiln.net.</summary>
[Collection(nameof(SteamGuardRelayTests))]  // the engine's statics and ModsBackend: never at once with the other engine tests
public sealed class NpcBodyGateTests : IDisposable
{
    private const uint P = 0xB9;
    private static readonly string A = new('a', 64), B = new('b', 64);

    private readonly AdmissionTests.FakeGate _gate = new();
    private readonly AdmissionTests.FakeNet _net = new();
    private readonly LogBackend _log = new();
    private readonly IKilnBackend _previousBackend = Backend.Current;
    private string? _manifest;

    public NpcBodyGateTests()
    {
        Backend.Current = _log;
        NetNative.TestTransport = _net;
        NetEngine.TestServerRole = true;
        NetAdmission.ResetForTests();
        NetAdmission.TestGate = _gate;
        NpcBodyRows.ResetForTests();
        NpcBodyRows.TestSource = () => _manifest;
        ModsBackend.Current = new FakeMods();
    }

    public void Dispose()
    {
        NetEngine.OnPeer(P, false);
        NetAdmission.ResetForTests();
        NetAdmission.TestGate = null;
        NetEngine.UseConfigForTests(new NetworkConfig());
        NetNative.TestTransport = null;
        NetEngine.TestServerRole = null;
        NpcBodyRows.TestSource = null;
        NpcBodyRows.ResetForTests();
        ModsBackend.Current = null;
        Backend.Current = _previousBackend;
    }

    private sealed class LogBackend : IKilnBackend
    {
        public readonly List<string> Lines = [];
        public string Version => "0.2.0-dev.31";
        public bool IsTickThread => true;
        public bool HasFeature(string name) => false;
        public void Log(ulong owner, LogLevel level, string text) { lock (Lines) Lines.Add($"[{level}] {text}"); }
        public nint Service(string name, uint minVersion) => 0;
    }

    private sealed class FakeMods : IModsBackend
    {
        public IReadOnlyList<ModEntry> All => [];
        public ModSide Side => ModSide.Server;
        public ModEntry? Get(string id) => null;
        public ModStats Stats(string id) => default;
        public bool Send(ModContext? sender, string targetId, string method, object? payload) => false;
        public Task<ModOperationResult> Request(ModOperation op, string arg) => throw new NotSupportedException();
        public void Request(ModOperation op, string arg, Action<ModOperationResult> done, ModContext? caller) => throw new NotSupportedException();
    }

    private static JsonObject Row(string mod, string npc, string state, string closure) => new()
    {
        ["mod"] = mod, ["npc"] = npc, ["key"] = npc, ["state"] = state, ["reason"] = state == "ready" ? "" : "refused",
        ["template"] = "", ["model"] = "", ["graph"] = "", ["closure"] = closure, ["generation"] = 1,
    };

    private void Server(params JsonObject[] rows) =>
        _manifest = new JsonObject { ["bodies"] = new JsonArray(rows.Select(r => (JsonNode)r).ToArray()) }.ToJsonString();

    private NetPeer Up()
    {
        NetEngine.OnPeer(P, true);
        return NetEngine.ServerPeers().Single(x => x.Id == P);
    }

    private void Hello(Dictionary<string, string> props) =>
        NetEngine.OnFrame(P, Wire.EncodeHello(new Wire.Hello(Wire.Protocol, "0.2.0-dev.31", 7, [], [], props)));

    private Wire.Reject? LastReject() => _net.Sent.Where(s => s.Peer == P && Wire.TryHeader(s.Frame, out var t) && t == Wire.Type.Reject)
        .Select(s => Wire.DecodeReject(s.Frame)).LastOrDefault();

    private Wire.Welcome? LastWelcome() => _net.Sent.Where(s => s.Peer == P && Wire.TryHeader(s.Frame, out var t) && t == Wire.Type.Welcome)
        .Select(s => Wire.DecodeWelcome(s.Frame)).LastOrDefault();

    private string Log() { lock (_log.Lines) return string.Join("\n", _log.Lines); }

    // ------------------------------------------------------------------------------------------------ the property

    [Fact]
    public void The_hash_is_sha256_over_the_sorted_ready_lines_and_empty_without_bodies()
    {
        Server(Row("zeta", "tobin", "ready", A), Row("alpha", "bram", "ready", B.ToUpperInvariant()), Row("alpha", "x", "failed", ""),
            Row("alpha", "old", "removed", ""));
        var p = NpcBodyGate.Properties();
        var lines = $"alpha:bram:{B}\nzeta:tobin:{A}";
        Assert.Equal(Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(lines))), p[NpcBodyGate.KeyHash]);
        Assert.Equal("2", p[NpcBodyGate.KeyCount]);
        Assert.Equal($"alpha:bram:{B},zeta:tobin:{A}", p[NpcBodyGate.KeyList]);
        Assert.False(p.ContainsKey(NpcBodyGate.KeyPending));

        Server();
        p = NpcBodyGate.Properties();
        Assert.Equal("", p[NpcBodyGate.KeyHash]);
        Assert.Equal("0", p[NpcBodyGate.KeyCount]);
        Assert.False(p.ContainsKey(NpcBodyGate.KeyList));
        _manifest = """{"mods":[]}""";  // an older native build: no "bodies"
        Assert.Equal("", NpcBodyGate.Properties()[NpcBodyGate.KeyHash]);
    }

    [Fact]
    public void A_late_definition_goes_in_the_pending_key()
    {
        NpcBodyRows.SetLate("m:tobin", true);
        Assert.Equal("m:tobin", NpcBodyGate.Properties()[NpcBodyGate.KeyPending]);
        NpcBodyRows.SetLate("m:tobin", false);
        Assert.False(NpcBodyGate.Properties().ContainsKey(NpcBodyGate.KeyPending));
    }

    // ------------------------------------------------------------------------------------------------ the words

    [Fact]
    public void The_same_bodies_and_an_older_peer_against_none_are_no_difference()
    {
        List<string> server = [$"m:tobin:{A}"];
        Assert.Null(NpcBodyGate.Difference(server, NpcBodyGate.Properties(server, [])));
        Assert.Null(NpcBodyGate.Difference([], new Dictionary<string, string>()));
        Assert.Null(NpcBodyGate.Difference([], NpcBodyGate.Properties([], [])));
    }

    [Fact]
    public void Another_closure_names_the_body_built_differently()
    {
        var text = NpcBodyGate.Difference([$"m:bram:{B}", $"m:tobin:{A}"], NpcBodyGate.Properties([$"m:bram:{B}", $"m:tobin:{B}"], []))!;
        Assert.StartsWith("This server's NPC bodies don't match your game's: the NPC body m:tobin is built differently on your game", text);
        Assert.EndsWith("Join with the same mods, and restart if you just changed them.", text);
    }

    [Fact]
    public void A_missing_and_an_extra_body_are_named()
    {
        Assert.Contains("the server has the NPC body m:bram and your game doesn't",
            NpcBodyGate.Difference([$"m:bram:{B}", $"m:tobin:{A}"], NpcBodyGate.Properties([$"m:tobin:{A}"], [])));
        Assert.Contains("your game has the NPC body m:extra and this server doesn't",
            NpcBodyGate.Difference([$"m:tobin:{A}"], NpcBodyGate.Properties([$"m:extra:{B}", $"m:tobin:{A}"], [])));
    }

    [Fact]
    public void An_older_peer_counts_as_having_no_bodies()
    {
        var text = NpcBodyGate.Difference([$"m:tobin:{A}"], new Dictionary<string, string>())!;
        Assert.Contains("the server has the NPC body m:tobin and your game doesn't", text);
    }

    [Fact]
    public void A_cut_list_names_nothing_beyond_it_as_missing()
    {
        var client = new Dictionary<string, string>
        {
            [NpcBodyGate.KeyHash] = new('f', 64), [NpcBodyGate.KeyCount] = "200", [NpcBodyGate.KeyList] = $"m:a:{A}",
        };
        var text = NpcBodyGate.Difference([$"m:a:{A}", $"m:b:{A}"], client)!;
        Assert.DoesNotContain("your game doesn't", text);
        Assert.Contains("the server has 2 NPC bodies, your game has 200 NPC bodies, starting with m:a", text);
    }

    [Fact]
    public void A_late_client_body_is_restart_required_whatever_the_hash()
    {
        var props = NpcBodyGate.Properties([$"m:tobin:{A}"], ["m:tobin", "m:bram"]);
        var text = NpcBodyGate.Difference([$"m:tobin:{A}"], props)!;
        Assert.Contains("NPC body m:tobin and 1 more after it started", text);
        Assert.Contains("restart required", text);
    }

    // ------------------------------------------------------------------------------------------------ through the engine

    [Fact]
    public void A_client_with_the_same_bodies_joins_and_the_welcome_carries_the_servers_hash()
    {
        Server(Row("m", "tobin", "ready", A));
        var p = Up();
        Hello(NpcBodyGate.Properties([$"m:tobin:{A}"], []));
        Assert.Equal(NetPeerState.Ready, p.State);
        Assert.Equal(NpcBodyGate.Properties()[NpcBodyGate.KeyHash], LastWelcome()!.Properties[NpcBodyGate.KeyHash]);
    }

    [Fact]
    public void A_client_with_another_closure_is_refused_with_the_bodys_name()
    {
        Server(Row("m", "tobin", "ready", A));
        var p = Up();
        Hello(NpcBodyGate.Properties([$"m:tobin:{B}"], []));
        Assert.Equal(NetPeerState.Rejected, p.State);
        Assert.Contains("the NPC body m:tobin is built differently on your game", LastReject()!.Reason);
        Assert.Contains($"refusing {p.Name}", Log());
    }

    [Fact]
    public void An_older_client_is_refused_by_a_server_with_bodies_and_admitted_by_one_without()
    {
        Server(Row("m", "tobin", "ready", A));
        var p = Up();
        Hello(new Dictionary<string, string>());
        Assert.Equal(NetPeerState.Rejected, p.State);
        Assert.Contains("the server has the NPC body m:tobin and your game doesn't", LastReject()!.Reason);

        NetEngine.OnPeer(P, false);
        Server(Row("m", "tobin", "failed", ""));  // nothing Ready on the server: nothing to differ
        p = Up();
        Hello(new Dictionary<string, string>());
        Assert.Equal(NetPeerState.Ready, p.State);
    }

    [Fact]
    public void A_late_PendingRestart_client_is_refused_with_restart_required()
    {
        Server();
        var p = Up();
        Hello(NpcBodyGate.Properties([], ["m:tobin"]));
        Assert.Equal(NetPeerState.Rejected, p.State);
        Assert.Contains("restart required", LastReject()!.Reason);
    }

    // ------------------------------------------------------------------------------------------------ many bodies (16f review3 M1)

    // 64 Ready bodies with long block names: a "mod:npc:closure" line is about 150 bytes, so all of them are about 9.6 KB, more than a
    // HELLO or WELCOME value may be (Wire.ReadProps: 4096 bytes). The lists are cut by bytes (BootGate's 2048); the count and the hash
    // still cover every body.
    private static (JsonObject[] Rows, List<string> Lines) Many(string closure)
    {
        var rows = new List<JsonObject>();
        var lines = new List<string>();
        for (var i = 0; i < 64; i++)
        {
            var npc = $"a-villager-with-a-rather-long-block-name-to-fill-the-handshake-{i:00}";
            rows.Add(Row("kiln-equipment-and-more", npc, "ready", closure));
            lines.Add($"kiln-equipment-and-more:{npc}:{closure}");
        }
        lines.Sort(StringComparer.Ordinal);
        return (rows.ToArray(), lines);
    }

    [Fact]
    public void Sixty_four_long_named_bodies_join_and_the_list_stays_within_the_budget()
    {
        var (rows, lines) = Many(A);
        Server(rows);
        var props = NpcBodyGate.Properties();
        Assert.True(Encoding.UTF8.GetByteCount(props[NpcBodyGate.KeyList]) <= 2048, $"{Encoding.UTF8.GetByteCount(props[NpcBodyGate.KeyList])} bytes");
        Assert.Equal("64", props[NpcBodyGate.KeyCount]);
        Assert.Equal(Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(string.Join('\n', lines)))), props[NpcBodyGate.KeyHash]);
        var listed = PlayerGraphGate.Split(props[NpcBodyGate.KeyList]);
        Assert.True(listed.Count > 0);
        Assert.Equal(lines.Take(listed.Count), listed);  // whole lines, in order
        var p = Up();
        Hello(NpcBodyGate.Properties(lines, []));
        Assert.Equal(NetPeerState.Ready, p.State);
        Assert.Equal(props[NpcBodyGate.KeyHash], LastWelcome()!.Properties[NpcBodyGate.KeyHash]);
    }

    [Fact]
    public void Sixty_four_long_named_bodies_that_differ_are_refused_with_a_reason()
    {
        Server(Many(A).Rows);
        var p = Up();
        Hello(NpcBodyGate.Properties(Many(B).Lines, []));
        Assert.Equal(NetPeerState.Rejected, p.State);
        Assert.Contains("is built differently on your game", LastReject()!.Reason);
    }

    [Fact]
    public void A_long_pending_list_stays_within_the_budget_and_is_refused()
    {
        var late = Enumerable.Range(0, 64).Select(i => $"kiln-equipment-and-more:a-villager-defined-from-code-with-a-long-name-{i:00}").ToList();
        var props = NpcBodyGate.Properties([], late);
        Assert.True(Encoding.UTF8.GetByteCount(props[NpcBodyGate.KeyPending]) <= 2048, $"{Encoding.UTF8.GetByteCount(props[NpcBodyGate.KeyPending])} bytes");
        Server();
        var p = Up();
        Hello(props);
        Assert.Equal(NetPeerState.Rejected, p.State);
        Assert.Contains("restart required", LastReject()!.Reason);
    }

    [Fact]
    public void Under_modsync_off_it_joins_and_the_log_says_so()
    {
        NetEngine.UseConfigForTests(new NetworkConfig { ModSync = ModSyncPolicy.Off, ModSyncStated = true, Advertise = false });
        Server(Row("m", "tobin", "ready", A));
        var p = Up();
        Hello(NpcBodyGate.Properties([$"m:tobin:{B}"], []));
        Assert.Equal(NetPeerState.Ready, p.State);
        Assert.Contains("joins with other NPC bodies (modSync off)", Log());
    }
}

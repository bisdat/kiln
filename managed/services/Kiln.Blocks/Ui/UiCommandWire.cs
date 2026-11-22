// SPDX-License-Identifier: MIT
// The wire of joined-server UI commands (dossier 09 §4.5, DESIGN.md §5): channel "kiln:ui-command" v1, optional (a vanilla or
// older-Kiln peer has no such channel and is never sent a frame), JSON messages registered in this order:
//   0 Hello          S->C {nonce}                                       a fresh 128-bit hex challenge per connection and player
//   1 Request        C->S {nonce,id,line}                               id: decimal u64, from 1, strictly increasing per nonce
//   2 Reply          S->C {nonce,id,state,ok,code,reason,output,truncated}
//   3 Cancel         C->S {nonce,id}
//   4 StatusRequest  C->S {nonce,id}
// No message names a player, an account, an admin flag or a mod: the server takes the player from the connection. Fields a
// message carries beyond these are ignored. The line is a literal command line: at most 1024 UTF-8 bytes, no NUL, CR or LF.
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

internal static class UiCommandWire
{
    public const string ChannelName = "kiln:ui-command";
    public const string ChannelVersion = "1";
    public const int MaxChannelBytes = 32 * 1024;
    public const int MaxLineBytes = 1024, MaxReasonBytes = 512, MaxOutputBytes = 4096, MaxOutputLines = 16;
    public const ushort HelloId = 0, RequestId = 1, ReplyId = 2, CancelId = 3, StatusRequestId = 4;

    /// <summary>The server's challenge for this connection and player (S->C).</summary>
    internal sealed record Hello([property: JsonPropertyName("nonce")] string? Nonce);

    /// <summary>Run a line as this connection's player (C->S).</summary>
    internal sealed record Request(
        [property: JsonPropertyName("nonce")] string? Nonce,
        [property: JsonPropertyName("id")] string? Id,
        [property: JsonPropertyName("line")] string? Line);

    /// <summary>A request's state (S->C).</summary>
    internal sealed record Reply(
        [property: JsonPropertyName("nonce")] string Nonce,
        [property: JsonPropertyName("id")] string Id,
        [property: JsonPropertyName("state")] string State,
        [property: JsonPropertyName("ok")] bool Ok,
        [property: JsonPropertyName("code")] string Code,
        [property: JsonPropertyName("reason")] string? Reason,
        [property: JsonPropertyName("output")] string[]? Output,
        [property: JsonPropertyName("truncated")] bool Truncated);

    /// <summary>Remove a queued request (C->S). Too late once it started.</summary>
    internal sealed record Cancel(
        [property: JsonPropertyName("nonce")] string? Nonce,
        [property: JsonPropertyName("id")] string? Id);

    /// <summary>Ask for a request's state (C->S). Never runs anything.</summary>
    internal sealed record StatusRequest(
        [property: JsonPropertyName("nonce")] string? Nonce,
        [property: JsonPropertyName("id")] string? Id);

    // its own options: the shared default instance would cache these types process-wide for no gain
    private static readonly JsonSerializerOptions Json = new() { MaxDepth = 8 };

    public static byte[] Encode<T>(T message) => JsonSerializer.SerializeToUtf8Bytes(message, Json);

    /// <summary>A message from its bytes. kiln.net pads game-transport frames with up to 3 zero bytes: JSON never ends in NUL.</summary>
    public static T Decode<T>(ReadOnlySpan<byte> body) where T : class =>
        JsonSerializer.Deserialize<T>(body.TrimEnd((byte)0), Json) ?? throw new JsonException("null message");

    /// <summary>A request id: canonical decimal (no sign, no leading zero), 1 to 2^64-1.</summary>
    public static bool TryParseId(string? text, out ulong id)
    {
        id = 0;
        if (text is null || text.Length is 0 or > 20 || text[0] == '0') return false;
        foreach (var c in text)
            if (c is < '0' or > '9') return false;
        return ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out id) && id != 0;
    }

    public static string IdText(ulong id) => id.ToString(CultureInfo.InvariantCulture);

    /// <summary>A nonce as Hello carries it: 32 lower-case hex digits.</summary>
    public static bool NonceOk(string? nonce)
    {
        if (nonce is not { Length: 32 }) return false;
        foreach (var c in nonce)
            if (c is not ((>= '0' and <= '9') or (>= 'a' and <= 'f'))) return false;
        return true;
    }

    /// <summary>A fresh 128-bit challenge.</summary>
    public static string NewNonce() => Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(16));

    /// <summary>Whether a line may be sent: not blank, at most 1024 UTF-8 bytes, no NUL, CR or LF, no lone surrogate (its
    /// bytes would not be the line). Null when it may, else why not.</summary>
    public static string? LineProblem(string? line)
    {
        if (line is null) return "no line";
        if (string.IsNullOrWhiteSpace(line)) return "an empty command line";
        foreach (var c in line)
            if (c is '\0' or '\r' or '\n') return "a command is one line, without NUL, CR or LF";
        int bytes;
        try { bytes = Strict.GetByteCount(line); }
        catch (EncoderFallbackException) { return "the line is not valid text (a lone surrogate)"; }
        return bytes > MaxLineBytes ? $"a command line is at most {MaxLineBytes} UTF-8 bytes (this one is {bytes})" : null;
    }

    private static readonly UTF8Encoding Strict = new(false, true);

    /// <summary>The digest a same-id duplicate is compared by: SHA-256 of the line's exact UTF-8 bytes (nothing normalized).</summary>
    public static byte[] Digest(string line) => SHA256.HashData(Strict.GetBytes(line));

    /// <summary>At most <paramref name="max"/> UTF-8 bytes of a text, cut at a character boundary.</summary>
    public static string Clip(string text, int max, out bool cut)
    {
        cut = false;
        if (Encoding.UTF8.GetByteCount(text) <= max) return text;
        cut = true;
        var bytes = 0;
        var i = 0;
        while (i < text.Length)
        {
            var n = char.IsHighSurrogate(text[i]) && i + 1 < text.Length && char.IsLowSurrogate(text[i + 1]) ? 2 : 1;
            var b = Encoding.UTF8.GetByteCount(text.AsSpan(i, n));
            if (bytes + b > max) break;
            bytes += b;
            i += n;
        }
        return text[..i];
    }

    /// <summary>A command's output as a reply carries it: at most 16 lines and 4096 bytes in all (a line's bytes plus one
    /// for its end), cut at a character boundary.</summary>
    public static string[] ClipOutput(IReadOnlyList<string> output, out bool truncated)
    {
        truncated = output.Count > MaxOutputLines;
        var lines = new List<string>(Math.Min(output.Count, MaxOutputLines));
        var left = MaxOutputBytes;
        foreach (var line in output.Take(MaxOutputLines))
        {
            if (left <= 0) { truncated = true; break; }
            var kept = Clip(line ?? "", Math.Max(0, left - 1), out var cut);
            lines.Add(kept);
            left -= Encoding.UTF8.GetByteCount(kept) + 1;
            if (cut) { truncated = true; break; }
        }
        return [.. lines];
    }

    public static string? ClipReason(string? reason) => reason is null ? null : Clip(reason, MaxReasonBytes, out _);

    public static string StateName(UiCommandState s) => s switch
    {
        UiCommandState.Queued => "queued",
        UiCommandState.Started => "started",
        UiCommandState.RunningOverBudget => "runningOverBudget",
        _ => "final",
    };

    public static bool TryState(string? name, out UiCommandState state)
    {
        state = name switch
        {
            "queued" => UiCommandState.Queued,
            "started" => UiCommandState.Started,
            "runningOverBudget" => UiCommandState.RunningOverBudget,
            "final" => UiCommandState.Final,
            _ => (UiCommandState)(-1),
        };
        return (int)state >= 0;
    }

    public static string CodeName(UiCommandCode code) => JsonNamingPolicy.CamelCase.ConvertName(code.ToString());

    public static bool TryCode(string? name, out UiCommandCode code)
    {
        code = UiCommandCode.None;
        foreach (var c in Enum.GetValues<UiCommandCode>())
            if (CodeName(c) == name) { code = c; return true; }
        return false;
    }

    /// <summary>What a final code says about whether the command ran (the wire has no outcome field).</summary>
    public static UiCommandOutcome OutcomeOf(UiCommandCode code) => code switch
    {
        UiCommandCode.None or UiCommandCode.HandlerFailure => UiCommandOutcome.Completed,
        UiCommandCode.PermissionDenied or UiCommandCode.ParseFailure or UiCommandCode.CanceledByCommandEvent or UiCommandCode.QueueFull
            or UiCommandCode.RateLimited or UiCommandCode.ExpiredBeforeStart or UiCommandCode.CanceledBeforeStart
            or UiCommandCode.AssociationChanged or UiCommandCode.ProtocolConflict => UiCommandOutcome.NotStarted,
        UiCommandCode.CanceledBeforeSend => UiCommandOutcome.NotSent,
        _ => UiCommandOutcome.Unknown,
    };
}

/// <summary>Started by Kiln's service autostart (ServiceTick.AutoStart, after Kiln.Network): opens kiln:ui-command on every
/// side that has kiln.net, so a dedicated server receives commands with no UI block mod and no overlay, and a client's
/// hello lists the channel before it joins.</summary>
internal static class UiCommandStartup
{
    static UiCommandStartup() => UiCommandChannel.Start();
}

/// <summary>kiln:ui-command in the game: the channel, the server's receiver on Kiln.Network's end-of-dispatch callback, the
/// client's sender on the client world's tick, and the row ui.command.server.</summary>
internal static class UiCommandChannel
{
    internal static readonly UiServerCommands Server = new();
    private static NetChannel? _channel;
    private static int _started;
    private static readonly List<(object, EntityId)> Live = new();

    /// <summary>The player of a connection: the one the game gave it. Lab test mods may stand a lab peer in for a player
    /// (a lab transport peer has none); nothing a client sends can.</summary>
    internal static Func<NetPeer, EntityId> PlayerOf = static p => p.Player;

    internal static bool Started => Volatile.Read(ref _started) != 0 && _channel is not null;

    internal static void Start()
    {
        if (Interlocked.Exchange(ref _started, 1) != 0) return;
        bool client;
        try { client = Client.Available && Client.IsClient; }
        catch (Exception e) when (e is InvalidOperationException or TypeLoadException or FileNotFoundException) { client = false; }
        if (!Network.Available)
        {
            ServiceCaps.Report(BlockUi.ServerCommandCapability, CapabilityState.Unavailable, CapabilityReason.ServiceMissing, "kiln.net is not here: no command reaches a server");
            return;
        }
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // Kiln's own channel and handlers, not a mod's
        try
        {
            _channel = Network.Channel(UiCommandWire.ChannelName, UiCommandWire.ChannelVersion, required: false, maxMessageBytes: UiCommandWire.MaxChannelBytes);
            Register<UiCommandWire.Hello>(UiCommandWire.HelloId, static (m, _) => UiCommandClient.Shared.OnHello(m.Nonce), NetDirection.ToClient);
            Register<UiCommandWire.Request>(UiCommandWire.RequestId, static (m, ctx) => Server.OnRequest(ctx.Sender, m), NetDirection.ToServer);
            Register<UiCommandWire.Reply>(UiCommandWire.ReplyId, static (m, _) => UiCommandClient.Shared.OnReply(m), NetDirection.ToClient);
            Register<UiCommandWire.Cancel>(UiCommandWire.CancelId, static (m, ctx) => Server.OnCancel(ctx.Sender, m), NetDirection.ToServer);
            Register<UiCommandWire.StatusRequest>(UiCommandWire.StatusRequestId, static (m, ctx) => Server.OnStatus(ctx.Sender, m), NetDirection.ToServer);

            Server.Execute = Run;
            Server.Send = static (conn, msg) => conn is NetPeer p && _channel is { } ch && ch.SendTo(p, msg);
            Server.CurrentPlayer = static conn => conn is NetPeer { State: NetPeerState.Ready } p && p.HasChannel(UiCommandWire.ChannelName) ? PlayerOf(p) : EntityId.None;
            Server.Log = static text => BlocksBridge.Log(LogLevel.Info, text);
            NetEngine.AfterServerDispatch += OnServerDispatched;

            var c = UiCommandClient.Shared;
            c.Send = static msg => _channel is { } ch && ch.SendToServer(msg);
            c.AvailabilityChanged = static _ => ReportClientRow();
            if (client)
            {
                Network.Disconnected += static _ => UiCommandClient.Shared.OnLinkLost();
                Network.ServerVanilla += static _ => ReportClientRow();
                Network.ServerReady += static _ => ReportClientRow();
                Client.Tick += static (in TickInfo _) => UiCommandClient.Shared.Pump();
            }
        }
        catch (Exception e) when (e is InvalidOperationException or ArgumentException or KilnUnavailableException or TypeInitializationException)
        {
            BlocksBridge.Log(LogLevel.Warn, $"blocks: ui-command: the channel {UiCommandWire.ChannelName} is unavailable: {e.Message}");
            ServiceCaps.Report(BlockUi.ServerCommandCapability, CapabilityState.Unavailable, CapabilityReason.Faulted, $"{UiCommandWire.ChannelName} could not be opened: {e.Message}");
            return;
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
        if (client) ReportClientRow();
        else ServiceCaps.Report(BlockUi.ServerCommandCapability, CapabilityState.Unavailable, CapabilityReason.WrongSide,
            "a dedicated server: it receives players' UI commands on kiln:ui-command (the receiver is on) and sends none");
        BlocksBridge.Log(LogLevel.Info, $"blocks: ui-command: {UiCommandWire.ChannelName} v{UiCommandWire.ChannelVersion} open: " +
            $"the receiver runs joined players' commands as those players{(client ? "; this game can send its own" : "")}");
    }

    /// <summary>The row before <see cref="Start"/> ran (Kiln.Blocks' rows at their first report): it says why it is off, so
    /// that a call marked with it explains itself also before the start. Start reports the real state; nothing here
    /// overwrites it afterwards.</summary>
    internal static void ReportBeforeStart()
    {
        if (Volatile.Read(ref _started) != 0) return;
        bool net;
        try { net = Network.Available; }
        catch (Exception e) when (e is InvalidOperationException or TypeLoadException or FileNotFoundException) { net = false; }
        if (!net)
            ServiceCaps.Report(BlockUi.ServerCommandCapability, CapabilityState.Unavailable, CapabilityReason.ServiceMissing, "kiln.net is not here: no command reaches a server");
        else
            ServiceCaps.Report(BlockUi.ServerCommandCapability, CapabilityState.Pending, CapabilityReason.Session,
                $"{UiCommandWire.ChannelName} opens at Kiln.Blocks' start (the first service tick)");
    }

    private static void Register<T>(ushort id, Action<T, NetContext> handler, NetDirection direction) where T : class =>
        _channel!.Register<T>(id, static (m, w) => w.Raw(UiCommandWire.Encode(m)), static (ref NetReader r) => UiCommandWire.Decode<T>(r.Rest()), handler, direction);

    /// <summary>The client's row: available while the joined (or hosted) server's hello holds.</summary>
    private static void ReportClientRow()
    {
        if (UiCommandClient.Shared.Available)
        {
            ServiceCaps.Report(BlockUi.ServerCommandCapability, CapabilityState.Available);
            return;
        }
        var link = Network.Server;
        if (link is { State: NetPeerState.Vanilla } || link is { State: NetPeerState.Ready } && !link.HasChannel(UiCommandWire.ChannelName))
            ServiceCaps.Report(BlockUi.ServerCommandCapability, CapabilityState.Unavailable, CapabilityReason.ServiceMissing,
                "the server this game joined has no kiln:ui-command (no Kiln, or an older Kiln): nothing is sent");
        else
            ServiceCaps.Report(BlockUi.ServerCommandCapability, CapabilityState.Pending, CapabilityReason.Session,
                link is null ? "not joined to a server" : "waiting for the server's hello on kiln:ui-command (it comes once this game's player is in its world)");
    }

    // Kiln.Network's end of a server step's dispatch (tick thread): every Request and Cancel delivered so far is in; now the
    // associations, then expiries and starts.
    private static void OnServerDispatched()
    {
        Live.Clear();
        foreach (var p in NetEngine.ServerPeers())
            if (p.State == NetPeerState.Ready && p.HasChannel(UiCommandWire.ChannelName)) Live.Add((p, PlayerOf(p)));
        Server.Sync(Live);
        Server.Drain();
    }

    /// <summary>A line as the player: the command registry's own permission, parsing and cancel events decide.</summary>
    private static UiCommandRun Run(EntityId player, string line) => Map(Commands.Execute(CommandSource.ForPlayer(player), line));

    internal static UiCommandRun Map(CommandResult r)
    {
        if (r.Completion is { } done)
            return new UiCommandRun(false, UiCommandCode.None, null, r.Output,
                done.ContinueWith(static t => Map(t.Result), CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default));
        var code = r.Ok ? UiCommandCode.None
            : r.Denied ? UiCommandCode.PermissionDenied
            : r.Canceled ? UiCommandCode.CanceledByCommandEvent
            : r.ParseError || !r.Found || r.Name.Length == 0 ? UiCommandCode.ParseFailure
            : UiCommandCode.HandlerFailure;
        return new UiCommandRun(r.Ok, code, r.Reason, r.Output);
    }
}

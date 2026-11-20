// SPDX-License-Identifier: MIT
// Issue #44 phase 1b. Internal, optional native-chat completion transport.
// Uses the existing Kiln.Commands command tree and the requesting PLAYER's
// permissions. No new mod API and no command execution over this channel.
using System.Collections.Concurrent;
using System.Text;

namespace Kiln;

internal sealed record ChatCompletionRequest(uint Id, string Text)
{
    internal const ushort MessageId = 1;
    internal static void Encode(ChatCompletionRequest m, NetWriter w) => w.UInt32(m.Id).String(m.Text);
    internal static ChatCompletionRequest Decode(ref NetReader r) => new(r.UInt32(), r.String(256));
}

internal sealed record ChatCompletionReply(uint Id, string Text, ushort Start, string[] Suggestions)
{
    internal const ushort MessageId = 2;
    internal static void Encode(ChatCompletionReply m, NetWriter w)
    {
        w.UInt32(m.Id).String(m.Text).UInt16(m.Start).Byte((byte)Math.Min(m.Suggestions.Length, 20));
        foreach (var suggestion in m.Suggestions.Take(20)) w.String(suggestion);
    }

    internal static ChatCompletionReply Decode(ref NetReader r)
    {
        var id = r.UInt32();
        var text = r.String(256);
        var start = r.UInt16();
        var count = Math.Min((int)r.Byte(), 20);
        var words = new string[count];
        for (var i = 0; i < count; i++) words[i] = r.String(128);
        return new ChatCompletionReply(id, text, start, words);
    }
}

/// <summary>Pure bounded rules: never take user identity from a request message.</summary>
internal static class ChatCompletionRules
{
    internal const int MaxTextBytes = 256, MaxSuggestions = 20;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    // The default Encoding.UTF8 silently replaces unpaired UTF-16 surrogates.
    // A completion request or suggestion must instead be valid Unicode before
    // it crosses the managed/network/native chat boundaries.
    private static bool Utf8LengthWithin(string text, int max)
    {
        try { return StrictUtf8.GetByteCount(text) <= max; }
        catch (EncoderFallbackException) { return false; }
    }

    internal static bool RequestText(string? text) =>
        text is { Length: > 1 } &&
        text[0] == '/' &&
        !text.Any(char.IsControl) &&
        Utf8LengthWithin(text, MaxTextBytes);

    internal static bool Candidate(string? word) =>
        !string.IsNullOrEmpty(word) &&
        !word.Any(char.IsControl) &&
        Utf8LengthWithin(word, 128);

    // The reply's start is a UTF-8 byte offset (the server converts
    // Commands.Complete's UTF-16 index), never one into the middle of a
    // codepoint. Verify that at the protocol boundary as well as in the
    // managed UI, so malformed replies cannot enter the active model.
    internal static bool StartBoundary(string text, ushort start)
    {
        if (!RequestText(text)) return false;
        byte[] bytes;
        try { bytes = StrictUtf8.GetBytes(text); }
        catch (EncoderFallbackException) { return false; }
        return start <= bytes.Length &&
            (start == bytes.Length || (bytes[start] & 0xC0) != 0x80);
    }

    internal static bool Matches(ChatCompletionReply reply, uint id, string text) =>
        reply.Id == id && reply.Text == text &&
        StartBoundary(text, reply.Start) &&
        reply.Suggestions.Length <= MaxSuggestions &&
        reply.Suggestions.All(Candidate);
}

/// <summary>
/// Kiln-owned optional channel; one request at a time from the active native
/// chat editor. Server commands are completed, NEVER executed. The server
/// derives its source from NetContext.Player (or the actual hosted player).
/// </summary>
internal static class ChatCompletionNetwork
{
    private const string ChannelName = "kiln:chat";
    private static readonly object Gate = new();
    private static NetChannel? _channel;
    private static readonly ConcurrentQueue<(EntityId Player, ChatCompletionRequest Request)> Local = new();
    private static readonly Dictionary<uint, Queue<long>> Rates = new();
    private static readonly object ReplyGate = new();
    private static ChatCompletionReply? _reply;
    private static int _nextId;
    private static bool _startedTick;
    private static NetPeer? _unsupportedOn;  // the server link that refused kiln:chat (client side)

    internal static void Start()
    {
        if (_channel is not null || !Network.Available) return;
        lock (Gate)
        {
            if (_channel is not null) return;
            var original = ModContext.CurrentMod;
            ModContext.CurrentMod = null;
            try
            {
                _channel = Network.Channel(ChannelName, "1", required: false)
                    .Register<ChatCompletionRequest>(ChatCompletionRequest.MessageId,
                        static (m, w) => ChatCompletionRequest.Encode(m, w),
                        static (ref NetReader r) => ChatCompletionRequest.Decode(ref r),
                        static (m, ctx) => Request(ctx.Player, m, ctx.Sender), NetDirection.ToServer)
                    .Register<ChatCompletionReply>(ChatCompletionReply.MessageId,
                        static (m, w) => ChatCompletionReply.Encode(m, w),
                        static (ref NetReader r) => ChatCompletionReply.Decode(ref r),
                        static (m, _) => Receive(m), NetDirection.ToClient);
            }
            catch (Exception e)
            {
                Backend.Current.Log(0, LogLevel.Warn, "chat completion channel unavailable: " + e.GetType().Name);
            }
            finally { ModContext.CurrentMod = original; }
        }
    }

    // Called from Kiln.Ui's render frame (client). SendToServer is any-thread.
    // A hosting client's local request uses the exact same tick-thread handler
    // as a remote client but has no network peer to impersonate.
    //
    // A server without kiln:chat (vanilla, or a Kiln older than completion) refuses every send. That connection is
    // remembered, so a "/" line kept open does not try again every frame; a new connection (a new link) asks again.
    internal static uint Ask(string text)
    {
        if (!ChatCompletionRules.RequestText(text)) return 0;
        Start();
        if (Client.IsHosting && Client.HasLocalPlayer)
        {
            var local = new ChatCompletionRequest(NextId(), text);
            EnqueueLocal(Client.LocalPlayer, local);
            return local.Id;
        }
        var server = Network.Server;
        if (server is null || ReferenceEquals(server, Volatile.Read(ref _unsupportedOn))) return 0;
        var request = new ChatCompletionRequest(NextId(), text);
        if (_channel?.SendToServer(request) == true) return request.Id;
        // still connecting: try again later; Kiln's handshake done (or no Kiln there) and still refused: not this link
        if (server.State is NetPeerState.Ready or NetPeerState.Vanilla) Volatile.Write(ref _unsupportedOn, server);
        return 0;
    }

    private static uint NextId() => unchecked((uint)Interlocked.Increment(ref _nextId));

    /// <summary>A hosting client's own request: answered by the server tick (<see cref="DrainLocal"/>), like a remote one.</summary>
    internal static void EnqueueLocal(EntityId player, ChatCompletionRequest request) => Local.Enqueue((player, request));

    internal static bool TakeReply(out ChatCompletionReply reply)
    {
        lock (ReplyGate)
        {
            reply = _reply!;
            _reply = null;
            return reply is not null;
        }
    }

    private static void Receive(ChatCompletionReply reply)
    {
        if (!ChatCompletionRules.Matches(reply, reply.Id, reply.Text)) return;
        lock (ReplyGate) _reply = reply; // latest wins; UI validates id AND text
    }

    // Server tick: once at a time, no sleep or disk I/O.
    internal static void StartServer()
    {
        if (_startedTick) return;
        _startedTick = true;
        ServiceTick.Add("kiln chat completion", OnServerTick);
        Start();
    }

    private static void OnServerTick(in TickInfo tick)
    {
        if (_channel is null) Start();
        DrainLocal();
    }

    internal static void DrainLocal()
    {
        while (Local.TryDequeue(out var local))
            Request(local.Player, local.Request, null);
    }

    // Server handler runs on Kiln.Network's tick thread. No player identity,
    // elevated source, or permission bit can be supplied on the wire.
    private static void Request(EntityId player, ChatCompletionRequest request, NetPeer? peer)
    {
        var response = Complete(player, request, peer?.Id ?? LocalKey, Environment.TickCount64);
        if (response is null) return;
        if (peer is null) Receive(response);
        else _channel?.SendTo(peer, response);
    }

    internal const uint LocalKey = uint.MaxValue;  // local host has a distinct rate bucket

    /// <summary>The reply to one request from <paramref name="player"/> (whose own permissions decide), or null when it
    /// is dropped: no player, a bad line, or more than 10 a second from <paramref name="key"/> (the peer).</summary>
    internal static ChatCompletionReply? Complete(EntityId player, ChatCompletionRequest request, uint key, long now)
    {
        if (player.IsNone || !ChatCompletionRules.RequestText(request.Text)) return null;
        lock (Gate)
        {
            if (!Rates.TryGetValue(key, out var q)) Rates[key] = q = new Queue<long>();
            while (q.Count > 0 && now - q.Peek() >= 1000) q.Dequeue();
            if (q.Count >= 10) return null;
            q.Enqueue(now);
            // Prune stale peer buckets occasionally (the queue is bounded).
            if (Rates.Count > 128)
                foreach (var old in Rates.Where(kv => kv.Value.Count == 0).Select(kv => kv.Key).ToArray())
                    Rates.Remove(old);
        }

        try
        {
            var source = CommandSource.ForPlayer(player); // NEVER HostConsole or Console
            var suggestions = Commands.Complete(source, request.Text, ChatCompletionRules.MaxSuggestions);
            var start = suggestions.Start;
            if (start < 0 || start > request.Text.Length) return null;
            var utf8Start = Encoding.UTF8.GetByteCount(request.Text.AsSpan(0, start));
            var safe = suggestions.Suggestions.Where(ChatCompletionRules.Candidate).Take(20).ToArray();
            return new ChatCompletionReply(request.Id, request.Text, (ushort)utf8Start, safe);
        }
        catch (Exception e)
        {
            // No typed text, names or command payload in logs.
            Backend.Current.Log(0, LogLevel.Warn, "chat completion refused: " + e.GetType().Name);
            return null;
        }
    }

    internal static void ResetForTests()
    {
        lock (Gate) Rates.Clear();
        while (Local.TryDequeue(out _)) { }
        lock (ReplyGate) _reply = null;
        Volatile.Write(ref _unsupportedOn, null);
    }
}

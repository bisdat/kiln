// SPDX-License-Identifier: MIT
// Kiln.Replication's message hooks, inject and the game's message codec over kiln.repl.net (native/core/abi/services/
// kiln_repl.h message_hook, message_inject, schema_*; the dossier's chapter 15 §4 M4, §5.2; DESIGN slice C3).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>The game's network channels, by their wire number.</summary>
public enum GameChannel : byte
{
    /// <summary>Login and its answer.</summary>
    Login = 0,
    /// <summary>The server's name, settings and the like (a schema message).</summary>
    ServerInfo = 1,
    /// <summary>Terrain voxel data.</summary>
    VoxelData = 2,
    /// <summary>Fog voxel data.</summary>
    FogVoxelData = 3,
    /// <summary>Entities, client channel (entity level: <see cref="Replication.Filter{TComponent}"/>; not hookable here).</summary>
    EcsClient = 4,
    /// <summary>Entities, prediction channel (not hookable here).</summary>
    EcsPrediction = 5,
    /// <summary>Entity snapping.</summary>
    EcsSnapping = 6,
    /// <summary>Entity proxies.</summary>
    EntityProxies = 7,
    /// <summary>A player's input.</summary>
    Input = 8,
    /// <summary>The character's save data.</summary>
    SaveGame = 9,
    /// <summary>A player's knowledge (a schema message).</summary>
    Knowledge = 10,
    /// <summary>Logic graph edits (a schema message).</summary>
    LogicGraph = 11,
    /// <summary>Kiln's own channel (not hookable).</summary>
    Debug = 12,
    /// <summary>Requests to resend entities from scratch.</summary>
    EcsReset = 13,
    /// <summary>Voice chat (no socket; not hookable).</summary>
    VoiceChat = 14,
    /// <summary>A player's custom strings (a schema message).</summary>
    CustomPlayerStrings = 15,
    /// <summary>Water.</summary>
    Water = 16,
    /// <summary>Twitch drops (read outside the loops; not hookable).</summary>
    TwitchDrop = 17,
    /// <summary>Weather (a schema message).</summary>
    Weather = 18,
    /// <summary>Chat lines (a schema message).</summary>
    ChatMessages = 19,
    /// <summary>Base sharing (a schema message).</summary>
    BaseSharing = 20,
    /// <summary>Admin and ban info (a schema message).</summary>
    AdminInfo = 21,
    /// <summary>Traders.</summary>
    Commerce = 22,
    /// <summary>Voxel damage.</summary>
    VoxelDamageSystem = 23,
    /// <summary>Factory info.</summary>
    FactoryInfo = 24,
}

/// <summary>Which way a hooked message goes.</summary>
[Flags]
public enum MessageDirection
{
    /// <summary>Messages this process sends (a server to its peers, a client to the server).</summary>
    Send = 1,
    /// <summary>Messages this process receives.</summary>
    Receive = 2,
}

/// <summary>What a <see cref="MessageHandler"/> does with a message.</summary>
public enum MessageAction
{
    /// <summary>The message goes on as it is.</summary>
    Pass = 0,
    /// <summary>The message is not sent (send) or not handled (receive), as if it never was.</summary>
    Drop = 1,
    /// <summary>The bytes in <see cref="MessageRewrite.Buffer"/> (<see cref="MessageRewrite.Length"/> of them) replace the message.</summary>
    Rewrite = 2,
}

/// <summary>The rewrite buffer of a hooked message: a copy of its bytes, its capacity the message's length.</summary>
public unsafe ref struct MessageRewrite
{
    private readonly byte* _buffer;
    private readonly int _cap;

    internal MessageRewrite(byte* buffer, int cap, int length)
    {
        _buffer = buffer;
        _cap = cap;
        Length = length;
    }

    /// <summary>The bytes to change (a copy of the message; as long as the message).</summary>
    public readonly Span<byte> Buffer => new(_buffer, _cap);

    /// <summary>The new length (at most <see cref="Buffer"/>'s length).</summary>
    public int Length { get; set; }
}

/// <summary>Looks at one message of a hooked channel and says what happens to it. Runs on the server's tick thread or
/// the client's main thread, inside the game's network loop: be quick. An exception passes the message unchanged.</summary>
/// <param name="peer">The peer it goes to or comes from (null: the server, on a client; a hosted world's own client).</param>
/// <param name="data">The message's bytes as the game codes them.</param>
/// <param name="rewrite">Write the new bytes here and return <see cref="MessageAction.Rewrite"/> (shorter or as long).</param>
public delegate MessageAction MessageHandler(NetPeer? peer, ReadOnlySpan<byte> data, ref MessageRewrite rewrite);

/// <summary>What <see cref="Replication.Inject"/> did.</summary>
/// <param name="Ok">The game's session took the message for sending.</param>
/// <param name="Reason">Why not.</param>
public sealed record InjectResult(bool Ok, string? Reason) : IKilnResult
{
    /// <summary>The channel it went to.</summary>
    public GameChannel Channel { get; init; }

    /// <summary>The peer it went to.</summary>
    public NetPeer? Peer { get; init; }
}

/// <summary>A message type of the game's codec, as a C# struct of the same layout (its schema's size).</summary>
public interface IGameMessage
{
    /// <summary>The game's schema name (<c>ServerInfoMessage</c>, <c>ChatMessageServerMessage</c>).</summary>
    static abstract string SchemaName { get; }
}

/// <summary>One field of a game message schema (<see cref="Replication.MessageSchema"/>).</summary>
/// <param name="Name">The field's name.</param>
/// <param name="Type">The wire type code (0 bool, 1 float, 3..10 integers, 11 string, 12 struct, 13 enum, 14 GUID, 15 bits, 16 varint).</param>
/// <param name="Element">How one element is stored in the struct (the game's element code).</param>
/// <param name="Shape">0 optional, 1 inline, 2 a list with a count.</param>
/// <param name="Offset">Its byte offset in the struct.</param>
/// <param name="CountOffset">An optional field's present flag, or a list's u32 count.</param>
/// <param name="FixedCount">An inline array's length, or a list's capacity.</param>
public sealed record MessageField(string Name, int Type, int Element, int Shape, int Offset, int CountOffset, int FixedCount);

/// <summary>A game message schema: its struct's size and fields (<see cref="Replication.MessageSchema"/>).</summary>
public sealed record MessageSchema(string Name, int Size, IReadOnlyList<MessageField> Fields);

public static partial class Replication
{
    /// <summary>
    /// Hooks the game's messages on <paramref name="channel"/>: <paramref name="handler"/> sees every message this process
    /// sends and/or receives there and may pass, drop or rewrite it. The hooks go in while one exists.
    /// </summary>
    /// <returns>Dispose it to remove the hook; it also goes when your mod unloads.</returns>
    /// <exception cref="ArgumentException">Channels 4 and 5 (entities: use <see cref="Filter{TComponent}"/>), 12 (Kiln's own), 14, 17;
    /// no direction.</exception>
    /// <exception cref="KilnUnavailableException"><c>repl.messages</c> (that direction) is off.</exception>
    [TickThreadOnly]
    [RequiresCapability(ReplicationCaps.Messages)]
    public static unsafe IDisposable OnMessage(GameChannel channel, MessageDirection direction, MessageHandler handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        if (channel is GameChannel.EcsClient or GameChannel.EcsPrediction or GameChannel.Debug or GameChannel.VoiceChat or GameChannel.TwitchDrop
            || (byte)channel > 24)
            throw new ArgumentException($"channel {channel} cannot be hooked", nameof(channel));
        if (direction == 0 || (direction & ~(MessageDirection.Send | MessageDirection.Receive)) != 0)
            throw new ArgumentException("no direction", nameof(direction));
        var t = ReplNetNative.T;
        if (t == null || (t->Capabilities() & (ReplNetNative.CapMessagesSend | ReplNetNative.CapMessagesReceive)) == 0)
            throw ReplicationCaps.Off(ReplicationCaps.Messages, "kiln.repl.net has no message hooks here");
        var box = new MessageHost.Box { Handler = handler, Channel = channel };
        box.Self = GCHandle.Alloc(box);
        ulong h = 0;
        var rc = t->MessageHook(OwnerOfCaller(), (byte)channel, (uint)direction, &MessageHost.Trampoline, (void*)GCHandle.ToIntPtr(box.Self), &h);
        if (rc != ReplNative.Ok)
        {
            box.Self.Free();
            if (rc == ReplNative.Unavailable) throw ReplicationCaps.Off(ReplicationCaps.Messages, $"kiln.repl.net has no {direction} hooks here");
            if (rc == ReplNative.WrongThread) throw new InvalidOperationException("Replication.OnMessage: call it on the server's tick thread or the client's main thread");
            throw new ArgumentException($"kiln.repl.net refused the hook ({rc}): 64 hooks are in place", nameof(handler));
        }
        box.Handle = h;
        if (ModContext.CurrentMod is { } mod) mod.Retiring += box.Dispose;
        MessageHost.Track(box);
        return box;
    }

    /// <summary>
    /// Sends <paramref name="bytes"/> to <paramref name="peer"/> as a game message on <paramref name="channel"/>: the schema
    /// channels the peer's machine socket carries (<see cref="GameChannel.ServerInfo"/>, <see cref="GameChannel.CustomPlayerStrings"/>,
    /// <see cref="GameChannel.ChatMessages"/>, <see cref="GameChannel.BaseSharing"/>). Code the bytes with <see cref="Encode{T}"/>.
    /// </summary>
    /// <exception cref="ArgumentException">Another channel (<see cref="GameChannel.AdminInfo"/> goes on the player socket: not
    /// supported yet), empty or too long bytes.</exception>
    /// <exception cref="KilnUnavailableException"><c>repl.messages</c> is off.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(ReplicationCaps.Messages)]
    public static unsafe InjectResult Inject(NetPeer peer, GameChannel channel, ReadOnlySpan<byte> bytes)
    {
        Services.RequireTickThread("Replication.Inject");
        ArgumentNullException.ThrowIfNull(peer);
        if (channel is not (GameChannel.ServerInfo or GameChannel.CustomPlayerStrings or GameChannel.ChatMessages or GameChannel.BaseSharing))
            throw new ArgumentException($"channel {channel} cannot be injected", nameof(channel));
        if (bytes.Length is 0 or > 0x4000) throw new ArgumentException("1..16384 bytes", nameof(bytes));
        var t = ReplNetNative.T;
        if (t == null || (t->Capabilities() & ReplNetNative.CapMessagesSend) == 0)
            throw ReplicationCaps.Off(ReplicationCaps.Messages, "kiln.repl.net cannot send game messages here");
        int rc;
        fixed (byte* p = bytes) rc = t->MessageInject(peer.Id, (byte)channel, p, (uint)bytes.Length);
        var result = rc switch
        {
            ReplNative.Ok => new InjectResult(true, null),
            ReplNative.InvalidArgument => new InjectResult(false, "peer gone (not a connected game peer)"),
            ReplNative.Unavailable => new InjectResult(false, "the session refused it"),
            _ => new InjectResult(false, $"kiln.repl.net refused it ({rc})"),
        } with { Channel = channel, Peer = peer };
        Injected?.Invoke(result);
        return result;
    }

    /// <summary>Raised on the tick thread for every <see cref="Inject"/>, with its result.</summary>
    public static event Action<InjectResult>? Injected;

    /// <summary>Decodes a game message with the game's own codec.</summary>
    /// <exception cref="FormatException">The game's decoder refused the bytes.</exception>
    /// <exception cref="KilnUnavailableException"><c>repl.schema</c> is off, the schema is not in this game build, or it uses a type
    /// Kiln does not know, or <typeparamref name="T"/>'s size is not the schema's.</exception>
    [AnyThread]
    [RequiresCapability(ReplicationCaps.Schema)]
    public static unsafe T Decode<T>(ReadOnlySpan<byte> bytes) where T : unmanaged, IGameMessage
    {
        var schema = MessageHost.SchemaOf(T.SchemaName, sizeof(T), out var t);
        T value = default;
        int rc;
        fixed (byte* p = bytes) rc = t->SchemaDecode(schema, p, (uint)bytes.Length, &value, (uint)sizeof(T));
        if (rc == ReplNative.InvalidArgument) throw new FormatException($"the game's decoder refused {bytes.Length} bytes as {T.SchemaName}");
        if (rc != ReplNative.Ok) throw ReplicationCaps.Off(ReplicationCaps.Schema, $"kiln.repl.net could not decode {T.SchemaName} ({rc})");
        return value;
    }

    /// <summary>Encodes a game message with the game's own codec.</summary>
    /// <exception cref="FormatException">The game's encoder refused it (a list count over its capacity, too long).</exception>
    /// <exception cref="KilnUnavailableException">As <see cref="Decode{T}"/>.</exception>
    [AnyThread]
    [RequiresCapability(ReplicationCaps.Schema)]
    public static unsafe byte[] Encode<T>(in T message) where T : unmanaged, IGameMessage
    {
        var schema = MessageHost.SchemaOf(T.SchemaName, sizeof(T), out var t);
        var buf = new byte[0x4000];
        uint n = 0;
        int rc;
        fixed (T* m = &message)
        fixed (byte* p = buf)
            rc = t->SchemaEncode(schema, m, (uint)sizeof(T), p, (uint)buf.Length, &n);
        if (rc is ReplNative.InvalidArgument or ReplNative.BufferTooSmall) throw new FormatException($"the game's encoder refused this {T.SchemaName} ({rc})");
        if (rc != ReplNative.Ok) throw ReplicationCaps.Off(ReplicationCaps.Schema, $"kiln.repl.net could not encode {T.SchemaName} ({rc})");
        return buf.AsSpan(0, (int)n).ToArray();
    }

    /// <summary>A game message schema by name: its struct's size and fields, to write a struct for <see cref="Decode{T}"/>.</summary>
    /// <returns>Null when this game build has no such schema, or Kiln refuses it (a type it does not know).</returns>
    [AnyThread]
    public static unsafe MessageSchema? Schema(string name)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        var t = ReplNetNative.T;
        if (t == null || (t->Capabilities() & ReplNetNative.CapSchema) == 0) return null;
        var bytes = System.Text.Encoding.UTF8.GetBytes(name);
        uint size = 0;
        ulong schema = 0;
        int rc;
        fixed (byte* p = bytes) rc = t->SchemaFind(new NativeStr { Ptr = p, Len = (nuint)bytes.Length }, &size, &schema);
        if (rc != ReplNative.Ok) return null;
        uint count = 0;
        t->SchemaFields(schema, null, 0, &count);
        var fields = new ReplNetNative.SchemaField[count];
        fixed (ReplNetNative.SchemaField* f = fields) t->SchemaFields(schema, f, count, &count);
        var list = fields.Select(f => new MessageField(f.Name.Ptr == null ? "" : Marshal.PtrToStringUTF8((nint)f.Name.Ptr, (int)f.Name.Len) ?? "",
            (int)f.Type, (int)f.Element, (int)f.Shape, (int)f.Offset, (int)f.CountOffset, (int)f.FixedCount)).ToList();
        return new MessageSchema(name, (int)size, list);
    }
}

/// <summary>The managed side of the message hooks: their boxes and the trampoline the game's loops call.</summary>
internal static unsafe class MessageHost
{
    internal sealed class Box : IDisposable
    {
        public required MessageHandler Handler;
        public required GameChannel Channel;
        public ulong Handle;
        public GCHandle Self;
        public volatile Exception? Fault;
        public bool Disposed;

        public void Dispose()
        {
            lock (Gate)
            {
                if (Disposed) return;
                Disposed = true;
                if (Handle != 0) ReplNetNative.Remove(Handle);
                Live.Remove(this);
                Retiring.Add((this, Environment.TickCount64 + 2000));  // the chain skips a removed hook at once; free later
            }
        }
    }

    private static readonly object Gate = new();
    private static readonly List<Box> Live = [];
    private static readonly List<(Box Box, long At)> Retiring = [];
    private static IDisposable? _tick;

    internal static void Track(Box box)
    {
        lock (Gate)
        {
            Live.Add(box);
            _tick ??= ServiceTick.Add("kiln replication messages", static (in TickInfo _) => Tick());
        }
    }

    private static void Tick()
    {
        List<Box> faulted;
        lock (Gate)
        {
            var now = Environment.TickCount64;
            for (var i = Retiring.Count - 1; i >= 0; i--)
                if (Retiring[i].At <= now)
                {
                    if (Retiring[i].Box.Self.IsAllocated) Retiring[i].Box.Self.Free();
                    Retiring.RemoveAt(i);
                }
            faulted = Live.Where(b => b.Fault != null).ToList();
        }
        foreach (var b in faulted)
        {
            ReplNetLog.Warn($"repl: a message hook on channel {b.Channel} threw {b.Fault!.GetType().Name}: {b.Fault.Message}; it was removed");
            b.Dispose();
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    internal static int Trampoline(ReplNetNative.Message* m, void* user)
    {
        Box? box = null;
        try
        {
            box = (Box)GCHandle.FromIntPtr((nint)user).Target!;
            if (box.Disposed || box.Fault != null) return ReplNetNative.Pass;
            var rewrite = new MessageRewrite(m->Rewrite, (int)m->RewriteCap, (int)m->Length);
            var peer = m->Peer == 0 ? null : PeerOf(m->Peer);
            var action = box.Handler(peer, new ReadOnlySpan<byte>(m->Data, (int)m->Length), ref rewrite);
            if (action == MessageAction.Rewrite)
            {
                if (rewrite.Length < 0 || rewrite.Length > m->RewriteCap) return ReplNetNative.Pass;
                *m->RewriteLength = (uint)rewrite.Length;
                return ReplNetNative.Rewrite;
            }
            return action == MessageAction.Drop ? ReplNetNative.Drop : ReplNetNative.Pass;
        }
        catch (Exception e)
        {
            if (box != null) box.Fault ??= e;
            return ReplNetNative.Pass;
        }
    }

    private static NetPeer? PeerOf(uint id)
    {
        try
        {
            foreach (var p in Network.Peers)
                if (p.Id == id) return p;
        }
        catch (Exception)
        {
            // kiln.net not here
        }
        return null;
    }

    internal static ulong SchemaOf(string name, int size, out ReplNetNative.Table* t)
    {
        t = ReplNetNative.T;
        if (t == null || (t->Capabilities() & ReplNetNative.CapSchema) == 0)
            throw ReplicationCaps.Off(ReplicationCaps.Schema, "kiln.repl.net has no message codec here");
        var bytes = System.Text.Encoding.UTF8.GetBytes(name);
        uint found = 0;
        ulong schema = 0;
        int rc;
        fixed (byte* p = bytes) rc = t->SchemaFind(new NativeStr { Ptr = p, Len = (nuint)bytes.Length }, &found, &schema);
        if (rc != ReplNative.Ok)
            throw ReplicationCaps.Off(ReplicationCaps.Schema, rc == ReplNative.NotFound ? $"no message schema {name} in this game build" : $"Kiln does not code {name} (a type it does not know)");
        if (found != size) throw ReplicationCaps.Off(ReplicationCaps.Schema, $"{name} is {found} bytes in this game build, the struct {size}");
        return schema;
    }
}

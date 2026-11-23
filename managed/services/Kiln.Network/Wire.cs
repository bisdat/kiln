// SPDX-License-Identifier: MIT
// Binary codec for Kiln network messages (little-endian, varints, UTF-8 strings) and the frame layout of Kiln's
// protocol on top of kiln.net (docs/kiln/network.md).
using System.Buffers.Binary;
using System.Text;

namespace Kiln;

/// <summary>A message could not be decoded (truncated, oversized field, bad value).</summary>
public sealed class NetFormatException(string message) : Exception(message);

/// <summary>Writes a message body. Little-endian; strings are a varint byte length followed by UTF-8.</summary>
public sealed class NetWriter
{
    private byte[] _buf;
    private int _len;

    /// <summary>Creates a writer with room for <paramref name="capacity"/> bytes (at least 16); it grows as needed.</summary>
    /// <param name="capacity">The starting buffer size in bytes.</param>
    public NetWriter(int capacity = 256) => _buf = new byte[Math.Max(16, capacity)];

    /// <summary>Bytes written so far.</summary>
    public int Length => _len;

    /// <summary>The bytes written so far, as a view of the writer's own buffer. It is only valid until the next write.</summary>
    public ReadOnlySpan<byte> WrittenSpan => _buf.AsSpan(0, _len);
    /// <summary>Copies the bytes written so far into a new array.</summary>
    public byte[] ToArray() => _buf.AsSpan(0, _len).ToArray();
    /// <summary>Forgets everything written so far, so the writer can be reused. The buffer keeps its size.</summary>
    public void Clear() => _len = 0;

    private Span<byte> Grow(int n)
    {
        if (_len + n > _buf.Length) Array.Resize(ref _buf, Math.Max(_buf.Length * 2, _len + n));
        var s = _buf.AsSpan(_len, n);
        _len += n;
        return s;
    }

    /// <summary>Writes one byte.</summary>
    /// <param name="v">The value.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter Byte(byte v) { Grow(1)[0] = v; return this; }
    /// <summary>Writes a boolean as one byte: 1 for true, 0 for false.</summary>
    /// <param name="v">The value.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter Bool(bool v) => Byte(v ? (byte)1 : (byte)0);
    /// <summary>Writes an unsigned 16-bit integer (2 bytes, little-endian).</summary>
    /// <param name="v">The value.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter UInt16(ushort v) { BinaryPrimitives.WriteUInt16LittleEndian(Grow(2), v); return this; }
    /// <summary>Writes a signed 16-bit integer (2 bytes, little-endian).</summary>
    /// <param name="v">The value.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter Int16(short v) { BinaryPrimitives.WriteInt16LittleEndian(Grow(2), v); return this; }
    /// <summary>Writes an unsigned 32-bit integer (4 bytes, little-endian).</summary>
    /// <param name="v">The value.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter UInt32(uint v) { BinaryPrimitives.WriteUInt32LittleEndian(Grow(4), v); return this; }
    /// <summary>Writes a signed 32-bit integer (4 bytes, little-endian).</summary>
    /// <param name="v">The value.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter Int32(int v) { BinaryPrimitives.WriteInt32LittleEndian(Grow(4), v); return this; }
    /// <summary>Writes an unsigned 64-bit integer (8 bytes, little-endian).</summary>
    /// <param name="v">The value.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter UInt64(ulong v) { BinaryPrimitives.WriteUInt64LittleEndian(Grow(8), v); return this; }
    /// <summary>Writes a signed 64-bit integer (8 bytes, little-endian).</summary>
    /// <param name="v">The value.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter Int64(long v) { BinaryPrimitives.WriteInt64LittleEndian(Grow(8), v); return this; }
    /// <summary>Writes a 32-bit float (4 bytes, little-endian).</summary>
    /// <param name="v">The value.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter Single(float v) { BinaryPrimitives.WriteSingleLittleEndian(Grow(4), v); return this; }
    /// <summary>Writes a 64-bit float (8 bytes, little-endian).</summary>
    /// <param name="v">The value.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter Double(double v) { BinaryPrimitives.WriteDoubleLittleEndian(Grow(8), v); return this; }

    /// <summary>Unsigned LEB128 (1 byte below 128).</summary>
    public NetWriter VarUInt(ulong v)
    {
        while (v >= 0x80)
        {
            Byte((byte)(v | 0x80));
            v >>= 7;
        }
        return Byte((byte)v);
    }

    /// <summary>Zig-zag signed varint.</summary>
    public NetWriter VarInt(long v) => VarUInt((ulong)((v << 1) ^ (v >> 63)));

    /// <summary>Writes a string: a varint byte count followed by the UTF-8 bytes. Read it back with
    /// <see cref="NetReader.String"/>.</summary>
    /// <param name="v">The text; null is written as an empty string.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter String(string? v)
    {
        v ??= "";
        var n = Encoding.UTF8.GetByteCount(v);
        VarUInt((ulong)n);
        Encoding.UTF8.GetBytes(v, Grow(n));
        return this;
    }

    /// <summary>Length-prefixed bytes.</summary>
    public NetWriter Bytes(ReadOnlySpan<byte> v)
    {
        VarUInt((ulong)v.Length);
        v.CopyTo(Grow(v.Length));
        return this;
    }

    /// <summary>Bytes without a length (the reader must know the size).</summary>
    public NetWriter Raw(ReadOnlySpan<byte> v)
    {
        v.CopyTo(Grow(v.Length));
        return this;
    }

    /// <summary>Writes a GUID as its 16 bytes.</summary>
    /// <param name="v">The value.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter Guid(Guid v)
    {
        v.TryWriteBytes(Grow(16));
        return this;
    }

    /// <summary>Writes a world position as three 64-bit floats (X, Y, Z; 24 bytes). Read it back with
    /// <see cref="NetReader.Position3"/>.</summary>
    /// <param name="p">The position.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter Position(WorldPosition p) => Double(p.X).Double(p.Y).Double(p.Z);
    /// <summary>Writes an entity id as its 32-bit value. Read it back with <see cref="NetReader.Entity"/>.</summary>
    /// <param name="e">The entity id.</param>
    /// <returns>This writer, so calls can be chained.</returns>
    public NetWriter Entity(EntityId e) => UInt32(e.Value);
}

/// <summary>Reads a message body written by <see cref="NetWriter"/>. Every read checks bounds and throws
/// <see cref="NetFormatException"/> on truncated input.</summary>
public ref struct NetReader
{
    private readonly ReadOnlySpan<byte> _data;
    private int _pos;

    /// <summary>Creates a reader over <paramref name="data"/>, positioned at its start. The reader only views the span; it
    /// copies nothing.</summary>
    /// <param name="data">The message body to read.</param>
    public NetReader(ReadOnlySpan<byte> data)
    {
        _data = data;
        _pos = 0;
    }

    /// <summary>Strings longer than this (bytes) are refused.</summary>
    public const int MaxString = 1 << 20;

    /// <summary>How many bytes have been read so far (the offset of the next read).</summary>
    public int Position => _pos;
    /// <summary>How many bytes are left to read.</summary>
    public int Remaining => _data.Length - _pos;
    /// <summary>True when every byte has been read.</summary>
    public bool End => _pos >= _data.Length;

    private ReadOnlySpan<byte> Take(int n)
    {
        if (n < 0 || _pos + n > _data.Length) throw new NetFormatException($"truncated message (need {n} bytes at {_pos}, have {_data.Length - _pos})");
        var s = _data.Slice(_pos, n);
        _pos += n;
        return s;
    }

    /// <summary>Reads one byte.</summary>
    /// <exception cref="NetFormatException">The message is truncated.</exception>
    public byte Byte() => Take(1)[0];
    /// <summary>Reads a boolean (one byte; any non-zero value is true).</summary>
    /// <exception cref="NetFormatException">The message is truncated.</exception>
    public bool Bool() => Byte() != 0;
    /// <summary>Reads an unsigned 16-bit integer (2 bytes, little-endian).</summary>
    /// <exception cref="NetFormatException">The message is truncated.</exception>
    public ushort UInt16() => BinaryPrimitives.ReadUInt16LittleEndian(Take(2));
    /// <summary>Reads a signed 16-bit integer (2 bytes, little-endian).</summary>
    /// <exception cref="NetFormatException">The message is truncated.</exception>
    public short Int16() => BinaryPrimitives.ReadInt16LittleEndian(Take(2));
    /// <summary>Reads an unsigned 32-bit integer (4 bytes, little-endian).</summary>
    /// <exception cref="NetFormatException">The message is truncated.</exception>
    public uint UInt32() => BinaryPrimitives.ReadUInt32LittleEndian(Take(4));
    /// <summary>Reads a signed 32-bit integer (4 bytes, little-endian).</summary>
    /// <exception cref="NetFormatException">The message is truncated.</exception>
    public int Int32() => BinaryPrimitives.ReadInt32LittleEndian(Take(4));
    /// <summary>Reads an unsigned 64-bit integer (8 bytes, little-endian).</summary>
    /// <exception cref="NetFormatException">The message is truncated.</exception>
    public ulong UInt64() => BinaryPrimitives.ReadUInt64LittleEndian(Take(8));
    /// <summary>Reads a signed 64-bit integer (8 bytes, little-endian).</summary>
    /// <exception cref="NetFormatException">The message is truncated.</exception>
    public long Int64() => BinaryPrimitives.ReadInt64LittleEndian(Take(8));
    /// <summary>Reads a 32-bit float (4 bytes, little-endian).</summary>
    /// <exception cref="NetFormatException">The message is truncated.</exception>
    public float Single() => BinaryPrimitives.ReadSingleLittleEndian(Take(4));
    /// <summary>Reads a 64-bit float (8 bytes, little-endian).</summary>
    /// <exception cref="NetFormatException">The message is truncated.</exception>
    public double Double() => BinaryPrimitives.ReadDoubleLittleEndian(Take(8));

    /// <summary>Reads an unsigned LEB128 varint, as written by <see cref="NetWriter.VarUInt"/>.</summary>
    /// <exception cref="NetFormatException">The message is truncated, or the varint is longer than 64 bits.</exception>
    public ulong VarUInt()
    {
        ulong v = 0;
        for (var shift = 0; shift < 64; shift += 7)
        {
            var b = Byte();
            v |= (ulong)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return v;
        }
        throw new NetFormatException("varint too long");
    }

    /// <summary>Reads a zig-zag signed varint, as written by <see cref="NetWriter.VarInt"/>.</summary>
    /// <exception cref="NetFormatException">The message is truncated, or the varint is longer than 64 bits.</exception>
    public long VarInt()
    {
        var u = VarUInt();
        return (long)(u >> 1) ^ -(long)(u & 1);
    }

    /// <summary>Reads a string written by <see cref="NetWriter.String"/>: a varint byte count, then UTF-8.</summary>
    /// <param name="maxBytes">The longest string to accept, in bytes. It never raises the limit above <see cref="MaxString"/>.</param>
    /// <returns>The text.</returns>
    /// <exception cref="NetFormatException">The message is truncated, or the string is longer than the limit.</exception>
    public string String(int maxBytes = MaxString)
    {
        var n = VarUInt();
        if (n > (ulong)Math.Min(maxBytes, MaxString)) throw new NetFormatException($"string of {n} bytes exceeds {maxBytes}");
        return Encoding.UTF8.GetString(Take((int)n));
    }

    /// <summary>Reads a length-prefixed byte array, as written by <see cref="NetWriter.Bytes"/>.</summary>
    /// <param name="maxBytes">The longest array to accept, in bytes (default 16 MiB).</param>
    /// <returns>A new array with a copy of the bytes.</returns>
    /// <exception cref="NetFormatException">The message is truncated, or the array is longer than the limit.</exception>
    public byte[] Bytes(int maxBytes = 1 << 24)
    {
        var n = VarUInt();
        if (n > (ulong)maxBytes) throw new NetFormatException($"byte array of {n} exceeds {maxBytes}");
        return Take((int)n).ToArray();
    }

    /// <summary>Reads <paramref name="n"/> bytes with no length prefix (the counterpart of <see cref="NetWriter.Raw"/>).</summary>
    /// <param name="n">How many bytes to take.</param>
    /// <returns>A view of those bytes in the message; no copy is made.</returns>
    /// <exception cref="NetFormatException">The message has fewer than <paramref name="n"/> bytes left.</exception>
    public ReadOnlySpan<byte> Raw(int n) => Take(n);
    /// <summary>Reads everything that is left. After this call <see cref="End"/> is true.</summary>
    /// <returns>A view of the remaining bytes in the message; no copy is made.</returns>
    public ReadOnlySpan<byte> Rest() => Take(Remaining);
    /// <summary>Reads a GUID (16 bytes).</summary>
    /// <exception cref="NetFormatException">The message is truncated.</exception>
    public Guid Guid() => new(Take(16));
    /// <summary>Reads a world position written by <see cref="NetWriter.Position"/>: three 64-bit floats (X, Y, Z).</summary>
    /// <exception cref="NetFormatException">The message is truncated.</exception>
    public WorldPosition Position3() => new(Double(), Double(), Double());
    /// <summary>Reads an entity id (a 32-bit value) written by <see cref="NetWriter.Entity"/>.</summary>
    /// <exception cref="NetFormatException">The message is truncated.</exception>
    public EntityId Entity() => new(UInt32());
}

/// <summary>Kiln protocol frames (one kiln.net frame each).</summary>
internal static class Wire
{
    public const byte Magic0 = (byte)'K', Magic1 = (byte)'N';
    public const byte Version = 1;
    public const ushort Protocol = 1;
    public const int HeaderSize = 4;

    public enum Type : byte
    {
        Hello = 1,      // client -> server: Kiln version, mods, channels, handshake properties
        Welcome = 2,    // server -> client: accepted; server mods, channel table, limits, warnings
        Reject = 3,     // server -> client: refused (missing mods ...); the server kicks shortly after
        Data = 4,       // u16 channel id, u16 message id, body
        Fragment = 5,   // u16 channel id, u32 message seq, u16 index, u16 count, [index 0: u32 total], chunk
        Ping = 6,       // u32 nonce, u64 sender time ms
        Pong = 7,       // u32 nonce, u64 echoed time ms
        Bye = 8,        // string reason
        Channels = 9,   // server -> client: channel table update (after a mod (un)loaded)
        Knock = 10,     // client -> server, no body: this client has Kiln (kiln.net sends and swallows it; kiln_net.h)
    }

    public static NetWriter Begin(Type t, int capacity = 64)
    {
        var w = new NetWriter(capacity);
        w.Byte(Magic0).Byte(Magic1).Byte(Version).Byte((byte)t);
        return w;
    }

    /// <summary>Validates the header; false for frames that are not Kiln's.</summary>
    public static bool TryHeader(ReadOnlySpan<byte> frame, out Type type)
    {
        type = 0;
        if (frame.Length < HeaderSize || frame[0] != Magic0 || frame[1] != Magic1 || frame[2] != Version) return false;
        type = (Type)frame[3];
        return true;
    }

    public sealed record ModEntry(string Id, string Version, ModSide Side);

    public sealed record ChannelEntry(ushort Id, string Name, string Version, bool Required);

    public sealed record Hello(ushort Protocol, string KilnVersion, uint Nonce, IReadOnlyList<ModEntry> Mods,
        IReadOnlyList<ChannelEntry> Channels, IReadOnlyDictionary<string, string> Properties);

    public sealed record Welcome(ushort Protocol, string KilnVersion, uint Nonce, string Policy, uint MaxMessage,
        IReadOnlyList<ModEntry> Mods, IReadOnlyList<ChannelEntry> Channels, IReadOnlyList<string> Warnings,
        IReadOnlyDictionary<string, string> Properties);

    /// <summary>REJECT. <see cref="Properties"/> trail the details (added in Kiln 0.2: server id/name, modpack,
    /// required mods); decoders before 0.2 stop after the details and never see them.</summary>
    public sealed record Reject(string Reason, IReadOnlyList<string> Details)
    {
        public IReadOnlyDictionary<string, string> Properties { get; init; } = new Dictionary<string, string>();
    }

    public const int MaxListItems = 4096;

    private static void WriteMods(NetWriter w, IReadOnlyList<ModEntry> mods)
    {
        w.VarUInt((ulong)mods.Count);
        foreach (var m in mods) w.String(m.Id).String(m.Version).Byte((byte)m.Side);
    }

    private static List<ModEntry> ReadMods(ref NetReader r)
    {
        var n = (int)Math.Min(r.VarUInt(), MaxListItems + 1UL);
        if (n > MaxListItems) throw new NetFormatException("too many mods");
        var list = new List<ModEntry>(n);
        for (var i = 0; i < n; i++)
        {
            var id = r.String(64);
            var ver = r.String(64);
            var side = r.Byte();
            list.Add(new ModEntry(id, ver, side <= 2 ? (ModSide)side : ModSide.Server));
        }
        return list;
    }

    public static void WriteChannels(NetWriter w, IReadOnlyList<ChannelEntry> chans)
    {
        w.VarUInt((ulong)chans.Count);
        foreach (var c in chans) w.UInt16(c.Id).String(c.Name).String(c.Version).Bool(c.Required);
    }

    public static List<ChannelEntry> ReadChannels(ref NetReader r)
    {
        var n = (int)Math.Min(r.VarUInt(), MaxListItems + 1UL);
        if (n > MaxListItems) throw new NetFormatException("too many channels");
        var list = new List<ChannelEntry>(n);
        for (var i = 0; i < n; i++) list.Add(new ChannelEntry(r.UInt16(), r.String(128), r.String(64), r.Bool()));
        return list;
    }

    private static void WriteProps(NetWriter w, IReadOnlyDictionary<string, string> p)
    {
        w.VarUInt((ulong)p.Count);
        foreach (var (k, v) in p) w.String(k).String(v);
    }

    private static Dictionary<string, string> ReadProps(ref NetReader r)
    {
        var n = (int)Math.Min(r.VarUInt(), 257UL);
        if (n > 256) throw new NetFormatException("too many properties");
        var d = new Dictionary<string, string>(n, StringComparer.Ordinal);
        for (var i = 0; i < n; i++) d[r.String(128)] = r.String(4096);
        return d;
    }

    public static byte[] EncodeHello(Hello h)
    {
        var w = Begin(Type.Hello, 512);
        w.UInt16(h.Protocol).String(h.KilnVersion).UInt32(h.Nonce);
        WriteMods(w, h.Mods);
        WriteChannels(w, h.Channels);
        WriteProps(w, h.Properties);
        return w.ToArray();
    }

    public static Hello DecodeHello(ReadOnlySpan<byte> frame)
    {
        var r = new NetReader(frame[HeaderSize..]);
        var proto = r.UInt16();
        var ver = r.String(64);
        var nonce = r.UInt32();
        var mods = ReadMods(ref r);
        var chans = ReadChannels(ref r);
        var props = r.End ? new Dictionary<string, string>() : ReadProps(ref r);
        return new Hello(proto, ver, nonce, mods, chans, props);
    }

    public static byte[] EncodeWelcome(Welcome h)
    {
        var w = Begin(Type.Welcome, 512);
        w.UInt16(h.Protocol).String(h.KilnVersion).UInt32(h.Nonce).String(h.Policy).UInt32(h.MaxMessage);
        WriteMods(w, h.Mods);
        WriteChannels(w, h.Channels);
        w.VarUInt((ulong)h.Warnings.Count);
        foreach (var s in h.Warnings) w.String(s);
        WriteProps(w, h.Properties);
        return w.ToArray();
    }

    public static Welcome DecodeWelcome(ReadOnlySpan<byte> frame)
    {
        var r = new NetReader(frame[HeaderSize..]);
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
        return new Welcome(proto, ver, nonce, policy, max, mods, chans, warns, props);
    }

    public static byte[] EncodeReject(Reject j)
    {
        var w = Begin(Type.Reject, 256);
        w.String(j.Reason).VarUInt((ulong)j.Details.Count);
        foreach (var d in j.Details) w.String(d);
        if (j.Properties.Count > 0) WriteProps(w, j.Properties);  // trailing: old decoders stop before it
        return w.ToArray();
    }

    public static Reject DecodeReject(ReadOnlySpan<byte> frame)
    {
        var r = new NetReader(frame[HeaderSize..]);
        var reason = r.String(2048);
        var n = (int)Math.Min(r.VarUInt(), 257UL);
        var list = new List<string>();
        for (var i = 0; i < n && i < 256; i++) list.Add(r.String(2048));
        Dictionary<string, string> props;
        try
        {
            // absent (an old server) or zero padding (the game transport pads frames to 4 bytes): no properties
            props = r.End ? new Dictionary<string, string>() : ReadProps(ref r);
        }
        catch (NetFormatException)
        {
            props = new Dictionary<string, string>();
        }
        return new Reject(reason, list) { Properties = props };
    }

    /// <summary>The REJECT decoder of Kiln 0.1 (reason + details, nothing after): kept to test that new frames still
    /// decode on old clients.</summary>
    internal static (string Reason, List<string> Details) DecodeRejectV1(ReadOnlySpan<byte> frame)
    {
        var r = new NetReader(frame[HeaderSize..]);
        var reason = r.String(2048);
        var n = (int)Math.Min(r.VarUInt(), 257UL);
        var list = new List<string>();
        for (var i = 0; i < n && i < 256; i++) list.Add(r.String(2048));
        return (reason, list);
    }

    /// <summary>Bytes the properties block of <paramref name="p"/> takes (count varint + pairs).</summary>
    internal static int PropsSize(IReadOnlyDictionary<string, string> p)
    {
        var w = new NetWriter(64);
        WriteProps(w, p);
        return w.Length;
    }
}

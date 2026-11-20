// SPDX-License-Identifier: MIT
// Kiln's channel kiln:repl v1 (DESIGN "Wire"; the dossier's chapter 15 §4 M2): the server's extension definitions and
// values for Kiln clients, and a client's request to send values again. NetWriter encoding, explicit message ids; an
// unknown id is dropped by kiln.net; a v2 adds ids and never changes these.
//   0 DEFS    S->C  varint n, n x [u16 extId, str name, u16 version, u16 size, u32 layoutHash]           n <= 1,024
//   1 UPDATE  S->C  u32 serverTick, varint n, n x [u32 entity, u32 createTick, u16 extId, size bytes]   n <= 4,096
//   2 CLEAR   S->C  u32 serverTick, varint n, n x [u32 entity, u32 createTick, u16 extId]               n <= 4,096
//   3 RESYNC  C->S  varint n, n x [u32 entity, u32 createTick]                                         n <= 256, 1/s
// createTick is the entity's generation for that peer: the server tick of the create message the peer got (0: the
// entity was created before the server began tracking; the client applies it to the generation it has).
namespace Kiln;

internal static class SidecarWire
{
    public const string Channel = "kiln:repl", ChannelVersion = "1";
    public const ushort Defs = 0, Update = 1, Clear = 2, Resync = 3;
    public const int MaxDefs = 1024, MaxItems = 4096, MaxResync = 256, MaxName = 64;

    /// <summary>One extension as the server numbers it.</summary>
    public readonly record struct Def(ushort Id, string Name, ushort Version, ushort Size, uint LayoutHash);

    /// <summary>A value of one extension on one generation of an entity.</summary>
    public readonly record struct Value(uint Entity, uint CreateTick, ushort ExtId, ReadOnlyMemory<byte> Bytes);

    /// <summary>What a CLEAR names.</summary>
    public readonly record struct Key(uint Entity, uint CreateTick, ushort ExtId);

    /// <summary>What a RESYNC asks for: every value of that generation.</summary>
    public readonly record struct ResyncItem(uint Entity, uint CreateTick);

    /// <summary>Bytes one UPDATE item takes for a value of <paramref name="size"/> bytes.</summary>
    public static int UpdateItemBytes(int size) => 4 + 4 + 2 + size;

    /// <summary>Bytes one CLEAR item takes.</summary>
    public const int ClearItemBytes = 10;

    /// <summary>The fixed part of an UPDATE or CLEAR (tick and a varint count of up to 4,096).</summary>
    public const int Header = 4 + 2;

    public static void WriteDefs(NetWriter w, IReadOnlyList<Def> defs)
    {
        if (defs.Count > MaxDefs) throw new ArgumentException($"DEFS of {defs.Count} extensions (at most {MaxDefs})", nameof(defs));
        w.VarUInt((ulong)defs.Count);
        foreach (var d in defs) w.UInt16(d.Id).String(d.Name).UInt16(d.Version).UInt16(d.Size).UInt32(d.LayoutHash);
    }

    public static List<Def> ReadDefs(ref NetReader r)
    {
        var n = Count(ref r, MaxDefs, "DEFS");
        var list = new List<Def>(n);
        for (var i = 0; i < n; i++)
        {
            var d = new Def(r.UInt16(), r.String(MaxName * 4), r.UInt16(), r.UInt16(), r.UInt32());
            if (d.Size is 0 or > LayoutHash.MaxSize) throw new NetFormatException($"extension {d.Name}: size {d.Size}");
            list.Add(d);
        }
        return list;
    }

    public static void WriteUpdate(NetWriter w, uint tick, IReadOnlyList<Value> items)
    {
        if (items.Count > MaxItems) throw new ArgumentException($"UPDATE of {items.Count} values (at most {MaxItems})", nameof(items));
        w.UInt32(tick).VarUInt((ulong)items.Count);
        foreach (var v in items) w.UInt32(v.Entity).UInt32(v.CreateTick).UInt16(v.ExtId).Raw(v.Bytes.Span);
    }

    /// <summary>An UPDATE; <paramref name="sizeOf"/> gives an id's value size (-1: unknown, the message is refused).</summary>
    public static (uint Tick, List<Value> Items) ReadUpdate(ref NetReader r, Func<ushort, int> sizeOf)
    {
        var tick = r.UInt32();
        var n = Count(ref r, MaxItems, "UPDATE");
        var list = new List<Value>(n);
        for (var i = 0; i < n; i++)
        {
            uint entity = r.UInt32(), createTick = r.UInt32();
            var id = r.UInt16();
            var size = sizeOf(id);
            if (size <= 0) throw new NetFormatException($"UPDATE names extension id {id}, which DEFS did not define");
            list.Add(new Value(entity, createTick, id, r.Raw(size).ToArray()));
        }
        return (tick, list);
    }

    public static void WriteClear(NetWriter w, uint tick, IReadOnlyList<Key> keys)
    {
        if (keys.Count > MaxItems) throw new ArgumentException($"CLEAR of {keys.Count} values (at most {MaxItems})", nameof(keys));
        w.UInt32(tick).VarUInt((ulong)keys.Count);
        foreach (var k in keys) w.UInt32(k.Entity).UInt32(k.CreateTick).UInt16(k.ExtId);
    }

    public static (uint Tick, List<Key> Keys) ReadClear(ref NetReader r)
    {
        var tick = r.UInt32();
        var n = Count(ref r, MaxItems, "CLEAR");
        var list = new List<Key>(n);
        for (var i = 0; i < n; i++) list.Add(new Key(r.UInt32(), r.UInt32(), r.UInt16()));
        return (tick, list);
    }

    public static void WriteResync(NetWriter w, IReadOnlyList<ResyncItem> items)
    {
        if (items.Count > MaxResync) throw new ArgumentException($"RESYNC of {items.Count} entities (at most {MaxResync})", nameof(items));
        w.VarUInt((ulong)items.Count);
        foreach (var i in items) w.UInt32(i.Entity).UInt32(i.CreateTick);
    }

    public static List<ResyncItem> ReadResync(ref NetReader r)
    {
        var n = Count(ref r, MaxResync, "RESYNC");
        var list = new List<ResyncItem>(n);
        for (var i = 0; i < n; i++) list.Add(new ResyncItem(r.UInt32(), r.UInt32()));
        return list;
    }

    private static int Count(ref NetReader r, int max, string what)
    {
        var n = r.VarUInt();
        if (n > (ulong)max) throw new NetFormatException($"{what} of {n} items (at most {max})");
        return (int)n;
    }
}

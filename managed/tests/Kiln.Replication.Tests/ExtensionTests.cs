// SPDX-License-Identifier: MIT
// Sidecar definitions (Replication.Extension): what a value type may be, its layout hash, names, versions and ids; the
// kiln:repl v1 codecs at their limits (DEFS, UPDATE, CLEAR, RESYNC). No game, no harness.
using System.Runtime.InteropServices;
using Kiln;
using Xunit;

namespace ReplicationTests;

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Water
{
    public uint Charges;
    public uint Max;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct WaterSwapped
{
    public uint Max;
    public uint Charges;
}

public enum Mode : ushort { Off, On }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Inner
{
    public short X;
    public double Y;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Outer
{
    public byte A;
    public Inner In;
    public Mode M;
    public float F;
    public sbyte S;
    public long L;
    public ulong U;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct WithEnum
{
    public Mode M;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct WithUShort
{
    public ushort M;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct HasBool { public bool B; }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct HasChar { public char C; }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct HasNint { public nint P; }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public unsafe struct HasPointer { public int* P; }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public unsafe struct HasFixed { public fixed byte B[4]; }

public struct DefaultPack { public byte A; public int B; }

[StructLayout(LayoutKind.Explicit, Pack = 1)]
public struct ExplicitLayout { [FieldOffset(0)] public int A; }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Empty { }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct NestedBad { public DefaultPack D; }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Big { public Kb A; public byte One; }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Kb { public Half512 A, B; }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Half512 { public Q64 A, B, C, D, E, F, G, H; }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Q64 { public ulong A, B, C, D, E, F, G, H; }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Exactly1024 { public Kb A; }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct HasHalf { public Half H; }

[Collection("sidecars")]
public sealed class ExtensionTests
{
    private static uint Fnv(string s)
    {
        var h = 2166136261u;
        foreach (var b in System.Text.Encoding.UTF8.GetBytes(s)) h = (h ^ b) * 16777619u;
        return h;
    }

    // ---- the layout hash ----

    [Fact]
    public void The_hash_is_FNV_over_path_type_and_offset_of_every_leaf()
    {
        var (size, hash) = LayoutHash.Of<Water>();
        Assert.Equal(8, size);
        Assert.Equal(Fnv("Charges:u32@0;Max:u32@4;"), hash);
        Assert.Equal(["A:u8@0;", "In.X:i16@1;", "In.Y:f64@3;", "M:u16@11;", "F:f32@13;", "S:i8@17;", "L:i64@18;", "U:u64@26;"],
            LayoutHash.LeafLines(typeof(Outer), 34));
        Assert.Equal(34, LayoutHash.Of<Outer>().Size);
    }

    [Fact]
    public void Field_order_and_names_change_the_hash_an_enum_is_its_underlying_type()
    {
        Assert.NotEqual(LayoutHash.Of<Water>().Hash, LayoutHash.Of<WaterSwapped>().Hash);  // same size, other order
        Assert.Equal(LayoutHash.Of<WithEnum>().Hash, LayoutHash.Of<WithUShort>().Hash);    // the same bytes travel
    }

    [Theory]
    [InlineData(typeof(HasBool), "field B is bool")]
    [InlineData(typeof(HasChar), "field C is char")]
    [InlineData(typeof(HasNint), "field P is nint")]
    [InlineData(typeof(HasPointer), "field P is a pointer")]
    [InlineData(typeof(HasFixed), "field B is a fixed buffer")]
    [InlineData(typeof(DefaultPack), "needs [StructLayout(LayoutKind.Sequential, Pack = 1)]")]
    [InlineData(typeof(ExplicitLayout), "needs [StructLayout(LayoutKind.Sequential, Pack = 1)]")]
    [InlineData(typeof(NestedBad), "DefaultPack (field D) needs")]
    [InlineData(typeof(Empty), "has no fields")]
    [InlineData(typeof(Big), "is 1025 bytes; an extension value is at most 1024")]
    [InlineData(typeof(HasHalf), "field H is Half")]
    [InlineData(typeof(int), "is not a struct")]
    public void A_value_type_outside_the_rule_is_refused(Type t, string why)
    {
        var size = (int)typeof(System.Runtime.CompilerServices.Unsafe).GetMethod("SizeOf")!.MakeGenericMethod(t).Invoke(null, null)!;
        var e = Assert.Throws<ArgumentException>(() => LayoutHash.LeafLines(t, size));
        Assert.Contains(why, e.Message);
        Assert.Equal("T", e.ParamName);
    }

    [Fact]
    public void Exactly_1024_bytes_is_allowed()
    {
        Assert.Equal(1024, LayoutHash.Of<Exactly1024>().Size);
    }

    // ---- registration ----

    [Fact]
    public void Registration_checks_name_and_version_and_gives_ids_in_order()
    {
        using var a = Replication.Extension<Water>("test:a", 1);
        using var b = Replication.Extension<Outer>("test:b", 3);
        Assert.Equal(("test:a", 1, 8), (a.Name, a.Version, a.Size));
        Assert.Equal(LayoutHash.Of<Water>().Hash, a.LayoutHash);
        Assert.True(b.Id > a.Id);
        Assert.True(a.IsRegistered);
        var taken = Assert.Throws<ArgumentException>(() => Replication.Extension<Water>("test:a", 2));
        Assert.Contains("extension name test:a is taken by", taken.Message);
        Assert.Throws<ArgumentException>(() => Replication.Extension<Water>("test:c", 0));
        Assert.Throws<ArgumentException>(() => Replication.Extension<Water>("test:c", 65536));
        Assert.Throws<ArgumentException>(() => Replication.Extension<Water>("", 1));
        Assert.Throws<ArgumentException>(() => Replication.Extension<Water>("has space", 1));
        Assert.Throws<ArgumentException>(() => Replication.Extension<Water>("semi;colon", 1));
        Assert.Throws<ArgumentException>(() => Replication.Extension<Water>("at@sign", 1));
        Assert.Throws<ArgumentException>(() => Replication.Extension<Water>(new string('x', 65), 1));
        Assert.Throws<ArgumentException>(() => Replication.Extension<HasBool>("test:bool", 1));
        // a disposed extension frees its name; the next registration gets a new id
        var oldId = a.Id;
        a.Dispose();
        Assert.False(a.IsRegistered);
        using var again = Replication.Extension<Water>("test:a", 1);
        Assert.True(again.Id > b.Id && again.Id != oldId);
        Assert.Contains(again, Sidecars.Registered());
        Assert.DoesNotContain(a, Sidecars.Registered());
    }

    [Fact]
    public void The_client_advertises_its_registered_extensions()
    {
        using var w = Replication.Extension<Water>("test:adv", 2);
        var value = Sidecars.AdvertisedValue();
        Assert.Contains(ExtensionAdvert.Format("test:adv", 2, 8, LayoutHash.Of<Water>().Hash), value.Split(';'));
    }

    // ---- the wire (kiln:repl v1) ----

    private static byte[] Bytes(Action<NetWriter> write)
    {
        var w = new NetWriter();
        write(w);
        return w.ToArray();
    }

    [Fact]
    public void Defs_round_trip_at_the_limit()
    {
        var defs = Enumerable.Range(0, SidecarWire.MaxDefs)
            .Select(i => new SidecarWire.Def((ushort)(i + 1), $"mod{i}:ext.{new string('n', 50)}", (ushort)(i % 7 + 1), (ushort)(i % 1024 + 1), (uint)i * 2654435761u))
            .ToList();
        var body = Bytes(w => SidecarWire.WriteDefs(w, defs));
        var r = new NetReader(body);
        Assert.Equal(defs, SidecarWire.ReadDefs(ref r));
        Assert.True(r.End);
        Assert.Throws<ArgumentException>(() => Bytes(w => SidecarWire.WriteDefs(w, [.. defs, defs[0]])));
        // a peer that sends more than the limit, or a size over 1,024: refused
        var over = Bytes(w => { w.VarUInt(SidecarWire.MaxDefs + 1); });
        Assert.Throws<NetFormatException>(() => { var rr = new NetReader(over); SidecarWire.ReadDefs(ref rr); });
        var big = Bytes(w => { w.VarUInt(1); w.UInt16(1).String("a:b").UInt16(1).UInt16(1025).UInt32(0); });
        Assert.Throws<NetFormatException>(() => { var rr = new NetReader(big); SidecarWire.ReadDefs(ref rr); });
    }

    [Fact]
    public void Update_round_trips_at_the_limit_and_needs_known_ids()
    {
        var sizes = new Dictionary<ushort, int> { [1] = 8, [2] = 1024 };
        var items = Enumerable.Range(0, SidecarWire.MaxItems).Select(i =>
        {
            var ext = (ushort)(i % 2 + 1);
            var v = new byte[sizes[ext]];
            v[0] = (byte)i;
            v[^1] = (byte)(i >> 8);
            return new SidecarWire.Value((uint)(1024 + i), (uint)(5000 + i), ext, v);
        }).ToList();
        var body = Bytes(w => SidecarWire.WriteUpdate(w, 123456, items));
        var r = new NetReader(body);
        var (tick, got) = SidecarWire.ReadUpdate(ref r, id => sizes.TryGetValue(id, out var s) ? s : -1);
        Assert.Equal(123456u, tick);
        Assert.Equal(items.Count, got.Count);
        for (var i = 0; i < items.Count; i++)
        {
            Assert.Equal((items[i].Entity, items[i].CreateTick, items[i].ExtId), (got[i].Entity, got[i].CreateTick, got[i].ExtId));
            Assert.True(items[i].Bytes.Span.SequenceEqual(got[i].Bytes.Span));
        }
        Assert.Throws<ArgumentException>(() => Bytes(w => SidecarWire.WriteUpdate(w, 1, [.. items, items[0]])));
        // an id the reader does not know: the message cannot be read past it
        var unknown = Bytes(w => SidecarWire.WriteUpdate(w, 1, [new SidecarWire.Value(5, 6, 9, new byte[4])]));
        Assert.Throws<NetFormatException>(() => { var rr = new NetReader(unknown); SidecarWire.ReadUpdate(ref rr, id => sizes.TryGetValue(id, out var s) ? s : -1); });
        // cut short
        var cut = body[..^1];
        Assert.Throws<NetFormatException>(() => { var rr = new NetReader(cut); SidecarWire.ReadUpdate(ref rr, id => sizes.TryGetValue(id, out var s) ? s : -1); });
        Assert.Equal(10 + 8, SidecarWire.UpdateItemBytes(8));
    }

    [Fact]
    public void Clear_and_Resync_round_trip_at_their_limits()
    {
        var keys = Enumerable.Range(0, SidecarWire.MaxItems).Select(i => new SidecarWire.Key((uint)i + 1, (uint)i * 3, (ushort)(i % 5 + 1))).ToList();
        var body = Bytes(w => SidecarWire.WriteClear(w, 77, keys));
        var r = new NetReader(body);
        var (tick, got) = SidecarWire.ReadClear(ref r);
        Assert.Equal(77u, tick);
        Assert.Equal(keys, got);
        Assert.Throws<ArgumentException>(() => Bytes(w => SidecarWire.WriteClear(w, 1, [.. keys, keys[0]])));

        var asks = Enumerable.Range(0, SidecarWire.MaxResync).Select(i => new SidecarWire.ResyncItem((uint)i + 9, (uint)i)).ToList();
        body = Bytes(w => SidecarWire.WriteResync(w, asks));
        r = new NetReader(body);
        Assert.Equal(asks, SidecarWire.ReadResync(ref r));
        Assert.Throws<ArgumentException>(() => Bytes(w => SidecarWire.WriteResync(w, [.. asks, asks[0]])));
        var over = Bytes(w => w.VarUInt(SidecarWire.MaxResync + 1));
        Assert.Throws<NetFormatException>(() => { var rr = new NetReader(over); SidecarWire.ReadResync(ref rr); });
    }
}

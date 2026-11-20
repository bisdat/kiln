// SPDX-License-Identifier: MIT
// The native service kiln.repl.net (native/core/abi/services/kiln_repl.h, since 0.2.0-dev.31): sidecar tracking,
// per-peer snapshot filters, touch, the item remap, message hooks and the game's message codec.
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class ReplNetNative
{
    public const string Service = "kiln.repl.net";
    public const uint Version = 1;

    public const uint CapTrack = 1, CapClientTrack = 2, CapFilter = 4, CapTouch = 8, CapRemap = 16, CapMessagesSend = 32,
        CapMessagesReceive = 64, CapSchema = 128;
    public const byte TrackCreated = 1, TrackDestroyed = 2, TrackLeft = 3, TrackPeerGone = 4, TrackGap = 5;
    public const uint ClientChannel = 1, PredictionChannel = 2;
    public const uint TouchQueued = 0, TouchNoCarrier = 1, TouchGone = 2;
    public const int Pass = 0, Drop = 1, Rewrite = 2;

    /// <summary>KilnReplTrackEvent (16 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct TrackEvent
    {
        public uint Peer, Entity, Tick;
        public byte Kind, PeerFlags, Machine, Pad;
    }

    /// <summary>KilnReplPeerEntity (12 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct PeerEntity
    {
        public uint Entity, CreateTick;
        public byte Acked, P0, P1, P2;
    }

    /// <summary>KilnReplFilterCtx (40 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct FilterCtx
    {
        public uint Size, Peer;
        public byte Channel, PeerFlags, Machine, Pad;
        public uint Entity, Tick, ComponentHash;
        public delegate* unmanaged[Cdecl]<FilterCtx*, uint, uint, uint*, void*> Read;
        public uint* Failed;
    }

    /// <summary>KilnReplItemSite (12 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ItemSite
    {
        public uint ComponentHash;
        public ushort Offset, Count, Stride, Pad;
    }

    /// <summary>KilnReplItemMap (8 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ItemMap
    {
        public uint From, To;
    }

    /// <summary>KilnReplRemapStats (40 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct RemapStats
    {
        public uint Size, Sites;
        public ulong Snapshots, Remapped, Zeroed, Faults;
    }

    /// <summary>KilnReplMessage (48 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Message
    {
        public uint Size, Peer;
        public byte Channel, Direction, PlayerSocket, Pad;
        public uint Length;
        public byte* Data;
        public byte* Rewrite;
        public uint RewriteCap, Pad2;
        public uint* RewriteLength;
    }

    /// <summary>KilnReplSchemaField (48 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct SchemaField
    {
        public NativeStr Name;
        public uint Type, Element, Shape, Offset, CountOffset, FixedCount;
        public ulong Nested;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<uint> Capabilities;
        public delegate* unmanaged[Cdecl]<ulong, int> Remove;
        public delegate* unmanaged[Cdecl]<ulong, uint, delegate* unmanaged[Cdecl]<TrackEvent*, uint, void*, void>, void*, ulong*, int> Track;
        public delegate* unmanaged[Cdecl]<uint, PeerEntity*, uint, uint*, int> PeerEntities;
        public delegate* unmanaged[Cdecl]<ulong, uint, uint*, uint, uint, int, uint*, uint,
            delegate* unmanaged[Cdecl]<byte*, uint, FilterCtx*, void*, void>, void*, ulong*, int> FilterAdd;
        public delegate* unmanaged[Cdecl]<ulong, uint*, uint, int> FilterSetPeers;
        public delegate* unmanaged[Cdecl]<uint, uint*, int> Touch;
        public delegate* unmanaged[Cdecl]<ItemSite*, uint, uint*, uint, ItemMap*, uint, int> RemapSet;
        public delegate* unmanaged[Cdecl]<uint*, uint, int> RemapPeers;
        public delegate* unmanaged[Cdecl]<RemapStats*, int> RemapStatsFn;
        public delegate* unmanaged[Cdecl]<ulong, byte, uint, delegate* unmanaged[Cdecl]<Message*, void*, int>, void*, ulong*, int> MessageHook;
        public delegate* unmanaged[Cdecl]<uint, byte, void*, uint, int> MessageInject;
        public delegate* unmanaged[Cdecl]<NativeStr, uint*, ulong*, int> SchemaFind;
        public delegate* unmanaged[Cdecl]<ulong, SchemaField*, uint, uint*, int> SchemaFields;
        public delegate* unmanaged[Cdecl]<ulong, void*, uint, void*, uint, int> SchemaDecode;
        public delegate* unmanaged[Cdecl]<ulong, void*, uint, void*, uint, uint*, int> SchemaEncode;
        public delegate* unmanaged[Cdecl]<uint*, byte*, uint, uint*, int> Peers;
    }

    private static Table* _table;
    private static bool _looked;

    /// <summary>The table, or null (an older kiln-repl.dll, or none). Callers check the slot they use is inside
    /// <see cref="Table"/>'s size (v1 has every slot).</summary>
    internal static Table* T
    {
        get
        {
            if (_table != null || _looked) return _table;
            var t = Services.Get<Table>(Service, Version);
            _table = t != null && t->Header.Size >= (uint)sizeof(Table) ? t : null;
            _looked = true;
            return _table;
        }
    }

    /// <summary>Forgets the table (the test kit publishes a new fake per harness).</summary>
    internal static void Reset()
    {
        _table = null;
        _looked = false;
    }

    public static bool Available => T != null;

    /// <summary>KILN_REPL_NET_CAP_* of this process; 0 without the table.</summary>
    public static uint Capabilities() => T != null ? T->Capabilities() : 0;

    public static int Remove(ulong handle) => T == null ? ReplNative.Unavailable : T->Remove(handle);

    /// <summary>Each machine's peer and KILN_REPL_PEER_* flags as kiln.repl's last Pre saw them (17 machines).</summary>
    public static int MachinePeers(Span<uint> peers, Span<byte> flags)
    {
        if (T == null) return ReplNative.Unavailable;
        uint count = 0;
        var cap = (uint)Math.Min(peers.Length, flags.Length);
        fixed (uint* p = peers)
        fixed (byte* f = flags)
            return T->Peers(p, f, cap, &count);
    }
}

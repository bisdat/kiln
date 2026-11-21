// SPDX-License-Identifier: MIT
// The native services kiln.repl and kiln.repl.lab (native/core/abi/services/kiln_repl.h).
using System.Runtime.InteropServices;

namespace Kiln;

internal static unsafe class ReplNative
{
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5;
    public const uint CapObserve = 1, CapClient = 2, CapLoopback = 4;
    public const uint AllMachines = 0xFFFFFFFF, FakePeer = 0x80000000;
    public const uint Send = 1, Receive = 2;
    public const byte PeerFake = 1, PeerBound = 2, PeerLocal = 4, PeerReceived = 8;
    public const byte KindCreate = 2, KindUpdate = 3, KindDestroyed = 4, KindEnded = 5;
    public const uint CompReplicated = 1, CompVersioned = 2, CompCustomCodec = 4, CompClient = 8, CompPrediction = 16;

    [StructLayout(LayoutKind.Sequential)]
    public struct Fake
    {
        public uint Machine, Slot;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct LoopbackStats
    {
        public uint Size, Fakes;
        public ulong Messages, Decoded, ComponentsEqual, ComponentsDiffer, CustomEqual, CustomDiffer, DecodeFaults, NoBaseline,
            Acked, Dropped, DestroyLists;
        public uint ActiveObjects, SlotMask;
    }

    /// <summary>KilnReplPeerStats (120 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct PeerStats
    {
        public uint Size, Peer;
        public byte Channel, Flags;
        private readonly byte _p0, _p1, _p2, _p3, _p4, _p5;
        public ulong Frames, Messages, Bytes, Refused, Creates, Updates, DestroyLists, DestroyedIds, Unchanged, TooBig, Acks, Resets,
            Dropped;
    }

    /// <summary>KilnReplComponent (24 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Component
    {
        public ushort Index, Size;
        public uint Hash;
        public byte CustomCodec;
        private readonly byte _p0, _p1, _p2, _p3, _p4, _p5, _p6;
        public byte* Data;
    }

    /// <summary>KilnReplWatchEvent (48 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct WatchEvent
    {
        public uint Size, Peer;
        public byte Direction, Channel, Kind, Gap;
        public uint Entity, Tick, Seq, BaselineTick, PayloadBytes;
        public ushort Count;
        public byte PeerFlags, Pad;
        public uint Reason;
        public Component* Components;
    }

    /// <summary>KilnReplComponentInfo (16 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ComponentInfo
    {
        public uint Size, Hash;
        public ushort Index, DataSize;
        public uint Flags;
    }

    /// <summary>KilnReplHookCost (136 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct HookCost
    {
        public uint Size, Frames;
        public ulong WindowNs, SendStepMeanNs, SendStepP50Ns, FrameIntervalMeanNs, FrameIntervalP50Ns, CallbackCalls, CallbackNs,
            WriterCalls, WriterNs, DeltaCalls, DeltaNs, TickCalls, TickNs, Allocations, WatchRecords, WatchDropped;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<uint> Capabilities;
        public delegate* unmanaged[Cdecl]<ulong, byte, int> StatsEnable;
        public delegate* unmanaged[Cdecl]<PeerStats*, uint, uint*, int> PeerStatsFn;
        public delegate* unmanaged[Cdecl]<ulong, uint, uint, uint, delegate* unmanaged[Cdecl]<WatchEvent*, void*, void>, void*, ulong*, int> Watch;
        public delegate* unmanaged[Cdecl]<ulong, int> Remove;
        public delegate* unmanaged[Cdecl]<uint, ComponentInfo*, int> ComponentInfoFn;
        public delegate* unmanaged[Cdecl]<ulong, uint, uint, NativeStr, int> Capture;
        // lab (loopback harness)
        public delegate* unmanaged[Cdecl]<uint, Fake*, uint, uint*, int> LabAddFakes;
        public delegate* unmanaged[Cdecl]<uint, uint, int> LabForceRelevant;
        public delegate* unmanaged[Cdecl]<int> LabRemoveFakes;
        public delegate* unmanaged[Cdecl]<LoopbackStats*, int> LabStats;
        public delegate* unmanaged[Cdecl]<uint, uint, uint, byte*, uint, uint*, uint*, int> LabDecoded;
    }

    /// <summary>kiln.repl.lab (since 0.2.0-dev.31).</summary>
    [StructLayout(LayoutKind.Sequential)]
    private struct LabTable
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<uint, uint, uint, byte*, uint, uint*, uint*, int> Payload;
        public delegate* unmanaged[Cdecl]<HookCost*, byte, int> HookCostFn;
    }

    private static Table* _table;
    private static bool _looked;
    private static LabTable* _lab;
    private static bool _labLooked;

    private static Table* T
    {
        get
        {
            if (_table != null || _looked) return _table;
            _table = Services.Get<Table>("kiln.repl", 1);
            _looked = true;
            return _table;
        }
    }

    private static LabTable* Lab
    {
        get
        {
            if (_lab != null || _labLooked) return _lab;
            _lab = Services.Get<LabTable>("kiln.repl.lab", 1);
            _labLooked = true;
            return _lab;
        }
    }

    /// <summary>Forgets the tables looked up (the test kit publishes a new fake per harness).</summary>
    internal static void Reset()
    {
        _table = null;
        _looked = false;
        _lab = null;
        _labLooked = false;
    }

    /// <summary>The table is here (also in the test kit, when a fake publishes one).</summary>
    public static bool Available => T != null;

    /// <summary>KILN_REPL_CAP_* of this process; 0 without the table.</summary>
    public static uint Capabilities() => T != null ? T->Capabilities() : 0;

    public static int StatsEnable(ulong owner, bool on) => T == null ? Unavailable : T->StatsEnable(owner, on ? (byte)1 : (byte)0);

    /// <summary>Every row kiln.repl has now (the buffer grows when it is too small).</summary>
    public static int PeerStatsAll(out PeerStats[] rows)
    {
        rows = [];
        if (T == null) return Unavailable;
        var buf = new PeerStats[8];
        for (var attempt = 0; attempt < 4; attempt++)
        {
            for (var i = 0; i < buf.Length; i++) buf[i].Size = (uint)sizeof(PeerStats);
            uint count = 0;
            int rc;
            fixed (PeerStats* p = buf) rc = T->PeerStatsFn(p, (uint)buf.Length, &count);
            if (rc == BufferTooSmall)
            {
                buf = new PeerStats[Math.Max(count, (uint)buf.Length * 2)];
                continue;
            }
            if (rc != Ok) return rc;
            rows = buf.AsSpan(0, (int)Math.Min(count, (uint)buf.Length)).ToArray();
            return Ok;
        }
        return BufferTooSmall;
    }

    public static int Watch(ulong owner, uint entity, uint peerOrAll, uint direction, delegate* unmanaged[Cdecl]<WatchEvent*, void*, void> fn,
        nint user, out ulong handle)
    {
        handle = 0;
        if (T == null) return Unavailable;
        ulong h = 0;
        var rc = T->Watch(owner, entity, peerOrAll, direction, fn, (void*)user, &h);
        handle = h;
        return rc;
    }

    public static int Remove(ulong handle) => T == null ? Unavailable : T->Remove(handle);

    public static int ComponentInfoOf(uint hash, out ComponentInfo info)
    {
        info = default;
        info.Size = (uint)sizeof(ComponentInfo);
        if (T == null) return Unavailable;
        fixed (ComponentInfo* p = &info) return T->ComponentInfoFn(hash, p);
    }

    public static int Capture(ulong owner, uint peer, uint seconds, string path)
    {
        if (T == null) return Unavailable;
        var bytes = System.Text.Encoding.UTF8.GetBytes(path);
        fixed (byte* p = bytes) return T->Capture(owner, peer, seconds, new NativeStr { Ptr = p, Len = (nuint)bytes.Length });
    }

    public static int AddFakes(uint count, Span<Fake> fakes, out uint added)
    {
        added = 0;
        if (T == null) return Unavailable;
        uint n = 0;
        int rc;
        fixed (Fake* p = fakes) rc = T->LabAddFakes(count, p, (uint)fakes.Length, &n);
        added = n;
        return rc;
    }

    public static int ForceRelevant(uint machine, uint entity) => T == null ? Unavailable : T->LabForceRelevant(machine, entity);

    public static int RemoveFakes() => T == null ? Unavailable : T->LabRemoveFakes();

    public static int Stats(out LoopbackStats stats)
    {
        stats = default;
        stats.Size = (uint)sizeof(LoopbackStats);
        if (T == null) return Unavailable;
        fixed (LoopbackStats* p = &stats) return T->LabStats(p);
    }

    public static int Decoded(uint machine, uint entity, uint hash, out byte[] bytes, out uint tick)
    {
        bytes = [];
        tick = 0;
        if (T == null) return Unavailable;
        uint len = 0, t = 0;
        var rc = T->LabDecoded(machine, entity, hash, null, 0, &len, &t);
        if (rc != Ok) return rc;
        var buf = new byte[len];
        fixed (byte* p = buf) rc = T->LabDecoded(machine, entity, hash, p, len, &len, &t);
        if (rc != Ok) return rc;
        bytes = buf;
        tick = t;
        return Ok;
    }

    /// <summary>kiln.repl.lab payload: the newest create (2) or update (3) payload fake <paramref name="machine"/> was sent
    /// for <paramref name="entity"/> on its client channel.</summary>
    public static int Payload(uint machine, uint entity, uint kind, out byte[] bytes, out uint tick)
    {
        bytes = [];
        tick = 0;
        if (Lab == null || Lab->Header.Size < (uint)sizeof(LabTable)) return Unavailable;
        uint len = 0, t = 0;
        var rc = Lab->Payload(machine, entity, kind, null, 0, &len, &t);
        if (rc != Ok) return rc;
        var buf = new byte[len];
        fixed (byte* p = buf) rc = Lab->Payload(machine, entity, kind, p, len, &len, &t);
        if (rc != Ok) return rc;
        bytes = buf;
        tick = t;
        return Ok;
    }

    /// <summary>kiln.repl.lab hook_cost.</summary>
    public static int HookCostOf(bool reset, out HookCost cost)
    {
        cost = default;
        cost.Size = (uint)sizeof(HookCost);
        if (Lab == null || Lab->Header.Size < (uint)sizeof(LabTable)) return Unavailable;
        fixed (HookCost* p = &cost) return Lab->HookCostFn(p, reset ? (byte)1 : (byte)0);
    }

    /// <summary>The engine's component hash: FNV-1a-32 of "keen::ecs::" + the short name (a name with "::" as it is),
    /// as kiln.world's component_hash.</summary>
    public static uint ComponentHash(string name)
    {
        var h = 2166136261u;
        if (!name.Contains("::", StringComparison.Ordinal))
            foreach (var c in "keen::ecs::") h = (h ^ (byte)c) * 16777619u;
        foreach (var b in System.Text.Encoding.UTF8.GetBytes(name)) h = (h ^ b) * 16777619u;
        return h;
    }
}

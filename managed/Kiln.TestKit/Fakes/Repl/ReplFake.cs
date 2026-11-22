// SPDX-License-Identifier: MIT
// The fake kiln.repl (native/core/abi/services/kiln_repl.h, v1) under the real Kiln.Replication: the network inspector's
// counters and watches answer from what a test scripts (KilnHarness.Repl). The rules are native/services/repl's: stats
// held per owner (the first hold of all zeroes the counters, a revoked owner lets go), one row per machine and channel
// with anything counted, watches delivered in the next Pre (the start of the next step) in the order the messages went,
// a destroy list's DESTROYED event, and ENDED as the last event (destroyed, id reused, peer left, world stopped). The
// game's send path, its codecs, the capture file and the loopback harness are not modelled.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

/// <summary>Mirror of <c>kiln_repl.h</c>. The sizes are pinned at registration to the header's.</summary>
internal static unsafe class ReplAbi
{
    public const string Service = "kiln.repl";
    public const string LabService = "kiln.repl.lab";
    public const uint Version = 1;

    public const uint CapObserve = 1, CapClient = 2, CapLoopback = 4;
    public const uint AllMachines = 0xFFFFFFFF, FakePeer = 0x80000000;
    public const uint Machines = 17;
    public const uint Send = 1, Receive = 2;
    public const byte PeerFake = 1, PeerBound = 2, PeerLocal = 4;
    public const byte KindCreate = 2, KindUpdate = 3, KindDestroyed = 4, KindEnded = 5;
    public const uint EndDestroyed = 1, EndIdReused = 2, EndPeerLeft = 3, EndWorldStopped = 4;
    public const uint CompReplicated = 1, CompVersioned = 2, CompCustomCodec = 4, CompClient = 8, CompPrediction = 16;

    /// <summary>KilnReplPeerStats (120 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct PeerStats
    {
        public uint Size, Peer;
        public byte Channel, Flags;
        public byte P0, P1, P2, P3, P4, P5;
        public ulong Frames, Messages, Bytes, Refused, Creates, Updates, DestroyLists, DestroyedIds, Unchanged, TooBig, Acks, Resets, Dropped;
    }

    /// <summary>KilnReplComponent (24 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Component
    {
        public ushort Index, Size;
        public uint Hash;
        public byte CustomCodec;
        public byte P0, P1, P2, P3, P4, P5, P6;
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

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<uint> Capabilities;
        public delegate* unmanaged[Cdecl]<ulong, byte, int> StatsEnable;
        public delegate* unmanaged[Cdecl]<PeerStats*, uint, uint*, int> PeerStatsFn;
        public delegate* unmanaged[Cdecl]<ulong, uint, uint, uint, delegate* unmanaged[Cdecl]<WatchEvent*, void*, void>, void*, ulong*, int> Watch;
        public delegate* unmanaged[Cdecl]<ulong, int> Remove;
        public delegate* unmanaged[Cdecl]<uint, ComponentInfo*, int> ComponentInfoFn;
        public delegate* unmanaged[Cdecl]<ulong, uint, uint, NativeStr, int> Capture;
        // lab (loopback harness), since 0.2.0-dev.28: not modelled
        public delegate* unmanaged[Cdecl]<uint, void*, uint, uint*, int> LabAddFakes;
        public delegate* unmanaged[Cdecl]<uint, uint, int> LabForceRelevant;
        public delegate* unmanaged[Cdecl]<int> LabRemoveFakes;
        public delegate* unmanaged[Cdecl]<void*, int> LabStats;
        public delegate* unmanaged[Cdecl]<uint, uint, uint, byte*, uint, uint*, uint*, int> LabDecoded;
    }

    /// <summary>The engine's component hash: FNV-1a-32 of "keen::ecs::" + the short name (a name with "::" as it is),
    /// as kiln_world.h's component_hash.</summary>
    public static uint ComponentHash(string name)
    {
        var h = 2166136261u;
        if (!name.Contains("::", StringComparison.Ordinal))
            foreach (var c in "keen::ecs::") h = (h ^ (byte)c) * 16777619u;
        foreach (var b in System.Text.Encoding.UTF8.GetBytes(name)) h = (h ^ b) * 16777619u;
        return h;
    }
}

/// <summary>One harness's kiln.repl: the machines, the counters, the registry, the watches and the next Pre's records.</summary>
internal sealed class ReplFakeState
{
    public sealed class Machine
    {
        public uint Peer;
        public byte Flags;
        public NetPeer? Net;
    }

    public sealed class Counters
    {
        public ulong Frames, Messages, Bytes, Refused, Creates, Updates, DestroyLists, DestroyedIds, Unchanged, TooBig, Acks, Resets, Dropped;
        public ulong FrameStep = ulong.MaxValue;
        public bool Any => (Frames | Messages | Bytes | Refused | Creates | Updates | DestroyLists | DestroyedIds | Unchanged | TooBig | Acks | Resets) != 0;
    }

    public sealed class Watch
    {
        public ulong Handle, Owner;
        public uint Entity, Peer, Machines, Direction;
        public nint Fn, User;
        public int? Template;
        public bool Ended, Removed, Destroyed;
    }

    /// <summary>A record for the next Pre: a message (kind 2/3), a destroy list's entry (4), as a watch hook would have
    /// written it in the frame.</summary>
    public sealed record Record(uint Direction, uint Machine, uint Peer, byte PeerFlags, byte Channel, byte Kind, uint Entity, uint Tick,
        uint Seq, uint BaselineTick, uint PayloadBytes, bool Gap, int Template, IReadOnlyList<(FakeReplComponent Component, byte[] Bytes)> Components);

    public readonly object Lock = new();
    public readonly ReplNetState Net = new();
    public readonly Machine?[] Machines = new Machine?[ReplAbi.Machines];
    public readonly Counters[,] Rows = new Counters[ReplAbi.Machines, 2];
    public readonly Dictionary<uint, FakeReplComponent> Registry = [];
    public readonly List<Watch> Watches = [];
    public readonly List<Record> Pending = [];
    public readonly List<uint> PeersLeft = [];
    public readonly bool[,] GapNext = new bool[ReplAbi.Machines, 2];
    public readonly Dictionary<uint, int> Templates = [];  // entity -> its template now (default 1)
    public readonly List<(ulong Owner, bool On)> StatsCalls = [];
    public bool ReceiveGapNext, WorldStopping, WorldSeen;
    public ulong Serial;

    public ReplFakeState()
    {
        for (var m = 0; m < ReplAbi.Machines; m++)
        for (var c = 0; c < 2; c++)
            Rows[m, c] = new Counters();
    }

    public int TemplateOf(uint entity) => Templates.TryGetValue(entity, out var t) ? t : 1;

    public int MachineOf(uint peer)
    {
        if (peer == 0) return -1;
        for (var m = 0; m < Machines.Length; m++)
            if (Machines[m] is { } x && x.Peer == peer) return m;
        return -1;
    }

    public void ClearRows(int machine)
    {
        for (var c = 0; c < 2; c++) Rows[machine, c] = new Counters();
    }
}

internal sealed unsafe class ReplFake : IKitFake
{
    public const string ObserveRow = "repl.observe", ClientRow = "repl.client", LoopbackRow = "repl.loopback";
    public const int MaxWatches = 64, MaxOwners = 64;

    private static ReplAbi.Table* _table;

    // Like kiln-repl.dll, the stats holders live for the process: Kiln.Replication keeps its own owner's hold in a static
    // for the process too. A mod's owner lets go when it is revoked (its harness's unload or end).
    private static readonly object HoldersGate = new();
    private static readonly List<ulong> Holders = [];

    public string Service => ReplAbi.Service;

    public void Register()
    {
        if (sizeof(ReplAbi.PeerStats) != 120 || sizeof(ReplAbi.Component) != 24 || sizeof(ReplAbi.WatchEvent) != 48 || sizeof(ReplAbi.ComponentInfo) != 16)
            throw new InvalidOperationException("the kit's mirror of kiln_repl.h does not match the header's sizes");
        if (_table == null)
        {
            var t = (ReplAbi.Table*)NativeMemory.AllocZeroed((nuint)sizeof(ReplAbi.Table));  // process lifetime, never freed
            t->Header = new ServiceHeader { Size = (uint)sizeof(ReplAbi.Table), Version = ReplAbi.Version };
            t->Capabilities = &Capabilities;
            t->StatsEnable = &StatsEnable;
            t->PeerStatsFn = &PeerStats;
            t->Watch = &Watch;
            t->Remove = &Remove;
            t->ComponentInfoFn = &ComponentInfo;
            t->Capture = &Capture;
            t->LabAddFakes = &LabAddFakes;
            t->LabForceRelevant = &LabForceRelevant;
            t->LabRemoveFakes = &LabRemoveFakes;
            t->LabStats = &LabStats;
            t->LabDecoded = &LabDecoded;
            _table = t;
        }
        KitBackend.Instance.RegisterTable(ReplAbi.Service, (nint)_table);
        FakeTables.Features([ObserveRow, ClientRow]);
        FakeTables.Offline(LoopbackRow, "the loopback harness runs the game's own replication on a lab server; the test kit has no game");
    }

    public void Start(KilnHarness harness) { }

    public void BeforeStep(KilnHarness harness, ulong step) => Pre(harness);

    public void End(KilnHarness harness)
    {
        var st = KitFakes.State<ReplFakeState>(harness);
        lock (st.Lock) st.WorldStopping = true;
        try
        {
            Pre(harness);  // the world stops with the harness: every watch still on gets ENDED "world stopped"
        }
        finally
        {
            ReleaseRevoked(harness);
        }
    }

    public void AddCoverage(List<CoverageRow> rows)
    {
        rows.Add(new CoverageRow("Network inspector (Replication)", CoverageKind.RecordingFake, [ReplAbi.Service, ObserveRow, ClientRow, LoopbackRow],
            "Kiln.Replication's own code on the kit's kiln.repl: the test connects peers to machine slots (KilnHarness.Repl.Connect, " +
            "ConnectFake, ConnectLocal) and scripts what the server sends them (Send, Destroy, Drop, Count) and what a client receives " +
            "(Receive, ReceiveDestroy). Replication.Stats holds the counters per owner (the first hold of all starts them from 0, a " +
            "mod's unload lets go) and reads one row per peer and channel; Replication.Watch and the receive side's watches get one " +
            "event per message at the start of the next step with the full component values by name (the registry the test adds, " +
            "AddComponent; names from kiln.types where the kit has it, else #index), Gap after Drop, DESTROYED then ENDED, and ENDED " +
            "'id reused' (ReuseId), 'peer left' (Leave) and 'world stopped' (StopWorld, the harness's end) as the last event; " +
            "Replication.Component reads the registry from the first step. NetPeer stays null on events and rows (the kit has no " +
            "kiln.net peers): PeerId, Local and Fake say who. The kit's mods run on the server side, so Replication.WatchReceived " +
            "reports repl.client off (wrong side); the receive watches are faked on a client-side harness only. Offline: the game's " +
            "send path, codecs and sizes, the capture file (repl capture), the loopback harness (repl.loopback, kiln.repl.lab)"));
        rows.Add(new CoverageRow("kiln.repl.lab", CoverageKind.Unavailable, [ReplAbi.LabService],
            "no fake: the loopback harness's payloads and hook cost exist only on a lab server running the game"));
    }

    // ---- what a test scripts (KilnHarness.Repl) ----

    internal static ReplFakeState StateOf(KilnHarness harness) => KitFakes.State<ReplFakeState>(harness);

    /// <summary>Some owner holds the counters now; owners revoked in <paramref name="harness"/> let go first.</summary>
    internal static bool CountingIn(KilnHarness harness)
    {
        ReleaseRevoked(harness);
        lock (HoldersGate) return Holders.Count > 0;
    }

    // ---- the native service's Pre ----

    /// <summary>The last frame's records to their watches, then the destroyed watches' ENDED, the peers that left, a world
    /// that stopped; owners revoked since the last Pre let go first (tick thread).</summary>
    private static void Pre(KilnHarness harness)
    {
        ReleaseRevoked(harness);
        var st = StateOf(harness);
        List<(ReplFakeState.Watch W, ReplFakeState.Record? R, uint Reason)> calls = [];
        lock (st.Lock)
        {
            if (harness.Options.Side != HarnessSide.JoinedClient) st.WorldSeen = true;  // the registry is read at the first Pre with a server world
            foreach (var w in st.Watches)
                if (!w.Removed && harness.ReplOwnerRevoked(w.Owner)) w.Removed = true;  // an owner's revoke: its watches go silently
            foreach (var r in st.Pending)
            {
                foreach (var w in st.Watches)
                {
                    if (w.Removed || w.Ended || w.Entity != r.Entity || w.Direction != r.Direction) continue;
                    if (r.Direction == ReplAbi.Send && ((w.Machines >> (int)r.Machine) & 1) == 0) continue;
                    if (r.Kind == ReplAbi.KindDestroyed)
                    {
                        calls.Add((w, r, 0));
                        w.Destroyed = true;
                        continue;
                    }
                    if (w.Template is null) w.Template = r.Template;
                    else if (w.Template != r.Template)
                    {
                        calls.Add((w, null, ReplAbi.EndIdReused));  // the id belongs to an entity of another template now
                        w.Ended = true;
                        continue;
                    }
                    calls.Add((w, r, 0));
                }
            }
            st.Pending.Clear();
            foreach (var w in st.Watches)
                if (w.Destroyed && !w.Ended && !w.Removed)
                {
                    w.Destroyed = false;
                    calls.Add((w, null, ReplAbi.EndDestroyed));
                    w.Ended = true;
                }
            foreach (var peer in st.PeersLeft)
                foreach (var w in st.Watches)
                    if (!w.Removed && !w.Ended && w.Peer == peer)
                    {
                        calls.Add((w, null, ReplAbi.EndPeerLeft));
                        w.Ended = true;
                    }
            st.PeersLeft.Clear();
            if (st.WorldStopping)
            {
                st.WorldStopping = false;
                foreach (var w in st.Watches)
                    if (!w.Removed && !w.Ended)
                    {
                        calls.Add((w, null, ReplAbi.EndWorldStopped));
                        w.Ended = true;
                    }
                for (var m = 0; m < st.Machines.Length; m++)
                {
                    st.Machines[m] = null;
                    st.ClearRows(m);
                }
            }
        }
        // the callbacks run outside the lock (a handler may dispose its watch: remove takes the lock); a watch removed
        // meanwhile gets nothing more, as the native deliver() checks 'removed' before each call
        foreach (var (w, r, reason) in calls)
        {
            Deliver(st, w, r, reason);
            if (reason != 0)
                lock (st.Lock) w.Removed = true;  // ENDED is the last event: kiln.repl gives the slot back by itself (dev.31)
        }
        lock (st.Lock) st.Watches.RemoveAll(w => w.Removed);  // watch_apply: removed watches free their slots
    }

    private static void Deliver(ReplFakeState st, ReplFakeState.Watch w, ReplFakeState.Record? r, uint reason)
    {
        lock (st.Lock)
            if (w.Removed) return;
        var count = r?.Components.Count ?? 0;
        var bytes = 0;
        if (r is not null)
            foreach (var (_, b) in r.Components) bytes += b.Length;
        var block = (byte*)NativeMemory.AllocZeroed((nuint)(sizeof(ReplAbi.WatchEvent) + count * sizeof(ReplAbi.Component) + Math.Max(bytes, 1)));
        try
        {
            var e = (ReplAbi.WatchEvent*)block;
            var comps = (ReplAbi.Component*)(block + sizeof(ReplAbi.WatchEvent));
            var data = (byte*)(comps + count);
            e->Size = (uint)sizeof(ReplAbi.WatchEvent);
            e->Entity = w.Entity;
            if (r is null)
            {
                e->Direction = (byte)ReplAbi.Send;  // as the native end(): ENDED carries no peer and no components
                e->Kind = ReplAbi.KindEnded;
                e->Reason = reason;
            }
            else
            {
                e->Peer = r.Peer;
                e->PeerFlags = r.PeerFlags;
                e->Direction = (byte)r.Direction;
                e->Channel = r.Channel;
                e->Kind = r.Kind;
                e->Gap = r.Gap ? (byte)1 : (byte)0;
                e->Tick = r.Tick;
                e->Seq = r.Seq;
                e->BaselineTick = r.BaselineTick;
                e->PayloadBytes = r.PayloadBytes;
                e->Count = (ushort)count;
                e->Components = count > 0 ? comps : null;
                for (var i = 0; i < count; i++)
                {
                    var (c, b) = r.Components[i];
                    b.CopyTo(new Span<byte>(data, b.Length));
                    comps[i] = new ReplAbi.Component
                    {
                        Index = c.Index, Size = (ushort)b.Length, Hash = ReplAbi.ComponentHash(c.Name), CustomCodec = c.CustomCodec ? (byte)1 : (byte)0, Data = data,
                    };
                    data += b.Length;
                }
            }
            ((delegate* unmanaged[Cdecl]<ReplAbi.WatchEvent*, void*, void>)w.Fn)(e, (void*)w.User);
        }
        finally
        {
            NativeMemory.Free(block);
        }
    }

    /// <summary>Owners revoked in this harness let go of the counters (kiln.repl's owner-retiring observer).</summary>
    private static void ReleaseRevoked(KilnHarness harness)
    {
        lock (HoldersGate) Holders.RemoveAll(harness.ReplOwnerRevoked);
    }

    // ---- the table ----

    private static KilnHarness? Active => FakeTables.Active(ReplAbi.Service);

    private static bool ObserveOn(KilnHarness h) => !FakeTables.Off(ObserveRow);

    private static bool ClientOn(KilnHarness h) => h.Options.Side != HarnessSide.DedicatedServer && !FakeTables.Off(ClientRow);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint Capabilities()
    {
        try
        {
            if (Active is not { } h) return 0;
            return (ObserveOn(h) ? ReplAbi.CapObserve : 0) | (ClientOn(h) ? ReplAbi.CapClient : 0);
        }
        catch
        {
            return 0;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int StatsEnable(ulong owner, byte on)
    {
        try
        {
            if (Active is not { } h || !ObserveOn(h)) return FakeTables.Unavailable;
            if (!FakeTables.OnTickThread) return FakeTables.WrongThread;
            if (owner == 0) return FakeTables.InvalidArgument;
            var st = StateOf(h);
            ReleaseRevoked(h);
            var first = false;
            lock (HoldersGate)
            {
                var i = Holders.IndexOf(owner);
                if (on != 0 && i < 0)
                {
                    if (Holders.Count >= MaxOwners) return FakeTables.InvalidArgument;
                    first = Holders.Count == 0;
                    Holders.Add(owner);
                }
                else if (on == 0 && i >= 0)
                {
                    Holders.RemoveAt(i);
                }
            }
            lock (st.Lock)
            {
                if (first)  // the first enable of all: counters from 0 (tick thread: nothing is sent now)
                    for (var m = 0; m < st.Machines.Length; m++)
                    {
                        var dropped = (st.Rows[m, 0].Dropped, st.Rows[m, 1].Dropped);  // the watch rings' drops are not the stats'
                        st.ClearRows(m);
                        (st.Rows[m, 0].Dropped, st.Rows[m, 1].Dropped) = dropped;
                    }
                st.StatsCalls.Add((owner, on != 0));
            }
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int PeerStats(ReplAbi.PeerStats* out_, uint cap, uint* count)
    {
        try
        {
            if (count != null) *count = 0;
            if (Active is not { } h || !ObserveOn(h)) return FakeTables.Unavailable;
            if (!FakeTables.OnTickThread) return FakeTables.WrongThread;
            if (count == null || (cap != 0 && out_ == null)) return FakeTables.InvalidArgument;
            var st = StateOf(h);
            uint rows = 0;
            lock (st.Lock)
            {
                for (var m = 0; m < st.Machines.Length; m++)
                {
                    var peer = st.Machines[m]?.Peer ?? 0;
                    var flags = st.Machines[m]?.Flags ?? 0;
                    for (var ch = 0; ch < 2; ch++)
                    {
                        var s = st.Rows[m, ch];
                        if (!s.Any && s.Dropped == 0) continue;
                        if (rows < cap)
                        {
                            ref var r = ref out_[rows];
                            if (r.Size < (uint)sizeof(ReplAbi.PeerStats)) return FakeTables.InvalidArgument;
                            r = new ReplAbi.PeerStats
                            {
                                Size = r.Size, Peer = peer, Channel = (byte)(ch == 0 ? 4 : 5), Flags = flags, Frames = s.Frames, Messages = s.Messages,
                                Bytes = s.Bytes, Refused = s.Refused, Creates = s.Creates, Updates = s.Updates, DestroyLists = s.DestroyLists,
                                DestroyedIds = s.DestroyedIds, Unchanged = s.Unchanged, TooBig = s.TooBig, Acks = s.Acks, Resets = s.Resets, Dropped = s.Dropped,
                            };
                        }
                        rows++;
                    }
                }
            }
            *count = rows;
            return rows > cap ? FakeTables.BufferTooSmall : FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Watch(ulong owner, uint entity, uint peerOrAll, uint direction, delegate* unmanaged[Cdecl]<ReplAbi.WatchEvent*, void*, void> fn,
        void* user, ulong* out_)
    {
        try
        {
            if (out_ != null) *out_ = 0;
            if (Active is not { } h) return FakeTables.Unavailable;
            if (direction == ReplAbi.Receive)
            {
                if (!ClientOn(h)) return FakeTables.Unavailable;  // the receive side runs on a game client
            }
            else if (!ObserveOn(h)) return FakeTables.Unavailable;
            if (!FakeTables.OnTickThread) return FakeTables.WrongThread;
            if (entity == 0 || fn == null || out_ == null || direction is not (ReplAbi.Send or ReplAbi.Receive)) return FakeTables.InvalidArgument;
            var st = StateOf(h);
            lock (st.Lock)
            {
                uint machines;
                if (direction == ReplAbi.Receive || peerOrAll == ReplAbi.AllMachines) machines = (1u << (int)ReplAbi.Machines) - 1;
                else
                {
                    var m = st.MachineOf(peerOrAll);
                    if (m < 0) return FakeTables.InvalidArgument;  // not connected
                    machines = 1u << m;
                }
                if (st.Watches.Count >= MaxWatches) return FakeTables.InvalidArgument;  // 64 watches (an ended one gives its slot back at once)
                var handle = (0x5245ul << 48) | ++st.Serial;
                st.Watches.Add(new ReplFakeState.Watch
                {
                    Handle = handle, Owner = owner, Entity = entity, Peer = direction == ReplAbi.Receive ? ReplAbi.AllMachines : peerOrAll, Machines = machines,
                    Direction = direction, Fn = (nint)fn, User = (nint)user,
                });
                *out_ = handle;
            }
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Remove(ulong handle)
    {
        try
        {
            if (handle == 0) return FakeTables.InvalidArgument;
            if (KitBackend.Instance.Harness is not { } h) return FakeTables.NotFound;  // any thread; also with the service switched off
            var st = StateOf(h);
            lock (st.Lock)
            {
                foreach (var w in st.Watches)
                    if (w.Handle == handle)
                    {
                        w.Removed = true;  // no event after this; the slot is free at the next Pre
                        return FakeTables.Ok;
                    }
            }
            return FakeTables.NotFound;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ComponentInfo(uint hash, ReplAbi.ComponentInfo* out_)
    {
        try
        {
            if (out_ == null || out_->Size < (uint)sizeof(ReplAbi.ComponentInfo)) return FakeTables.InvalidArgument;
            if (Active is not { } h) return FakeTables.Unavailable;
            var st = StateOf(h);
            lock (st.Lock)
            {
                if (!st.WorldSeen) return FakeTables.Unavailable;  // no world seen yet
                if (!st.Registry.TryGetValue(hash, out var c)) return FakeTables.NotFound;
                out_->Hash = hash;
                out_->Index = c.Index;
                out_->DataSize = c.Size;
                out_->Flags = (c.Replicated ? ReplAbi.CompReplicated : 0) | (c.Versioned ? ReplAbi.CompVersioned : 0) |
                              (c.CustomCodec ? ReplAbi.CompCustomCodec : 0) | (c.ClientChannel ? ReplAbi.CompClient : 0) |
                              (c.PredictionChannel ? ReplAbi.CompPrediction : 0);
                return FakeTables.Ok;
            }
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    // not modelled: the capture file and the loopback harness
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Capture(ulong owner, uint peer, uint seconds, NativeStr path) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int LabAddFakes(uint count, void* out_, uint cap, uint* added)
    {
        if (added != null) *added = 0;
        return FakeTables.Unavailable;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int LabForceRelevant(uint machine, uint entity) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int LabRemoveFakes() => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int LabStats(void* out_) => FakeTables.Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int LabDecoded(uint machine, uint entity, uint hash, byte* out_, uint cap, uint* len, uint* tick)
    {
        if (len != null) *len = 0;
        if (tick != null) *tick = 0;
        return FakeTables.Unavailable;
    }
}

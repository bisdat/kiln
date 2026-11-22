// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using static Kiln.ReplNetNative;
using static Kiln.TestKit.FakeTables;

namespace Kiln.TestKit;

internal sealed unsafe partial class ReplNetFake : IKitFake
{
    private static long _serial;

    public string Service => ReplNetNative.Service;

    public void Register()
    {
        var table = (Table*)NativeMemory.AllocZeroed((nuint)sizeof(Table)); // process lifetime
        *table = new Table
        {
            Header = new ServiceHeader { Size = (uint)sizeof(Table), Version = ReplNetNative.Version },
            Capabilities = &GetCapabilities,
            Remove = &RemoveRegistration,
            Track = &TrackUnavailable,
            PeerEntities = &PeerEntitiesUnavailable,
            FilterAdd = &AddFilter,
            FilterSetPeers = &SetFilterPeers,
            Touch = &TouchEntity,
            RemapSet = &SetRemap,
            RemapPeers = &SetRemapPeers,
            RemapStatsFn = &GetRemapStats,
            MessageHook = &AddHook,
            MessageInject = &Inject,
            SchemaFind = &SchemaFindUnavailable,
            SchemaFields = &SchemaFieldsUnavailable,
            SchemaDecode = &SchemaDecodeUnavailable,
            SchemaEncode = &SchemaEncodeUnavailable,
            Peers = &GetPeers,
        };
        KitBackend.Instance.RegisterTable(Service, (nint)table);
        Features([ReplicationCaps.Filter, ReplicationCaps.Touch, ReplicationCaps.ItemRemap, ReplicationCaps.Messages]);
        Offline(ReplicationCaps.Sidecar, "the test kit has no sidecar transport or entity relevance tracking");
        Offline(ReplicationCaps.Schema, "the test kit has no game message schemas or codec");
    }

    public void Start(KilnHarness harness)
    {
        ReplNetNative.Reset();
        if (harness.Options.Side == HarnessSide.JoinedClient)
        {
            Features([ReplicationCaps.MessagesClient]);
            Offline(ReplicationCaps.MessagesServer, "a joined-client harness has no server message loop");
        }
        else
        {
            Features([ReplicationCaps.MessagesServer]);
            Offline(ReplicationCaps.MessagesClient, "the client message loop is scripted on a joined-client harness");
        }
        FilterHost.OfflinePeers = () =>
        {
            var repl = ReplFake.StateOf(harness);
            lock (repl.Lock) return repl.Machines.Select(m => m?.Net).OfType<NetPeer>().ToArray();
        };
        ItemRemap.OfflineItems = () =>
        {
            var repl = ReplFake.StateOf(harness);
            lock (repl.Lock) return (repl.Net.LookAlikes.Count, repl.Net.LookAlikes.Values.Count(id => id == 0));
        };
    }

    public void BeforeStep(KilnHarness harness, ulong step)
    {
        var repl = ReplFake.StateOf(harness);
        lock (repl.Lock)
        {
            repl.Net.Filters.RemoveAll(f => harness.ReplOwnerRevoked(f.Owner));
            repl.Net.Hooks.RemoveAll(h => harness.ReplOwnerRevoked(h.Owner));
        }
    }

    public void End(KilnHarness harness)
    {
        var repl = ReplFake.StateOf(harness);
        lock (repl.Lock)
        {
            repl.Net.Filters.Clear();
            repl.Net.Hooks.Clear();
        }
        var stopped = harness.TickThread.AbandonReason is null;
        FilterHost.EndOfflineSession(stopped);
        MessageHost.EndOfflineSession(stopped);
        ItemRemap.EndOfflineSession();
    }

    public void AddCoverage(List<CoverageRow> rows) =>
        rows.Add(new CoverageRow(Service, CoverageKind.RecordingFake, [Service, ReplicationCaps.Filter, ReplicationCaps.Touch,
            ReplicationCaps.ItemRemap, ReplicationCaps.Messages, ReplicationCaps.Sidecar, ReplicationCaps.Schema],
            "KilnHarness.Repl.Send runs per-peer component filters and scripted item remapping. Watches see the resulting bytes. " +
            "KilnHarness.ReplNet records touch and inject requests and drives message hooks. Sidecars, the game codec and the " +
            "simulation that schedules and transports replication are unavailable offline."));

    private static uint Caps(KilnHarness harness)
    {
        var client = harness.Options.Side == HarnessSide.JoinedClient;
        var messages = !Off(ReplicationCaps.Messages)
            && !Off(client ? ReplicationCaps.MessagesClient : ReplicationCaps.MessagesServer);
        var caps = messages ? CapMessagesSend | CapMessagesReceive : 0;
        if (client) return caps;
        if (!Off(ReplicationCaps.Filter)) caps |= CapFilter;
        if (!Off(ReplicationCaps.Touch)) caps |= CapTouch;
        if (!Off(ReplicationCaps.Filter) && !Off(ReplicationCaps.ItemRemap)) caps |= CapRemap;
        return caps;
    }

    private static KilnHarness? ActiveFor(uint cap) =>
        Active(ReplNetNative.Service) is { } harness && (Caps(harness) & cap) != 0 ? harness : null;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint GetCapabilities() => Active(ReplNetNative.Service) is { } harness ? Caps(harness) : 0;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GetPeers(uint* ids, byte* flags, uint cap, uint* count)
    {
        if (Active(ReplNetNative.Service) is not { } harness) return Unavailable;
        if (!OnTickThread) return WrongThread;
        if (count == null) return InvalidArgument;
        var repl = ReplFake.StateOf(harness);
        lock (repl.Lock)
        {
            *count = (uint)repl.Machines.Length;
            for (var m = 0; m < Math.Min(cap, *count); m++)
            {
                var peer = harness.Options.Side == HarnessSide.JoinedClient ? null : repl.Machines[m];
                if (ids != null) ids[m] = peer?.Peer ?? 0;
                if (flags != null) flags[m] = peer?.Flags ?? 0;
            }
        }
        return Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int RemoveRegistration(ulong handle)
    {
        // A registration must still be disposable when the test has turned a capability off.
        if (KitBackend.Instance.Harness is not { } harness) return Unavailable;
        var repl = ReplFake.StateOf(harness);
        lock (repl.Lock)
            return repl.Net.Filters.RemoveAll(f => f.Handle == handle) + repl.Net.Hooks.RemoveAll(h => h.Handle == handle) != 0
                ? Ok : NotFound;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int TrackUnavailable(ulong owner, uint direction, delegate* unmanaged[Cdecl]<TrackEvent*, uint, void*, void> fn,
        void* user, ulong* handle) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int PeerEntitiesUnavailable(uint peer, PeerEntity* entities, uint cap, uint* count) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SchemaFindUnavailable(NativeStr name, uint* size, ulong* schema) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SchemaFieldsUnavailable(ulong schema, SchemaField* fields, uint cap, uint* count) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SchemaDecodeUnavailable(ulong schema, void* bytes, uint length, void* output, uint size) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SchemaEncodeUnavailable(ulong schema, void* message, uint size, void* output, uint cap, uint* length) => Unavailable;
}

internal sealed unsafe class ReplNetState
{
    public sealed class Filter(ulong handle, ulong owner, uint hash, uint[] peers, uint channels, int priority,
        delegate* unmanaged[Cdecl]<byte*, uint, FilterCtx*, void*, void> callback, void* user)
    {
        public readonly ulong Handle = handle, Owner = owner;
        public readonly uint Hash = hash, Channels = channels;
        public readonly int Priority = priority;
        public readonly delegate* unmanaged[Cdecl]<byte*, uint, FilterCtx*, void*, void> Callback = callback;
        public readonly void* User = user;
        public uint[] Peers = peers;
    }

    public sealed record Hook(ulong Handle, ulong Owner, byte Channel, uint Directions, nint Callback, nint User);

    public readonly List<Filter> Filters = [];
    public readonly List<Hook> Hooks = [];
    public readonly List<EntityId> Touches = [];
    public readonly List<(uint Peer, GameChannel Channel, byte[] Bytes)> Injections = [];
    public Dictionary<uint, ItemSite[]> ItemSites = [];
    public HashSet<uint> GameItems = [];
    public Dictionary<uint, uint> LookAlikes = [];
    public HashSet<uint> RemapPeers = [];
    public int SiteCount;
    public ulong Snapshots, Remapped, Zeroed, Faults;
    public Func<GameChannel, ReadOnlyMemory<byte>, bool>? ValidateSend;
}

// SPDX-License-Identifier: MIT
using System.Buffers.Binary;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using static Kiln.ReplNetNative;
using static Kiln.TestKit.FakeTables;

namespace Kiln.TestKit;

internal sealed unsafe partial class ReplNetFake
{
    private static readonly uint[] Carriers =
        new[] { "CurrentTransform", "ItemState", "Inventory", "NetworkServerProgress" }
        .Select(ReplAbi.ComponentHash).ToArray();

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AddFilter(ulong owner, uint hash, uint* peers, uint peerCount, uint channels, int priority,
        uint* sources, uint sourceCount, delegate* unmanaged[Cdecl]<byte*, uint, FilterCtx*, void*, void> callback,
        void* user, ulong* handle)
    {
        if (ActiveFor(CapFilter) is not { } harness) return Unavailable;
        if (!OnTickThread) return WrongThread;
        if (peerCount > ReplAbi.Machines || (peerCount != 0 && peers == null) || sourceCount > 8 ||
            (sourceCount != 0 && sources == null) || callback == null || handle == null ||
            channels == 0 || (channels & ~(ClientChannel | PredictionChannel)) != 0) return InvalidArgument;
        // Explicit sends have no simulation invalidation step.
        if (sourceCount != 0 && (channels & ClientChannel) != 0) return Unavailable;
        var repl = ReplFake.StateOf(harness);
        lock (repl.Lock)
        {
            if (!repl.Registry.TryGetValue(hash, out var component) || component.Size is 0 or > 8192 ||
                ((channels & ClientChannel) != 0 && !component.ClientChannel) ||
                ((channels & PredictionChannel) != 0 && !component.PredictionChannel)) return InvalidArgument;
            for (var i = 0; i < sourceCount; i++)
                if (!repl.Registry.ContainsKey(sources[i])) return InvalidArgument;
            var filters = repl.Net.Filters;
            if (filters.Count >= 256 || filters.Count(f => f.Hash == hash) >= 16) return InvalidArgument;
            *handle = (ulong)Interlocked.Increment(ref _serial);
            filters.Add(new ReplNetState.Filter(*handle, owner, hash, new ReadOnlySpan<uint>(peers, (int)peerCount).ToArray(),
                channels, priority, callback, user));
        }
        return Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SetFilterPeers(ulong handle, uint* peers, uint peerCount)
    {
        if (ActiveFor(CapFilter) is not { } harness) return Unavailable;
        if (!OnTickThread) return WrongThread;
        if (peerCount > ReplAbi.Machines || (peerCount != 0 && peers == null)) return InvalidArgument;
        var repl = ReplFake.StateOf(harness);
        lock (repl.Lock)
        {
            var filter = repl.Net.Filters.Find(f => f.Handle == handle);
            if (filter is null) return NotFound;
            filter.Peers = new ReadOnlySpan<uint>(peers, (int)peerCount).ToArray();
        }
        return Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void* ReadComponent(FilterCtx* ctx, uint entity, uint hash, uint* size)
    {
        *size = 0;
        if (KitBackend.Instance.Harness?.EntitiesFake?.Block(entity, hash) is not { } slot) return null;
        *size = (uint)slot.Size;
        return (void*)slot.Data;
    }

    // The inspector resolved and copied the scripted bytes before calling this on the harness thread.
    internal static void ApplySnapshot(KilnHarness harness, ReplFakeState repl, int machine, ReplFakeState.Machine peer, FakeReplMessage message,
        IReadOnlyList<(FakeReplComponent Component, byte[] Bytes)> components)
    {
        if (Active(ReplNetNative.Service) != harness) return;
        var caps = Caps(harness);
        var net = repl.Net;
        var channel = message.Channel == 4 ? ClientChannel : PredictionChannel;
        ReplNetState.Filter[] filters;
        lock (repl.Lock)
            filters = (caps & CapFilter) == 0 ? [] : net.Filters
                .Where(f => (f.Channels & channel) != 0 && (f.Peers.Length == 0 || f.Peers.Contains(peer.Peer)))
                .OrderByDescending(f => f.Priority).ToArray();
        foreach (var (component, bytes) in components)
        {
            var hash = ReplAbi.ComponentHash(component.Name);
            byte[]? scratch = null;
            var failed = false;
            foreach (var filter in filters)
            {
                if (filter.Hash != hash) continue;
                lock (repl.Lock)
                    if (!net.Filters.Contains(filter) || harness.ReplOwnerRevoked(filter.Owner)) continue;
                scratch ??= (byte[])bytes.Clone();
                uint fault = 0;
                var ctx = new FilterCtx
                {
                    Size = (uint)sizeof(FilterCtx), Peer = peer.Peer, PeerFlags = peer.Flags, Machine = (byte)machine,
                    Channel = message.Channel, Entity = message.Entity.Value, Tick = message.Tick, ComponentHash = hash,
                    Read = &ReadComponent, Failed = &fault,
                };
                fixed (byte* data = scratch) filter.Callback(data, (uint)scratch.Length, &ctx, filter.User);
                if (fault == 0) continue;
                failed = true;
                lock (repl.Lock) net.Filters.Remove(filter);
            }
            if (scratch is not null && !failed) scratch.CopyTo(bytes, 0);
            lock (repl.Lock)
                if ((caps & CapRemap) != 0 && net.RemapPeers.Contains(peer.Peer) && net.ItemSites.TryGetValue(hash, out var sites))
                    ApplyRemap(net, sites, bytes);
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int TouchEntity(uint entity, uint* status)
    {
        if (ActiveFor(CapTouch) is not { } harness) return Unavailable;
        if (!OnTickThread) return WrongThread;
        if (entity == 0 || status == null) return InvalidArgument;
        var world = harness.EntitiesFake;
        *status = Carriers.Any(hash => world?.Block(entity, hash) is not null) ? TouchQueued
            : world?.Exists(entity) == true ? TouchNoCarrier : TouchGone;
        var repl = ReplFake.StateOf(harness);
        lock (repl.Lock) repl.Net.Touches.Add(new EntityId(entity));
        return Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SetRemap(ItemSite* sites, uint siteCount, uint* gameIds, uint gameCount, ItemMap* map, uint mapCount)
    {
        if (ActiveFor(CapRemap) is not { } harness) return Unavailable;
        if (!OnTickThread) return WrongThread;
        if (siteCount == 0)
        {
            gameCount = 0;
            mapCount = 0;
        }
        else if (sites == null || siteCount > 4096 || gameIds == null || gameCount is 0 or > (1u << 20) ||
            mapCount > 65536 || (mapCount != 0 && map == null)) return InvalidArgument;
        var grouped = new Dictionary<uint, List<ItemSite>>();
        var repl = ReplFake.StateOf(harness);
        lock (repl.Lock)
        {
            for (var i = 0; i < siteCount; i++)
            {
                var site = sites[i];
                if (!repl.Registry.TryGetValue(site.ComponentHash, out var component)) continue;
                if (site.Count == 0) site.Count = 1;
                if ((site.Count > 1 && site.Stride < 4) || EndOf(site) > component.Size) return InvalidArgument;
                if (!grouped.TryGetValue(site.ComponentHash, out var group)) grouped[site.ComponentHash] = group = [];
                group.Add(site);
            }
            var game = new HashSet<uint>(new ReadOnlySpan<uint>(gameIds, (int)gameCount).ToArray());
            var lookAlikes = new Dictionary<uint, uint>();
            for (var i = 0; i < mapCount; i++)
                if (map[i].From == 0 || game.Contains(map[i].From) ||
                    (map[i].To != 0 && !game.Contains(map[i].To)) || !lookAlikes.TryAdd(map[i].From, map[i].To))
                    return InvalidArgument;
            repl.Net.ItemSites = grouped.ToDictionary(g => g.Key, g => g.Value.ToArray());
            repl.Net.SiteCount = grouped.Values.Sum(g => g.Count);
            repl.Net.GameItems = game;
            repl.Net.LookAlikes = lookAlikes;
        }
        return Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int SetRemapPeers(uint* peers, uint count)
    {
        if (ActiveFor(CapRemap) is not { } harness) return Unavailable;
        if (!OnTickThread) return WrongThread;
        if (count > ReplAbi.Machines || (count != 0 && peers == null)) return InvalidArgument;
        var repl = ReplFake.StateOf(harness);
        lock (repl.Lock) repl.Net.RemapPeers = new HashSet<uint>(new ReadOnlySpan<uint>(peers, (int)count).ToArray());
        return Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GetRemapStats(RemapStats* output)
    {
        const int prefix = 40; // v1 callers may ask for the counters through Faults
        if (output == null || output->Size < prefix) return InvalidArgument;
        if (ActiveFor(CapRemap) is not { } harness) return Unavailable;
        var repl = ReplFake.StateOf(harness);
        lock (repl.Lock)
        {
            var net = repl.Net;
            var stats = new RemapStats
            {
                Size = output->Size, Sites = (uint)net.SiteCount,
                Running = output->Size >= sizeof(RemapStats) ? 1u : 0u, Snapshots = net.Snapshots,
                Remapped = net.Remapped, Zeroed = net.Zeroed, Faults = net.Faults,
            };
            var size = (int)Math.Min(output->Size, (uint)sizeof(RemapStats));
            new ReadOnlySpan<byte>(&stats, size).CopyTo(new Span<byte>(output, size));
        }
        return Ok;
    }

    private static long EndOf(ItemSite site) => site.Offset + (site.Count - 1L) * site.Stride + sizeof(uint);

    private static void ApplyRemap(ReplNetState net, ItemSite[] sites, byte[] bytes)
    {
        net.Snapshots++;
        var invalid = sites.Any(site => EndOf(site) > bytes.Length);
        if (invalid) net.Faults++;
        foreach (var site in sites)
        for (var i = 0; i < site.Count; i++)
        {
            var offset = site.Offset + i * (long)site.Stride;
            if (offset + sizeof(uint) > bytes.Length) break;
            var field = bytes.AsSpan((int)offset, sizeof(uint));
            if (invalid)
            {
                field.Clear();
                continue;
            }
            var id = BinaryPrimitives.ReadUInt32LittleEndian(field);
            if (id == 0 || net.GameItems.Contains(id)) continue;
            var replacement = net.LookAlikes.GetValueOrDefault(id);
            BinaryPrimitives.WriteUInt32LittleEndian(field, replacement);
            if (replacement == 0) net.Zeroed++;
            else net.Remapped++;
        }
    }
}

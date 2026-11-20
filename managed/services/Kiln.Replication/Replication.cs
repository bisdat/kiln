// SPDX-License-Identifier: MIT
// Kiln.Replication: the network inspector over kiln.repl (native/core/abi/services/kiln_repl.h; the dossier's chapter 11
// §5.2). What the server sends each peer, read only: counters per peer and channel, and watches on single entities.
using System.Collections.Concurrent;

namespace Kiln;

/// <summary>
/// The network inspector: what the server sends each player, read only. Counters per player and channel
/// (<see cref="Stats"/>), the full component values of one entity as each player is sent them (<see cref="Watch"/>), and
/// what the game's component registry says about a component (<see cref="Component"/>). Nothing here changes the game's
/// traffic.
/// <para>The counters and watches cost a little on the server's network jobs while they are on: Kiln hooks the game's
/// send path only while some mod holds the stats or a watch, and a watch copies only its own entity.</para>
/// </summary>
public static partial class Replication
{
    private static readonly object Gate = new();
    private static readonly HashSet<ulong> StatsOwners = [];
    private static ulong _serviceOwner;

    /// <summary>
    /// Counters per peer and channel: messages, bytes, creates, updates, destroys, unchanged and oversized entities,
    /// acknowledgements, resends asked for, and what the session refused. The first call by a mod turns the counters on
    /// for that mod until it unloads; they count from the moment the first holder turned them on (so a call that turns
    /// them on returns no rows yet, unless another mod or Kiln's own surfaces already hold them), and a peer's rows start
    /// over when another player takes its connection slot.
    /// </summary>
    /// <returns>One row per peer and channel that has been sent anything. A lab's fake player without a bound lab peer
    /// and a hosted world's own client have a null <see cref="ReplPeerStats.Peer"/>.</returns>
    /// <exception cref="KilnUnavailableException"><c>repl.observe</c> is off on this game build.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(ReplicationCaps.Observe)]
    public static IReadOnlyList<ReplPeerStats> Stats()
    {
        Services.RequireTickThread("Replication.Stats");
        RequireObserve();
        return StatsFor(OwnerOfCaller(), ModContext.CurrentMod);
    }

    /// <summary>What the game's component registry says about the component called <paramref name="name"/> (its
    /// short name, as Kiln's world reads use): its index, size, and how it travels.</summary>
    /// <returns>Null when this world has no such component, or no world was seen yet.</returns>
    /// <exception cref="ArgumentException"><paramref name="name"/> is null or empty.</exception>
    [AnyThread]
    public static ReplComponentInfo? Component(string name)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        if (ReplNative.ComponentInfoOf(ReplNative.ComponentHash(name), out var info) != ReplNative.Ok) return null;
        return new ReplComponentInfo(name, info.Index, info.DataSize, info.Hash,
            (info.Flags & ReplNative.CompReplicated) != 0, (info.Flags & ReplNative.CompVersioned) != 0,
            (info.Flags & ReplNative.CompCustomCodec) != 0, (info.Flags & ReplNative.CompClient) != 0,
            (info.Flags & ReplNative.CompPrediction) != 0);
    }

    /// <summary>
    /// Watches what the server sends of <paramref name="entity"/>: one <see cref="ReplWatch.Sent"/> event per message to
    /// <paramref name="peer"/> (every peer when null) with the full component values the game coded, then
    /// <see cref="ReplKind.Destroyed"/> when a destroy list names it, and <see cref="ReplWatch.Ended"/> as the last event.
    /// Events come on the tick thread at the start of the next step. Dispose the watch when you are done; it also ends
    /// when your mod unloads.
    /// </summary>
    /// <exception cref="ArgumentException"><paramref name="entity"/> is <see cref="EntityId.None"/>, or
    /// <paramref name="peer"/> is not connected.</exception>
    /// <exception cref="InvalidOperationException">64 watches are on already.</exception>
    /// <exception cref="KilnUnavailableException"><c>repl.observe</c> is off on this game build.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(ReplicationCaps.Observe)]
    public static ReplWatch Watch(EntityId entity, NetPeer? peer = null)
    {
        Services.RequireTickThread("Replication.Watch");
        if (entity.IsNone) throw new ArgumentException("no entity", nameof(entity));
        RequireObserve();
        return ReplWatch.Start(OwnerOfCaller(), ModContext.CurrentMod, entity, peer?.Id ?? ReplNative.AllMachines, peer,
            ReplNative.Send);
    }

    /// <summary>
    /// Watches what this client receives of <paramref name="entity"/>: one <see cref="ReplWatch.Received"/> event per
    /// message with the values the game decoded, then <see cref="ReplWatch.Ended"/>. Events come on the client's main
    /// thread at the start of its next step.
    /// </summary>
    /// <exception cref="ArgumentException"><paramref name="entity"/> is <see cref="EntityId.None"/>.</exception>
    /// <exception cref="KilnUnavailableException"><c>repl.client</c> is off: not a game client, or this game build's
    /// receive side is not supported.</exception>
    [ClientOnly]
    [TickThreadOnly]
    [RequiresCapability(ReplicationCaps.Client)]
    public static ReplWatch WatchReceived(EntityId entity)
    {
        if (entity.IsNone) throw new ArgumentException("no entity", nameof(entity));
        ReplicationCaps.Rows.Ensure();
        if ((ReplNative.Capabilities() & ReplNative.CapClient) == 0 || !Capabilities.Is(ReplicationCaps.Client))
            throw ReplicationCaps.Off(ReplicationCaps.Client, "the receive side runs on a game client");
        return ReplWatch.Start(OwnerOfCaller(), ModContext.CurrentMod, entity, ReplNative.AllMachines, null, ReplNative.Receive);
    }

    // ---- Kiln's own surfaces (commands, the menu, Kiln.Admin) ----

    /// <summary>The counters for <paramref name="owner"/>: turned on for it at the first call (tick thread).</summary>
    internal static IReadOnlyList<ReplPeerStats> StatsFor(ulong owner, ModContext? mod = null)
    {
        lock (Gate)
        {
            if (!StatsOwners.Contains(owner))
            {
                var rc = ReplNative.StatsEnable(owner, true);
                if (rc == ReplNative.Unavailable) throw ReplicationCaps.Off(ReplicationCaps.Observe, "kiln.repl refused the stats");
                if (rc != ReplNative.Ok) throw new InvalidOperationException($"kiln.repl refused the stats ({rc})");
                StatsOwners.Add(owner);
                // kiln.repl lets go of a mod's hold when the mod's owner is revoked; this list follows
                if (mod is not null && mod.Owner == owner) mod.Retiring += () => { lock (Gate) StatsOwners.Remove(owner); };
            }
        }
        var rcRows = ReplNative.PeerStatsAll(out var rows);
        if (rcRows != ReplNative.Ok) throw new InvalidOperationException($"kiln.repl could not list the stats ({rcRows})");
        var peers = PeerTable();
        var list = new List<ReplPeerStats>(rows.Length);
        foreach (var r in rows)
        {
            var (peer, local, fake) = Resolve(r.Peer, r.Flags, peers);
            list.Add(new ReplPeerStats(peer, (ReplChannel)r.Channel, (long)r.Frames, (long)r.Messages, (long)r.Bytes, (long)r.Refused,
                (long)r.Creates, (long)r.Updates, (long)r.DestroyLists, (long)r.DestroyedIds, (long)r.Unchanged, (long)r.TooBig,
                (long)r.Acks, (long)r.Resets, (long)r.Dropped) { PeerId = r.Peer, Local = local, Fake = fake });
        }
        return list;
    }

    /// <summary>Whether <paramref name="owner"/> holds the counters.</summary>
    internal static bool StatsOn(ulong owner)
    {
        lock (Gate) return StatsOwners.Contains(owner);
    }

    /// <summary>Turns the counters off for <paramref name="owner"/> (tick thread); the hooks go with the last holder.</summary>
    internal static void StopStats(ulong owner)
    {
        lock (Gate)
        {
            if (!StatsOwners.Remove(owner)) return;
        }
        ReplNative.StatsEnable(owner, false);
    }

    /// <summary>Kiln's own owner for the commands and the menu (made once; never revoked).</summary>
    internal static ulong ServiceOwner
    {
        get
        {
            lock (Gate)
            {
                if (_serviceOwner == 0) _serviceOwner = Backend.Current.OwnerCreate("kiln.replication");
                return _serviceOwner;
            }
        }
    }

    /// <summary>The native owner of the calling mod, or Kiln's own for code that runs outside a mod.</summary>
    internal static ulong OwnerOfCaller() => ModContext.CurrentMod is { Owner: not 0 } mod ? mod.Owner : ServiceOwner;

    internal static void RequireObserve()
    {
        ReplicationCaps.Rows.Ensure();
        if ((ReplNative.Capabilities() & ReplNative.CapObserve) == 0 || !Capabilities.Is(ReplicationCaps.Observe))
            throw ReplicationCaps.Off(ReplicationCaps.Observe, "this game build's replication is not supported");
    }

    /// <summary>The server's peers by id, now (kiln.net's list).</summary>
    internal static Dictionary<uint, NetPeer> PeerTable()
    {
        var d = new Dictionary<uint, NetPeer>();
        try
        {
            foreach (var p in Network.Peers) d[p.Id] = p;
        }
        catch (Exception e) when (e is TypeLoadException or FileNotFoundException or MissingMethodException)
        {
            // no Kiln.Network: rows carry their peer ids only
        }
        return d;
    }

    internal static (NetPeer? Peer, bool Local, bool Fake) Resolve(uint id, byte flags, Dictionary<uint, NetPeer> peers)
    {
        var local = (flags & ReplNative.PeerLocal) != 0;
        var fake = (flags & ReplNative.PeerFake) != 0;
        if (local || (fake && (flags & ReplNative.PeerBound) == 0)) return (null, local, fake);
        return (peers.GetValueOrDefault(id), local, fake);
    }

    // ---- component names (hash -> the game's short name), for watch events ----
    private static readonly ConcurrentDictionary<uint, string> Names = new();

    internal static string NameOf(uint hash, ushort index)
    {
        if (Names.TryGetValue(hash, out var n)) return n;
        string? name = null;
        try
        {
            name = GameTypes.ByHash(hash)?.Name;
        }
        catch (Exception)
        {
            // kiln.types is not there: the registry index names it
        }
        if (string.IsNullOrEmpty(name)) return "#" + index.ToString(System.Globalization.CultureInfo.InvariantCulture);
        var colon = name.LastIndexOf("::", StringComparison.Ordinal);
        if (colon >= 0) name = name[(colon + 2)..];
        return Names[hash] = name;
    }
}

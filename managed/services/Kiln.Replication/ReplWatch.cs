// SPDX-License-Identifier: MIT
// A watch on one entity's replication (kiln_repl.h watch / remove; the dossier's chapter 11 §4.1, §5.2).
using System.Collections.Concurrent;
using System.Collections.Frozen;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>
/// A watch on what one entity's replication sends (<see cref="Replication.Watch"/>) or receives
/// (<see cref="Replication.WatchReceived"/>). Events come on the tick thread (a server) or the main thread (a client) at
/// the start of the next step, in the order the game sent the messages; <see cref="Ended"/> is the last one.
/// </summary>
public sealed unsafe class ReplWatch : IDisposable
{
    private static readonly ConcurrentDictionary<long, ReplWatch> Live = new();
    private static long _next;
    private const int MaxWatches = 64;

    private readonly long _id;
    private readonly ModContext? _mod;
    private ulong _handle;
    private int _disposed;

    private ReplWatch(long id, ModContext? mod, EntityId entity, NetPeer? peer)
    {
        _id = id;
        _mod = mod;
        Entity = entity;
        Peer = peer;
    }

    /// <summary>The watched entity.</summary>
    public EntityId Entity { get; }

    /// <summary>The one peer the watch is for; null for every peer (and on a client).</summary>
    public NetPeer? Peer { get; }

    /// <summary>The watch still delivers events: not disposed, not ended.</summary>
    public bool IsActive => Volatile.Read(ref _disposed) == 0 && !HasEnded;

    /// <summary>The watch has ended (its <see cref="Ended"/> event came).</summary>
    public bool HasEnded { get; private set; }

    /// <summary>A message a peer was sent about the entity (a server's watch).</summary>
    public event Action<ReplSent>? Sent;

    /// <summary>A message this client received about the entity (a client's watch).</summary>
    public event Action<ReplReceived>? Received;

    /// <summary>The watch is over (destroyed, the id reused for another template, the peer left, the world stopped).
    /// Its last event; dispose the watch afterwards (or not: it holds nothing then).</summary>
    public event Action<ReplWatchEnded>? Ended;

    /// <summary>Stops the watch: no event comes after the start of the next step. Idempotent; any thread.</summary>
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        Live.TryRemove(_id, out _);
        Release();
    }

    /// <summary>Gives kiln.repl's watch slot back (once): an ended watch keeps its slot there until it is removed.</summary>
    private void Release()
    {
        var handle = Interlocked.Exchange(ref _handle, 0);
        if (handle != 0) ReplNative.Remove(handle);
    }

    internal static ReplWatch Start(ulong owner, ModContext? mod, EntityId entity, uint peerOrAll, NetPeer? peer, uint direction)
    {
        if (Live.Count >= MaxWatches) throw new InvalidOperationException($"{MaxWatches} replication watches are on already");
        var id = Interlocked.Increment(ref _next);
        var w = new ReplWatch(id, mod, entity, peer);
        Live[id] = w;
        var rc = ReplNative.Watch(owner, entity.Value, peerOrAll, direction, &OnEvent, (nint)id, out var handle);
        if (rc != ReplNative.Ok)
        {
            Live.TryRemove(id, out _);
            throw rc switch
            {
                ReplNative.InvalidArgument when peer is not null => new ArgumentException($"peer {peer.Name} is not connected", nameof(peer)),
                ReplNative.InvalidArgument => new InvalidOperationException($"kiln.repl has no room for another watch ({MaxWatches})"),
                ReplNative.WrongThread => new InvalidOperationException(direction == ReplNative.Receive
                    ? "Replication.WatchReceived must be called on the game's main thread (a client mod's tick)"
                    : "Replication.Watch must be called on the tick thread"),
                ReplNative.Unavailable => ReplicationCaps.Off(direction == ReplNative.Receive ? ReplicationCaps.Client : ReplicationCaps.Observe,
                    "kiln.repl could not hook the game for a watch (see kiln.log)"),
                _ => new InvalidOperationException($"kiln.repl refused the watch ({rc})"),
            };
        }
        w._handle = handle;
        if (mod is not null) mod.Retiring += w.Dispose;
        return w;
    }

    /// <summary>Every live watch's handler runs here (kiln.repl's Pre, tick or main thread).</summary>
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnEvent(ReplNative.WatchEvent* e, void* user)
    {
        try
        {
            if (e == null || !Live.TryGetValue((long)(nint)user, out var w) || Volatile.Read(ref w._disposed) != 0) return;
            w.Deliver(e);
        }
        catch (Exception)
        {
            // never unwind into kiln.repl; a handler's own exception is reported in Deliver
        }
    }

    private void Deliver(ReplNative.WatchEvent* e)
    {
        var outer = ModContext.CurrentMod;
        try
        {
            ModContext.CurrentMod = _mod;
            if (e->Kind == ReplNative.KindEnded)
            {
                HasEnded = true;
                Live.TryRemove(_id, out _);
                Release();  // the slot goes back now: a mod need not dispose an ended watch
                Ended?.Invoke(new ReplWatchEnded(Entity, e->Reason switch
                {
                    1 => "destroyed",
                    2 => "id reused",
                    3 => "peer left",
                    4 => "world stopped",
                    _ => "ended",
                }));
                return;
            }
            var kind = (ReplKind)e->Kind;
            var components = new Dictionary<string, ReadOnlyMemory<byte>>(e->Count, StringComparer.Ordinal);
            HashSet<string>? packed = null;
            for (var i = 0; i < e->Count; i++)
            {
                var c = e->Components[i];
                var name = Replication.NameOf(c.Hash, c.Index);
                components[name] = c.Data == null ? ReadOnlyMemory<byte>.Empty : new ReadOnlySpan<byte>(c.Data, c.Size).ToArray();
                if (c.CustomCodec != 0) (packed ??= new HashSet<string>(StringComparer.Ordinal)).Add(name);
            }
            if (e->Direction == ReplNative.Receive)
            {
                Received?.Invoke(new ReplReceived(kind, new EntityId(e->Entity), e->Tick, e->Gap != 0, components));
                return;
            }
            var (peer, local, fake) = Replication.Resolve(e->Peer, e->PeerFlags, Replication.PeerTable());
            Sent?.Invoke(new ReplSent(peer, (ReplChannel)e->Channel, kind, new EntityId(e->Entity), e->Tick, e->Seq, e->BaselineTick,
                (int)e->PayloadBytes, e->Gap != 0, components, (IReadOnlySet<string>?)packed ?? FrozenSet<string>.Empty)
            { PeerId = e->Peer, Local = local, Fake = fake });
        }
        catch (Exception ex)
        {
            Report($"a replication watch handler of entity {Entity.Value} threw", ex);
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    private void Report(string what, Exception e)
    {
        try
        {
            if (_mod != null) _mod.Log.Error(e, what);
            else Backend.Current.Log(0, LogLevel.Error, $"{what}: {e}");
        }
        catch
        {
            // logging must never unwind into the game's thread
        }
    }
}
